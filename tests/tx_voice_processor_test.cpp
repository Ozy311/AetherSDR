#include "core/ClientTube.h"
#include "core/ClientGate.h"
#include "core/RNNoiseFilter.h"
#include "core/Resampler.h"
#include "core/TxVoiceProcessor.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using AetherSDR::ClientTube;
using AetherSDR::ClientGate;
using AetherSDR::RNNoiseFilter;
using AetherSDR::Resampler;
using AetherSDR::TxVoiceProcessor;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-68s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

QByteArray makeCanonicalTone(int frames, int sampleRate, float frequencyHz)
{
    QByteArray result(
        frames * 2 * static_cast<int>(sizeof(int16_t)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<int16_t*>(result.data());
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int frame = 0; frame < frames; ++frame) {
        const float value = 0.25f * static_cast<float>(
            std::sin(kTwoPi * frequencyHz * frame / sampleRate));
        const int16_t quantized = static_cast<int16_t>(value * 32767.0f);
        samples[frame * 2] = quantized;
        samples[frame * 2 + 1] = quantized;
    }
    return result;
}

std::vector<float> makeFloatStereoTone(
    int frames, int sampleRate, float frequencyHz)
{
    std::vector<float> result(static_cast<size_t>(frames * 2));
    constexpr double kTwoPi = 6.28318530717958647692;
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = 0.25f * static_cast<float>(
            std::sin(kTwoPi * frequencyHz * frame / sampleRate));
        result[static_cast<size_t>(frame * 2)] = sample;
        result[static_cast<size_t>(frame * 2 + 1)] = sample;
    }
    return result;
}

double leftChannelRms(const QByteArray& floatStereo, int skipFrames)
{
    const auto* samples = reinterpret_cast<const float*>(
        floatStereo.constData());
    const int frames = floatStereo.size()
        / (2 * static_cast<int>(sizeof(float)));
    if (!samples || frames <= skipFrames) {
        return 0.0;
    }

    double sumSquares = 0.0;
    for (int frame = skipFrames; frame < frames; ++frame) {
        const double sample = samples[frame * 2];
        sumSquares += sample * sample;
    }
    return std::sqrt(sumSquares / (frames - skipFrames));
}

double transportToneRms(float frequencyHz)
{
    constexpr int kInputFrames = 48000;
    constexpr int kSettlingOutputFrames = 4000;
    const std::vector<float> input = makeFloatStereoTone(
        kInputFrames, TxVoiceProcessor::kDspRate, frequencyHz);

    TxVoiceProcessor processor;
    if (!processor.prepare(TxVoiceProcessor::kDspRate, kInputFrames)
        || !processor.processFloat48(input.data(), kInputFrames)) {
        return 0.0;
    }
    return leftChannelRms(
        processor.transportFloat32Stereo(), kSettlingOutputFrames);
}

bool finiteFloatBuffer(const QByteArray& bytes)
{
    const auto* samples = reinterpret_cast<const float*>(bytes.constData());
    const int count = bytes.size() / static_cast<int>(sizeof(float));
    for (int index = 0; index < count; ++index) {
        if (!std::isfinite(samples[index])) {
            return false;
        }
    }
    return true;
}

bool duplicatedStereo(const QByteArray& bytes, bool floatSamples)
{
    const int sampleBytes = floatSamples
        ? static_cast<int>(sizeof(float))
        : static_cast<int>(sizeof(int16_t));
    const int frames = bytes.size() / (2 * sampleBytes);
    if (floatSamples) {
        const auto* samples = reinterpret_cast<const float*>(bytes.constData());
        for (int frame = 0; frame < frames; ++frame) {
            if (samples[frame * 2] != samples[frame * 2 + 1]) {
                return false;
            }
        }
    } else {
        const auto* samples = reinterpret_cast<const int16_t*>(bytes.constData());
        for (int frame = 0; frame < frames; ++frame) {
            if (samples[frame * 2] != samples[frame * 2 + 1]) {
                return false;
            }
        }
    }
    return true;
}

uint64_t packedSingleStage(TxVoiceProcessor::Stage stage)
{
    return static_cast<uint64_t>(stage);
}

