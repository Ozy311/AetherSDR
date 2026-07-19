// AetherClock WS-1 — WWV/WWVH decoder test (plain main() + CHECK, NOT QtTest).
//
// Self-contained: the WWV signal synthesizer, the AWGN helper, and a tiny WAV
// writer all live in this file (std only). It exercises WwvDecoder through
// process() with realistic streaming chunk sizes and asserts every BCD field
// bit-exact against the synthesized truth.
//
// Signal model + levels are PINNED by SPEC.md §"Signal synthesizer spec"
// (PRD-A §8): 24 kHz; carrier 1000 Hz @ 0.5; 100 Hz subcarrier depth 0.30
// pulse-on / 0.06 pulse-off / 0 during the s0 minute hole; pulse per the NIST
// 170/470/770 ms @ +30 ms encoding; 5 ms tick @ 0.25 at the 2000 Hz image
// (WWVH: 2200 Hz). Field map per the NIST WWV time-code table (SP 432).
//
// The decoder .cpp implementations are authored in parallel; this file is
// syntax-checked against the frozen headers only. Value asserts therefore fix
// exact FIELD values and assert confidence RANGES/FLOORS, never exact
// confidence numbers.

#include "core/TimeFrameVoter.h"
#include "core/WwvDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace AetherSDR;

// ---- test harness (per SPEC.md §"Repo conventions") -----------------------
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int    kFs = 24000;              // pinned sample rate

// Pinned NIST WWV noise floor: bit-exact decode at >= 20 dB SNR in 700-1300 Hz.
constexpr double kWwvSnrFloorDb = 20.0;

// QA-documented confidence floor for a clean / near-clean lock (ranges only —
// exact matched-filter margins are impl-defined). Loosen here if a correct
// implementation legitimately reports lower; never tighten silently.
// Calibrated against the streaming decoder on the clean golden vector
// (observed minima: frameConfidence 0.475, quality 0.500 — the streaming
// biquad chain's matched-filter margins sit slightly below the batch
// prototype's). Noise-only input never locks at all, so these floors bound
// the clean/degraded cases with real headroom.
constexpr float  kConfidenceFloor      = 0.40f;  // clean golden vector
constexpr float  kConfidenceFloor20dB  = 0.25f;  // at the pinned 20 dB floor

// ---- WWV time-code synthesizer --------------------------------------------
// Per-second pulse class of the 100 Hz subcarrier.
enum class Sym { Hole, Zero, One, Marker };

// Broadcast truth for one minute; the synthesizer encodes ALL of these so
// bit-exactness of every field is assertable.
struct Truth {
    int  minute = 0, hour = 0, doy = 0, year2 = 0;
    int  dut1Tenths = 0;   // signed tenths (e.g. -3 => -0.3 s)
    bool dst1 = false;     // WWV s2
    bool dst2 = false;     // WWV s55
    bool lsw  = false;     // WWV s3 (leap-second warning)
};

struct SynthOpts {
    int          numFrames     = 3;                 // >= 3 consecutive minutes
    int          leadInSeconds = 10;                // tail of the prior minute
    int          leadOutSeconds = 10;               // head of the next minute
    ClockStation station       = ClockStation::Wwv; // tick image 2000/2200 Hz
    bool         removeHole     = false; // s0 gets a normal pulse (kills the cue)
    bool         corruptMarkers = false; // markers flattened to 170 ms zeros
    int          truncateSeconds = -1;   // stop the final frame after N seconds
};

using BcdMap = std::vector<std::pair<int,int>>; // (secondOfFrame, weight)

// Field maps, LSB-first within field, per the NIST WWV time-code table.
const BcdMap kMapMin{{10,1},{11,2},{12,4},{13,8},{15,10},{16,20},{17,40}};
const BcdMap kMapHr {{20,1},{21,2},{22,4},{23,8},{25,10},{26,20}};
const BcdMap kMapDoy{{30,1},{31,2},{32,4},{33,8},{35,10},{36,20},{37,40},
                     {38,80},{40,100},{41,200}};
const BcdMap kMapYr {{4,1},{5,2},{6,4},{7,8},{51,10},{52,20},{53,40},{54,80}};

void setBcd(std::array<Sym,60>& s, const BcdMap& m, int value) {
    for (const auto& pr : m) {
        int sec = pr.first, w = pr.second;
        int place = 1;
        while (w / place >= 10) place *= 10;   // decade of this weight
        int bw    = w / place;                  // 1|2|4|8 within the decade
        int digit = (value / place) % 10;
        if (digit & bw) s[sec] = Sym::One;
    }
}

