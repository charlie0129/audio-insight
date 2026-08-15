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
    }
};

DashboardLayoutTests dashboardLayoutTests;
} // namespace
} // namespace audio_insight