void testFixedRateContract()
{
    report("DSP rate is pinned to 48 kHz", TxVoiceProcessor::kDspRate == 48000);
    report("transport rate remains 24 kHz",
           TxVoiceProcessor::kTransportRate == 24000);
    report("TX voice SRC transition profile is 12 percent",
           TxVoiceProcessor::kVoiceSrcTransitionBandPercent == 12.0);
}

void testVoiceSrcBandwidthAndAliasRejection()
{
    const double referenceRms = transportToneRms(1000.0f);
    const double tenKhzRms = transportToneRms(10000.0f);
    const double tenKhzGainDb = 20.0 * std::log10(
        std::max(tenKhzRms / referenceRms, 1.0e-15));

    report("voice SRC remains within 0.1 dB through 10 kHz",
           referenceRms > 0.0 && std::abs(tenKhzGainDb) <= 0.1,
           "gainDb=" + std::to_string(tenKhzGainDb));

    double worstAliasDb = -300.0;
    for (const float frequencyHz : {
             14000.0f, 16000.0f, 18000.0f, 20000.0f, 22000.0f}) {
        const double aliasRms = transportToneRms(frequencyHz);
        const double aliasDb = 20.0 * std::log10(
            std::max(aliasRms / referenceRms, 1.0e-15));
        worstAliasDb = std::max(worstAliasDb, aliasDb);
    }
    report("aliases folding into 0-10 kHz remain below -100 dB",
           referenceRms > 0.0 && worstAliasDb <= -100.0,
           "worstAliasDb=" + std::to_string(worstAliasDb));
}

void testVoiceSrcLatencyBudgets()
{
    struct ExpectedLatency {
        int inputRate;
        int frames48;
    };
    constexpr ExpectedLatency kExpected[] = {
        {48000, 394},
        {44100, 615},
        {24000, 788},
    };

    bool exact = true;
    bool withinTwentyMs = true;
    std::string detail;
    for (const ExpectedLatency expected : kExpected) {
        TxVoiceProcessor processor;
        const bool prepared = processor.prepare(expected.inputRate, 1024);
        const int frames = processor.latencyFrames();
        exact = prepared && frames == expected.frames48 && exact;
        withinTwentyMs = prepared && frames <= 960 && withinTwentyMs;
        detail += std::to_string(expected.inputRate) + "Hz="
            + std::to_string(frames) + " ";
    }

    report("SRC group delay is reported for every capture rate", exact, detail);
    report("worst-case serial SRC group delay remains below 20 ms",
           withinTwentyMs,
           detail);
}

void testReusableBuffersPreserveCapacity()
{
    constexpr int kFrames = 480;
    std::vector<float> input(kFrames, 0.0f);
    Resampler resampler(48000, 24000, kFrames,
                        TxVoiceProcessor::kVoiceSrcTransitionBandPercent);
    QByteArray output;
    output.reserve(16384);
    const qsizetype reservedCapacity = output.capacity();
    const int outputFrames = resampler.process(input.data(), kFrames, output);

    report("reusable SRC output preserves caller reservation",
           outputFrames == kFrames / 2
               && output.capacity() >= reservedCapacity,
           "reserved=" + std::to_string(reservedCapacity)
               + " capacity=" + std::to_string(output.capacity()));

    constexpr int kPreparedFrames = 1024;
    TxVoiceProcessor processor;
    const bool prepared = processor.prepare(48000, kPreparedFrames);
    const qsizetype normalizedCapacity =
        processor.normalizedFloat48Stereo().capacity();
    const qsizetype postStripCapacity =
        processor.postChannelStripFloat48Stereo().capacity();
    const qsizetype transportFloatCapacity =
        processor.transportFloat32Stereo().capacity();
    const qsizetype transportInt16Capacity =
        processor.transportInt16Stereo().capacity();
    const bool reservationsSurvivedPrepare = prepared
        && normalizedCapacity >= kPreparedFrames * 2 * static_cast<int>(sizeof(float))
        && postStripCapacity >= kPreparedFrames * 2 * static_cast<int>(sizeof(float))
        && transportFloatCapacity > 0
        && transportInt16Capacity > 0;

    processor.reset();
    report("TX prepare and reset preserve realtime buffer reservations",
           reservationsSurvivedPrepare
               && processor.normalizedFloat48Stereo().capacity()
                   >= normalizedCapacity
               && processor.postChannelStripFloat48Stereo().capacity()
                   >= postStripCapacity
               && processor.transportFloat32Stereo().capacity()
                   >= transportFloatCapacity
               && processor.transportInt16Stereo().capacity()
                   >= transportInt16Capacity);
}

