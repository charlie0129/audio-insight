// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SpectrumAnalysisConfiguration.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
inline constexpr float minimumSpectrumDecibels = -180.0F;
inline constexpr float minimumDisplayDecibels = -120.0F;
inline constexpr std::size_t maximumStereoFieldPointCount = 4'096;
inline constexpr double stereoFieldHistorySeconds = 0.250;

/** One fixed-full-scale point in the conventional rotated stereo field. */
struct StereoFieldPoint final {
    float horizontal = 0.0F;
    float vertical = 0.0F;
    // Zero is the newest represented input; one is 250 ms old.
    float normalizedAge = 0.0F;
};

struct VisualizationFrame {
    std::array<float, maximumSpectrumBinCount> spectrumDecibels { };
    std::array<float, maximumSpectrumBinCount> spectrumPeakHoldDecibels { };
    std::array<StereoFieldPoint, maximumStereoFieldPointCount> stereoFieldPoints { };
    std::array<float, 2> peakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> rmsDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> heldPeakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<bool, 2> over { false, false };

    std::uint64_t generation = 0;
    std::uint64_t fftGeneration = 0;
    std::uint64_t spectrumSequence = 0;
    std::uint64_t meterSequence = 0;
    std::uint64_t stereoSequence = 0;
    std::uint64_t capturedFrameEnd = 0;
    std::uint64_t spectrumCapturedFrameEnd = 0;
    std::uint64_t stereoCapturedFrameEnd = 0;
    std::uint64_t droppedChunks = 0;
    std::uint32_t spectrumFftSize = 0;
    std::uint32_t spectrumBinCount = 0;
    std::uint32_t stereoFieldPointCount = 0;
    std::uint32_t stereoPointStrideFrames = 0;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    float stereoCorrelation = 0.0F;
    bool spectrumValid = false;
    bool spectrumPeakHoldValid = false;
    bool meterValid = false;
    bool stereoFieldValid = false;
    bool stereoCorrelationValid = false;
    bool stereoMono = false;
};
} // namespace audio_insight
