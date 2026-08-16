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
    static constexpr double offMilliseconds = 0.0;
    static constexpr double minimumAttackMilliseconds = 5.0;
    static constexpr double maximumAttackMilliseconds = 500.0;
    static constexpr double defaultAttackMilliseconds = offMilliseconds;
    static constexpr double minimumReleaseMilliseconds = 25.0;
    static constexpr double maximumReleaseMilliseconds = 2'000.0;
    static constexpr double defaultReleaseMilliseconds = 250.0;

    // Zero disables averaging in that direction and follows the current FFT
    // power immediately.
    double attackMilliseconds = defaultAttackMilliseconds;
    double releaseMilliseconds = defaultReleaseMilliseconds;
    SpectrumPeakHoldMode peakHoldMode = SpectrumPeakHoldMode::off;
    double finitePeakHoldSeconds = 2.0;

    [[nodiscard]] constexpr bool operator==(
        const SpectrumTemporalConfiguration& other) const noexcept
    {
        return std::bit_cast<std::uint64_t>(attackMilliseconds)
            == std::bit_cast<std::uint64_t>(other.attackMilliseconds)
            && std::bit_cast<std::uint64_t>(releaseMilliseconds)
            == std::bit_cast<std::uint64_t>(other.releaseMilliseconds)
            && peakHoldMode == other.peakHoldMode
            && std::bit_cast<std::uint64_t>(finitePeakHoldSeconds)
            == std::bit_cast<std::uint64_t>(other.finitePeakHoldSeconds);
    }
};
} // namespace audio_insight