void test48kBypassAndMeasurementBoundaries()
{
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    const bool prepared = processor.prepare(48000, 1024);
    const bool processed = processor.processCapturedInt16(
        makeCanonicalTone(480, 48000, 1000.0f));

    report("48 kHz processor prepares", prepared);
    report("48 kHz block processes", processed);
    report("normalized measurement contains 480 stereo float frames",
           processor.normalizedFloat48Stereo().size()
               == 480 * 2 * static_cast<int>(sizeof(float)));
    report("post-strip measurement contains 480 stereo float frames",
           processor.postChannelStripFloat48Stereo().size()
               == 480 * 2 * static_cast<int>(sizeof(float)));
    report("egress contains 240 stereo float frames",
           processor.transportFloat32Stereo().size()
               == 240 * 2 * static_cast<int>(sizeof(float)));
    report("egress contains 240 stereo int16 frames",
           processor.transportInt16Stereo().size()
               == 240 * 2 * static_cast<int>(sizeof(int16_t)));
    report("bypass output remains finite",
           finiteFloatBuffer(processor.transportFloat32Stereo()));
    report("mono voice remains duplicated stereo through egress",
           duplicatedStereo(processor.transportFloat32Stereo(), true)
               && duplicatedStereo(processor.transportInt16Stereo(), false));
}

void testDeviceRateNormalization()
{
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    const bool prepared = processor.prepare(44100, 1024);
    const bool processed = processor.processCapturedInt16(
        makeCanonicalTone(441, 44100, 1000.0f));
    const int normalizedFrames = processor.normalizedFloat48Stereo().size()
        / (2 * static_cast<int>(sizeof(float)));
    const int transportFrames = processor.transportInt16Stereo().size()
        / (2 * static_cast<int>(sizeof(int16_t)));

    report("44.1 kHz capture rate prepares", prepared);
    report("44.1 kHz capture block processes", processed);
    report("10 ms at 44.1 kHz normalizes to 480 frames",
           normalizedFrames == 480,
           "frames=" + std::to_string(normalizedFrames));
    report("normalized 10 ms block exits as 240 transport frames",
           transportFrames == 240,
           "frames=" + std::to_string(transportFrames));
}

void testFloat48OfflineEntryAvoidsInputQuantization()
{
    std::vector<float> input(480 * 2);
    for (int frame = 0; frame < 480; ++frame) {
        const float sample = 1.0e-6f * static_cast<float>(frame + 1);
        input[static_cast<size_t>(frame * 2)] = sample;
        input[static_cast<size_t>(frame * 2 + 1)] = -sample;
    }

    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(true);
    processor.prepare(48000, 480);
    const bool processed = processor.processFloat48(input.data(), 480);

    report("native float 48 kHz offline block processes", processed);
    report("native float entry preserves sub-int16 input values exactly",
           processor.normalizedFloat48Stereo().size()
                   == static_cast<int>(input.size() * sizeof(float))
               && std::memcmp(processor.normalizedFloat48Stereo().constData(),
                              input.data(), input.size() * sizeof(float)) == 0);
}

void testChannelStripRunsAt48k()
{
    ClientTube tube;
    tube.setEnabled(true);
    tube.setDriveDb(12.0f);
    tube.setDryWet(1.0f);

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.tube = &tube;
    processor.setProcessors(processors);
    processor.setStageOrder(packedSingleStage(TxVoiceProcessor::Stage::Tube));
    processor.setMeasurementCaptureEnabled(true);
    processor.prepare(48000, 1024);
    const bool processed = processor.processCapturedInt16(
        makeCanonicalTone(480, 48000, 1000.0f));

    report("channel-strip block processes", processed);
    report("tube is prepared at the canonical 48 kHz rate",
           std::fabs(tube.sampleRate() - 48000.0) < 0.1);
    report("enabled tube changes the post-strip measurement",
           processor.normalizedFloat48Stereo()
               != processor.postChannelStripFloat48Stereo());
}

