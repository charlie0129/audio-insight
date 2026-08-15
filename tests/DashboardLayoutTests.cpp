// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DashboardLayout.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace audio_insight {
namespace {
constexpr double comparisonTolerance = 1.0e-9;
constexpr auto minimumInteger = std::numeric_limits<int>::min();
constexpr auto maximumInteger = std::numeric_limits<int>::max();

constexpr std::array dashboardPanels {
    DashboardPanel::spectrum,
    DashboardPanel::peakRms,
    DashboardPanel::spectrogram,
    DashboardPanel::stereoField,
    DashboardPanel::loudness,
};

constexpr std::array dashboardSplitters {
    DashboardSplitter::horizontal,
    DashboardSplitter::upper,
    DashboardSplitter::lowerLeft,
    DashboardSplitter::lowerRight,
};

[[nodiscard]] bool approximatelyEqual(const double first, const double second) noexcept
{
    return std::abs(first - second) <= comparisonTolerance;
}

[[nodiscard]] bool overlaps(
    const DashboardLogicalBounds& first, const DashboardLogicalBounds& second) noexcept
{
    return first.x < second.right() && second.x < first.right() && first.y < second.bottom()
        && second.y < first.bottom();
}

[[nodiscard]] bool approximatelyEqual(
    const DashboardLogicalBounds& first, const DashboardLogicalBounds& second) noexcept
{
    return approximatelyEqual(first.x, second.x) && approximatelyEqual(first.y, second.y)
        && approximatelyEqual(first.width, second.width)
        && approximatelyEqual(first.height, second.height);
}

[[nodiscard]] bool isFinite(const DashboardLogicalBounds& bounds) noexcept
{
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width)
        && std::isfinite(bounds.height);
}

[[nodiscard]] double centreX(const DashboardLogicalBounds& bounds) noexcept
{
    return bounds.x + (bounds.width * 0.5);
}

[[nodiscard]] double centreY(const DashboardLogicalBounds& bounds) noexcept
{
    return bounds.y + (bounds.height * 0.5);
}

[[nodiscard]] bool hasAcceptedConstraints(const DashboardLayoutSplits& splits) noexcept
{
    return splits.horizontal >= 14 && splits.horizontal <= 26 && splits.upper >= 24
        && splits.upper <= 40 && splits.lowerLeft >= 16 && splits.lowerRight >= 36
        && splits.lowerRight <= 42 && splits.lowerLeft <= splits.lowerRight - 8;
}

[[nodiscard]] DashboardSplitterRange acceptedRange(
    const DashboardLayoutSplits& splits, const DashboardSplitter splitter) noexcept
{
    switch (splitter) {
    case DashboardSplitter::horizontal:
        return { 14, 26 };
    case DashboardSplitter::upper:
        return { 24, 40 };
    case DashboardSplitter::lowerLeft:
        return { 16, splits.lowerRight - 8 };
    case DashboardSplitter::lowerRight:
        return { std::max(36, splits.lowerLeft + 8), 42 };
    }

    return { };
}

void setSplitterPosition(
    DashboardLayoutSplits& splits, const DashboardSplitter splitter, const int position) noexcept
{
    switch (splitter) {
    case DashboardSplitter::horizontal:
        splits.horizontal = position;
        break;
    case DashboardSplitter::upper:
        splits.upper = position;
        break;
    case DashboardSplitter::lowerLeft:
        splits.lowerLeft = position;
        break;
    case DashboardSplitter::lowerRight:
        splits.lowerRight = position;
        break;
    }
}

