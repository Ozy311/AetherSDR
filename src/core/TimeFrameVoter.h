#pragma once

// AetherClock shared time-frame machinery: the per-second/per-frame result
// types both time-signal decoders emit, plus cross-frame confidence-weighted
// bit voting over a sliding window of decoded frames.
//
// Field maps are supplied by each decoder from the NIST time-code tables
// (WWV/WWVH per NIST SP 432; WWVB legacy AM per NIST SP 250-67). This unit is
// map-agnostic: it votes bits, scores minute increments across consecutive
// frames, and reports lock + aggregate confidence.
//
// Pure DSP/logic — no Qt, no GUI (engine-boundary EB1/EB2).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace AetherSDR {

// Classified per-second symbol of an AM time-code frame.
enum class ClockSymbol : int8_t {
    Unknown = -1,
    Zero    = 0,
    One     = 1,
    Marker  = 2,
};

enum class ClockLockState : int {
    NoSignal  = 0,
    Acquiring = 1,
    Locked    = 2,
};

enum class ClockStation : int {
    Unknown = 0,
    Wwv     = 1,
    Wwvh    = 2,
    Wwvb    = 3,
};

// One BCD map entry: which second-of-frame carries which weight. A field's
// value is the sum of weights whose seconds decoded as One.
struct ClockBitWeight {
    int second;   // 0..59
    int weight;
};
using ClockFieldMap = std::vector<ClockBitWeight>;

// Emitted once per classified second (drives the alignment display).
struct ClockSecondInfo {
    int64_t edgeSample = 0;      // sample index (total consumed) of the second edge
    ClockSymbol symbol = ClockSymbol::Unknown;
    float confidence = 0.0f;     // matched-filter margin (best - runner-up), >= 0
    int secondOfFrame = -1;      // 0..59 once frame-synced, else -1
    // 1 s alignment window at the decoder's series rate:
    std::vector<float> envelope; // received amplitude series (normalized 0..1-ish)
    std::vector<float> expected; // zero-mean matched template of `symbol`
    int seriesRateHz = 0;        // 200 for WWV/WWVH, 100 for WWVB
};

// Emitted once per completed frame decode (raw, pre-voting).
struct ClockFrameInfo {
    int minute = -1, hour = -1, doy = -1, year2 = -1; // per-frame BCD decode
    int dut1Tenths = 0;          // signed tenths of a second (e.g. -3 = -0.3 s)
    bool dst1 = false;           // WWV s2  (WWVB: DST-code bit s57)
    bool dst2 = false;           // WWV s55 (WWVB: DST-code bit s58)
    bool leapPending = false;    // leap-second warning bit
    bool leapYear = false;       // WWVB LYI s55 (always false for WWV)
    float frameConfidence = 0.0f;    // 0..1
    int64_t frameStartSample = 0;    // sample index of second 0 of this frame
    ClockStation station = ClockStation::Unknown;
};

// The voted broadcast time once locked.
struct ClockTimeInfo {
    int minute = -1, hour = -1, doy = -1, year2 = -1;
    float quality = 0.0f;            // voter lockConfidence, 0..1
    int64_t lastEdgeSample = 0;      // sample index of the most recent second edge
    int lastEdgeSecondOfFrame = -1;  // second-of-frame of that edge
    ClockStation station = ClockStation::Unknown;
};

// A complete broadcast timestamp decoded from a single frame. minute/hour are
// 0-based; doy is 1-based day-of-year; year2 is the two-digit year (century
// assumption: full year = 2000 + year2, i.e. the 20xx century).
struct TimeFields {
    int minute = -1;
    int hour   = -1;
    int doy    = -1;
    int year2  = -1;
    bool operator==(const TimeFields&) const = default;
};

// Advance a timestamp forward by `minutes` (>= 0) using calendar arithmetic:
// minute 59 -> 0 carries the hour, hour 23 -> 0 carries the day-of-year, and
// doy wraps at the year length (365, or 366 when 2000 + year2 is a Gregorian
// leap year) carrying year2 (mod 100). Pure/stateless so it is unit-testable in
// isolation. Inputs are assumed already range-valid.
TimeFields advanceMinutes(TimeFields t, int minutes);