void testNonFiniteSamplesCannotPoisonEgressSrc()
{
    std::vector<float> input(480 * 2, 0.1f);
    input[20] = std::nanf("");
    input[51] = std::numeric_limits<float>::infinity();
    input[92] = -std::numeric_limits<float>::infinity();

    TxVoiceProcessor processor;
    processor.prepare(48000, 480);
    const bool processed = processor.processFloat48(input.data(), 480);

    report("non-finite input block still processes", processed);
    report("non-finite samples cannot poison float transport output",
           finiteFloatBuffer(processor.transportFloat32Stereo()));
}

void testMeasurementCaptureCanBeDisabled()
{
    TxVoiceProcessor processor;
    processor.setMeasurementCaptureEnabled(false);
    processor.prepare(48000, 1024);
    processor.processCapturedInt16(makeCanonicalTone(480, 48000, 1000.0f));

    report("disabled normalized tap holds no copied block",
           processor.normalizedFloat48Stereo().isEmpty());
    report("disabled post-strip tap holds no copied block",
           processor.postChannelStripFloat48Stereo().isEmpty());
    report("transport output remains available with taps disabled",
           !processor.transportInt16Stereo().isEmpty());
}

void testBlockBoundaryContinuityAndReset()
{
    const QByteArray input = makeCanonicalTone(4800, 48000, 997.0f);

    TxVoiceProcessor whole;
    whole.prepare(48000, 4800);
    whole.processCapturedInt16(input);
    const QByteArray wholeOutput = whole.transportInt16Stereo();

    TxVoiceProcessor blocked;
    blocked.prepare(48000, 480);
    QByteArray blockedOutput;
    constexpr int kInputBlockBytes = 480 * 2 * static_cast<int>(sizeof(int16_t));
    for (int offset = 0; offset < input.size(); offset += kInputBlockBytes) {
        const bool processed = blocked.processCapturedInt16(
            input.mid(offset, kInputBlockBytes));
        if (!processed) {
            report("streaming blocks all produce output", false);
            return;
        }
        blockedOutput.append(blocked.transportInt16Stereo());
    }

    report("48 -> 24 SRC is invariant to 10 ms block boundaries",
           blockedOutput == wholeOutput,
           "wholeBytes=" + std::to_string(wholeOutput.size())
               + " blockedBytes=" + std::to_string(blockedOutput.size()));

    blocked.reset();
    QByteArray afterReset;
    for (int offset = 0; offset < input.size(); offset += kInputBlockBytes) {
        blocked.processCapturedInt16(input.mid(offset, kInputBlockBytes));
        afterReset.append(blocked.transportInt16Stereo());
    }
    report("reset restores deterministic SRC stream state",
           afterReset == blockedOutput);
}

void testOversizedCaptureBlockIsProcessed()
{
    constexpr int kPreparedInputFrames = 1024;
    constexpr int kInputFrames = 2000;
    constexpr int kFrameBytes = 2 * static_cast<int>(sizeof(int16_t));
    const QByteArray input = makeCanonicalTone(kInputFrames, 48000, 997.0f);

    TxVoiceProcessor oversized;
    oversized.prepare(48000, kPreparedInputFrames);
    const bool oversizedProcessed = oversized.processCapturedInt16(input);
    const QByteArray oversizedOutput = oversized.transportInt16Stereo();

    TxVoiceProcessor partitioned;
    partitioned.prepare(48000, kPreparedInputFrames);
    QByteArray partitionedOutput;
    const bool firstProcessed = partitioned.processCapturedInt16(
        input.left(kPreparedInputFrames * kFrameBytes));
    partitionedOutput.append(partitioned.transportInt16Stereo());
    const bool secondProcessed = partitioned.processCapturedInt16(
        input.mid(kPreparedInputFrames * kFrameBytes));
    partitionedOutput.append(partitioned.transportInt16Stereo());

    report("capture block larger than prepared size is processed",
           oversizedProcessed);
    report("oversized capture block preserves expected transport frame count",
           oversizedOutput.size()
               == (kInputFrames / 2) * kFrameBytes,
           "bytes=" + std::to_string(oversizedOutput.size()));
    report("oversized capture processing is stream-continuous",
           firstProcessed && secondProcessed
               && oversizedOutput == partitionedOutput,
           "oversizedBytes=" + std::to_string(oversizedOutput.size())
               + " partitionedBytes="
               + std::to_string(partitionedOutput.size()));
}

