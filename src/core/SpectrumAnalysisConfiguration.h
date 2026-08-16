// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>

namespace audio_insight {
inline constexpr std::size_t minimumFftOrder = 10;
inline constexpr std::size_t defaultFftOrder = 13;
inline constexpr std::size_t maximumFftOrder = 14;
inline constexpr std::size_t fftOrder = defaultFftOrder;
inline constexpr std::size_t fftSize = std::size_t { 1 } << defaultFftOrder;
inline constexpr std::size_t spectrumBinCount = (fftSize / 2) + 1;
inline constexpr std::size_t maximumFftSize = std::size_t { 1 } << maximumFftOrder;
inline constexpr std::size_t maximumSpectrumBinCount = (maximumFftSize / 2) + 1;

enum class FftWindow {
    rectangular,
    periodicHann,
    fourTermBlackmanHarris,
    fiveTermFlatTop,
};

/** Immutable worker-side settings for the shared FFT analysis. */
struct SpectrumAnalysisConfiguration final {
    std::size_t fftSize = audio_insight::fftSize;
    FftWindow window = FftWindow::fiveTermFlatTop;
    int requestedSliceRateHz = 60;

    constexpr bool operator==(const SpectrumAnalysisConfiguration&) const noexcept = default;
};
} // namespace audio_insight