// Cross-frame confidence-weighted bit voter (reference: wwv_decode_proto.py —
// weight = max(confidence, 0.01); production adds frame aging).
class TimeFrameVoter {
public:
    // Well-known indices into Config::fields.
    enum FieldIndex : size_t {
        FieldMinutes = 0,   // special: expected to increment +1 per frame
        FieldHours   = 1,
        FieldDoy     = 2,
        FieldYear    = 3,
        FieldCount   = 4,
    };

    struct Config {
        std::array<ClockFieldMap, FieldCount> fields;
        std::vector<int> markerSeconds;  // marker positions within the frame
        size_t window = 8;               // sliding window of most recent frames
        int minFramesForLock = 2;        // consistent frames required for lock
        float agingFactor = 0.9f;        // per-frame-age weight multiplier
    };

    explicit TimeFrameVoter(Config cfg);

    // Add one complete 60 s frame of classified symbols + confidences. Frames
    // MUST be consecutive broadcast minutes — the caller resets on gaps or
    // re-anchoring. Markers are excluded from bit votes automatically.
    void addFrame(const std::array<ClockSymbol, 60>& symbols,
                  const std::array<float, 60>& confidence);

    void reset();

    int frameCount() const;   // frames currently in the window

    // Lock = at least minFramesForLock frames in the window, AND at least
    // (minFramesForLock - 1) adjacent frame pairs whose per-frame minutes
    // decode increments by exactly +1 (mod 60), AND voted static fields
    // self-consistent. Increment support is COUNTED across the window, not
    // required of every pair — live per-frame decodes are routinely imperfect
    // and one corrupted frame must not permanently prevent or drop the lock
    // (the reference scores increments: marker_score + 4 * increment_count).
    bool locked() const;

    // Voted value of a timestamp field (minute/hour/doy/year), reported "as of
    // the newest frame". All four fields come from a single VALUE-level vote:
    // each frame's complete timestamp is decoded from that frame's own bits,
    // extrapolated forward by its age in minutes (calendar arithmetic), and the
    // whole tuple is majority-voted with per-frame weight
    // max(meanConfidence, 0.01) * agingFactor^age (age 0 = newest frame). Voting
    // the tuple — not each field's bits independently across frames — is what
    // makes an hour/day rollover safe: a stale hour cannot outvote the current
    // one for a window length, and per-bit mixing can never synthesize a field
    // value that no frame in the window actually carried. Per-bit voting remains
    // only WITHIN a single frame's decode (never across frames for these
    // fields). Returns -1 if no frame decodes to a range-valid timestamp.
    int votedField(FieldIndex field) const;

    // Raw per-frame minutes decode of the newest frame (no voting).
    int lastFrameMinute() const;

    // Aggregate lock quality 0..1: mean winning-vote margin across the voted
    // bits of the static fields, saturating with frame count. Monotonically
    // non-decreasing for repeated clean frames; ~0 with < minFramesForLock
    // frames.
    float lockConfidence() const;

private:
    struct Frame {
        std::array<ClockSymbol, 60> symbols;
        std::array<float, 60> confidence;
    };
    Config m_cfg;
    std::vector<Frame> m_frames;   // newest last

    // Per-frame BCD decode of one field: sum the field-map weights whose second
    // classified as One (Marker/Unknown contribute nothing).
    int decodeField(const Frame& f, FieldIndex field) const;
    int decodeMinutes(const Frame& f) const;  // == decodeField(f, FieldMinutes)

    // Value-level, age-extrapolated majority vote of the whole timestamp across
    // the window (the source of truth for votedField's four fields). Frames
    // whose decoded fields fall outside valid broadcast ranges are excluded.
    // Returns all-(-1) TimeFields when no candidate qualifies.
    TimeFields votedTimestamp() const;
};

} // namespace AetherSDR
