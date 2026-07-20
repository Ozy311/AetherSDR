#include "TimeFrameVoter.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// Cross-frame confidence-weighted voting per the AetherClock reference decoder:
// a bit's vote is the sum of its per-frame matched-filter margins (floored at
// 0.01), with older frames discounted by agingFactor^age. Markers and Unknown
// symbols never vote. The timestamp vote is NORMALIZE-then-per-bit: every frame
// is first extrapolated to the newest epoch (removing the epoch skew that makes
// a stale hour dangerous), then bits are voted in that normalized space, where
// frames are supposed to agree. Lock gates on consecutive +1 minute increments
// plus self-consistent static fields.

namespace AetherSDR {

namespace {

// Per-frame vote weight for one bit: max(confidence, 0.01) aged by
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

std::vector<TimeFrameVoter::NormalizedFrame>
TimeFrameVoter::buildNormalizedFrames() const {
    const int n = static_cast<int>(m_frames.size());
    std::vector<NormalizedFrame> out;
    out.reserve(static_cast<std::size_t>(n));

    // Greedy descending-weight BCD encode of `value` over a field map: the maps
    // carry canonical BCD weights, so subtracting weights largest-first
    // reconstructs the exact bit pattern (One/Zero, parallel to the map).
    auto encodeField = [](const ClockFieldMap& map, int value) {
        std::vector<ClockSymbol> bits(map.size(), ClockSymbol::Zero);
        std::vector<std::size_t> order(map.size());
        for (std::size_t k = 0; k < map.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return map[a].weight > map[b].weight;
                  });
        int rem = value;
        for (std::size_t k : order) {
            if (map[k].weight > 0 && map[k].weight <= rem) {
                bits[k] = ClockSymbol::One;
                rem -= map[k].weight;
            }
        }
        return bits;
    };

    // Weakest of the original per-second confidences over one field map, floored
    // at 0.01 (a re-encoded field weights every bit by this). A BCD field VALUE is
    // exactly as reliable as its least-reliable bit — one flipped bit changes the
    // whole value — so a re-encoded value's vote must carry the weakest bit's
    // margin, never the average, which would dilute a single faded bit ~1/N.
    auto fieldMinConf = [&](const Frame& f, FieldIndex fld) -> float {
        float lo = -1.0f;
        for (const auto& bw : m_cfg.fields[fld]) {
            if (bw.second >= 0 && bw.second < 60) {
                lo = (lo < 0.0f) ? f.confidence[bw.second]
                                 : std::min(lo, f.confidence[bw.second]);
            }
        }
        return std::max(lo < 0.0f ? 0.0f : lo, 0.01f);
    };