// One minute of classified per-second symbols from broadcast truth.
std::array<Sym,60> encodeMinute(const Truth& t) {
    std::array<Sym,60> s;
    s.fill(Sym::Zero);
    s[0] = Sym::Hole;                                   // minute-mark subcarrier hole
    for (int m : {9,19,29,39,49,59}) s[m] = Sym::Marker; // P1..P5, P0
    setBcd(s, kMapMin, t.minute);
    setBcd(s, kMapHr,  t.hour);
    setBcd(s, kMapDoy, t.doy);
    setBcd(s, kMapYr,  t.year2);
    if (t.dut1Tenths > 0) s[50] = Sym::One;             // DUT1 sign (1 = +)
    int mag = std::abs(t.dut1Tenths);
    if (mag & 1) s[56] = Sym::One;                      // DUT1 magnitude 0.1
    if (mag & 2) s[57] = Sym::One;                      // DUT1 magnitude 0.2
    if (mag & 4) s[58] = Sym::One;                      // DUT1 magnitude 0.4
    if (t.dst1) s[2]  = Sym::One;
    if (t.dst2) s[55] = Sym::One;
    if (t.lsw)  s[3]  = Sym::One;
    return s;
}

// Append one second (kFs samples) of the pinned WWV waveform.
//   sample(t) = 0.5*sin(2pi*1000*t)*(1 + d(t)*sin(2pi*100*t)) + tick(t)
void appendSecond(std::vector<float>& sig, Sym sym, int tickFreq) {
    for (int k = 0; k < kFs; ++k) {
        const double t   = static_cast<double>(sig.size()) / kFs; // absolute -> phase-continuous
        const double tau = static_cast<double>(k) / kFs;          // within-second
        const double car = 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
        double d;
        if (sym == Sym::Hole) {
            d = 0.0;                                    // no subcarrier at all
        } else {
            const double dur = (sym == Sym::Zero) ? 0.170
                             : (sym == Sym::One)  ? 0.470
                                                  : 0.770;         // marker
            d = (tau >= 0.030 && tau < 0.030 + dur) ? 0.30 : 0.06; // +30 ms start
        }
        const double sub  = 1.0 + d * std::sin(2.0 * kPi * 100.0 * t);
        const double tick = (tau < 0.005)                          // 5 ms burst
                          ? 0.25 * std::sin(2.0 * kPi * tickFreq * t)
                          : 0.0;
        sig.push_back(static_cast<float>(car * sub + tick));
    }
}

std::vector<float> synthWwv(const Truth& start, const SynthOpts& o) {
    const int tickFreq = (o.station == ClockStation::Wwvh) ? 2200 : 2000;
    std::vector<float> sig;
    sig.reserve(static_cast<size_t>((o.leadInSeconds + 60 * o.numFrames)) * kFs);

    auto emit = [&](std::array<Sym,60> sym, int secStart, int secEnd) {
        for (int sec = secStart; sec < secEnd; ++sec) {
            Sym s = sym[sec];
            if (o.corruptMarkers && s == Sym::Marker) s = Sym::Zero; // flatten markers
            if (o.removeHole && sec == 0)             s = Sym::Zero; // fill the hole
            appendSecond(sig, s, tickFreq);
        }
    };

    // Lead-in: the tail seconds of the prior minute, correctly encoded.
    Truth lead = start;
    lead.minute -= 1;
    if (lead.minute < 0) { lead.minute += 60; lead.hour = (lead.hour + 23) % 24; }
    emit(encodeMinute(lead), 60 - o.leadInSeconds, 60);

    // Consecutive full frames, minutes monotonically incrementing.
    for (int i = 0; i < o.numFrames; ++i) {
        Truth cur = start;
        const int total = start.minute + i;
        cur.minute = total % 60;
        cur.hour   = (start.hour + total / 60) % 24;
        int lastSec = 60;
        if (o.truncateSeconds >= 0 && i == o.numFrames - 1) lastSec = o.truncateSeconds;
        emit(encodeMinute(cur), 0, lastSec);
    }

    // Lead-out: the head seconds of the NEXT minute, correctly encoded, so the
    // final frame can complete (streaming decoders need trailing signal to
    // close the last second/frame). Skipped when truncating — the truncation
    // section depends on the signal ending mid-frame.
    if (o.truncateSeconds < 0 && o.leadOutSeconds > 0) {
        Truth next = start;
        const int total = start.minute + o.numFrames;
        next.minute = total % 60;
        next.hour   = (start.hour + total / 60) % 24;
        emit(encodeMinute(next), 0, o.leadOutSeconds);
    }
    return sig;
}

