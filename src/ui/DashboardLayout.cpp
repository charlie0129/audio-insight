// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DashboardLayout.h"

#include <algorithm>
#include <cmath>

namespace audio_insight {
namespace {
constexpr int minimumHorizontalSplit = 14;
constexpr int maximumHorizontalSplit = 26;
constexpr int minimumUpperSplit = 24;
constexpr int maximumUpperSplit = 40;
constexpr int minimumSpectrogramColumns = 16;
constexpr int minimumStereoColumns = 8;
constexpr int minimumLoudnessColumns = 6;
constexpr int maximumLoudnessColumns = 12;

struct InnerDashboardBounds {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    bool usable = false;
};

DashboardLogicalBounds makeBounds(
    const double left, const double top, const double right, const double bottom) noexcept
{
    return { left, top, std::max(0.0, right - left), std::max(0.0, bottom - top) };
}

InnerDashboardBounds calculateInnerBounds(const DashboardLogicalBounds bounds) noexcept
{
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width)
        || !std::isfinite(bounds.height)) {
        return { };
    }

    const auto safeWidth = std::max(0.0, bounds.width);
    const auto safeHeight = std::max(0.0, bounds.height);
    const auto horizontalInset = std::min(DashboardLayout::outerInset, safeWidth * 0.5);
    const auto verticalInset = std::min(DashboardLayout::outerInset, safeHeight * 0.5);
    const auto left = bounds.x + horizontalInset;
    const auto top = bounds.y + verticalInset;
    const auto right = bounds.x + safeWidth - horizontalInset;
    const auto bottom = bounds.y + safeHeight - verticalInset;

    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right)
        || !std::isfinite(bottom) || right < left || bottom < top) {
        return { };
    }

    return { left, top, right, bottom, true };
}

DashboardLogicalBounds makeHorizontalBand(
    const double left, const double right, const double centreY, const double thickness) noexcept
{
    return { left, centreY - (thickness * 0.5), std::max(0.0, right - left), thickness };
}

DashboardLogicalBounds makeVerticalBand(
    const double top, const double bottom, const double centreX, const double thickness) noexcept
{
    return { centreX - (thickness * 0.5), top, thickness, std::max(0.0, bottom - top) };
}

bool contains(const DashboardLogicalBounds bounds, const DashboardLogicalPoint point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y) && bounds.width > 0.0
        && bounds.height > 0.0 && point.x >= bounds.x && point.x <= bounds.right()
        && point.y >= bounds.y && point.y <= bounds.bottom();
}

double percentage(const int tracks, const int totalTracks) noexcept
{
    return 100.0 * static_cast<double>(tracks) / static_cast<double>(totalTracks);
}
} // namespace

bool DashboardLayout::isValid(const DashboardLayoutSplits& splits) noexcept
{
    return splits.horizontal >= minimumHorizontalSplit
        && splits.horizontal <= maximumHorizontalSplit && splits.upper >= minimumUpperSplit
        && splits.upper <= maximumUpperSplit
        && splits.lowerRight >= columnCount - maximumLoudnessColumns
        && splits.lowerRight <= columnCount - minimumLoudnessColumns
        && splits.lowerLeft >= minimumSpectrogramColumns
        && splits.lowerLeft <= splits.lowerRight - minimumStereoColumns;
}

DashboardLayoutSplits DashboardLayout::validOrDefault(const DashboardLayoutSplits& splits) noexcept
{
    return isValid(splits) ? splits : defaultSplits;
}

DashboardSplitterRange DashboardLayout::legalRange(
    const DashboardLayoutSplits& splits, const DashboardSplitter splitter) noexcept
{
    const auto validSplits = validOrDefault(splits);

    switch (splitter) {
    case DashboardSplitter::horizontal:
        return { minimumHorizontalSplit, maximumHorizontalSplit };
    case DashboardSplitter::upper:
        return { minimumUpperSplit, maximumUpperSplit };
    case DashboardSplitter::lowerLeft:
        return { minimumSpectrogramColumns, validSplits.lowerRight - minimumStereoColumns };
    case DashboardSplitter::lowerRight:
        return { std::max(columnCount - maximumLoudnessColumns,
                     validSplits.lowerLeft + minimumStereoColumns),
            columnCount - minimumLoudnessColumns };
    }

    return { };
}