    for (int i = 0; i < n; ++i) {
        const int age = (n - 1) - i;  // newest -> age 0
        const Frame& f = m_frames[i];

        TimeFields raw;
        raw.minute = decodeField(f, FieldMinutes);
        raw.hour   = decodeField(f, FieldHours);
        raw.doy    = decodeField(f, FieldDoy);
        raw.year2  = decodeField(f, FieldYear);

        // Whole-frame range gate — a frame decoding outside valid broadcast
        // ranges is excluded, the same spirit as excluding Marker/Unknown from a
        // bit vote. (Guards the calendar arithmetic below, too.)
        if (raw.minute < 0 || raw.minute > 59) continue;
        if (raw.hour   < 0 || raw.hour   > 23) continue;
        if (raw.doy    < 1 || raw.doy    > 366) continue;
        if (raw.year2  < 0 || raw.year2  > 99) continue;

        NormalizedFrame nf;
        nf.age = age;
        nf.ext = advanceMinutes(raw, age);

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
        nf.meanConfidence = count ? sum / count : 0.0;

        const int rawByField[FieldCount] = {raw.minute, raw.hour, raw.doy, raw.year2};
        const int extByField[FieldCount] = {nf.ext.minute, nf.ext.hour,
                                            nf.ext.doy, nf.ext.year2};

        for (std::size_t fld = 0; fld < FieldCount; ++fld) {
            const ClockFieldMap& map = m_cfg.fields[fld];
            std::vector<NormalizedFrame::Bit>& dst = nf.bits[fld];
            dst.resize(map.size());

            if (extByField[fld] == rawByField[fld]) {
                // Extrapolation left this field unchanged (the common case away
                // from a boundary): vote its ORIGINAL symbols with their ORIGINAL
                // per-second confidences — a faded bit carries a low matched-
                // filter margin and loses the cross-frame vote. This is the
                // reference model that rescues single-bit fades in noisy corpora.
                for (std::size_t k = 0; k < map.size(); ++k) {
                    const int sec = map[k].second;
                    const bool inRange = sec >= 0 && sec < 60;
                    dst[k].symbol = inRange ? f.symbols[sec] : ClockSymbol::Unknown;
                    dst[k].confidence = inRange ? f.confidence[sec] : 0.0f;
                }
            } else {
                // Extrapolation moved this field across a carry: re-encode the
                // NORMALIZED value and weight every bit by the field-MIN original
                // confidence, so a value whose weakest bit faded votes at that
                // faded margin. Blending is thus confined to normalized space,
                // where the frames are supposed to agree.
                const std::vector<ClockSymbol> bits = encodeField(map, extByField[fld]);
                const float w = fieldMinConf(f, static_cast<FieldIndex>(fld));
                for (std::size_t k = 0; k < map.size(); ++k) {
                    dst[k].symbol = bits[k];
                    dst[k].confidence = w;
                }
            }
        }
        out.push_back(std::move(nf));
    }
    return out;
}

// Per-field, per-bit tally across the normalized window: heavier of One/Zero
// wins. `frames` is the shared normalized view.
namespace {
struct BitTally { float w0 = 0.0f, w1 = 0.0f; };
} // namespace

TimeFields TimeFrameVoter::votedTimestamp() const {
    const std::vector<NormalizedFrame> frames = buildNormalizedFrames();
    if (frames.empty()) {
        return TimeFields{};
    }

    int valueByField[FieldCount] = {0, 0, 0, 0};
    for (std::size_t fld = 0; fld < FieldCount; ++fld) {
        const ClockFieldMap& map = m_cfg.fields[fld];
        for (std::size_t k = 0; k < map.size(); ++k) {
            BitTally t;
            for (const NormalizedFrame& nf : frames) {
                const NormalizedFrame::Bit b = nf.bits[fld][k];
                if (b.symbol == ClockSymbol::One || b.symbol == ClockSymbol::Zero) {
                    const float w = agedWeight(b.confidence, m_cfg.agingFactor, nf.age);
                    (b.symbol == ClockSymbol::One ? t.w1 : t.w0) += w;
                }
            }
            if (t.w1 > t.w0) {
                valueByField[fld] += map[k].weight;
            }
        }
    }

    TimeFields voted;
    voted.minute = valueByField[FieldMinutes];
    voted.hour   = valueByField[FieldHours];
    voted.doy    = valueByField[FieldDoy];
    voted.year2  = valueByField[FieldYear];

    // Final-range guard: per-bit blending can in principle synthesize an
    // out-of-range BCD value (e.g. a minute summing 5+10+20+40). If it did, fall
    // back to the single highest-aged-weight frame's extrapolated tuple rather
    // than emit an impossible broadcast time.
    const bool inRange =
        voted.minute >= 0 && voted.minute <= 59 &&
        voted.hour   >= 0 && voted.hour   <= 23 &&
        voted.doy    >= 1 && voted.doy    <= 366 &&
        voted.year2  >= 0 && voted.year2  <= 99;
    if (inRange) {
        return voted;
    }

    const NormalizedFrame* best = nullptr;
    double bestWeight = -1.0;
    for (const NormalizedFrame& nf : frames) {
        const double w = agedWeight(static_cast<float>(nf.meanConfidence),
                                    m_cfg.agingFactor, nf.age);
        if (w > bestWeight) {
            bestWeight = w;
            best = &nf;
        }
    }
    return best ? best->ext : TimeFields{};
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

    // Static-field self-consistency in the SAME normalized space the vote uses:
    // each static field must carry at least one confident bit vote (a strictly
    // positive winning margin). No qualifying frame (e.g. an all-Unknown window
    // whose doy decodes out of range) yields zero margin everywhere — must not
    // lock.
    const std::vector<NormalizedFrame> frames = buildNormalizedFrames();
    if (frames.empty()) {
        return false;
    }
    for (std::size_t fi = FieldHours; fi <= FieldYear; ++fi) {
        const ClockFieldMap& map = m_cfg.fields[fi];
        double margin = 0.0;
        for (std::size_t k = 0; k < map.size(); ++k) {
            BitTally t;
            for (const NormalizedFrame& nf : frames) {
                const NormalizedFrame::Bit b = nf.bits[fi][k];
                if (b.symbol == ClockSymbol::One || b.symbol == ClockSymbol::Zero) {
                    const float w = agedWeight(b.confidence, m_cfg.agingFactor, nf.age);
                    (b.symbol == ClockSymbol::One ? t.w1 : t.w0) += w;
                }
            }
            margin += std::max(t.w0, t.w1) - std::min(t.w0, t.w1);
        }
        if (!(margin > 0.0)) {
            return false;
        }
    }

    return true;
}

float TimeFrameVoter::lockConfidence() const {
    // Mean normalized winning margin over the voted timestamp bits, in the SAME
    // normalized (age-extrapolated) space as votedField. Measuring the margin
    // after normalization is what couples quality to the reported value: a clean
    // rollover reads as unambiguous (every frame agrees once expressed at the
    // newest epoch, so no dip), while a window whose frames genuinely disagree
    // at comparable confidence carries that disagreement into the quality. The
    // mean-margin x frame-count-saturation shape is unchanged.
    const int n = static_cast<int>(m_frames.size());
    if (n < m_cfg.minFramesForLock) {
        return 0.0f;
    }

    const std::vector<NormalizedFrame> frames = buildNormalizedFrames();
    if (frames.empty()) {
        return 0.0f;
    }

    constexpr float kEpsilon = 1e-6f;
    double marginSum = 0.0;
    int bitCount = 0;

    for (std::size_t fld = 0; fld < FieldCount; ++fld) {
        const ClockFieldMap& map = m_cfg.fields[fld];
        for (std::size_t k = 0; k < map.size(); ++k) {
            BitTally t;
            for (const NormalizedFrame& nf : frames) {
                const NormalizedFrame::Bit b = nf.bits[fld][k];
                if (b.symbol == ClockSymbol::One || b.symbol == ClockSymbol::Zero) {
                    const float w = agedWeight(b.confidence, m_cfg.agingFactor, nf.age);
                    (b.symbol == ClockSymbol::One ? t.w1 : t.w0) += w;
                }
            }
            const float winning = std::max(t.w0, t.w1);
            const float losing = std::min(t.w0, t.w1);
            marginSum += (winning - losing) / (t.w0 + t.w1 + kEpsilon);
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