// ---- AWGN with SNR measured/scaled in the 700-1300 Hz band ----------------
// RBJ constant-0-dB-peak bandpass, f0=1000, Q=f0/BW with BW=600 Hz -> -3 dB
// edges near 700/1300 Hz. The SAME filter measures signal and noise power, so
// the resulting ratio IS the band-limited SNR of SPEC's floor definition.
std::vector<float> bandpass700_1300(const std::vector<float>& x) {
    const double f0 = 1000.0, Q = 1000.0 / 600.0;
    const double w0 = 2.0 * kPi * f0 / kFs;
    const double cs = std::cos(w0), sn = std::sin(w0), alpha = sn / (2.0 * Q);
    double b0 = alpha, b1 = 0.0, b2 = -alpha;
    const double a0 = 1.0 + alpha, a1 = -2.0 * cs, a2 = 1.0 - alpha;
    b0 /= a0; b1 /= a0; b2 /= a0;
    const double na1 = a1 / a0, na2 = a2 / a0;
    std::vector<float> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double xi = x[i];
        const double yi = b0 * xi + b1 * x1 + b2 * x2 - na1 * y1 - na2 * y2;
        y[i] = static_cast<float>(yi);
        x2 = x1; x1 = xi; y2 = y1; y1 = yi;
    }
    return y;
}

double meanSquare(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : s / x.size();
}
double bandPower(const std::vector<float>& x) { return meanSquare(bandpass700_1300(x)); }

// Add AWGN scaled so band-SNR == snrDb. Deterministic (caller owns the PRNG).
std::vector<float> addAwgnAtBandSnr(const std::vector<float>& sig, double snrDb,
                                    std::mt19937& rng) {
    const double psig = bandPower(sig);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<float> noise(sig.size());
    for (auto& v : noise) v = static_cast<float>(nd(rng));
    const double pn     = bandPower(noise);
    const double snrLin = std::pow(10.0, snrDb / 10.0);
    const double g      = (pn > 0.0) ? std::sqrt(psig / (snrLin * pn)) : 0.0;
    std::vector<float> out(sig.size());
    for (size_t i = 0; i < sig.size(); ++i)
        out[i] = static_cast<float>(sig[i] + g * noise[i]);
    return out;
}

std::vector<float> pureAwgn(size_t n, double amp, std::mt19937& rng) {
    std::normal_distribution<double> nd(0.0, amp);
    std::vector<float> v(n);
    for (auto& x : v) x = static_cast<float>(nd(rng));
    return v;
}

// Measured band-SNR of a noisy vector vs its clean reference.
double measuredBandSnrDb(const std::vector<float>& clean, const std::vector<float>& noisy) {
    std::vector<float> resid(clean.size());
    for (size_t i = 0; i < clean.size(); ++i) resid[i] = noisy[i] - clean[i];
    const double ps = bandPower(clean), pn = bandPower(resid);
    return (pn > 0.0) ? 10.0 * std::log10(ps / pn) : 999.0;
}

// ---- tiny WAV writer (16-bit PCM, stereo dup, 24 kHz) ---------------------
void writeWavStereo(const std::string& path, const std::vector<float>& mono) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const uint16_t nch = 2, bits = 16;
    const uint16_t blockAlign = static_cast<uint16_t>(nch * bits / 8);
    const uint32_t byteRate   = static_cast<uint32_t>(kFs) * blockAlign;
    const uint32_t dataBytes  = static_cast<uint32_t>(mono.size()) * blockAlign;
    const uint32_t riffSize   = 36u + dataBytes;
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); w32(riffSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(nch);
    w32(static_cast<uint32_t>(kFs)); w32(byteRate); w16(blockAlign); w16(bits);
    f.write("data", 4); w32(dataBytes);
    for (float s : mono) {
        float c = std::max(-1.0f, std::min(1.0f, s));
        int16_t v = static_cast<int16_t>(std::lround(c * 32767.0f));
        w16(static_cast<uint16_t>(v)); // L
        w16(static_cast<uint16_t>(v)); // R (duplicated mono)
    }
}

