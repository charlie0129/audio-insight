// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/VisualizationFrame.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/** One immutable endpoint from the sample-domain Peak/RMS temporal model. */
struct PeakRmsBallisticsFrame final {
    std::array<float, 2> liveSamplePeakLinear { };
    std::array<float, 2> rmsLinear { };
    std::array<float, 2> heldSamplePeakLinear { };
    std::array<float, 2> liveSamplePeakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> rmsDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> heldSamplePeakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<bool, 2> over { false, false };
    std::array<bool, 2> channelValid { false, false };

    std::uint64_t generation = 0;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    bool valid = false;
};

/**
    Producer-owned, sample-domain temporal state for the sample-peak/RMS meter.

    Every active-channel sample is processed in source order. Worker polling and
    endpoint-snapshot coalescing therefore cannot change the 300 ms RMS result,
    the 20 dB/s live-peak release, or the two-second held-peak timing.
*/
class PeakRmsBallistics final {
public:
    static constexpr double rmsTimeConstantSeconds = 0.300;
    static constexpr double peakHoldSeconds = 2.0;
    static constexpr double peakReleaseDecibelsPerSecond = 20.0;
    static constexpr double holdDecayDecibelsPerSecond = 20.0;

    /**
        Processes one raw audio block and returns the complete endpoint state.

        Null channel pointers and non-finite samples are silence. Invalid format
        metadata or an empty block clears the model and returns an invalid frame.
        Generation, sample-rate, channel-count, and explicit discontinuity changes
        reset all temporal state before the supplied block is processed.
    */
    [[nodiscard]] PeakRmsBallisticsFrame processBlock(const float* left, const float* right,
        std::size_t frameCount, double sampleRate, std::uint64_t generation,
        std::uint32_t channelCount, bool followsDiscontinuity = false) noexcept;

    /** Clears every temporal value and makes current() invalid. */
    void reset() noexcept;

    /** Clears only held peaks and OVER latches. */
    void userReset() noexcept;

    /** Clears live sample-peak and RMS state while preserving holds and OVER. */
    void clearLiveMeasurements() noexcept;

    [[nodiscard]] const PeakRmsBallisticsFrame& current() const noexcept
    {
        return output_;
    }

private:
    struct ChannelState final {
        double rmsMeanSquare = 0.0;
        double liveSamplePeak = 0.0;
        double heldSamplePeak = 0.0;
        double holdSamplesRemaining = 0.0;
        bool over = false;
    };

    [[nodiscard]] static bool isFormatValid(std::size_t frameCount, double sampleRate,
        std::uint64_t generation, std::uint32_t channelCount) noexcept;
    [[nodiscard]] static float linearToDecibels(double value) noexcept;
    void configureFormat(
        double sampleRate, std::uint64_t generation, std::uint32_t channelCount) noexcept;
    void processSample(ChannelState& state, float sample) const noexcept;
    void resetState() noexcept;
    void publish() noexcept;

    std::array<ChannelState, 2> channels_ { };
    PeakRmsBallisticsFrame output_;
    std::uint64_t generation_ = 0;
    std::uint32_t channelCount_ = 0;
    double sampleRate_ = 0.0;
    double rmsDecayPerSample_ = 0.0;
    double rmsInputPerSample_ = 1.0;
    double peakReleasePerSample_ = 0.0;
    double holdDecayPerSample_ = 0.0;
    double holdSamples_ = 0.0;
    bool initialized_ = false;
};
} // namespace audio_insight
