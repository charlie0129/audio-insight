// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "VisualizationFrame.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
inline constexpr std::size_t maximumSpectrogramRowCount = 1024;

/**
    One immutable, frequency-mapped Spectrogram history column.

    sequence counts every offered column, including columns later reclaimed or
    dropped by the bounded handoff. rawTransformSequence identifies the source
    FFT and is deliberately unchanged when a frequency-spacing edit remaps the
    cached transform as a seed. capturedFrameEnd and sampleRate are the exact
    audio-domain timestamp; render cadence is not a history clock.

    A resetMarker has no payload (`rowCount` and `rawTransformSequence` are
    zero). Its sequence and generation fields tell the renderer to clear
    incompatible history when a reset or remap has no FFT column to carry that
    boundary. A real column can carry an ordinary FFT/mapping boundary, so
    markers are not required before every changed generation. Capture
    discontinuities are different: their dedicated marker has captureBoundary
    set and must be delivered independently of the tagged VisualizationFrame.
*/
struct SpectrogramColumn final {
    std::array<float, maximumSpectrogramRowCount> decibels { };

    std::uint64_t sequence = 0;
    std::uint64_t rawTransformSequence = 0;
    std::uint64_t captureGeneration = 0;
    std::uint64_t fftGeneration = 0;
    std::uint64_t mappingGeneration = 0;
    std::uint64_t resetEpoch = 0;
    std::uint64_t capturedFrameEnd = 0;
    double sampleRate = 0.0;
    std::uint32_t fftSize = 0;
    std::uint32_t binCount = 0;
    std::uint32_t rowCount = 0;
    std::uint32_t hopSizeFrames = 0;
    std::uint32_t requestedSliceRateHz = 0;
    bool resetMarker = false;
    bool mappingSeed = false;
    // True only for the dedicated capture-discontinuity reset marker.
    bool captureBoundary = false;

    SpectrogramColumn() noexcept
    {
        decibels.fill(minimumSpectrumDecibels);
    }
};
} // namespace audio_insight
