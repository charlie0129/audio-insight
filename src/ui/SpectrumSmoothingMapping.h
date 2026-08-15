// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "state/AnalyzerConfiguration.h"

#include <algorithm>
#include <cmath>

namespace audio_insight {
/**
    Temporary mapping between the normalized renderer Smooth control and the
    saved time-based analyzer configuration.

    The first non-zero slider step maps exactly to the accepted 25 ms minimum,
    avoiding an unreachable range between Off and the first enabled value.
*/
class SpectrumSmoothingMapping final {
public:
    static constexpr double minimumEnabledNormalized = 0.01;
    static constexpr double maximumRendererMilliseconds = 450.0;

    [[nodiscard]] static double toNormalized(const TemporalAveragingSettings& averaging) noexcept
    {
        if (!averaging.enabled)
            return 0.0;

        const auto milliseconds = std::isfinite(averaging.milliseconds)
            ? std::clamp(averaging.milliseconds, TemporalAveragingSettings::minimumMilliseconds,
                  maximumRendererMilliseconds)
            : TemporalAveragingSettings::defaultMilliseconds;
        const auto timeRange
            = maximumRendererMilliseconds - TemporalAveragingSettings::minimumMilliseconds;
        const auto timeProportion
            = (milliseconds - TemporalAveragingSettings::minimumMilliseconds) / timeRange;
        return minimumEnabledNormalized
            + ((1.0 - minimumEnabledNormalized) * std::sqrt(timeProportion));
    }

    [[nodiscard]] static double toMilliseconds(const double normalized) noexcept
    {
        const auto strength = std::isfinite(normalized)
            ? std::clamp(normalized, minimumEnabledNormalized, 1.0)
            : minimumEnabledNormalized;
        const auto timeProportion
            = (strength - minimumEnabledNormalized) / (1.0 - minimumEnabledNormalized);
        const auto timeRange
            = maximumRendererMilliseconds - TemporalAveragingSettings::minimumMilliseconds;
        return TemporalAveragingSettings::minimumMilliseconds
            + (timeRange * timeProportion * timeProportion);
    }
};
} // namespace audio_insight
