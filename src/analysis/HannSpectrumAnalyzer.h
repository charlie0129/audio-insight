// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "StereoSampleCapture.h"
#include "core/VisualizationFrame.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/**
    Streaming 4096-point Hann-window spectrum analysis.

    Input chunks may have any size. The analyzer keeps overlap state and emits at
    approximately targetUpdatesPerSecond. Mono is transformed once. Stereo is
    transformed once per channel and publishes the larger channel magnitude for
    each bin, avoiding phase cancellation in the shared display.

    A generation change, chunk-sequence gap, captured-frame gap, or sample-rate
    change invalidates the current spectrum and clears all overlap state before
    accepting the new chunk.
*/
class HannSpectrumAnalyzer final {
public:
    struct Statistics {
        std::uint64_t inputChunks = 0;
        std::uint64_t transforms = 0;
        std::uint64_t temporalResets = 0;
        std::uint64_t sequenceGapResets = 0;
    };

    explicit HannSpectrumAnalyzer(double targetUpdatesPerSecond = 60.0);

    HannSpectrumAnalyzer(const HannSpectrumAnalyzer&) = delete;
    HannSpectrumAnalyzer& operator=(const HannSpectrumAnalyzer&) = delete;

    /**
        Consumes one captured chunk and updates destination for every completed
        transform. Returns true if at least one new spectrum was produced.
    */
    [[nodiscard]] bool process(
        const CapturedStereoChunkView& chunk, VisualizationFrame& destination) noexcept;

    /** Explicit lifecycle reset. The next full FFT window starts a fresh stream. */
    void reset(VisualizationFrame* destinationToInvalidate = nullptr) noexcept;

    [[nodiscard]] Statistics statistics() const noexcept
    {
        return statistics_;
    }
    [[nodiscard]] std::size_t hopSize() const noexcept
    {
        return hopSize_;
    }
    [[nodiscard]] double sampleRate() const noexcept
    {
        return sampleRate_;
    }

private:
    static constexpr std::size_t transformWorkspaceSize = fftSize * 2;

    enum class ResetReason {
        explicitReset,
        generationChange,
        sequenceGap,
        capturedFrameGap,
        sampleRateChange
    };

    void resetTemporalState(ResetReason reason, VisualizationFrame* destination) noexcept;
    void configureSampleRate(double sampleRate) noexcept;
    void runTransform(std::uint64_t generation, std::uint64_t capturedFrameEnd,
        std::uint32_t channelCount, VisualizationFrame& destination) noexcept;
    void prepareChannelTransform(const std::array<float, fftSize>& ring,
        std::array<float, transformWorkspaceSize>& workspace) noexcept;
    [[nodiscard]] static float magnitudeToDecibels(float magnitude) noexcept;

    juce::dsp::FFT fft_ { static_cast<int>(fftOrder) };
    std::array<float, fftSize> hannWindow_ { };
    std::array<float, fftSize> leftRing_ { };
    std::array<float, fftSize> rightRing_ { };
    std::array<float, transformWorkspaceSize> leftWorkspace_ { };
    std::array<float, transformWorkspaceSize> rightWorkspace_ { };

    double targetUpdatesPerSecond_ = 60.0;
    double sampleRate_ = 0.0;
    float interiorBinScale_ = 0.0F;
    float edgeBinScale_ = 0.0F;
    std::size_t hopSize_ = 1;
    std::size_t writeIndex_ = 0;
    std::size_t validSampleCount_ = 0;
    std::size_t samplesSinceTransform_ = 0;
    bool hasProducedSinceReset_ = false;
    bool hasPreviousChunk_ = false;
    std::uint64_t previousGeneration_ = 0;
    std::uint64_t previousChunkSequence_ = 0;
    std::uint64_t previousCapturedFrameEnd_ = 0;
    std::uint32_t previousChannelCount_ = 0;
    std::uint64_t nextSpectrumSequence_ = 1;
    Statistics statistics_ { };
};
} // namespace audio_insight
