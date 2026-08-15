// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SpectrumAnalysisConfiguration.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
inline constexpr float minimumSpectrumDecibels = -180.0F;
inline constexpr float minimumDisplayDecibels = -120.0F;

struct VisualizationFrame {
    std::array<float, maximumSpectrumBinCount> spectrumDecibels { };
    std::array<float, 2> peakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> rmsDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> heldPeakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<bool, 2> over { false, false };

    std::uint64_t generation = 0;
    std::uint64_t fftGeneration = 0;
    std::uint64_t spectrumSequence = 0;
    std::uint64_t meterSequence = 0;
    std::uint64_t capturedFrameEnd = 0;
    std::uint64_t droppedChunks = 0;
    std::uint32_t spectrumFftSize = 0;
    std::uint32_t spectrumBinCount = 0;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    bool spectrumValid = false;
    bool meterValid = false;
};
} // namespace audio_insight