void testTransportTpdfDither()
{
    constexpr int kInputFrames = 48000;
    std::vector<float> silence(kInputFrames * 2, 0.0f);

    TxVoiceProcessor processor;
    processor.prepare(48000, kInputFrames);
    const bool processed = processor.processFloat48(
        silence.data(), kInputFrames);
    const QByteArray firstOutput = processor.transportInt16Stereo();
    const QByteArray floatOutput = processor.transportFloat32Stereo();
    const auto* quantized = reinterpret_cast<const int16_t*>(
        firstOutput.constData());
    const int sampleCount = firstOutput.size()
        / static_cast<int>(sizeof(int16_t));

    bool bounded = true;
    int nonZeroSamples = 0;
    int64_t sampleSum = 0;
    for (int sample = 0; sample < sampleCount; ++sample) {
        bounded = quantized[sample] >= -1 && quantized[sample] <= 1
            && bounded;
        nonZeroSamples += quantized[sample] != 0 ? 1 : 0;
        sampleSum += quantized[sample];
    }

    report("silent float transport block processes with dither", processed);
    report("dither does not alter the float transport measurement tap",
           finiteFloatBuffer(floatOutput)
               && std::all_of(
                   reinterpret_cast<const float*>(floatOutput.constData()),
                   reinterpret_cast<const float*>(floatOutput.constData())
                       + floatOutput.size() / static_cast<int>(sizeof(float)),
                   [](float sample) { return sample == 0.0f; }));
    report("linked TPDF preserves duplicated mono transport channels",
           duplicatedStereo(firstOutput, false));
    report("silent-input TPDF quantization remains within one int16 LSB",
           bounded);
    report("TPDF decorrelates digital silence from the zero code",
           nonZeroSamples > sampleCount / 10,
           "nonZero=" + std::to_string(nonZeroSamples)
               + " samples=" + std::to_string(sampleCount));
    report("silent-input TPDF has near-zero DC bias",
           std::abs(sampleSum) < sampleCount / 100,
           "sum=" + std::to_string(sampleSum));

    processor.reset();
    processor.processFloat48(silence.data(), kInputFrames);
    report("reset restores deterministic TPDF stream state",
           processor.transportInt16Stereo() == firstOutput);
}

void testDitheredTransportSaturatesAtInt16Rails()
{
    constexpr int kInputFrames = 48000;
    const auto verifyRail = [](float inputSample, int16_t expectedRail) {
        std::vector<float> input(kInputFrames * 2, inputSample);
        TxVoiceProcessor processor;
        processor.prepare(48000, kInputFrames);
        if (!processor.processFloat48(input.data(), kInputFrames)) {
            return false;
        }

        const QByteArray& floatBytes = processor.transportFloat32Stereo();
        const QByteArray& int16Bytes = processor.transportInt16Stereo();
        const auto* floatSamples = reinterpret_cast<const float*>(
            floatBytes.constData());
        const auto* int16Samples = reinterpret_cast<const int16_t*>(
            int16Bytes.constData());
        const int sampleCount = int16Bytes.size()
            / static_cast<int>(sizeof(int16_t));
        bool testedOverRangeSample = false;
        for (int sample = 0; sample < sampleCount; ++sample) {
            const bool overRange = expectedRail > 0
                ? floatSamples[sample] >= 1.0f
                : floatSamples[sample] <= -1.0f;
            if (overRange) {
                testedOverRangeSample = true;
                if (int16Samples[sample] != expectedRail) {
                    return false;
                }
            }
        }
        return testedOverRangeSample;
    };

    report("dithered quantizer saturates positive over-range samples",
           verifyRail(100.0f, std::numeric_limits<int16_t>::max()));
    report("dithered quantizer saturates negative over-range samples",
           verifyRail(-100.0f, std::numeric_limits<int16_t>::min()));
}

