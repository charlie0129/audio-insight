// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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

inline constexpr std::size_t dashboardSplitterCount = 4;

// This order is also the accepted Tab, Shift-Tab, and overlapping hit-test
// priority order for edit-mode handles.
inline constexpr std::array dashboardSplitterTabOrder {
    DashboardSplitter::horizontal,
    DashboardSplitter::upper,
    DashboardSplitter::lowerLeft,
    DashboardSplitter::lowerRight,
};

enum class DashboardSplitterAxis : std::uint8_t {
    horizontal,
    vertical,
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

struct DashboardLogicalPoint {
    double x = 0.0;
    double y = 0.0;
};

struct DashboardSplitterGeometry {
    DashboardSplitter splitter = DashboardSplitter::horizontal;
    DashboardSplitterAxis axis = DashboardSplitterAxis::horizontal;
    DashboardLogicalBounds visualBounds { };
    DashboardLogicalBounds pointerHitBounds { };
};

struct DashboardSplitterLayout {
    std::array<DashboardSplitterGeometry, dashboardSplitterCount> splitters { };

    [[nodiscard]] constexpr const DashboardSplitterGeometry& operator[](
        const std::size_t index) const noexcept
    {
        return splitters[index];
    }
};

/**
    Allocation-free source data for an accessible adjustable separator.

    Percentages describe each adjacent region against the complete normalized
    row or column span. They intentionally do not necessarily sum to 100 for a
    lower-row separator because the third lower-row panel is unaffected.
*/
struct DashboardSplitterAccessibilityValue {
    DashboardSplitter splitter = DashboardSplitter::horizontal;
    std::string_view name { };
    std::string_view firstRegionName { };
    std::string_view secondRegionName { };
    int firstRegionTracks = 0;
    int secondRegionTracks = 0;
    int totalTracks = 0;
    double firstRegionPercentage = 0.0;
    double secondRegionPercentage = 0.0;
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
    static constexpr double splitterVisualThickness = 2.0;
    static constexpr double minimumSplitterPointerHitThickness = 24.0;

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

    // Returns exactly four handle geometries in dashboardSplitterTabOrder. A
    // handle occupies the relevant row or column boundary, with a pointer hit
    // band that remains at least 24 logical points thick on its active axis.
    [[nodiscard]] static DashboardSplitterLayout calculateSplitterLayout(
        DashboardLogicalBounds dashboardBounds, const DashboardLayoutSplits& splits) noexcept;

    // Bounds are tested in dashboardSplitterTabOrder so the horizontal handle
    // wins at the three unavoidable handle intersections.
    [[nodiscard]] static std::optional<DashboardSplitter> hitTestSplitter(
        const DashboardSplitterLayout& layout, DashboardLogicalPoint pointer) noexcept;

    // Converts a top-origin logical pointer position to the nearest normalized
    // grid boundary. The horizontal splitter uses y and all other splitters use
    // x. Unusable/non-finite geometry returns no index; finite positions beyond
    // the dashboard saturate at its outer grid boundary.
    [[nodiscard]] static std::optional<int> nearestGridIndexForPointer(
        DashboardLogicalBounds dashboardBounds, DashboardSplitter splitter,
        DashboardLogicalPoint pointer) noexcept;

    // Convenience for a drag: map, snap, then clamp through moveSplitter(). If
    // mapping is impossible, return the validated current layout unchanged.
    [[nodiscard]] static DashboardLayoutSplits moveSplitterToPointer(
        const DashboardLayoutSplits& splits, DashboardSplitter splitter,
        DashboardLogicalBounds dashboardBounds, DashboardLogicalPoint pointer) noexcept;

    [[nodiscard]] static DashboardSplitterAccessibilityValue accessibilityValue(
        const DashboardLayoutSplits& splits, DashboardSplitter splitter) noexcept;
};
} // namespace audio_insight