DashboardLayoutSplits DashboardLayout::moveSplitter(const DashboardLayoutSplits& splits,
    const DashboardSplitter splitter, const int requestedGridIndex) noexcept
{
    auto moved = validOrDefault(splits);
    const auto range = legalRange(moved, splitter);
    const auto position = std::clamp(requestedGridIndex, range.minimum, range.maximum);

    switch (splitter) {
    case DashboardSplitter::horizontal:
        moved.horizontal = position;
        break;
    case DashboardSplitter::upper:
        moved.upper = position;
        break;
    case DashboardSplitter::lowerLeft:
        moved.lowerLeft = position;
        break;
    case DashboardSplitter::lowerRight:
        moved.lowerRight = position;
        break;
    }

    return moved;
}

int DashboardLayout::splitterPosition(
    const DashboardLayoutSplits& splits, const DashboardSplitter splitter) noexcept
{
    const auto validSplits = validOrDefault(splits);

    switch (splitter) {
    case DashboardSplitter::horizontal:
        return validSplits.horizontal;
    case DashboardSplitter::upper:
        return validSplits.upper;
    case DashboardSplitter::lowerLeft:
        return validSplits.lowerLeft;
    case DashboardSplitter::lowerRight:
        return validSplits.lowerRight;
    }

    return 0;
}

DashboardGridBounds DashboardLayout::gridBounds(
    const DashboardLayoutSplits& splits, const DashboardPanel panel) noexcept
{
    const auto validSplits = validOrDefault(splits);

    switch (panel) {
    case DashboardPanel::spectrum:
        return { 0, 0, validSplits.upper, validSplits.horizontal };
    case DashboardPanel::peakRms:
        return { validSplits.upper, 0, columnCount - validSplits.upper, validSplits.horizontal };
    case DashboardPanel::spectrogram:
        return { 0, validSplits.horizontal, validSplits.lowerLeft,
            rowCount - validSplits.horizontal };
    case DashboardPanel::stereoField:
        return { validSplits.lowerLeft, validSplits.horizontal,
            validSplits.lowerRight - validSplits.lowerLeft, rowCount - validSplits.horizontal };
    case DashboardPanel::loudness:
        return { validSplits.lowerRight, validSplits.horizontal,
            columnCount - validSplits.lowerRight, rowCount - validSplits.horizontal };
    }

    return { };
}

DashboardTileLayout DashboardLayout::calculateTileLayout(
    const DashboardLogicalBounds dashboardBounds, const DashboardLayoutSplits& splits) noexcept
{
    const auto validSplits = validOrDefault(splits);
    const auto safeWidth = std::max(0.0, dashboardBounds.width);
    const auto safeHeight = std::max(0.0, dashboardBounds.height);
    const auto horizontalInset = std::min(outerInset, safeWidth * 0.5);
    const auto verticalInset = std::min(outerInset, safeHeight * 0.5);

    const auto left = dashboardBounds.x + horizontalInset;
    const auto right = dashboardBounds.x + safeWidth - horizontalInset;
    const auto top = dashboardBounds.y + verticalInset;
    const auto bottom = dashboardBounds.y + safeHeight - verticalInset;
    const auto innerWidth = right - left;
    const auto innerHeight = bottom - top;
    const auto halfHorizontalGutter = std::min(gutter * 0.5, innerWidth * 0.5);
    const auto halfVerticalGutter = std::min(gutter * 0.5, innerHeight * 0.5);

    const auto rowSplit = top
        + innerHeight * static_cast<double>(validSplits.horizontal) / static_cast<double>(rowCount);
    const auto upperSplit = left
        + innerWidth * static_cast<double>(validSplits.upper) / static_cast<double>(columnCount);
    const auto lowerLeftSplit = left
        + innerWidth * static_cast<double>(validSplits.lowerLeft)
            / static_cast<double>(columnCount);
    const auto lowerRightSplit = left
        + innerWidth * static_cast<double>(validSplits.lowerRight)
            / static_cast<double>(columnCount);
    const auto clampX
        = [left, right](const double value) { return std::clamp(value, left, right); };
    const auto clampY
        = [top, bottom](const double value) { return std::clamp(value, top, bottom); };

    DashboardTileLayout layout;
    layout.tiles[static_cast<std::size_t>(DashboardPanel::spectrum)] = makeBounds(left, top,
        clampX(upperSplit - halfHorizontalGutter), clampY(rowSplit - halfVerticalGutter));
    layout.tiles[static_cast<std::size_t>(DashboardPanel::peakRms)]
        = makeBounds(clampX(upperSplit + halfHorizontalGutter), top, right,
            clampY(rowSplit - halfVerticalGutter));
    layout.tiles[static_cast<std::size_t>(DashboardPanel::spectrogram)]
        = makeBounds(left, clampY(rowSplit + halfVerticalGutter),
            clampX(lowerLeftSplit - halfHorizontalGutter), bottom);
    layout.tiles[static_cast<std::size_t>(DashboardPanel::stereoField)] = makeBounds(
        clampX(lowerLeftSplit + halfHorizontalGutter), clampY(rowSplit + halfVerticalGutter),
        clampX(lowerRightSplit - halfHorizontalGutter), bottom);
    layout.tiles[static_cast<std::size_t>(DashboardPanel::loudness)]
        = makeBounds(clampX(lowerRightSplit + halfHorizontalGutter),
            clampY(rowSplit + halfVerticalGutter), right, bottom);
    return layout;
}

