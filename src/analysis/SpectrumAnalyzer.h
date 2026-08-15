// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "StereoSampleCapture.h"
#include "core/SpectrumAnalysisConfiguration.h"
#include "core/VisualizationFrame.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/**
    Streaming, configurable, coherently calibrated spectrum analysis.

    Input chunks may have any size. The analyzer retains one FFT window and
    emits at approximately the configured slice rate. Mono is transformed once;
    stereo publishes the greater magnitude in each bin so phase cancellation
    cannot erase energy from the shared Spectrum/Spectrogram result.

    All FFT plans and maximum-size working storage are created with the analyzer.
    reconfigure() is a non-audio-thread operation and performs no allocation.
*/
class SpectrumAnalyzer final {
public:
    struct Statistics {
        std::uint64_t inputChunks = 0;
        std::uint64_t transforms = 0;
        std::uint64_t temporalResets = 0;
        std::uint64_t sequenceGapResets = 0;
        std::uint64_t configurationChanges = 0;
    };

    SpectrumAnalyzer();

    SpectrumAnalyzer(const SpectrumAnalyzer&) = delete;
    SpectrumAnalyzer& operator=(const SpectrumAnalyzer&) = delete;

    /**
        Applies a validated FFT configuration after worker activity is quiescent.
        A changed configuration resets overlap and invalidates only Spectrum.
    */
    [[nodiscard]] bool reconfigure(const SpectrumAnalysisConfiguration& configuration,
        std::uint64_t fftGeneration,
        VisualizationFrame* destinationToInvalidate = nullptr) noexcept;

    /** Consumes one captured chunk and publishes every completed transform. */
    [[nodiscard]] bool process(
        const CapturedStereoChunkView& chunk, VisualizationFrame& destination) noexcept;

    /** Explicit capture/lifecycle reset. */
    void reset(VisualizationFrame* destinationToInvalidate = nullptr) noexcept;

    [[nodiscard]] Statistics statistics() const noexcept
    {
        return statistics_;
    }
    [[nodiscard]] SpectrumAnalysisConfiguration configuration() const noexcept
    {
        return configuration_;
    }
    [[nodiscard]] std::size_t hopSize() const noexcept
    {
        return hopSize_;
    }
    [[nodiscard]] std::size_t configuredFftSize() const noexcept
    {
        return configuration_.fftSize;
    }
    [[nodiscard]] std::size_t configuredBinCount() const noexcept
    {
        return configuredBinCount_;
    }
    [[nodiscard]] std::uint64_t fftGeneration() const noexcept
    {
        return fftGeneration_;
    }
    [[nodiscard]] double sampleRate() const noexcept
    {
        return sampleRate_;
    }

    [[nodiscard]] static bool isSupportedConfiguration(
        const SpectrumAnalysisConfiguration& configuration) noexcept;

private:
    static constexpr std::size_t maximumTransformWorkspaceSize = maximumFftSize * 2;

    enum class ResetReason {
        explicitReset,
        configurationChange,
        generationChange,
        sequenceGap,
        capturedFrameGap,
        sampleRateChange
    };

    void resetTemporalState(ResetReason reason, VisualizationFrame* destination) noexcept;
    void configureSampleRate(double sampleRate) noexcept;
    void buildWindow() noexcept;
    void runTransform(std::uint64_t generation, std::uint64_t capturedFrameEnd,
        std::uint32_t channelCount, VisualizationFrame& destination) noexcept;
    void prepareChannelTransform(const std::array<float, maximumFftSize>& ring,
        std::array<float, maximumTransformWorkspaceSize>& workspace) noexcept;
    [[nodiscard]] juce::dsp::FFT& selectedFft() noexcept;
    [[nodiscard]] static float magnitudeToDecibels(float magnitude) noexcept;

    juce::dsp::FFT fft1024_ { 10 };
    juce::dsp::FFT fft2048_ { 11 };
    juce::dsp::FFT fft4096_ { 12 };
    juce::dsp::FFT fft8192_ { 13 };
    juce::dsp::FFT fft16384_ { 14 };
    std::array<float, maximumFftSize> window_ { };
    std::array<float, maximumFftSize> leftRing_ { };
    std::array<float, maximumFftSize> rightRing_ { };
    std::array<float, maximumTransformWorkspaceSize> leftWorkspace_ { };
    std::array<float, maximumTransformWorkspaceSize> rightWorkspace_ { };

    SpectrumAnalysisConfiguration configuration_;
    std::uint64_t fftGeneration_ = 1;
    double sampleRate_ = 0.0;
    float interiorBinScale_ = 0.0F;
    float edgeBinScale_ = 0.0F;
    std::size_t configuredBinCount_ = spectrumBinCount;
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
    Statistics statistics_ { };
};
} // namespace audio_insight