class DashboardLayoutTests final : public juce::UnitTest {
public:
    DashboardLayoutTests() : UnitTest("Dashboard layout", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Compiled defaults reproduce the accepted five-tile grid");
        {
            constexpr DashboardLayoutSplits expectedSplits { 22, 36, 28, 40 };
            expect(DashboardLayout::defaultSplits == expectedSplits);
            expect(DashboardLayout::isValid(DashboardLayout::defaultSplits));

            expect(DashboardLayout::gridBounds(expectedSplits, DashboardPanel::spectrum)
                == DashboardGridBounds { 0, 0, 36, 22 });
            expect(DashboardLayout::gridBounds(expectedSplits, DashboardPanel::peakRms)
                == DashboardGridBounds { 36, 0, 12, 22 });
            expect(DashboardLayout::gridBounds(expectedSplits, DashboardPanel::spectrogram)
                == DashboardGridBounds { 0, 22, 28, 18 });
            expect(DashboardLayout::gridBounds(expectedSplits, DashboardPanel::stereoField)
                == DashboardGridBounds { 28, 22, 12, 18 });
            expect(DashboardLayout::gridBounds(expectedSplits, DashboardPanel::loudness)
                == DashboardGridBounds { 40, 22, 8, 18 });
        }

        beginTest("Every physical-grid split tuple is classified and defaulted atomically");
        {
            std::uint64_t validCount = 0;
            std::uint64_t invalidCount = 0;
            std::uint64_t validityMismatches = 0;
            std::uint64_t fallbackMismatches = 0;

            for (auto horizontal = 0; horizontal <= DashboardLayout::rowCount; ++horizontal) {
                for (auto upper = 0; upper <= DashboardLayout::columnCount; ++upper) {
                    for (auto lowerLeft = 0; lowerLeft <= DashboardLayout::columnCount;
                        ++lowerLeft) {
                        for (auto lowerRight = 0; lowerRight <= DashboardLayout::columnCount;
                            ++lowerRight) {
                            const DashboardLayoutSplits candidate { horizontal, upper, lowerLeft,
                                lowerRight };
                            const auto expectedValid = hasAcceptedConstraints(candidate);
                            const auto actualValid = DashboardLayout::isValid(candidate);
                            validityMismatches += expectedValid != actualValid ? 1U : 0U;

                            if (expectedValid) {
                                ++validCount;
                                fallbackMismatches
                                    += DashboardLayout::validOrDefault(candidate) != candidate ? 1U
                                                                                               : 0U;
                            } else {
                                ++invalidCount;
                                fallbackMismatches += DashboardLayout::validOrDefault(candidate)
                                        != DashboardLayout::defaultSplits
                                    ? 1U
                                    : 0U;
                            }
                        }
                    }
                }
            }

            expect(validityMismatches == 0,
                juce::String("Validity mismatches: ") + juce::String(validityMismatches));
            expect(fallbackMismatches == 0,
                juce::String("Atomic fallback mismatches: ") + juce::String(fallbackMismatches));
            expect(validCount == 24'752,
                juce::String("Unexpected legal-layout count: ") + juce::String(validCount));
            expect(invalidCount == 4'798'857,
                juce::String("Unexpected invalid-layout count: ") + juce::String(invalidCount));
        }

        beginTest("Extreme malformed integers are rejected without partial repair");
        {
            constexpr std::array invalidLayouts {
                DashboardLayoutSplits { minimumInteger, 36, 28, 40 },
                DashboardLayoutSplits { maximumInteger, 36, 28, 40 },
                DashboardLayoutSplits { 22, minimumInteger, 28, 40 },
                DashboardLayoutSplits { 22, maximumInteger, 28, 40 },
                DashboardLayoutSplits { 22, 36, minimumInteger, 40 },
                DashboardLayoutSplits { 22, 36, maximumInteger, 40 },
                DashboardLayoutSplits { 22, 36, 28, minimumInteger },
                DashboardLayoutSplits { 22, 36, 28, maximumInteger },
                DashboardLayoutSplits { 22, 36, minimumInteger, maximumInteger },
                DashboardLayoutSplits { 22, 36, maximumInteger, minimumInteger },
            };
            constexpr DashboardLogicalBounds dashboard { 11.0, 17.0, 1200.0, 800.0 };
            const auto defaultLayout
                = DashboardLayout::calculateTileLayout(dashboard, DashboardLayout::defaultSplits);

            for (const auto& invalid : invalidLayouts) {
                expect(!DashboardLayout::isValid(invalid));
                expect(DashboardLayout::validOrDefault(invalid) == DashboardLayout::defaultSplits);

                for (const auto panel : dashboardPanels) {
                    expect(DashboardLayout::gridBounds(invalid, panel)
                        == DashboardLayout::gridBounds(DashboardLayout::defaultSplits, panel));
                }

                const auto invalidLayout = DashboardLayout::calculateTileLayout(dashboard, invalid);
                for (std::size_t index = 0; index < dashboardPanelCount; ++index) {
                    expect(
                        approximatelyEqual(invalidLayout.tiles[index], defaultLayout.tiles[index]));
                }
            }

            const auto moved = DashboardLayout::moveSplitter(
                invalidLayouts.front(), DashboardSplitter::upper, 30);
            expect(moved == DashboardLayoutSplits { 22, 30, 28, 40 });
        }

        beginTest("All legal layouts expose exact ranges and bounded splitter moves");
        {
            std::uint64_t rangeMismatches = 0;
            std::uint64_t positionMismatches = 0;
            std::uint64_t moveMismatches = 0;

            for (auto horizontal = 14; horizontal <= 26; ++horizontal) {
                for (auto upper = 24; upper <= 40; ++upper) {
                    for (auto lowerRight = 36; lowerRight <= 42; ++lowerRight) {
                        for (auto lowerLeft = 16; lowerLeft <= lowerRight - 8; ++lowerLeft) {
                            const DashboardLayoutSplits splits { horizontal, upper, lowerLeft,
                                lowerRight };

                            for (const auto splitter : dashboardSplitters) {
                                const auto range = DashboardLayout::legalRange(splits, splitter);
                                const auto expected = acceptedRange(splits, splitter);
                                rangeMismatches += range != expected ? 1U : 0U;

                                DashboardLayoutSplits expectedPosition = splits;
                                setSplitterPosition(expectedPosition, splitter,
                                    DashboardLayout::splitterPosition(splits, splitter));
                                positionMismatches += expectedPosition != splits ? 1U : 0U;

                                for (auto requested = -1;
                                    requested <= DashboardLayout::columnCount + 1; ++requested) {
                                    auto expectedMove = splits;
                                    setSplitterPosition(expectedMove, splitter,
                                        std::clamp(requested, expected.minimum, expected.maximum));
                                    const auto actualMove = DashboardLayout::moveSplitter(
                                        splits, splitter, requested);
                                    moveMismatches += actualMove != expectedMove ? 1U : 0U;
                                    moveMismatches
                                        += !DashboardLayout::isValid(actualMove) ? 1U : 0U;
                                }

                                for (const auto requested : { minimumInteger, maximumInteger }) {
                                    auto expectedMove = splits;
                                    setSplitterPosition(expectedMove, splitter,
                                        std::clamp(requested, expected.minimum, expected.maximum));
                                    const auto actualMove = DashboardLayout::moveSplitter(
                                        splits, splitter, requested);
                                    moveMismatches += actualMove != expectedMove ? 1U : 0U;
                                }
                            }
                        }
                    }
                }
            }

            expect(rangeMismatches == 0,
                juce::String("Range mismatches: ") + juce::String(rangeMismatches));
            expect(positionMismatches == 0,
                juce::String("Position mismatches: ") + juce::String(positionMismatches));
            expect(moveMismatches == 0,
                juce::String("Move mismatches: ") + juce::String(moveMismatches));
        }

        beginTest("Every legal layout preserves grid partitions, insets, and gutters");
        {
            constexpr DashboardLogicalBounds dashboard { 100.0, 50.0, 1200.0, 800.0 };
            constexpr auto innerLeft = dashboard.x + DashboardLayout::outerInset;
            constexpr auto innerTop = dashboard.y + DashboardLayout::outerInset;
            constexpr auto innerRight = dashboard.x + dashboard.width - DashboardLayout::outerInset;
            constexpr auto innerBottom
                = dashboard.y + dashboard.height - DashboardLayout::outerInset;
            constexpr auto innerWidth = dashboard.width - (2.0 * DashboardLayout::outerInset);
            constexpr auto innerHeight = dashboard.height - (2.0 * DashboardLayout::outerInset);
            std::uint64_t geometryMismatches = 0;

            const auto verify = [&geometryMismatches](const bool condition) {
                geometryMismatches += condition ? 0U : 1U;
            };

            for (auto horizontal = 14; horizontal <= 26; ++horizontal) {
                for (auto upper = 24; upper <= 40; ++upper) {
                    for (auto lowerRight = 36; lowerRight <= 42; ++lowerRight) {
                        for (auto lowerLeft = 16; lowerLeft <= lowerRight - 8; ++lowerLeft) {
                            const DashboardLayoutSplits splits { horizontal, upper, lowerLeft,
                                lowerRight };
                            const auto gridSpectrum
                                = DashboardLayout::gridBounds(splits, DashboardPanel::spectrum);
                            const auto gridPeakRms
                                = DashboardLayout::gridBounds(splits, DashboardPanel::peakRms);
                            const auto gridSpectrogram
                                = DashboardLayout::gridBounds(splits, DashboardPanel::spectrogram);
                            const auto gridStereo
                                = DashboardLayout::gridBounds(splits, DashboardPanel::stereoField);
                            const auto gridLoudness
                                = DashboardLayout::gridBounds(splits, DashboardPanel::loudness);

                            verify(gridSpectrum == DashboardGridBounds { 0, 0, upper, horizontal });
                            verify(gridPeakRms
                                == DashboardGridBounds {
                                    upper, 0, DashboardLayout::columnCount - upper, horizontal });
                            verify(gridSpectrogram
                                == DashboardGridBounds { 0, horizontal, lowerLeft,
                                    DashboardLayout::rowCount - horizontal });
                            verify(gridStereo
                                == DashboardGridBounds { lowerLeft, horizontal,
                                    lowerRight - lowerLeft,
                                    DashboardLayout::rowCount - horizontal });
                            verify(gridLoudness
                                == DashboardGridBounds { lowerRight, horizontal,
                                    DashboardLayout::columnCount - lowerRight,
                                    DashboardLayout::rowCount - horizontal });

                            const auto layout
                                = DashboardLayout::calculateTileLayout(dashboard, splits);
                            const auto& spectrum = layout[DashboardPanel::spectrum];
                            const auto& peakRms = layout[DashboardPanel::peakRms];
                            const auto& spectrogram = layout[DashboardPanel::spectrogram];
                            const auto& stereo = layout[DashboardPanel::stereoField];
                            const auto& loudness = layout[DashboardPanel::loudness];

                            for (const auto& tile : layout.tiles) {
                                verify(tile.width > 0.0);
                                verify(tile.height > 0.0);
                                verify(tile.x >= innerLeft - comparisonTolerance);
                                verify(tile.y >= innerTop - comparisonTolerance);
                                verify(tile.right() <= innerRight + comparisonTolerance);
                                verify(tile.bottom() <= innerBottom + comparisonTolerance);
                            }

                            for (std::size_t first = 0; first < layout.tiles.size(); ++first) {
                                for (auto second = first + 1; second < layout.tiles.size();
                                    ++second) {
                                    verify(!overlaps(layout.tiles[first], layout.tiles[second]));
                                }
                            }

                            verify(approximatelyEqual(spectrum.x, innerLeft));
                            verify(approximatelyEqual(spectrum.y, innerTop));
                            verify(approximatelyEqual(peakRms.y, innerTop));
                            verify(approximatelyEqual(peakRms.right(), innerRight));
                            verify(approximatelyEqual(spectrogram.x, innerLeft));
                            verify(approximatelyEqual(spectrogram.bottom(), innerBottom));
                            verify(approximatelyEqual(stereo.bottom(), innerBottom));
                            verify(approximatelyEqual(loudness.right(), innerRight));
                            verify(approximatelyEqual(loudness.bottom(), innerBottom));

                            verify(approximatelyEqual(
                                peakRms.x - spectrum.right(), DashboardLayout::gutter));
                            verify(approximatelyEqual(
                                stereo.x - spectrogram.right(), DashboardLayout::gutter));
                            verify(approximatelyEqual(
                                loudness.x - stereo.right(), DashboardLayout::gutter));
                            verify(approximatelyEqual(
                                spectrogram.y - spectrum.bottom(), DashboardLayout::gutter));
                            verify(approximatelyEqual(
                                stereo.y - peakRms.bottom(), DashboardLayout::gutter));
                            verify(approximatelyEqual(
                                loudness.y - peakRms.bottom(), DashboardLayout::gutter));

                            verify(approximatelyEqual(
                                spectrum.right() + (DashboardLayout::gutter * 0.5),
                                innerLeft
                                    + (innerWidth * static_cast<double>(upper)
                                        / DashboardLayout::columnCount)));
                            verify(approximatelyEqual(
                                spectrogram.right() + (DashboardLayout::gutter * 0.5),
                                innerLeft
                                    + (innerWidth * static_cast<double>(lowerLeft)
                                        / DashboardLayout::columnCount)));
                            verify(
                                approximatelyEqual(stereo.right() + (DashboardLayout::gutter * 0.5),
                                    innerLeft
                                        + (innerWidth * static_cast<double>(lowerRight)
                                            / DashboardLayout::columnCount)));
                            verify(approximatelyEqual(
                                spectrum.bottom() + (DashboardLayout::gutter * 0.5),
                                innerTop
                                    + (innerHeight * static_cast<double>(horizontal)
                                        / DashboardLayout::rowCount)));
                        }
                    }
                }
            }

            expect(geometryMismatches == 0,
                juce::String("Geometry mismatches: ") + juce::String(geometryMismatches));
        }

        beginTest("Degenerate dashboard sizes remain contained and non-overlapping");
        {
            constexpr std::array dashboards {
                DashboardLogicalBounds { 5.0, 7.0, -10.0, -20.0 },
                DashboardLogicalBounds { 5.0, 7.0, 0.0, 0.0 },
                DashboardLogicalBounds { 5.0, 7.0, 1.0, 1.0 },
                DashboardLogicalBounds { 5.0, 7.0, 7.0, 7.0 },
                DashboardLogicalBounds { 5.0, 7.0, 8.0, 8.0 },
                DashboardLogicalBounds { 5.0, 7.0, 15.0, 15.0 },
                DashboardLogicalBounds { 5.0, 7.0, 16.0, 16.0 },
                DashboardLogicalBounds { 5.0, 7.0, 24.0, 24.0 },
            };

            for (const auto& dashboard : dashboards) {
                const auto safeWidth = std::max(0.0, dashboard.width);
                const auto safeHeight = std::max(0.0, dashboard.height);
                const auto horizontalInset = std::min(DashboardLayout::outerInset, safeWidth * 0.5);
                const auto verticalInset = std::min(DashboardLayout::outerInset, safeHeight * 0.5);
                const auto innerLeft = dashboard.x + horizontalInset;
                const auto innerRight = dashboard.x + safeWidth - horizontalInset;
                const auto innerTop = dashboard.y + verticalInset;
                const auto innerBottom = dashboard.y + safeHeight - verticalInset;
                const auto layout = DashboardLayout::calculateTileLayout(
                    dashboard, DashboardLayout::defaultSplits);

                for (const auto& tile : layout.tiles) {
                    expect(tile.width >= 0.0);
                    expect(tile.height >= 0.0);
                    expect(tile.x >= innerLeft - comparisonTolerance);
                    expect(tile.y >= innerTop - comparisonTolerance);
                    expect(tile.right() <= innerRight + comparisonTolerance);
                    expect(tile.bottom() <= innerBottom + comparisonTolerance);
                }

                for (std::size_t first = 0; first < layout.tiles.size(); ++first) {
                    for (auto second = first + 1; second < layout.tiles.size(); ++second) {
                        expect(!overlaps(layout.tiles[first], layout.tiles[second]));
                    }
                }
            }
        }

        beginTest("Default splitter geometry follows the normalized grid and Tab order");
        {
            constexpr DashboardLogicalBounds dashboard { 100.0, 50.0, 976.0, 816.0 };
            constexpr auto innerLeft = 108.0;
            constexpr auto innerTop = 58.0;
            constexpr auto innerWidth = 960.0;
            constexpr auto innerHeight = 800.0;
            constexpr auto rowSplit = innerTop + (22.0 * 20.0);
            const auto layout = DashboardLayout::calculateSplitterLayout(
                dashboard, DashboardLayout::defaultSplits);

            expectEquals(layout.splitters.size(), dashboardSplitterCount);
            for (std::size_t index = 0; index < dashboardSplitterCount; ++index)
                expect(layout[index].splitter == dashboardSplitterTabOrder[index]);

            const auto& horizontal = layout[0];
            expect(horizontal.axis == DashboardSplitterAxis::horizontal);
            expect(approximatelyEqual(horizontal.visualBounds,
                DashboardLogicalBounds { innerLeft, rowSplit - 1.0, innerWidth, 2.0 }));
            expect(approximatelyEqual(horizontal.pointerHitBounds,
                DashboardLogicalBounds { innerLeft, rowSplit - 12.0, innerWidth, 24.0 }));

            const auto& upper = layout[1];
            expect(upper.axis == DashboardSplitterAxis::vertical);
            expect(approximatelyEqual(upper.visualBounds,
                DashboardLogicalBounds {
                    innerLeft + (36.0 * 20.0) - 1.0, innerTop, 2.0, rowSplit - innerTop }));
            expect(approximatelyEqual(upper.pointerHitBounds,
                DashboardLogicalBounds {
                    innerLeft + (36.0 * 20.0) - 12.0, innerTop, 24.0, rowSplit - innerTop }));

            const auto& lowerLeft = layout[2];
            expect(lowerLeft.axis == DashboardSplitterAxis::vertical);
            expect(approximatelyEqual(lowerLeft.visualBounds,
                DashboardLogicalBounds { innerLeft + (28.0 * 20.0) - 1.0, rowSplit, 2.0,
                    innerTop + innerHeight - rowSplit }));
            expect(approximatelyEqual(lowerLeft.pointerHitBounds,
                DashboardLogicalBounds { innerLeft + (28.0 * 20.0) - 12.0, rowSplit, 24.0,
                    innerTop + innerHeight - rowSplit }));

            const auto& lowerRight = layout[3];
            expect(lowerRight.axis == DashboardSplitterAxis::vertical);
            expect(approximatelyEqual(lowerRight.visualBounds,
                DashboardLogicalBounds { innerLeft + (40.0 * 20.0) - 1.0, rowSplit, 2.0,
                    innerTop + innerHeight - rowSplit }));
            expect(approximatelyEqual(lowerRight.pointerHitBounds,
                DashboardLogicalBounds { innerLeft + (40.0 * 20.0) - 12.0, rowSplit, 24.0,
                    innerTop + innerHeight - rowSplit }));
        }

        beginTest("Every legal layout produces bounded splitter centres and hit bands");
        {
            constexpr DashboardLogicalBounds dashboard { 100.0, 50.0, 1200.0, 800.0 };
            constexpr auto innerLeft = dashboard.x + DashboardLayout::outerInset;
            constexpr auto innerTop = dashboard.y + DashboardLayout::outerInset;
            constexpr auto innerWidth = dashboard.width - (2.0 * DashboardLayout::outerInset);
            constexpr auto innerHeight = dashboard.height - (2.0 * DashboardLayout::outerInset);
            std::uint64_t geometryMismatches = 0;
            const auto verify = [&geometryMismatches](const bool condition) {
                geometryMismatches += condition ? 0U : 1U;
            };

            for (auto horizontal = 14; horizontal <= 26; ++horizontal) {
                for (auto upper = 24; upper <= 40; ++upper) {
                    for (auto lowerRight = 36; lowerRight <= 42; ++lowerRight) {
                        for (auto lowerLeft = 16; lowerLeft <= lowerRight - 8; ++lowerLeft) {
                            const DashboardLayoutSplits splits { horizontal, upper, lowerLeft,
                                lowerRight };
                            const auto layout
                                = DashboardLayout::calculateSplitterLayout(dashboard, splits);
                            const auto expectedRow = innerTop
                                + innerHeight * static_cast<double>(horizontal)
                                    / DashboardLayout::rowCount;
                            const std::array expectedColumns { upper, lowerLeft, lowerRight };

                            for (std::size_t index = 0; index < dashboardSplitterCount; ++index) {
                                verify(layout[index].splitter == dashboardSplitterTabOrder[index]);
                                verify(isFinite(layout[index].visualBounds));
                                verify(isFinite(layout[index].pointerHitBounds));
                            }

                            verify(
                                approximatelyEqual(centreY(layout[0].visualBounds), expectedRow));
                            verify(approximatelyEqual(layout[0].visualBounds.height,
                                DashboardLayout::splitterVisualThickness));
                            verify(approximatelyEqual(layout[0].pointerHitBounds.height,
                                DashboardLayout::minimumSplitterPointerHitThickness));
                            verify(layout[0].pointerHitBounds.height >= 24.0);
                            verify(approximatelyEqual(layout[0].visualBounds.x, innerLeft));
                            verify(approximatelyEqual(layout[0].visualBounds.width, innerWidth));

                            for (std::size_t verticalIndex = 0; verticalIndex < 3;
                                ++verticalIndex) {
                                const auto geometryIndex = verticalIndex + 1;
                                const auto expectedColumn = innerLeft
                                    + innerWidth
                                        * static_cast<double>(expectedColumns[verticalIndex])
                                        / DashboardLayout::columnCount;
                                verify(approximatelyEqual(
                                    centreX(layout[geometryIndex].visualBounds), expectedColumn));
                                verify(approximatelyEqual(layout[geometryIndex].visualBounds.width,
                                    DashboardLayout::splitterVisualThickness));
                                verify(
                                    approximatelyEqual(layout[geometryIndex].pointerHitBounds.width,
                                        DashboardLayout::minimumSplitterPointerHitThickness));
                                verify(layout[geometryIndex].pointerHitBounds.width >= 24.0);
                            }

                            verify(approximatelyEqual(layout[1].visualBounds.y, innerTop));
                            verify(
                                approximatelyEqual(layout[1].visualBounds.bottom(), expectedRow));
                            verify(approximatelyEqual(layout[2].visualBounds.y, expectedRow));
                            verify(approximatelyEqual(
                                layout[2].visualBounds.bottom(), innerTop + innerHeight));
                            verify(approximatelyEqual(layout[3].visualBounds.y, expectedRow));
                            verify(approximatelyEqual(
                                layout[3].visualBounds.bottom(), innerTop + innerHeight));
                        }
                    }
                }
            }

            expect(geometryMismatches == 0,
                juce::String("Splitter geometry mismatches: ") + juce::String(geometryMismatches));
        }

        beginTest("Splitter hit testing uses the accepted overlap priority");
        {
            constexpr DashboardLogicalBounds dashboard { 100.0, 50.0, 976.0, 816.0 };
            constexpr auto innerLeft = 108.0;
            constexpr auto innerTop = 58.0;
            constexpr auto rowSplit = innerTop + (22.0 * 20.0);
            constexpr auto upperSplit = innerLeft + (36.0 * 20.0);
            constexpr auto lowerLeftSplit = innerLeft + (28.0 * 20.0);
            constexpr auto lowerRightSplit = innerLeft + (40.0 * 20.0);
            const auto layout = DashboardLayout::calculateSplitterLayout(
                dashboard, DashboardLayout::defaultSplits);

            const auto expectHit = [this, &layout](const DashboardLogicalPoint point,
                                       const DashboardSplitter expected) {
                const auto hit = DashboardLayout::hitTestSplitter(layout, point);
                expect(hit.has_value());
                if (hit.has_value())
                    expect(*hit == expected);
            };

            expectHit({ innerLeft + 40.0, rowSplit }, DashboardSplitter::horizontal);
            expectHit({ upperSplit, innerTop + 40.0 }, DashboardSplitter::upper);
            expectHit({ lowerLeftSplit, rowSplit + 40.0 }, DashboardSplitter::lowerLeft);
            expectHit({ lowerRightSplit, rowSplit + 40.0 }, DashboardSplitter::lowerRight);
            expectHit({ upperSplit, rowSplit }, DashboardSplitter::horizontal);
            expectHit({ lowerLeftSplit, rowSplit }, DashboardSplitter::horizontal);
            expectHit({ lowerRightSplit, rowSplit }, DashboardSplitter::horizontal);
            expect(!DashboardLayout::hitTestSplitter(layout, { 0.0, 0.0 }).has_value());
        }

        beginTest("Pointer positions snap to the nearest top-origin grid boundary");
        {
            constexpr DashboardLogicalBounds dashboard { 100.0, 50.0, 976.0, 816.0 };
            constexpr auto innerLeft = 108.0;
            constexpr auto innerTop = 58.0;
            constexpr auto trackSize = 20.0;

            for (auto column = 0; column <= DashboardLayout::columnCount; ++column) {
                const auto mapped = DashboardLayout::nearestGridIndexForPointer(dashboard,
                    DashboardSplitter::upper,
                    { innerLeft + (static_cast<double>(column) * trackSize), innerTop });
                expect(mapped.has_value());
                if (mapped.has_value())
                    expectEquals(*mapped, column);
            }

            for (auto row = 0; row <= DashboardLayout::rowCount; ++row) {
                const auto mapped = DashboardLayout::nearestGridIndexForPointer(dashboard,
                    DashboardSplitter::horizontal,
                    { innerLeft, innerTop + (static_cast<double>(row) * trackSize) });
                expect(mapped.has_value());
                if (mapped.has_value())
                    expectEquals(*mapped, row);
            }

            const auto beforeHalfColumn = DashboardLayout::nearestGridIndexForPointer(
                dashboard, DashboardSplitter::upper, { innerLeft + 9.999, innerTop });
            const auto atHalfColumn = DashboardLayout::nearestGridIndexForPointer(
                dashboard, DashboardSplitter::upper, { innerLeft + 10.0, innerTop });
            const auto beforeHalfRow = DashboardLayout::nearestGridIndexForPointer(
                dashboard, DashboardSplitter::horizontal, { innerLeft, innerTop + 9.999 });
            const auto atHalfRow = DashboardLayout::nearestGridIndexForPointer(
                dashboard, DashboardSplitter::horizontal, { innerLeft, innerTop + 10.0 });
            expect(beforeHalfColumn == std::optional<int> { 0 });
            expect(atHalfColumn == std::optional<int> { 1 });
            expect(beforeHalfRow == std::optional<int> { 0 });
            expect(atHalfRow == std::optional<int> { 1 });
        }

        beginTest("Pointer movement saturates, snaps, and applies legal splitter clamps");
        {
            constexpr DashboardLogicalBounds dashboard { 100.0, 50.0, 976.0, 816.0 };
            constexpr auto lowest = std::numeric_limits<double>::lowest();
            constexpr auto highest = std::numeric_limits<double>::max();
            constexpr auto initial = DashboardLayout::defaultSplits;

            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::horizontal, dashboard, { 100.0, lowest })
                == DashboardLayoutSplits { 14, 36, 28, 40 });
            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::horizontal, dashboard, { 100.0, highest })
                == DashboardLayoutSplits { 26, 36, 28, 40 });
            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::upper, dashboard, { lowest, 100.0 })
                == DashboardLayoutSplits { 22, 24, 28, 40 });
            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::upper, dashboard, { highest, 100.0 })
                == DashboardLayoutSplits { 22, 40, 28, 40 });
            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::lowerLeft, dashboard, { highest, 100.0 })
                == DashboardLayoutSplits { 22, 36, 32, 40 });
            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::lowerRight, dashboard, { lowest, 100.0 })
                == DashboardLayoutSplits { 22, 36, 28, 36 });
            expect(DashboardLayout::moveSplitterToPointer(
                       initial, DashboardSplitter::lowerRight, dashboard, { highest, 100.0 })
                == DashboardLayoutSplits { 22, 36, 28, 42 });

            constexpr DashboardLayoutSplits orderedLowerRow { 22, 36, 32, 42 };
            expect(DashboardLayout::moveSplitterToPointer(
                       orderedLowerRow, DashboardSplitter::lowerRight, dashboard, { lowest, 100.0 })
                == DashboardLayoutSplits { 22, 36, 32, 40 });
        }

        beginTest("Degenerate geometry and non-finite pointers fail inertly");
        {
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            const auto infinity = std::numeric_limits<double>::infinity();
            constexpr std::array degenerateDashboards {
                DashboardLogicalBounds { 5.0, 7.0, -10.0, -20.0 },
                DashboardLogicalBounds { 5.0, 7.0, 0.0, 0.0 },
                DashboardLogicalBounds { 5.0, 7.0, 1.0, 1.0 },
                DashboardLogicalBounds { 5.0, 7.0, 16.0, 16.0 },
                DashboardLogicalBounds { 5.0, 7.0, 100.0, 16.0 },
                DashboardLogicalBounds { 5.0, 7.0, 16.0, 100.0 },
            };

            for (const auto dashboard : degenerateDashboards) {
                const auto layout = DashboardLayout::calculateSplitterLayout(
                    dashboard, DashboardLayout::defaultSplits);
                for (const auto& geometry : layout.splitters) {
                    expect(isFinite(geometry.visualBounds));
                    expect(isFinite(geometry.pointerHitBounds));
                    expect(geometry.visualBounds.width >= 0.0);
                    expect(geometry.visualBounds.height >= 0.0);
                    expect(geometry.pointerHitBounds.width >= 0.0);
                    expect(geometry.pointerHitBounds.height >= 0.0);
                }

                expect(!DashboardLayout::hitTestSplitter(layout, { 5.0, 7.0 }).has_value());
                for (const auto splitter : dashboardSplitters) {
                    expect(!DashboardLayout::nearestGridIndexForPointer(
                        dashboard, splitter, { 5.0, 7.0 })
                            .has_value());
                    expect(DashboardLayout::moveSplitterToPointer(
                               DashboardLayout::defaultSplits, splitter, dashboard, { 5.0, 7.0 })
                        == DashboardLayout::defaultSplits);
                }
            }

            const std::array invalidDashboards {
                DashboardLogicalBounds { nan, 7.0, 100.0, 100.0 },
                DashboardLogicalBounds { 5.0, nan, 100.0, 100.0 },
                DashboardLogicalBounds { 5.0, 7.0, infinity, 100.0 },
                DashboardLogicalBounds { 5.0, 7.0, 100.0, -infinity },
            };
            for (const auto dashboard : invalidDashboards) {
                const auto layout = DashboardLayout::calculateSplitterLayout(
                    dashboard, DashboardLayout::defaultSplits);
                for (const auto& geometry : layout.splitters) {
                    expect(isFinite(geometry.visualBounds));
                    expect(isFinite(geometry.pointerHitBounds));
                }
                expect(!DashboardLayout::hitTestSplitter(layout, { 0.0, 0.0 }).has_value());
                expect(!DashboardLayout::nearestGridIndexForPointer(
                    dashboard, DashboardSplitter::upper, { 0.0, 0.0 })
                        .has_value());
            }

            constexpr DashboardLogicalBounds validDashboard { 100.0, 50.0, 976.0, 816.0 };
            const auto layout = DashboardLayout::calculateSplitterLayout(
                validDashboard, DashboardLayout::defaultSplits);
            for (const auto invalidCoordinate : { nan, infinity, -infinity }) {
                expect(!DashboardLayout::hitTestSplitter(layout, { invalidCoordinate, 100.0 })
                        .has_value());
                expect(!DashboardLayout::hitTestSplitter(layout, { 100.0, invalidCoordinate })
                        .has_value());
                for (const auto splitter : dashboardSplitters) {
                    expect(!DashboardLayout::nearestGridIndexForPointer(
                        validDashboard, splitter, { invalidCoordinate, 100.0 })
                            .has_value());
                    expect(!DashboardLayout::nearestGridIndexForPointer(
                        validDashboard, splitter, { 100.0, invalidCoordinate })
                            .has_value());
                }
            }

            constexpr DashboardLayoutSplits invalidSplits { 0, 0, 0, 0 };
            expect(DashboardLayout::moveSplitterToPointer(
                       invalidSplits, DashboardSplitter::upper, validDashboard, { nan, 100.0 })
                == DashboardLayout::defaultSplits);
        }

        beginTest("Accessibility values name both affected regions and exact percentages");
        {
            const auto horizontal = DashboardLayout::accessibilityValue(
                DashboardLayout::defaultSplits, DashboardSplitter::horizontal);
            expect(horizontal.name == "Upper and lower dashboard height");
            expect(horizontal.firstRegionName == "Upper band");
            expect(horizontal.secondRegionName == "Lower band");
            expectEquals(horizontal.firstRegionTracks, 22);
            expectEquals(horizontal.secondRegionTracks, 18);
            expectEquals(horizontal.totalTracks, 40);
            expect(approximatelyEqual(horizontal.firstRegionPercentage, 55.0));
            expect(approximatelyEqual(horizontal.secondRegionPercentage, 45.0));

            const auto upper = DashboardLayout::accessibilityValue(
                DashboardLayout::defaultSplits, DashboardSplitter::upper);
            expect(upper.name == "Spectrum and Peak/RMS width");
            expect(upper.firstRegionName == "Spectrum");
            expect(upper.secondRegionName == "Peak/RMS");
            expectEquals(upper.firstRegionTracks, 36);
            expectEquals(upper.secondRegionTracks, 12);
            expect(approximatelyEqual(upper.firstRegionPercentage, 75.0));
            expect(approximatelyEqual(upper.secondRegionPercentage, 25.0));

            const auto lowerLeft = DashboardLayout::accessibilityValue(
                DashboardLayout::defaultSplits, DashboardSplitter::lowerLeft);
            expect(lowerLeft.name == "Spectrogram and Stereo width");
            expect(lowerLeft.firstRegionName == "Spectrogram");
            expect(lowerLeft.secondRegionName == "Stereo");
            expectEquals(lowerLeft.firstRegionTracks, 28);
            expectEquals(lowerLeft.secondRegionTracks, 12);
            expect(approximatelyEqual(lowerLeft.firstRegionPercentage, 100.0 * 28.0 / 48.0));
            expect(approximatelyEqual(lowerLeft.secondRegionPercentage, 25.0));

            const auto lowerRight = DashboardLayout::accessibilityValue(
                DashboardLayout::defaultSplits, DashboardSplitter::lowerRight);
            expect(lowerRight.name == "Stereo and Loudness width");
            expect(lowerRight.firstRegionName == "Stereo");
            expect(lowerRight.secondRegionName == "Loudness");
            expectEquals(lowerRight.firstRegionTracks, 12);
            expectEquals(lowerRight.secondRegionTracks, 8);
            expect(approximatelyEqual(lowerRight.firstRegionPercentage, 25.0));
            expect(approximatelyEqual(lowerRight.secondRegionPercentage, 100.0 / 6.0));

            std::uint64_t percentageMismatches = 0;
            for (auto horizontalSplit = 14; horizontalSplit <= 26; ++horizontalSplit) {
                for (auto upperSplit = 24; upperSplit <= 40; ++upperSplit) {
                    for (auto lowerRightSplit = 36; lowerRightSplit <= 42; ++lowerRightSplit) {
                        for (auto lowerLeftSplit = 16; lowerLeftSplit <= lowerRightSplit - 8;
                            ++lowerLeftSplit) {
                            const DashboardLayoutSplits splits { horizontalSplit, upperSplit,
                                lowerLeftSplit, lowerRightSplit };
                            for (const auto splitter : dashboardSplitters) {
                                const auto value
                                    = DashboardLayout::accessibilityValue(splits, splitter);
                                percentageMismatches
                                    += !approximatelyEqual(value.firstRegionPercentage,
                                        100.0 * static_cast<double>(value.firstRegionTracks)
                                            / static_cast<double>(value.totalTracks));
                                percentageMismatches
                                    += !approximatelyEqual(value.secondRegionPercentage,
                                        100.0 * static_cast<double>(value.secondRegionTracks)
                                            / static_cast<double>(value.totalTracks));
                                percentageMismatches += value.name.empty();
                                percentageMismatches += value.firstRegionName.empty();
                                percentageMismatches += value.secondRegionName.empty();
                            }
                        }
                    }
                }
            }
            expect(percentageMismatches == 0,
                juce::String("Accessibility value mismatches: ")
                    + juce::String(percentageMismatches));
        }
    }
};

DashboardLayoutTests dashboardLayoutTests;
} // namespace
} // namespace audio_insight