void testRnnoiseNative48kIsland()
{
    RNNoiseFilter rnnoise(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.rnnoise = &rnnoise;
    processor.setProcessors(processors);
    processor.setRnnoiseEnabled(true);
    processor.prepare(48000, 480);

    bool allProcessed = true;
    bool allSized = true;
    bool allDuplicated = true;
    for (int block = 0; block < 12; ++block) {
        allProcessed = processor.processCapturedInt16(
            makeCanonicalTone(480, 48000, 700.0f)) && allProcessed;
        allSized = processor.transportInt16Stereo().size()
            == 240 * 2 * static_cast<int>(sizeof(int16_t)) && allSized;
        allDuplicated = duplicatedStereo(
            processor.transportInt16Stereo(), false) && allDuplicated;
    }

    report("RNNoise native 48 kHz island processes complete frames", allProcessed);
    report("RNNoise path preserves 10 ms transport framing", allSized);
    report("TX RNNoise ProcessedMono remains duplicated stereo", allDuplicated);
}

void testDisabledRnnoiseIsNotDereferencedDuringPrepare()
{
    auto rnnoise = std::make_unique<RNNoiseFilter>(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);
    TxVoiceProcessor processor;
    processor.setRnnoise(rnnoise.get());
    processor.setRnnoiseEnabled(false);

    // Simulate the stale association that exposed the original ownership bug.
    // A disabled processor must not touch it while prepare() resets the graph.
    rnnoise.reset();
    const bool prepared = processor.prepare(48000, 480);
    processor.setRnnoise(nullptr);
    const bool processed = processor.processCapturedInt16(
        makeCanonicalTone(480, 48000, 700.0f));

    report("disabled stale RNNoise association is not reset during prepare",
           prepared && processed);
}

void testLatencyAccounting()
{
    ClientGate gate;
    gate.setEnabled(true);
    gate.setLookaheadMs(2.0f);
    RNNoiseFilter rnnoise(
        RNNoiseFilter::OutputMode::ProcessedMono,
        RNNoiseFilter::RateDomain::Native48k);

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.gate = &gate;
    processors.rnnoise = &rnnoise;
    processor.setProcessors(processors);
    processor.setStageOrder(packedSingleStage(TxVoiceProcessor::Stage::Gate));
    processor.setRnnoiseEnabled(true);
    processor.prepare(48000, 480);

    report("latency report includes RNNoise frame plus gate lookahead",
           processor.latencyFrames() == 394 + 480 + 96,
           "frames=" + std::to_string(processor.latencyFrames()));
}

} // namespace

int main()
{
    testFixedRateContract();
    testVoiceSrcBandwidthAndAliasRejection();
    testVoiceSrcLatencyBudgets();
    testReusableBuffersPreserveCapacity();
    test48kBypassAndMeasurementBoundaries();
    testDeviceRateNormalization();
    testFloat48OfflineEntryAvoidsInputQuantization();
    testChannelStripRunsAt48k();
    testNonFiniteSamplesCannotPoisonEgressSrc();
    testMeasurementCaptureCanBeDisabled();
    testBlockBoundaryContinuityAndReset();
    testOversizedCaptureBlockIsProcessed();
    testTransportTpdfDither();
    testDitheredTransportSaturatesAtInt16Rails();
    testRnnoiseNative48kIsland();
    testDisabledRnnoiseIsNotDereferencedDuringPrepare();
    testLatencyAccounting();

    std::printf("\n%s (%d failure%s)\n",
                g_failed == 0 ? "PASS" : "FAIL",
                g_failed,
                g_failed == 1 ? "" : "s");
    return g_failed == 0 ? 0 : 1;
}