DashboardSplitterLayout DashboardLayout::calculateSplitterLayout(
    const DashboardLogicalBounds dashboardBounds, const DashboardLayoutSplits& splits) noexcept
{
    const auto validSplits = validOrDefault(splits);
    const auto inner = calculateInnerBounds(dashboardBounds);
    const auto innerWidth = std::max(0.0, inner.right - inner.left);
    const auto innerHeight = std::max(0.0, inner.bottom - inner.top);
    const auto rowSplit = inner.top
        + innerHeight * static_cast<double>(validSplits.horizontal) / static_cast<double>(rowCount);
    const auto upperSplit = inner.left
        + innerWidth * static_cast<double>(validSplits.upper) / static_cast<double>(columnCount);
    const auto lowerLeftSplit = inner.left
        + innerWidth * static_cast<double>(validSplits.lowerLeft)
            / static_cast<double>(columnCount);
    const auto lowerRightSplit = inner.left
        + innerWidth * static_cast<double>(validSplits.lowerRight)
            / static_cast<double>(columnCount);
    const auto horizontalVisualThickness = innerHeight > 0.0 ? splitterVisualThickness : 0.0;
    const auto horizontalHitThickness
        = innerHeight > 0.0 ? minimumSplitterPointerHitThickness : 0.0;
    const auto verticalVisualThickness = innerWidth > 0.0 ? splitterVisualThickness : 0.0;
    const auto verticalHitThickness = innerWidth > 0.0 ? minimumSplitterPointerHitThickness : 0.0;

    DashboardSplitterLayout layout;
    layout.splitters = {
        DashboardSplitterGeometry { DashboardSplitter::horizontal,
            DashboardSplitterAxis::horizontal,
            makeHorizontalBand(inner.left, inner.right, rowSplit, horizontalVisualThickness),
            makeHorizontalBand(inner.left, inner.right, rowSplit, horizontalHitThickness) },
        DashboardSplitterGeometry { DashboardSplitter::upper, DashboardSplitterAxis::vertical,
            makeVerticalBand(inner.top, rowSplit, upperSplit, verticalVisualThickness),
            makeVerticalBand(inner.top, rowSplit, upperSplit, verticalHitThickness) },
        DashboardSplitterGeometry { DashboardSplitter::lowerLeft, DashboardSplitterAxis::vertical,
            makeVerticalBand(rowSplit, inner.bottom, lowerLeftSplit, verticalVisualThickness),
            makeVerticalBand(rowSplit, inner.bottom, lowerLeftSplit, verticalHitThickness) },
        DashboardSplitterGeometry { DashboardSplitter::lowerRight, DashboardSplitterAxis::vertical,
            makeVerticalBand(rowSplit, inner.bottom, lowerRightSplit, verticalVisualThickness),
            makeVerticalBand(rowSplit, inner.bottom, lowerRightSplit, verticalHitThickness) }
    };
    return layout;
}

