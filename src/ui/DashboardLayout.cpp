// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DashboardLayout.h"

#include <algorithm>

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

DashboardLogicalBounds makeBounds(
    const double left, const double top, const double right, const double bottom) noexcept
{
    return { left, top, std::max(0.0, right - left), std::max(0.0, bottom - top) };
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
} // namespace audio_insight
