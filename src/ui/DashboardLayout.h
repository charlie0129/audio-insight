// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
enum class DashboardPanel : std::uint8_t {
    spectrum,
    peakRms,
    spectrogram,
    stereoField,
    loudness,
};

inline constexpr std::size_t dashboardPanelCount = 5;

enum class DashboardSplitter : std::uint8_t {
    horizontal,
    upper,
    lowerLeft,
    lowerRight,
};

struct DashboardLayoutSplits {
    int horizontal = 22;
    int upper = 36;
    int lowerLeft = 28;
    int lowerRight = 40;

    [[nodiscard]] bool operator==(const DashboardLayoutSplits&) const noexcept = default;
};

struct DashboardSplitterRange {
    int minimum = 0;
    int maximum = 0;

    [[nodiscard]] bool operator==(const DashboardSplitterRange&) const noexcept = default;
};

struct DashboardGridBounds {
    int column = 0;
    int row = 0;
    int columnSpan = 0;
    int rowSpan = 0;

    [[nodiscard]] bool operator==(const DashboardGridBounds&) const noexcept = default;
};

struct DashboardLogicalBounds {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] constexpr double right() const noexcept
    {
        return x + width;
    }

    [[nodiscard]] constexpr double bottom() const noexcept
    {
        return y + height;
    }
};

struct DashboardTileLayout {
    std::array<DashboardLogicalBounds, dashboardPanelCount> tiles { };

    [[nodiscard]] constexpr const DashboardLogicalBounds& operator[](
        const DashboardPanel panel) const noexcept
    {
        return tiles[static_cast<std::size_t>(panel)];
    }
};

/**
    Renderer-independent model for the accepted fixed-topology dashboard.

    Only four split indices are mutable. This makes overlap prevention a matter
    of clamping ordered splitters rather than running a tile-placement
    algorithm. Logical bounds are suitable for later conversion to either JUCE
    coordinates or physical Metal pixels.
*/
class DashboardLayout final {
public:
    static constexpr int columnCount = 48;
    static constexpr int rowCount = 40;
    static constexpr double outerInset = 8.0;
    static constexpr double gutter = 8.0;

    inline static constexpr DashboardLayoutSplits defaultSplits { 22, 36, 28, 40 };

    [[nodiscard]] static bool isValid(const DashboardLayoutSplits& splits) noexcept;

    // Invalid persisted data is rejected as a whole, as required by the
    // preference migration policy, instead of being partially repaired.
    [[nodiscard]] static DashboardLayoutSplits validOrDefault(
        const DashboardLayoutSplits& splits) noexcept;

    [[nodiscard]] static DashboardSplitterRange legalRange(
        const DashboardLayoutSplits& splits, DashboardSplitter splitter) noexcept;

    // Moves one splitter and clamps it against the accepted grid and its
    // adjacent splitter. An invalid input layout starts from compiled defaults.
    [[nodiscard]] static DashboardLayoutSplits moveSplitter(const DashboardLayoutSplits& splits,
        DashboardSplitter splitter, int requestedGridIndex) noexcept;

    [[nodiscard]] static int splitterPosition(
        const DashboardLayoutSplits& splits, DashboardSplitter splitter) noexcept;

    [[nodiscard]] static DashboardGridBounds gridBounds(
        const DashboardLayoutSplits& splits, DashboardPanel panel) noexcept;

    // Split positions are evaluated across the inset dashboard span. Half of a
    // fixed gutter is then removed from each adjacent tile. Degenerate input
    // bounds saturate tile extents at zero rather than producing negative sizes.
    [[nodiscard]] static DashboardTileLayout calculateTileLayout(
        DashboardLogicalBounds dashboardBounds, const DashboardLayoutSplits& splits) noexcept;
};
} // namespace audio_insight
