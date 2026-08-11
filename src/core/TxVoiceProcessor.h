#pragma once

#include <QByteArray>

#include <cstdint>
#include <memory>
#include <vector>

namespace AetherSDR {

class ClientComp;
class ClientDeEss;
class ClientEq;
class ClientFinalLimiter;
class ClientGate;
class ClientPudu;
class ClientQuindarTone;
class ClientReverb;
class ClientTube;
class ClientTxTestTone;
class RNNoiseFilter;
class Resampler;

// Headless, backend-independent TX voice rate-domain processor. AudioEngine
// remains responsible for capture normalization, mode routing, metering, and
// transport. This class makes the canonical 48 kHz float DSP island explicit
// and returns the unchanged 24 kHz stereo int16 transport representation.
class TxVoiceProcessor {
public:
    static constexpr int kDspRate = 48000;
    static constexpr int kTransportRate = 24000;
    static constexpr int kChannels = 2;
    static constexpr int kMaxStages = 8;
    // Voice only: preserve the supported 10 kHz modulation passband while
    // avoiding the delay of r8brain's general-purpose 2% transition profile.
    static constexpr double kVoiceSrcTransitionBandPercent = 12.0;

    enum class Stage : uint8_t {
        None = 0,
        Gate = 1,
        Eq = 2,
        DeEss = 3,
        Comp = 4,
        Tube = 5,
        Enh = 6,
        Reverb = 7,
    };

    struct Processors {
        using EqTap = void (*)(void* context, const float* stereo, int frames);

        ClientEq* eq{nullptr};
        ClientComp* comp{nullptr};
        ClientGate* gate{nullptr};
        ClientDeEss* deEss{nullptr};
        ClientTube* tube{nullptr};
        ClientPudu* pudu{nullptr};
        ClientReverb* reverb{nullptr};
        ClientFinalLimiter* finalLimiter{nullptr};
        ClientTxTestTone* testTone{nullptr};
        ClientQuindarTone* quindar{nullptr};
        RNNoiseFilter* rnnoise{nullptr};
        EqTap eqTap{nullptr};
        void* eqTapContext{nullptr};
    };

    TxVoiceProcessor();
    ~TxVoiceProcessor();

    TxVoiceProcessor(const TxVoiceProcessor&) = delete;
    TxVoiceProcessor& operator=(const TxVoiceProcessor&) = delete;

    // Call outside the realtime callback when the capture format changes.
    bool prepare(int inputRate, int maxInputFrames);
    void reset();

    void setProcessors(const Processors& processors) noexcept;
    void setStageOrder(uint64_t packedStages) noexcept;
    void setRnnoiseEnabled(bool enabled) noexcept;
    void setRnnoise(RNNoiseFilter* rnnoise) noexcept;
    void setMicGain(float gain) noexcept;
    void setMeasurementCaptureEnabled(bool enabled) noexcept;

    // Input must already be canonical duplicated-stereo int16. The channel
    // selection/averaging policy remains in TxMicChannelNormalizer.
    bool processCapturedInt16(const QByteArray& canonicalInput);

    // Offline/test entry point for audio already in the canonical DSP domain.
    // Input is interleaved stereo float32 at exactly 48 kHz.
    bool processFloat48(const float* interleavedStereo, int frames);

    const QByteArray& transportInt16Stereo() const noexcept;
    const QByteArray& transportFloat32Stereo() const noexcept;
    const QByteArray& normalizedFloat48Stereo() const noexcept;
    const QByteArray& postChannelStripFloat48Stereo() const noexcept;

    int inputRate() const noexcept { return m_inputRate; }
    bool isPrepared() const noexcept { return m_prepared; }

    // Deterministic end-to-end delay expressed in 48 kHz DSP frames. Includes
    // serial ingress/egress SRC group delay, RNNoise's one-frame WOLA delay,
    // and enabled gate lookahead. The matched L/R egress SRCs run in parallel
    // and therefore contribute one delay. Reverb pre-delay is an artistic
    // wet-path parameter rather than whole-signal latency.
    int latencyFrames() const noexcept;

private:
    static constexpr uint64_t kDitherSeed = 0x6A09E667F3BCC909ULL;

    void processChannelStrip(QByteArray& float48Stereo) noexcept;
    bool processWorkBuffer(int frames48);
    void prepareProcessors();
    uint32_t nextDitherRandom24() noexcept;
    float nextTpdfDitherLsb() noexcept;
    static int16_t quantizeTransportSample(
        float sample, float ditherLsb) noexcept;

    int m_inputRate{kDspRate};
    int m_maxInputFrames{0};
    int m_maxDspFrames{0};
    bool m_prepared{false};
    bool m_rnnoiseEnabled{false};
    bool m_captureMeasurements{false};
    float m_micGain{1.0f};
    uint64_t m_packedStages{0};
    uint64_t m_ditherState{kDitherSeed};
    Processors m_processors;

    std::unique_ptr<Resampler> m_inputResampler;
    std::unique_ptr<Resampler> m_outputLeftResampler;
    std::unique_ptr<Resampler> m_outputRightResampler;
    std::vector<float> m_inputMono;
    std::vector<float> m_outputLeft;
    std::vector<float> m_outputRight;
    QByteArray m_resampledMono48;
    QByteArray m_resampledLeft24;
    QByteArray m_resampledRight24;
    QByteArray m_rnnoiseOutput48;
    QByteArray m_work48;
    QByteArray m_normalized48;
    QByteArray m_postStrip48;
    QByteArray m_transportFloat;
    QByteArray m_transportInt16;
};

} // namespace AetherSDR