std::optional<DashboardSplitter> DashboardLayout::hitTestSplitter(
    const DashboardSplitterLayout& layout, const DashboardLogicalPoint pointer) noexcept
{
    for (const auto& geometry : layout.splitters) {
        if (contains(geometry.pointerHitBounds, pointer))
            return geometry.splitter;
    }

    return std::nullopt;
}

std::optional<int> DashboardLayout::nearestGridIndexForPointer(
    const DashboardLogicalBounds dashboardBounds, const DashboardSplitter splitter,
    const DashboardLogicalPoint pointer) noexcept
{
    const auto inner = calculateInnerBounds(dashboardBounds);
    if (!inner.usable || !std::isfinite(pointer.x) || !std::isfinite(pointer.y))
        return std::nullopt;

    if (inner.right <= inner.left || inner.bottom <= inner.top)
        return std::nullopt;

    const auto horizontal = splitter == DashboardSplitter::horizontal;
    const auto coordinate = horizontal ? pointer.y : pointer.x;
    const auto start = horizontal ? inner.top : inner.left;
    const auto end = horizontal ? inner.bottom : inner.right;
    const auto trackCount = horizontal ? rowCount : columnCount;
    if (!std::isfinite(coordinate) || end <= start)
        return std::nullopt;

    if (coordinate <= start)
        return 0;
    if (coordinate >= end)
        return trackCount;

    const auto unitPosition = (coordinate - start) / (end - start);
    return static_cast<int>(std::floor(unitPosition * static_cast<double>(trackCount) + 0.5));
}

DashboardLayoutSplits DashboardLayout::moveSplitterToPointer(const DashboardLayoutSplits& splits,
    const DashboardSplitter splitter, const DashboardLogicalBounds dashboardBounds,
    const DashboardLogicalPoint pointer) noexcept
{
    const auto validSplits = validOrDefault(splits);
    const auto gridIndex = nearestGridIndexForPointer(dashboardBounds, splitter, pointer);
    return gridIndex.has_value() ? moveSplitter(validSplits, splitter, *gridIndex) : validSplits;
}

DashboardSplitterAccessibilityValue DashboardLayout::accessibilityValue(
    const DashboardLayoutSplits& splits, const DashboardSplitter splitter) noexcept
{
    const auto validSplits = validOrDefault(splits);

    switch (splitter) {
    case DashboardSplitter::horizontal:
        return { splitter, "Upper and lower dashboard height", "Upper band", "Lower band",
            validSplits.horizontal, rowCount - validSplits.horizontal, rowCount,
            percentage(validSplits.horizontal, rowCount),
            percentage(rowCount - validSplits.horizontal, rowCount) };
    case DashboardSplitter::upper:
        return { splitter, "Spectrum and Peak/RMS width", "Spectrum", "Peak/RMS", validSplits.upper,
            columnCount - validSplits.upper, columnCount,
            percentage(validSplits.upper, columnCount),
            percentage(columnCount - validSplits.upper, columnCount) };
    case DashboardSplitter::lowerLeft:
        return { splitter, "Spectrogram and Stereo width", "Spectrogram", "Stereo",
            validSplits.lowerLeft, validSplits.lowerRight - validSplits.lowerLeft, columnCount,
            percentage(validSplits.lowerLeft, columnCount),
            percentage(validSplits.lowerRight - validSplits.lowerLeft, columnCount) };
    case DashboardSplitter::lowerRight:
        return { splitter, "Stereo and Loudness width", "Stereo", "Loudness",
            validSplits.lowerRight - validSplits.lowerLeft, columnCount - validSplits.lowerRight,
            columnCount, percentage(validSplits.lowerRight - validSplits.lowerLeft, columnCount),
            percentage(columnCount - validSplits.lowerRight, columnCount) };
    }

    return { };
}
} // namespace audio_insight
