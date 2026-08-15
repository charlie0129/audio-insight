// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <bit>
#include <cstdint>

namespace audio_insight {
enum class SpectrumPeakHoldMode {
    off,
    finite,
    infinite,
};

/** Worker-owned Spectrum state that is independent of FFT configuration. */
struct SpectrumTemporalConfiguration final {
    bool averagingEnabled = true;
    double averagingMilliseconds = 75.0;
    SpectrumPeakHoldMode peakHoldMode = SpectrumPeakHoldMode::off;
    double finitePeakHoldSeconds = 2.0;

    [[nodiscard]] constexpr bool operator==(
        const SpectrumTemporalConfiguration& other) const noexcept
    {
        return averagingEnabled == other.averagingEnabled
            && std::bit_cast<std::uint64_t>(averagingMilliseconds)
            == std::bit_cast<std::uint64_t>(other.averagingMilliseconds)
            && peakHoldMode == other.peakHoldMode
            && std::bit_cast<std::uint64_t>(finitePeakHoldSeconds)
            == std::bit_cast<std::uint64_t>(other.finitePeakHoldSeconds);
    }
};
} // namespace audio_insight
