// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight
{
inline constexpr std::size_t fftOrder = 12;
inline constexpr std::size_t fftSize = std::size_t { 1 } << fftOrder;
inline constexpr std::size_t spectrumBinCount = (fftSize / 2) + 1;
inline constexpr float minimumDisplayDecibels = -120.0F;

struct VisualizationFrame
{
    std::array<float, spectrumBinCount> spectrumDecibels {};
    std::array<float, 2> peakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> rmsDecibels { minimumDisplayDecibels, minimumDisplayDecibels };

    std::uint64_t generation = 0;
    std::uint64_t spectrumSequence = 0;
    std::uint64_t capturedFrameEnd = 0;
    std::uint64_t droppedChunks = 0;
    double sampleRate = 0.0;
    bool spectrumValid = false;
};
} // namespace audio_insight
