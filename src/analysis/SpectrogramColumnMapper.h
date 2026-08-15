// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SpectrumTransformSink.h"
#include "core/SpectrogramColumn.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/** Maps calibrated FFT power into uniform intervals of the shared u coordinate. */
class SpectrogramColumnMapper final {
public:
    SpectrogramColumnMapper() noexcept = default;

    [[nodiscard]] bool setFrequencySpacing(
        double spacing, std::uint64_t mappingGeneration) noexcept;

    [[nodiscard]] bool map(const SpectrumTransformView& transform, std::uint64_t columnSequence,
        bool mappingSeed, SpectrogramColumn& destination) noexcept;

    [[nodiscard]] double frequencySpacing() const noexcept
    {
        return frequencySpacing_;
    }
    [[nodiscard]] std::uint64_t mappingGeneration() const noexcept
    {
        return mappingGeneration_;
    }
    [[nodiscard]] std::uint32_t rowCount() const noexcept
    {
        return static_cast<std::uint32_t>(rowCount_);
    }

private:
    struct RowMapping final {
        std::uint32_t firstBin = 0;
        std::uint32_t onePastLastBin = 0;
        std::uint32_t interpolationLowerBin = 0;
        std::uint32_t interpolationUpperBin = 0;
        float interpolationWeight = 0.0F;
    };

    [[nodiscard]] bool rebuildMapping(const SpectrumTransformView& transform) noexcept;
    [[nodiscard]] double mapFrequencyToUnit(double frequencyHz) const noexcept;
    [[nodiscard]] double inverseMapUnitToFrequency(double unit) const noexcept;
    [[nodiscard]] static float powerToDecibels(float power) noexcept;

    std::array<RowMapping, maximumSpectrogramRowCount> rows_ { };
    double frequencySpacing_ = 1.0;
    std::uint64_t mappingGeneration_ = 1;
    double mappedSampleRate_ = 0.0;
    double minimumFrequencyHz_ = 20.0;
    double maximumFrequencyHz_ = 20'000.0;
    std::size_t mappedFftSize_ = 0;
    std::size_t mappedBinCount_ = 0;
    std::size_t rowCount_ = 0;
    bool mappingValid_ = false;
};
} // namespace audio_insight