// ---- decoder driving + callback capture -----------------------------------
struct Capture {
    std::vector<ClockFrameInfo> frames;
    std::vector<ClockTimeInfo>  times;
    std::vector<ClockLockState> states;
    int lockedTransitions = 0;
};

void wire(WwvDecoder& d, Capture& c) {
    d.onFrame        = [&c](const ClockFrameInfo& f) { c.frames.push_back(f); };
    d.onTime         = [&c](const ClockTimeInfo&  t) { c.times.push_back(t); };
    d.onStateChanged = [&c](ClockLockState s) {
        c.states.push_back(s);
        if (s == ClockLockState::Locked) ++c.lockedTransitions;
    };
}

void feedChunks(WwvDecoder& d, const std::vector<float>& sig, size_t chunk) {
    for (size_t i = 0; i < sig.size(); ) {
        const size_t n = std::min(chunk, sig.size() - i);
        d.process(sig.data() + i, n);
        i += n;
    }
}

// Irregular chunking to shake out chunk-boundary bugs (edges split mid-tick,
// mid-pulse, single-sample dribbles).
void feedIrregular(WwvDecoder& d, const std::vector<float>& sig) {
    static const size_t sizes[] = {1, 7, 63, 512, 4096, 333, 2048, 9};
    size_t i = 0, k = 0;
    while (i < sig.size()) {
        const size_t n = std::min(sizes[k % 8], sig.size() - i);
        d.process(sig.data() + i, n);
        i += n; ++k;
    }
}

bool minutesIncrement(const std::vector<ClockFrameInfo>& f) {
    if (f.size() < 2) return false;
    for (size_t i = 1; i < f.size(); ++i)
        if (f[i].minute != (f[i - 1].minute + 1) % 60) return false;
    return true;
}

bool acquiringBeforeLock(const std::vector<ClockLockState>& s) {
    size_t li = s.size();
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == ClockLockState::Locked) { li = i; break; }
    if (li == s.size()) return false;
    for (size_t i = 0; i < li; ++i)
        if (s[i] == ClockLockState::Acquiring) return true;
    return false;
}

// The golden broadcast truth: 06:20 -> 06:21 -> 06:22 on doy 200, yr 26,
// DUT1 = -0.3 s, DST1 set, DST2 clear, leap-second warning set.
const Truth   kGold{20, 6, 200, 26, /*dut1*/ -3, /*dst1*/ true, /*dst2*/ false, /*lsw*/ true};
constexpr int kExpectNewestMin = 22; // start.minute + numFrames - 1

// ==== test sections ========================================================

// [wwv.clean] Clean 3-frame synth -> every BCD field bit-exact, station tag,
// lock, minutes increment, callback sequencing, confidence floor.
void sectionCleanBitExact(const std::vector<float>& clean) {
    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, clean, 512);

    CHECK(dec.state()   == ClockLockState::Locked);
    CHECK(dec.station() == ClockStation::Wwv);

    // Voted time "as of the newest frame".
    CHECK(!cap.times.empty());
    if (!cap.times.empty()) {
        const ClockTimeInfo& t = cap.times.back();
        CHECK(t.minute == kExpectNewestMin);
        CHECK(t.hour   == 6);
        CHECK(t.doy    == 200);
        CHECK(t.year2  == 26);
        CHECK(t.station == ClockStation::Wwv);
        CHECK(t.quality >= kConfidenceFloor && t.quality <= 1.0f);
    }

    // Per-frame raw decode: fields including DUT1 sign+magnitude, DST, LSW.
    CHECK(!cap.frames.empty());
    if (!cap.frames.empty()) {
        const ClockFrameInfo& f = cap.frames.back();
        CHECK(f.minute      == kExpectNewestMin);
        CHECK(f.hour        == 6);
        CHECK(f.doy         == 200);
        CHECK(f.year2       == 26);
        CHECK(f.dut1Tenths  == -3);          // sign + magnitude bit-exact
        CHECK(f.dst1        == true);
        CHECK(f.dst2        == false);
        CHECK(f.leapPending == true);        // WWV LSW s3
        CHECK(f.leapYear    == false);       // always false for WWV
        CHECK(f.station     == ClockStation::Wwv);
        CHECK(f.frameConfidence >= kConfidenceFloor && f.frameConfidence <= 1.0f);
    }

    CHECK(minutesIncrement(cap.frames));

    // Sequencing: Acquiring precedes Locked; exactly ONE transition to Locked.
    CHECK(acquiringBeforeLock(cap.states));
    CHECK(cap.lockedTransitions == 1);
}

