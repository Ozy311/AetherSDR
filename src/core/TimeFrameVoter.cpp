#include "TimeFrameVoter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

// Cross-frame confidence-weighted bit voting per the AetherClock reference
// decoder: a bit's vote is the sum of its per-frame matched-filter margins
// (floored at 0.01), with older frames discounted by agingFactor^age. Markers
// and Unknown symbols never vote. Lock gates on consecutive +1 minute
// increments plus self-consistent static fields.

namespace AetherSDR {

namespace {

// Per-frame vote weight for one static bit: max(confidence, 0.01) aged by
// agingFactor^age (age 0 = newest frame).
inline float agedWeight(float confidence, float agingFactor, int age) {
    const float base = std::max(confidence, 0.01f);
    return base * std::pow(agingFactor, static_cast<float>(age));
}

} // namespace

TimeFrameVoter::TimeFrameVoter(Config cfg) : m_cfg(std::move(cfg)) {}

void TimeFrameVoter::addFrame(const std::array<ClockSymbol, 60>& symbols,
                              const std::array<float, 60>& confidence) {
    Frame f;
    f.symbols = symbols;
    f.confidence = confidence;
    m_frames.push_back(f);
    // Drop oldest beyond the sliding window (newest kept at the back).
    while (m_frames.size() > m_cfg.window) {
        m_frames.erase(m_frames.begin());
    }
}

void TimeFrameVoter::reset() {
    m_frames.clear();
}

int TimeFrameVoter::frameCount() const {
    return static_cast<int>(m_frames.size());
}

// Per-frame BCD minutes decode: sum the minutes-map weights whose second
// classified as One. Marker/Unknown contribute nothing.
int TimeFrameVoter::decodeMinutes(const Frame& f) const {
    int value = 0;
    for (const auto& bw : m_cfg.fields[FieldMinutes]) {
        if (bw.second >= 0 && bw.second < 60 &&
            f.symbols[bw.second] == ClockSymbol::One) {
            value += bw.weight;
        }
    }
    return value;
}

bool TimeFrameVoter::locked() const {
    const int n = static_cast<int>(m_frames.size());
    if (n < m_cfg.minFramesForLock) {
        return false;
    }

    // Count adjacent frame pairs whose decoded minutes increment by exactly +1
    // (mod 60; 59 -> 0 valid). Live per-frame decodes are routinely imperfect,
    // so increment support is COUNTED across the window rather than required of
    // every pair (cf. the reference's marker_score + 4 * increment_count) — one
    // corrupted frame must not permanently prevent or drop the lock.
    int increments = 0;
    for (int i = 1; i < n; ++i) {
        const int prev = decodeMinutes(m_frames[i - 1]);
        const int cur = decodeMinutes(m_frames[i]);
        if (((prev + 1) % 60) == cur) {
            ++increments;
        }
    }
    if (increments < m_cfg.minFramesForLock - 1) {
        return false;
    }

    // Confidence-weighted vote weights for one mapped second across the window.
    auto bitWeights = [&](int second) -> std::pair<float, float> {
        float w0 = 0.0f, w1 = 0.0f;
        if (second < 0 || second >= 60) {
            return {w0, w1};
        }
        for (int i = 0; i < n; ++i) {
            const int age = (n - 1) - i; // newest -> age 0
            const Frame& f = m_frames[i];
            const ClockSymbol s = f.symbols[second];
            if (s == ClockSymbol::One || s == ClockSymbol::Zero) {
                const float w =
                    agedWeight(f.confidence[second], m_cfg.agingFactor, age);
                (s == ClockSymbol::One ? w1 : w0) += w;
            }
        }
        return {w0, w1};
    };

    // Static-field self-consistency: each static field must carry at least one
    // confident bit vote (a strictly positive winning margin). An all-Unknown
    // window yields zero margin everywhere and must not lock.
    for (std::size_t fi = FieldHours; fi <= FieldYear; ++fi) {
        double margin = 0.0;
        for (const auto& bw : m_cfg.fields[fi]) {
            const auto [w0, w1] = bitWeights(bw.second);
            margin += std::max(w0, w1) - std::min(w0, w1);
        }
        if (!(margin > 0.0)) {
            return false;
        }
    }

    return true;
}

int TimeFrameVoter::votedField(FieldIndex field) const {
    const int n = static_cast<int>(m_frames.size());
    if (n == 0) {
        return -1;
    }

    // FieldMinutes is special: bits change frame-to-frame, so vote on the
    // decoded VALUE normalized to the newest frame (older frames + their age,
    // mod 60), weighted by each frame's mean minutes-map confidence, aged.
    if (field == FieldMinutes) {
        const auto& mmap = m_cfg.fields[FieldMinutes];
        std::unordered_map<int, double> weightByValue;
        for (int i = 0; i < n; ++i) {
            const int age = (n - 1) - i;
            const Frame& f = m_frames[i];
            const int normalized = ((decodeMinutes(f) + age) % 60 + 60) % 60;

            double confSum = 0.0;
            int confCount = 0;
            for (const auto& bw : mmap) {
                if (bw.second >= 0 && bw.second < 60) {
                    confSum += f.confidence[bw.second];
                    ++confCount;
                }
            }
            const double meanConf = confCount ? confSum / confCount : 0.0;
            weightByValue[normalized] +=
                meanConf * std::pow(static_cast<double>(m_cfg.agingFactor),
                                    static_cast<double>(age));
        }

        int best = -1;
        double bestWeight = -1.0;
        for (const auto& [value, weight] : weightByValue) {
            if (weight > bestWeight) {
                bestWeight = weight;
                best = value;
            }
        }
        return best;
    }

    // Static fields: per-bit confidence-weighted majority; sum the weights of
    // the bits that voted One.
    int value = 0;
    for (const auto& bw : m_cfg.fields[field]) {
        if (bw.second < 0 || bw.second >= 60) {
            continue;
        }
        float w0 = 0.0f, w1 = 0.0f;
        for (int i = 0; i < n; ++i) {
            const int age = (n - 1) - i;
            const Frame& f = m_frames[i];
            const ClockSymbol s = f.symbols[bw.second];
            if (s == ClockSymbol::One || s == ClockSymbol::Zero) {
                const float w =
                    agedWeight(f.confidence[bw.second], m_cfg.agingFactor, age);
                (s == ClockSymbol::One ? w1 : w0) += w;
            }
        }
        if (w1 > w0) {
            value += bw.weight;
        }
    }
    return value;
}

int TimeFrameVoter::lastFrameMinute() const {
    if (m_frames.empty()) {
        return -1;
    }
    return decodeMinutes(m_frames.back());
}

float TimeFrameVoter::lockConfidence() const {
    const int n = static_cast<int>(m_frames.size());
    if (n < m_cfg.minFramesForLock) {
        return 0.0f;
    }

    constexpr float kEpsilon = 1e-6f;
    double marginSum = 0.0;
    int bitCount = 0;

    // Mean normalized winning margin over every static-field mapped bit.
    for (std::size_t fi = FieldHours; fi <= FieldYear; ++fi) {
        for (const auto& bw : m_cfg.fields[fi]) {
            if (bw.second < 0 || bw.second >= 60) {
                continue;
            }
            float w0 = 0.0f, w1 = 0.0f;
            for (int i = 0; i < n; ++i) {
                const int age = (n - 1) - i;
                const Frame& f = m_frames[i];
                const ClockSymbol s = f.symbols[bw.second];
                if (s == ClockSymbol::One || s == ClockSymbol::Zero) {
                    const float w = agedWeight(f.confidence[bw.second],
                                               m_cfg.agingFactor, age);
                    (s == ClockSymbol::One ? w1 : w0) += w;
                }
            }
            const float winning = std::max(w0, w1);
            const float losing = std::min(w0, w1);
            marginSum += (winning - losing) / (w0 + w1 + kEpsilon);
            ++bitCount;
        }
    }

    const double meanMargin = bitCount ? marginSum / bitCount : 0.0;
    const double saturation =
        std::min(1.0, static_cast<double>(n) /
                          (2.0 * static_cast<double>(m_cfg.minFramesForLock)));
    const double quality = meanMargin * saturation;
    return static_cast<float>(std::clamp(quality, 0.0, 1.0));
}

} // namespace AetherSDR
