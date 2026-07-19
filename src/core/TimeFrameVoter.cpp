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

// True when the two-digit year names a Gregorian leap year, under the 20xx
// century assumption (full year = 2000 + year2). Within 2000..2099 this reduces
// to year2 % 4 == 0 — 2000 is a 400-divisible leap year and no year in that
// span is century-divisible — but the full rule is written out so the helper
// stays correct if the century assumption is ever revisited.
inline bool isLeapYear2(int year2) {
    const int y = 2000 + year2;
    return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
}

} // namespace

// Forward calendar arithmetic with minute/hour/doy/year carries. `minutes` is
// small in practice (bounded by the voter window), but the loops are written to
// carry across any number of day/year boundaries.
TimeFields advanceMinutes(TimeFields t, int minutes) {
    t.minute += minutes;
    t.hour   += t.minute / 60;
    t.minute %= 60;
    int carryDays = t.hour / 24;
    t.hour %= 24;
    t.doy += carryDays;
    // doy is 1-based; wrap at the current year's length, carrying year2 (mod 100).
    for (int len = isLeapYear2(t.year2) ? 366 : 365; t.doy > len;
         len = isLeapYear2(t.year2) ? 366 : 365) {
        t.doy -= len;
        t.year2 = (t.year2 + 1) % 100;
    }
    return t;
}

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

// Per-frame BCD field decode: sum the field-map weights whose second classified
// as One. Marker/Unknown contribute nothing.
int TimeFrameVoter::decodeField(const Frame& f, FieldIndex field) const {
    int value = 0;
    for (const auto& bw : m_cfg.fields[field]) {
        if (bw.second >= 0 && bw.second < 60 &&
            f.symbols[bw.second] == ClockSymbol::One) {
            value += bw.weight;
        }
    }
    return value;
}

int TimeFrameVoter::decodeMinutes(const Frame& f) const {
    return decodeField(f, FieldMinutes);
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

TimeFields TimeFrameVoter::votedTimestamp() const {
    const int n = static_cast<int>(m_frames.size());

    // Field-map bit indices whose per-frame confidences represent a frame's
    // timestamp-decode quality (union of the four voted field maps).
    auto meanTimestampConfidence = [&](const Frame& f) -> double {
        double sum = 0.0;
        int count = 0;
        for (const FieldIndex fld : {FieldMinutes, FieldHours, FieldDoy, FieldYear}) {
            for (const auto& bw : m_cfg.fields[fld]) {
                if (bw.second >= 0 && bw.second < 60) {
                    sum += f.confidence[bw.second];
                    ++count;
                }
            }
        }
        return count ? sum / count : 0.0;
    };

    // Encode a range-valid timestamp tuple to a collision-free key so equal
    // extrapolated timestamps accumulate their weight together.
    auto encode = [](const TimeFields& t) -> int64_t {
        return (static_cast<int64_t>(t.year2) * 100000000) +
               (static_cast<int64_t>(t.doy) * 100000) +
               (static_cast<int64_t>(t.hour) * 1000) +
               static_cast<int64_t>(t.minute);
    };

    std::unordered_map<int64_t, double> weightByTuple;
    std::unordered_map<int64_t, TimeFields> tupleByKey;

    for (int i = 0; i < n; ++i) {
        const int age = (n - 1) - i;  // newest -> age 0
        const Frame& f = m_frames[i];

        TimeFields tf;
        tf.minute = decodeField(f, FieldMinutes);
        tf.hour   = decodeField(f, FieldHours);
        tf.doy    = decodeField(f, FieldDoy);
        tf.year2  = decodeField(f, FieldYear);

        // Exclude frames whose fields decode outside valid broadcast ranges —
        // the same spirit as excluding Marker/Unknown symbols from a bit vote.
        if (tf.minute < 0 || tf.minute > 59) continue;
        if (tf.hour   < 0 || tf.hour   > 23) continue;
        if (tf.doy    < 1 || tf.doy    > 366) continue;
        if (tf.year2  < 0 || tf.year2  > 99) continue;

        // Extrapolate this frame's timestamp forward to the newest frame, so all
        // in-window frames vote on the SAME (newest) timestamp.
        const TimeFields ext = advanceMinutes(tf, age);
        const double w =
            agedWeight(static_cast<float>(meanTimestampConfidence(f)),
                       m_cfg.agingFactor, age);
        const int64_t key = encode(ext);
        weightByTuple[key] += w;
        tupleByKey[key] = ext;
    }

    int64_t bestKey = 0;
    double bestWeight = -1.0;
    bool have = false;
    for (const auto& [key, weight] : weightByTuple) {
        if (weight > bestWeight) {
            bestWeight = weight;
            bestKey = key;
            have = true;
        }
    }
    return have ? tupleByKey[bestKey] : TimeFields{};
}

int TimeFrameVoter::votedField(FieldIndex field) const {
    if (m_frames.empty()) {
        return -1;
    }
    const TimeFields t = votedTimestamp();
    switch (field) {
        case FieldMinutes: return t.minute;
        case FieldHours:   return t.hour;
        case FieldDoy:     return t.doy;
        case FieldYear:    return t.year2;
        default:           return -1;
    }
}

int TimeFrameVoter::lastFrameMinute() const {
    if (m_frames.empty()) {
        return -1;
    }
    return decodeMinutes(m_frames.back());
}

float TimeFrameVoter::lockConfidence() const {
    // Deliberately retained as the per-bit winning-margin over the static-field
    // bits of the window, NOT switched to the winning-tuple weight margin. It
    // measures raw bit stability, which is the honest quantity here: during an
    // hour/day rollover the window straddles two timestamps, the per-bit margins
    // dip, and reporting that dip is correct — quality SHOULD soften through a
    // transition even though the tuple vote (votedTimestamp) now resolves the
    // reported time unambiguously.
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