// [wwv.wwvh] WWVH variant (2200 Hz tick image) -> station() == Wwvh, fields
// decode identically (same code, only the tick tone tags the station).
void sectionWwvhStation() {
    SynthOpts o; o.station = ClockStation::Wwvh;
    const std::vector<float> sig = synthWwv(kGold, o);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, sig, 512);

    CHECK(dec.station() == ClockStation::Wwvh);
    CHECK(dec.state()   == ClockLockState::Locked);
    if (!cap.frames.empty()) {
        CHECK(cap.frames.back().station == ClockStation::Wwvh);
        CHECK(cap.frames.back().minute  == kExpectNewestMin);
    }
    if (!cap.times.empty())
        CHECK(cap.times.back().station == ClockStation::Wwvh);
}

// [wwv.snr20] Pinned noise floor: bit-exact at exactly 20 dB SNR in-band.
void sectionSnr20(const std::vector<float>& clean, std::mt19937& rng) {
    CHECK(kWwvSnrFloorDb == 20.0);           // pinned floor is documented in-test

    const std::vector<float> noisy = addAwgnAtBandSnr(clean, kWwvSnrFloorDb, rng);

    // Independent of the decoder: prove the synth/AWGN path really hit 20 dB.
    const double snrMeas = measuredBandSnrDb(clean, noisy);
    CHECK(std::fabs(snrMeas - kWwvSnrFloorDb) < 0.5);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedIrregular(dec, noisy);               // irregular chunks under noise

    CHECK(dec.state() == ClockLockState::Locked);
    if (!cap.frames.empty()) {
        const ClockFrameInfo& f = cap.frames.back();
        CHECK(f.minute     == kExpectNewestMin);
        CHECK(f.hour       == 6);
        CHECK(f.doy        == 200);
        CHECK(f.year2      == 26);
        CHECK(f.dut1Tenths == -3);
        CHECK(f.dst1 == true && f.dst2 == false && f.leapPending == true);
        CHECK(f.frameConfidence >= kConfidenceFloor20dB && f.frameConfidence <= 1.0f);
    }
    if (!cap.times.empty())
        CHECK(cap.times.back().minute == kExpectNewestMin);
}

// [wwv.noiseonly] >= 5 minutes of pure AWGN -> NEVER locks.
void sectionNoiseOnlyNeverLocks(std::mt19937& rng) {
    const size_t n = static_cast<size_t>(5 * 60) * kFs;   // 5 minutes
    const std::vector<float> noise = pureAwgn(n, 0.3, rng);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, noise, 512);

    CHECK(dec.state() != ClockLockState::Locked);
    CHECK(cap.lockedTransitions == 0);
    CHECK(cap.times.empty());                // no voted time ever emitted
}

// [wwv.badmarkers] Corrupted markers (P-pulses flattened to 170 ms zeros) ->
// no frame sync, no false lock.
void sectionCorruptMarkersNoFalseLock() {
    SynthOpts o; o.corruptMarkers = true;
    const std::vector<float> sig = synthWwv(kGold, o);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, sig, 512);

    CHECK(dec.state() != ClockLockState::Locked);
    CHECK(cap.lockedTransitions == 0);
}

// [wwv.truncated] Truncated final frame (30 s) -> no partial-field emission
// for the incomplete minute.
void sectionTruncatedNoPartial() {
    SynthOpts o; o.truncateSeconds = 30;     // frame min=22 cut off at s30
    const std::vector<float> sig = synthWwv(kGold, o);
    const int truncatedMin = kExpectNewestMin;   // 22 — must never be emitted

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, sig, 512);

    for (const ClockFrameInfo& f : cap.frames)
        CHECK(f.minute != truncatedMin);     // no partial frame surfaced
    if (!cap.frames.empty())
        CHECK(cap.frames.back().minute == truncatedMin - 1);  // last COMPLETE = 21
    if (!cap.times.empty())
        CHECK(cap.times.back().minute != truncatedMin);
}

// [wwv.decade] Decade degeneracy: marker-only anchoring is degenerate mod 10 s.
// (a) On the clean vector a correct decode landing exactly on truth IS the
//     rejection proof — a 10 s-shifted anchor would decode shifted fields.
// (b) On a hole-omitted-but-marker-consistent vector, the decoder must either
//     refuse to lock or still land on truth.
void sectionDecadeDegeneracy(const std::vector<float>& clean) {
    // (a) clean — decoded minutes match truth exactly, not truth+/-shift.
    {
        WwvDecoder dec(kFs);
        Capture cap; wire(dec, cap);
        feedChunks(dec, clean, 512);
        CHECK(dec.state() == ClockLockState::Locked);
        if (!cap.times.empty())
            CHECK(cap.times.back().minute == kExpectNewestMin);
        // Every per-frame minute is exactly one of the encoded truths.
        for (const ClockFrameInfo& f : cap.frames)
            CHECK(f.minute == 20 || f.minute == 21 || f.minute == 22);
        CHECK(minutesIncrement(cap.frames));
    }
    // (b) hole omitted (s0 carries a normal pulse), markers still consistent.
    {
        SynthOpts o; o.removeHole = true;
        const std::vector<float> sig = synthWwv(kGold, o);
        WwvDecoder dec(kFs);
        Capture cap; wire(dec, cap);
        feedChunks(dec, sig, 512);
        const bool refused = (dec.state() != ClockLockState::Locked);
        const bool onTruth = (!cap.times.empty() &&
                              cap.times.back().minute == kExpectNewestMin);
        CHECK(refused || onTruth);           // refuse, or still land on truth
    }
}

// [wwv.confidence] Documented confidence floor: reported per-frame and voted
// confidences sit in [0,1] and clear the QA floor on the clean vector.
void sectionConfidenceFloor(const std::vector<float>& clean) {
    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, clean, 512);

    for (const ClockFrameInfo& f : cap.frames) {
        CHECK(f.frameConfidence >= 0.0f && f.frameConfidence <= 1.0f);
        CHECK(f.frameConfidence >= kConfidenceFloor);   // documented floor
    }
    for (const ClockTimeInfo& t : cap.times) {
        CHECK(t.quality >= 0.0f && t.quality <= 1.0f);
        CHECK(t.quality >= kConfidenceFloor);
    }
    float minFrameConf = 1.0f, minQuality = 1.0f;
    for (const ClockFrameInfo& f : cap.frames)
        minFrameConf = std::min(minFrameConf, f.frameConfidence);
    for (const ClockTimeInfo& t : cap.times)
        minQuality = std::min(minQuality, t.quality);
    std::fprintf(stderr, "[wwv.confidence] documented floor = %.2f "
                 "(observed clean minima: frameConfidence %.3f, quality %.3f)\n",
                 static_cast<double>(kConfidenceFloor),
                 static_cast<double>(minFrameConf),
                 static_cast<double>(minQuality));
}

} // namespace

int main() {
    std::mt19937 rng(0xC0FFEEu);             // fixed seed -> reproducible AWGN

    // Build the clean golden vector once; reuse across sections + WAV dump.
    SynthOpts opts;
    const std::vector<float> clean = synthWwv(kGold, opts);

    sectionCleanBitExact(clean);
    sectionWwvhStation();
    sectionSnr20(clean, rng);
    sectionNoiseOnlyNeverLocks(rng);
    sectionCorruptMarkersNoFalseLock();
    sectionTruncatedNoPartial();
    sectionDecadeDegeneracy(clean);
    sectionConfidenceFloor(clean);

    // Print encoded truth for the python cross-check gate.
    std::printf("golden truth: min=%d hr=%d doy=%d yr=%d frames=%d\n",
                kGold.minute, kGold.hour, kGold.doy, kGold.year2, opts.numFrames);
    std::printf("golden extra: dut1_tenths=%d dst1=%d dst2=%d lsw=%d station=WWV\n",
                kGold.dut1Tenths, kGold.dst1 ? 1 : 0, kGold.dst2 ? 1 : 0,
                kGold.lsw ? 1 : 0);

    // WAV dump for the cross-check gate: 16-bit PCM stereo (dup mono), 24 kHz.
    if (const char* dir = std::getenv("AETHERCLOCK_DUMP_WAV_DIR")) {
        const std::string path = std::string(dir) + "/wwv_golden.wav";
        writeWavStereo(path, clean);
        std::fprintf(stderr, "wrote golden vector: %s (%zu samples)\n",
                     path.c_str(), clean.size());
    }

    if (g_failures == 0) {
        std::printf("wwv_decoder_test: all checks passed\n");
        return 0;
    }
    std::printf("wwv_decoder_test: %d checks FAILED\n", g_failures);
    return 1;
}
