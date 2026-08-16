// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/MetalVisualization.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace audio_insight {
namespace {
static_assert(detail::MetalVisualizationGeometryLimits::maximumGeneratedVertices
    <= detail::MetalVisualizationGeometryLimits::vertexCapacity);
static_assert(detail::maximumFrequencyAxisLabelGlyphs >= 10);
static_assert(detail::MetalVisualizationGeometryLimits::maximumDecibelLabelGlyphs >= 7);
static_assert(detail::maximumPeakRmsReadoutGlyphs >= 6);
static_assert(detail::maximumStereoCorrelationReadoutGlyphs >= 5);
static_assert(detail::maximumLoudnessReadoutGlyphs >= 6);

class StubVisualizationDataSource final : public VisualizationDataSource {
public:
    void requestAnalysis() noexcept override
    {
    }

    void setVisualizationActive(bool) noexcept override
    {
    }

    void resetSpectrum() noexcept override
    {
    }

    void resetPeakRms() noexcept override
    {
    }

    void resetLoudness() noexcept override
    {
        ++loudnessResetCount;
    }

    void discardPendingSpectrogramColumns() noexcept override
    {
        ++spectrogramDiscardCount;
        pendingSpectrogramColumns = 0;
    }

    [[nodiscard]] bool copyLatestVisualizationFrame(VisualizationFrame&) const noexcept override
    {
        return false;
    }

    int loudnessResetCount = 0;
    int spectrogramDiscardCount = 0;
    int pendingSpectrogramColumns = 0;
};

class MetalVisualizationTests final : public juce::UnitTest {
public:
    MetalVisualizationTests() : UnitTest("Metal visualization", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Frequency mapping shares linear, intermediate, and logarithmic coordinates");

        const detail::FrequencyAxisMapping logarithmicMapping { 20.0F, 20'000.0F, 1.0F };
        const detail::FrequencyAxisMapping linearMapping { 20.0F, 20'000.0F, 0.0F };
        const detail::FrequencyAxisMapping intermediateMapping { 20.0F, 20'000.0F, 0.5F };
        expectWithinAbsoluteError(
            detail::mapFrequencyToUnit(linearMapping, 1'000.0F), 0.049049049F, 0.000001F);
        expectWithinAbsoluteError(
            detail::mapFrequencyToUnit(intermediateMapping, 1'000.0F), 0.307686192F, 0.000001F);
        expectWithinAbsoluteError(
            detail::mapFrequencyToUnit(logarithmicMapping, 1'000.0F), 0.566323335F, 0.000001F);
        expectEquals(detail::mapFrequencyToUnit(logarithmicMapping, 20.0F), 0.0F);
        expectEquals(detail::mapFrequencyToUnit(logarithmicMapping, 20'000.0F), 1.0F);
        expectEquals(detail::mapFrequencyToUnit(logarithmicMapping, 1.0F), 0.0F);
        expectEquals(detail::mapFrequencyToUnit(logarithmicMapping, 40'000.0F), 1.0F);

        const detail::FrequencyAxisMapping lowNyquistMapping { 20.0F, 11'025.0F, 0.65F };
        expectEquals(detail::mapFrequencyToUnit(lowNyquistMapping, 20.0F), 0.0F);
        expectEquals(detail::mapFrequencyToUnit(lowNyquistMapping, 11'025.0F), 1.0F);
        auto previousPosition = 0.0F;
        for (auto frequency = 20.0F; frequency <= lowNyquistMapping.maximumFrequencyHz;
            frequency += 37.0F) {
            const auto position = detail::mapFrequencyToUnit(lowNyquistMapping, frequency);
            expect(position >= previousPosition);
            previousPosition = position;
        }

        expectEquals(detail::mapFrequencyToUnit({ 20.0F, 20.0F, 1.0F }, 20.0F), 0.0F);

        beginTest("Spectrogram history dimensions and timestamps are bounded and audio-derived");

        expect(detail::calculateSpectrogramHistoryColumnCount(10, 60) == 600);
        expect(detail::calculateSpectrogramHistoryColumnCount(60, 120) == 7'200);
        expect(detail::calculateSpectrogramHistoryColumnCount(3, 60) == 0);
        expect(detail::calculateSpectrogramHistoryColumnCount(10, 90) == 0);
        expect(detail::calculateSpectrogramTimelineSlot(4'096, 48'000.0, 60) == 5);
        expect(detail::calculateSpectrogramTimelineSlot(4'896, 48'000.0, 60) == 6);
        expect(detail::calculateSpectrogramTimelineSlot(0, 48'000.0, 60) == 0);
        expect(detail::calculateSpectrogramTimelineSlot(
                   1, std::numeric_limits<double>::quiet_NaN(), 60)
            == 0);

        beginTest("Spectrogram presentation time keeps one continuous target domain");

        detail::SpectrogramPresentationTimebase presentationTimebase;
        expect(std::isnan(presentationTimebase.presentationTime(
            9.99, 10.0, std::numeric_limits<double>::quiet_NaN())));
        expectWithinAbsoluteError(
            presentationTimebase.presentationTime(9.99, 10.0, 10.008), 10.008, 1.0e-12);
        expectWithinAbsoluteError(presentationTimebase.presentationTime(
                                      9.998, 10.008, std::numeric_limits<double>::quiet_NaN()),
            10.016, 1.0e-12);
        expectWithinAbsoluteError(presentationTimebase.presentationTime(
                                      10.006, 0.0, std::numeric_limits<double>::quiet_NaN()),
            10.024, 1.0e-12);
        presentationTimebase.reset();
        expect(std::isnan(presentationTimebase.presentationTime(
            10.0, 0.0, std::numeric_limits<double>::quiet_NaN())));

        beginTest("Spectrogram Scroll clock advances between whole FFT columns");

        detail::SpectrogramScrollClock scrollClock;
        const auto anchored = scrollClock.update(100, 60, 10.0, 600);
        expect(anchored.valid);
        expect(anchored.initializedThisUpdate);
        expectEquals(anchored.headOffsetColumns, -1.0F);
        expect(!anchored.underrun);

        const auto halfColumn = scrollClock.update(100, 60, 10.0 + (1.0 / 120.0), 600);
        expect(halfColumn.valid);
        expect(!halfColumn.initializedThisUpdate);
        expectWithinAbsoluteError(halfColumn.headOffsetColumns, -0.5F, 0.00001F);
        expect(!halfColumn.underrun);

        const auto wholeColumn = scrollClock.update(100, 60, 10.0 + (1.0 / 60.0), 600);
        expectWithinAbsoluteError(wholeColumn.headOffsetColumns, 0.0F, 0.00001F);
        expect(!wholeColumn.underrun);

        const auto ordinaryArrival = scrollClock.update(101, 60, 10.0 + (1.0 / 60.0), 600);
        expect(!ordinaryArrival.initializedThisUpdate);
        expectWithinAbsoluteError(ordinaryArrival.headOffsetColumns, -1.0F, 0.00001F);

        const auto burstArrival = scrollClock.update(108, 60, 10.0 + (1.0 / 60.0), 600);
        expect(!burstArrival.initializedThisUpdate);
        expectWithinAbsoluteError(burstArrival.headOffsetColumns, -8.0F, 0.00001F);
        const auto afterBurstHalfColumn = scrollClock.update(108, 60, 10.0 + (3.0 / 120.0), 600);
        expectWithinAbsoluteError(afterBurstHalfColumn.headOffsetColumns, -7.5F, 0.00001F);

        const auto regressedTimestamp = scrollClock.update(108, 60, 9.0, 600);
        expectWithinAbsoluteError(regressedTimestamp.headOffsetColumns, -7.5F, 0.00001F);
        const auto saturatedFuture = scrollClock.update(108, 60, 100.0, 600);
        expectEquals(saturatedFuture.headOffsetColumns, 1.0F);
        expect(saturatedFuture.underrun);
        const auto resumedAfterStall = scrollClock.update(109, 60, 100.0 + (1.0 / 60.0), 600);
        expectEquals(resumedAfterStall.headOffsetColumns, -1.0F);
        expect(resumedAfterStall.initializedThisUpdate);
        expect(!resumedAfterStall.underrun);

        detail::SpectrogramScrollClock replacedHistoryClock;
        static_cast<void>(replacedHistoryClock.update(100, 60, 10.0, 5));
        const auto replacedHistory = replacedHistoryClock.update(105, 60, 20.0, 5);
        expectEquals(replacedHistory.headOffsetColumns, -1.0F);
        expect(replacedHistory.initializedThisUpdate);

        detail::SpectrogramScrollClock expiredHeadClock;
        static_cast<void>(expiredHeadClock.update(100, 60, 10.0, 5));
        const auto expiredHead = expiredHeadClock.update(104, 60, 10.0, 5);
        expectEquals(expiredHead.headOffsetColumns, -1.0F);
        expect(expiredHead.initializedThisUpdate);

        detail::SpectrogramScrollClock invalidTimestampClock;
        const auto invalidTimestampAnchor
            = invalidTimestampClock.update(100, 60, std::numeric_limits<double>::quiet_NaN(), 600);
        expectEquals(invalidTimestampAnchor.headOffsetColumns, -1.0F);
        const auto firstValidTimestamp = invalidTimestampClock.update(100, 60, 10.0, 600);
        expectEquals(firstValidTimestamp.headOffsetColumns, -1.0F);

        scrollClock.reset();
        expect(!scrollClock.update(0, 60, 10.0, 600).valid);
        expect(!scrollClock.update(100, 0, 10.0, 600).valid);
        expect(!scrollClock.update(100, 60, 10.0, 0).valid);

        beginTest("Fractional Spectrogram mapping keeps expired and future cells black");

        expect(detail::spectrogramScrollSourceColumn(0.0F, 5, 0.0F) == 0);
        expect(detail::spectrogramScrollSourceColumn(1.0F, 5, 0.0F) == 4);
        expect(!detail::spectrogramScrollSourceColumn(0.05F, 5, -0.5F).has_value());
        expect(detail::spectrogramScrollSourceColumn(0.99F, 5, -0.5F) == 4);
        expect(detail::spectrogramScrollSourceColumn(0.0F, 5, 0.5F) == 0);
        expect(!detail::spectrogramScrollSourceColumn(0.99F, 5, 0.5F).has_value());
        expect(
            !detail::spectrogramScrollSourceColumn(std::numeric_limits<float>::quiet_NaN(), 5, 0.0F)
                .has_value());

        beginTest("Spectrogram ring preserves timestamp gaps and both history interpretations");

        detail::SpectrogramHistoryRing ring;
        ring.configure(5);
        const auto firstColumn = ring.append(10, 1);
        expect(firstColumn.accepted);
        expect(firstColumn.writeColumn == 0);
        expect(ring.timelineSpan() == 1);
        expect(!ring.physicalColumnForScreenColumn(3, detail::SpectrogramRenderHistoryMode::scroll)
                .has_value());
        expect(*ring.physicalColumnForScreenColumn(4, detail::SpectrogramRenderHistoryMode::scroll)
            == 0);

        const auto replacementColumn = ring.append(10, 2);
        expect(replacementColumn.accepted);
        expect(replacementColumn.writeColumn == 0);
        expect(replacementColumn.gapColumnCount == 0);
        expect(ring.nextWriteColumn() == 1);
        expect(ring.timelineSpan() == 1);

        detail::SpectrogramHistoryRing coalescedSameSlotRing;
        coalescedSameSlotRing.configure(5);
        expect(coalescedSameSlotRing.append(10, 1).accepted);
        const auto adjacentAfterDroppedSameSlot = coalescedSameSlotRing.append(11, 3);
        expect(adjacentAfterDroppedSameSlot.accepted);
        expect(adjacentAfterDroppedSameSlot.gapColumnCount == 0);
        expect(coalescedSameSlotRing.timelineSpan() == 2);

        const auto adjacentColumn = ring.append(11, 3);
        expect(adjacentColumn.accepted);
        expect(adjacentColumn.gapColumnCount == 0);
        const auto gappedColumn = ring.append(14, 5);
        expect(gappedColumn.accepted);
        expect(gappedColumn.gapColumnCount == 2);
        expect(gappedColumn.writeColumn == 4);
        expect(ring.timelineSpan() == 5);
        expect(ring.isColumnValid(0));
        expect(ring.isColumnValid(1));
        expect(!ring.isColumnValid(2));
        expect(!ring.isColumnValid(3));
        expect(ring.isColumnValid(4));
        for (std::uint32_t screen = 0; screen < 5; ++screen) {
            expect(*ring.physicalColumnForScreenColumn(
                       screen, detail::SpectrogramRenderHistoryMode::scroll)
                == screen);
            expect(*ring.physicalColumnForScreenColumn(
                       screen, detail::SpectrogramRenderHistoryMode::overwrite)
                == screen);
        }

        const auto staleColumn = ring.append(14, 5);
        expect(!staleColumn.accepted);
        const auto wrappedColumn = ring.append(15, 6);
        expect(wrappedColumn.accepted);
        expect(wrappedColumn.writeColumn == 0);
        expect(ring.nextWriteColumn() == 1);
        expect(*ring.physicalColumnForScreenColumn(4, detail::SpectrogramRenderHistoryMode::scroll)
            == 0);

        const auto hugeGap = ring.append(30, 21);
        expect(hugeGap.accepted);
        expect(hugeGap.discardedPreviousSpan);
        expect(hugeGap.gapColumnCount == 14);
        expect(ring.timelineSpan() == 5);
        auto validAfterHugeGap = 0;
        for (std::uint32_t column = 0; column < 5; ++column)
            validAfterHugeGap += ring.isColumnValid(column) ? 1 : 0;
        expectEquals(validAfterHugeGap, 1);

        ring.configure(3);
        expect(ring.columnCount() == 3);
        expect(ring.timelineSpan() == 0);
        expect(!ring.physicalColumnForScreenColumn(0, detail::SpectrogramRenderHistoryMode::scroll)
                .has_value());

        beginTest("Pending Spectrogram destinations stay black until upload promotion");

        detail::SpectrogramHistoryRing validityRing;
        validityRing.configure(5);
        for (std::uint64_t slot = 1; slot <= 5; ++slot)
            expect(validityRing.append(slot, slot).accepted);

        std::array<std::uint8_t, detail::maximumSpectrogramHistoryColumnCount> renderValidity { };
        renderValidity.fill(0x7f);
        const std::array<std::uint32_t, detail::maximumSpectrogramColumnsDrainedPerFrame>
            maskedColumns { 0, 4, 4, 99 };
        detail::copySpectrogramRenderValidity(validityRing, renderValidity.data(),
            renderValidity.size(), maskedColumns.data(), 4, false);
        expect(renderValidity[0] == 0);
        expect(renderValidity[1] == 1);
        expect(renderValidity[3] == 1);
        expect(renderValidity[4] == 0);
        detail::copySpectrogramRenderValidity(
            validityRing, renderValidity.data(), renderValidity.size(), nullptr, 0, true);
        expect(std::all_of(renderValidity.begin(), renderValidity.end(),
            [](const auto value) { return value == 0; }));

        beginTest("Spectrogram upload settlement promotes only matching revisions");

        detail::PendingSpectrogramUpload pendingUpload;
        pendingUpload.begin(7, 3, 4, maskedColumns.data(), 4);
        expect(pendingUpload.isActive());
        expect(pendingUpload.destinationCount() == 4);
        expect(pendingUpload.destinations()[0] == 0);
        expect(pendingUpload.resolve(detail::SpectrogramUploadCompletion { 7, true }, 3, 4)
            == detail::SpectrogramUploadResolution::promote);
        expect(!pendingUpload.isActive());

        pendingUpload.begin(8, 3, 4, maskedColumns.data(), 2);
        expect(pendingUpload.resolve(detail::SpectrogramUploadCompletion { 8, false }, 3, 4)
            == detail::SpectrogramUploadResolution::clearHistory);
        pendingUpload.begin(9, 3, 4, maskedColumns.data(), 2);
        expect(pendingUpload.resolve(detail::SpectrogramUploadCompletion { 9, false }, 5, 4)
            == detail::SpectrogramUploadResolution::stale);
        pendingUpload.begin(10, 3, 4, maskedColumns.data(), 2);
        expect(pendingUpload.resolve(detail::SpectrogramUploadCompletion { 11, true }, 3, 4)
            == detail::SpectrogramUploadResolution::clearHistory);

        beginTest("Spectrogram upload gate publishes completion before admitting another callback");

        detail::SpectrogramUploadGate uploadGate;
        expect(!uploadGate.isUploadInFlight());
        expect(!uploadGate.consumeCompletion().has_value());

        uploadGate.beginUpload(1);
        expect(uploadGate.isUploadInFlight());
        expect(!uploadGate.consumeCompletion().has_value());
        uploadGate.completeUpload(1, true);
        expect(!uploadGate.isUploadInFlight());
        const auto success = uploadGate.consumeCompletion();
        expect(success.has_value());
        expect(success.has_value() && success->transaction == 1 && success->succeeded);
        expect(!uploadGate.consumeCompletion().has_value());

        uploadGate.beginUpload(2);
        expect(uploadGate.isUploadInFlight());
        uploadGate.completeUpload(2, false);
        expect(!uploadGate.isUploadInFlight());
        const auto failure = uploadGate.consumeCompletion();
        expect(failure.has_value());
        expect(failure.has_value() && failure->transaction == 2 && !failure->succeeded);
        expect(!uploadGate.consumeCompletion().has_value());

        beginTest("Spectrogram invalidation scopes distinguish clears from reallocations");

        const detail::SpectrogramHistorySignature baseSignature { 1, 2, 3, 4, 4096, 1024, 600, 60,
            48'000.0 };
        const auto initialTransition
            = detail::spectrogramHistoryTransition(std::nullopt, baseSignature);
        expect(initialTransition.clear);
        expect(initialTransition.reallocate);
        const auto unchangedTransition
            = detail::spectrogramHistoryTransition(baseSignature, baseSignature);
        expect(!unchangedTransition.clear);
        expect(!unchangedTransition.reallocate);

        auto mappingChanged = baseSignature;
        ++mappingChanged.mappingGeneration;
        const auto mappingTransition
            = detail::spectrogramHistoryTransition(baseSignature, mappingChanged);
        expect(mappingTransition.clear);
        expect(!mappingTransition.reallocate);

        auto windowChanged = baseSignature;
        ++windowChanged.fftGeneration;
        const auto windowTransition
            = detail::spectrogramHistoryTransition(baseSignature, windowChanged);
        expect(windowTransition.clear);
        expect(!windowTransition.reallocate);

        auto rowsChanged = baseSignature;
        rowsChanged.rowCount = 426;
        const auto rowTransition = detail::spectrogramHistoryTransition(baseSignature, rowsChanged);
        expect(rowTransition.clear);
        expect(rowTransition.reallocate);

        auto durationChanged = baseSignature;
        durationChanged.columnCount = 1'200;
        const auto durationTransition
            = detail::spectrogramHistoryTransition(baseSignature, durationChanged);
        expect(durationTransition.clear);
        expect(durationTransition.reallocate);

        auto resetChanged = baseSignature;
        ++resetChanged.resetEpoch;
        const auto resetTransition
            = detail::spectrogramHistoryTransition(baseSignature, resetChanged);
        expect(resetTransition.clear);
        expect(!resetTransition.reallocate);

        auto sliceRateChanged = baseSignature;
        sliceRateChanged.requestedSliceRateHz = 120;
        sliceRateChanged.columnCount = 1'200;
        const auto sliceRateTransition
            = detail::spectrogramHistoryTransition(baseSignature, sliceRateChanged);
        expect(sliceRateTransition.clear);
        expect(sliceRateTransition.reallocate);

        auto invalidSignature = baseSignature;
        invalidSignature.rowCount = 0;
        const auto invalidTransition
            = detail::spectrogramHistoryTransition(baseSignature, invalidSignature);
        expect(!invalidTransition.clear);
        expect(!invalidTransition.reallocate);

        beginTest("Spectrogram response and palettes preserve literal black and luminance order");

        expectEquals(detail::spectrogramPaletteCoordinate(-120.0F, -120.0F, 0.0F, 0.0F), 0.0F);
        expectEquals(detail::spectrogramPaletteCoordinate(0.0F, -120.0F, 0.0F, 0.0F), 1.0F);
        expectWithinAbsoluteError(
            detail::spectrogramPaletteCoordinate(-60.0F, -120.0F, 0.0F, 0.0F), 0.5F, 0.000001F);
        expect(detail::spectrogramPaletteCoordinate(-90.0F, -120.0F, 0.0F, -2.0F)
            > detail::spectrogramPaletteCoordinate(-90.0F, -120.0F, 0.0F, 2.0F));
        expectEquals(detail::spectrogramPaletteCoordinate(
                         std::numeric_limits<float>::quiet_NaN(), -120.0F, 0.0F, 0.0F),
            0.0F);

        constexpr std::array palettes { detail::SpectrogramRenderPalette::blueFire,
            detail::SpectrogramRenderPalette::inferno, detail::SpectrogramRenderPalette::viridis,
            detail::SpectrogramRenderPalette::grayscale };
        for (const auto palette : palettes) {
            const auto black = detail::spectrogramPaletteColour(palette, 0.0F);
            expectEquals(black.red, 0.0F);
            expectEquals(black.green, 0.0F);
            expectEquals(black.blue, 0.0F);
            auto previousLuminance = 0.0F;
            auto previousQuantizedLuminance = 0.0F;
            for (auto index = 0; index <= 256; ++index) {
                const auto colour
                    = detail::spectrogramPaletteColour(palette, static_cast<float>(index) / 256.0F);
                const auto luminance = detail::spectrogramPerceivedLuminance(colour);
                expect(luminance + 0.000001F >= previousLuminance);
                previousLuminance = luminance;

                if (index < 256) {
                    const auto quantize = [](const float component) noexcept {
                        return std::round(std::clamp(component, 0.0F, 1.0F) * 255.0F) / 255.0F;
                    };
                    const auto lutColour = detail::spectrogramPaletteColour(
                        palette, static_cast<float>(index) / 255.0F);
                    const detail::SpectrogramPaletteColour quantizedColour {
                        quantize(lutColour.red),
                        quantize(lutColour.green),
                        quantize(lutColour.blue),
                    };
                    const auto quantizedLuminance
                        = detail::spectrogramPerceivedLuminance(quantizedColour);
                    expect(quantizedLuminance + 0.000001F >= previousQuantizedLuminance,
                        "Quantized palette " + juce::String(static_cast<int>(palette))
                            + " at LUT index " + juce::String(index));
                    previousQuantizedLuminance = quantizedLuminance;
                }
            }
        }

        expectEquals(detail::spectrogramFrequencyCoordinate(1.0F), 0.0F);
        expectEquals(detail::spectrogramFrequencyCoordinate(0.0F), 1.0F);
        expectWithinAbsoluteError(detail::spectrogramLogicalPixelWidth(1.0F), 1.0F, 0.000001F);
        expectWithinAbsoluteError(
            detail::spectrogramLogicalPixelWidth(1.5F), 2.0F / 3.0F, 0.000001F);
        expectWithinAbsoluteError(detail::spectrogramLogicalPixelWidth(2.0F), 0.5F, 0.000001F);

        beginTest("Frequency labels are compact, locale-independent, and bounded");

        struct LabelCase final {
            float frequencyHz = 0.0F;
            std::string_view expected;
        };
        constexpr std::array labelCases {
            LabelCase { 20.0F, "20 Hz" },
            LabelCase { 999.0F, "999 Hz" },
            LabelCase { 1'000.0F, "1 kHz" },
            LabelCase { 1'050.0F, "1.05 kHz" },
            LabelCase { 1'500.0F, "1.5 kHz" },
            LabelCase { 10'000.0F, "10 kHz" },
            LabelCase { 11'025.0F, "11.025 kHz" },
            LabelCase { 19'999.0F, "19.999 kHz" },
            LabelCase { 20'000.0F, "20 kHz" },
        };
        for (const auto& testCase : labelCases) {
            std::array<char, detail::frequencyAxisLabelStorage> label { };
            const auto length = detail::formatFrequencyAxisLabel(testCase.frequencyHz, label);
            expectEquals(length, testCase.expected.size());
            expect(std::string_view(label.data(), length) == testCase.expected);
            expectEquals(label[length], '\0');
        }

        for (auto frequency = 20; frequency <= 20'000; ++frequency) {
            std::array<char, detail::frequencyAxisLabelStorage> label { };
            const auto length
                = detail::formatFrequencyAxisLabel(static_cast<float>(frequency), label);
            expect(length > 0 && length <= detail::maximumFrequencyAxisLabelGlyphs);
            expectEquals(label[length], '\0');
        }

        std::array<char, detail::frequencyAxisLabelStorage> invalidLabel { };
        expectEquals(detail::formatFrequencyAxisLabel(-1.0F, invalidLabel), std::size_t { 0 });
        expectEquals(
            detail::formatFrequencyAxisLabel(std::numeric_limits<float>::quiet_NaN(), invalidLabel),
            std::size_t { 0 });
        expectEquals(detail::formatFrequencyAxisLabel(20'001.0F, invalidLabel), std::size_t { 0 });

        beginTest("Frequency tick selection retains exact endpoints and culls collisions");

        constexpr detail::FrequencyAxisLabelMetrics compactMetrics { 0.0F, 10.0F, 10.0F };
        constexpr detail::FrequencyAxisLabelMetrics wideMetrics { 0.0F, 30.0F, 12.0F };
        constexpr detail::FrequencyAxisLabelMetrics spaciousMetrics { 0.0F, 36.0F, 12.0F };
        constexpr detail::FrequencyAxisLabelMetrics measuredMetrics { 6.0F, 7.0F, 12.0F };
        const auto labelExtent = [](const detail::FrequencyAxisTickSelection::Tick& tick,
                                     const detail::FrequencyAxisLabelMetrics& metrics,
                                     const detail::FrequencyAxisOrientation orientation) {
            std::array<char, detail::frequencyAxisLabelStorage> label { };
            const auto glyphCount = detail::formatFrequencyAxisLabel(tick.frequencyHz, label);
            return orientation == detail::FrequencyAxisOrientation::horizontal
                ? metrics.glyphWidth + (static_cast<float>(glyphCount - 1) * metrics.glyphAdvance)
                : metrics.glyphHeight;
        };
        const auto verifyNoOverlap = [this, &labelExtent](
                                         const detail::FrequencyAxisMapping& mapping,
                                         const float axisLength,
                                         const detail::FrequencyAxisTickSelection& selection,
                                         const detail::FrequencyAxisLabelMetrics& metrics,
                                         const detail::FrequencyAxisOrientation orientation) {
            auto previousEnd = -4.0F;
            for (std::size_t index = 0; index < selection.count; ++index) {
                const auto& tick = selection.ticks[index];
                const auto extent = labelExtent(tick, metrics, orientation);
                const auto centre
                    = detail::mapFrequencyToUnit(mapping, tick.frequencyHz) * axisLength;
                const auto start = std::clamp(centre - (extent * 0.5F), 0.0F, axisLength - extent);
                expect(start >= previousEnd + 4.0F - 0.0001F);
                previousEnd = start + extent;
                expect(tick.frequencyHz >= mapping.minimumFrequencyHz);
                expect(tick.frequencyHz <= mapping.maximumFrequencyHz);
                expectEquals(static_cast<std::uint64_t>(tick.labelFrequencyHz),
                    static_cast<std::uint64_t>(std::lround(tick.frequencyHz)));
                if (index != 0)
                    expect(selection.ticks[index - 1].frequencyHz < tick.frequencyHz);
            }
            expect(selection.count <= detail::maximumFrequencyAxisTickCount);
            expect(selection.generatedCandidateCount <= detail::maximumFrequencyAxisCandidateCount);
        };
        const auto containsFrequency
            = [](const detail::FrequencyAxisTickSelection& selection, const float frequency) {
                  for (std::size_t index = 0; index < selection.count; ++index) {
                      if (std::abs(selection.ticks[index].frequencyHz - frequency) < 0.01F)
                          return true;
                  }

                  return false;
              };

        const auto lowNyquistTicks = detail::selectFrequencyAxisTicks({ 20.0F, 11'025.0F, 1.0F },
            2'000.0F, compactMetrics, detail::FrequencyAxisOrientation::horizontal);
        expect(lowNyquistTicks.count > 2);
        expectEquals(lowNyquistTicks.ticks[0].frequencyHz, 20.0F);
        expectEquals(lowNyquistTicks.ticks[lowNyquistTicks.count - 1].frequencyHz, 11'025.0F);
        expectEquals(static_cast<std::uint64_t>(
                         lowNyquistTicks.ticks[lowNyquistTicks.count - 1].labelFrequencyHz),
            std::uint64_t { 11'025 });
        expect(!lowNyquistTicks.candidateCapacityExceeded);
        verifyNoOverlap({ 20.0F, 11'025.0F, 1.0F }, 2'000.0F, lowNyquistTicks, compactMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const detail::FrequencyAxisMapping fractionalNyquistMapping { 20.0F, 11'025.4F, 0.65F };
        const auto fractionalNyquistTicks
            = detail::selectFrequencyAxisTicks(fractionalNyquistMapping, 2'000.0F, compactMetrics,
                detail::FrequencyAxisOrientation::horizontal);
        expectWithinAbsoluteError(
            fractionalNyquistTicks.ticks[fractionalNyquistTicks.count - 1].frequencyHz, 11'025.4F,
            0.001F);
        expectEquals(
            static_cast<std::uint64_t>(
                fractionalNyquistTicks.ticks[fractionalNyquistTicks.count - 1].labelFrequencyHz),
            std::uint64_t { 11'025 });
        verifyNoOverlap(fractionalNyquistMapping, 2'000.0F, fractionalNyquistTicks, compactMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const auto deduplicatedEndpoint = detail::selectFrequencyAxisTicks(logarithmicMapping,
            1'000.0F, compactMetrics, detail::FrequencyAxisOrientation::horizontal);
        expect(deduplicatedEndpoint.count > 2);
        expectEquals(
            deduplicatedEndpoint.ticks[deduplicatedEndpoint.count - 1].frequencyHz, 20'000.0F);
        verifyNoOverlap(logarithmicMapping, 1'000.0F, deduplicatedEndpoint, compactMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const detail::FrequencyAxisMapping narrowLinearMapping { 20.0F, 200.0F, 0.0F };
        const auto culledTicks = detail::selectFrequencyAxisTicks(
            narrowLinearMapping, 180.0F, wideMetrics, detail::FrequencyAxisOrientation::horizontal);
        expectEquals(culledTicks.ticks[0].frequencyHz, 20.0F);
        expect(containsFrequency(culledTicks, 100.0F));
        expectEquals(culledTicks.ticks[culledTicks.count - 1].frequencyHz, 200.0F);
        verifyNoOverlap(narrowLinearMapping, 180.0F, culledTicks, wideMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const auto spaciousLinearTicks = detail::selectFrequencyAxisTicks(
            linearMapping, 800.0F, spaciousMetrics, detail::FrequencyAxisOrientation::horizontal);
        constexpr std::array requestedLinearLabels { 3'000.0F, 6'000.0F, 8'000.0F, 12'000.0F,
            14'000.0F, 15'000.0F, 16'000.0F, 18'000.0F };
        for (const auto frequency : requestedLinearLabels)
            expect(containsFrequency(spaciousLinearTicks, frequency));
        expect(!spaciousLinearTicks.candidateCapacityExceeded);
        verifyNoOverlap(linearMapping, 800.0F, spaciousLinearTicks, spaciousMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const auto measuredSpaciousLinearTicks = detail::selectFrequencyAxisTicks(
            linearMapping, 1'200.0F, measuredMetrics, detail::FrequencyAxisOrientation::horizontal);
        for (const auto frequency : requestedLinearLabels)
            expect(containsFrequency(measuredSpaciousLinearTicks, frequency));
        verifyNoOverlap(linearMapping, 1'200.0F, measuredSpaciousLinearTicks, measuredMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const auto compactLinearTicks = detail::selectFrequencyAxisTicks(
            linearMapping, 420.0F, spaciousMetrics, detail::FrequencyAxisOrientation::horizontal);
        expect(compactLinearTicks.count < spaciousLinearTicks.count);
        verifyNoOverlap(linearMapping, 420.0F, compactLinearTicks, spaciousMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const auto spaciousLogarithmicTicks = detail::selectFrequencyAxisTicks(logarithmicMapping,
            800.0F, spaciousMetrics, detail::FrequencyAxisOrientation::horizontal);
        expect(!containsFrequency(spaciousLogarithmicTicks, 12'000.0F));
        expect(!containsFrequency(spaciousLogarithmicTicks, 15'000.0F));
        expect(!containsFrequency(spaciousLogarithmicTicks, 18'000.0F));
        expect(containsFrequency(spaciousLogarithmicTicks, 10'000.0F));
        expect(containsFrequency(spaciousLogarithmicTicks, 20'000.0F));
        verifyNoOverlap(logarithmicMapping, 800.0F, spaciousLogarithmicTicks, spaciousMetrics,
            detail::FrequencyAxisOrientation::horizontal);

        const auto verticalLinearTicks = detail::selectFrequencyAxisTicks(
            linearMapping, 400.0F, spaciousMetrics, detail::FrequencyAxisOrientation::vertical);
        expect(containsFrequency(verticalLinearTicks, 12'000.0F));
        expect(containsFrequency(verticalLinearTicks, 15'000.0F));
        expect(containsFrequency(verticalLinearTicks, 18'000.0F));
        verifyNoOverlap(linearMapping, 400.0F, verticalLinearTicks, spaciousMetrics,
            detail::FrequencyAxisOrientation::vertical);

        for (auto spacingIndex = 0; spacingIndex <= 10; ++spacingIndex) {
            const auto spacing = static_cast<float>(spacingIndex) * 0.1F;
            const detail::FrequencyAxisMapping mapping { 20.0F, 20'000.0F, spacing };
            const auto horizontal = detail::selectFrequencyAxisTicks(
                mapping, 800.0F, measuredMetrics, detail::FrequencyAxisOrientation::horizontal);
            const auto repeated = detail::selectFrequencyAxisTicks(
                mapping, 800.0F, measuredMetrics, detail::FrequencyAxisOrientation::horizontal);
            expectEquals(horizontal.count, repeated.count);
            for (std::size_t index = 0; index < horizontal.count; ++index)
                expectEquals(
                    horizontal.ticks[index].frequencyHz, repeated.ticks[index].frequencyHz);
            expect(!horizontal.candidateCapacityExceeded);
            verifyNoOverlap(mapping, 800.0F, horizontal, measuredMetrics,
                detail::FrequencyAxisOrientation::horizontal);

            const auto vertical = detail::selectFrequencyAxisTicks(
                mapping, 240.0F, measuredMetrics, detail::FrequencyAxisOrientation::vertical);
            expect(!vertical.candidateCapacityExceeded);
            verifyNoOverlap(mapping, 240.0F, vertical, measuredMetrics,
                detail::FrequencyAxisOrientation::vertical);
        }

        const auto capacityTicks = detail::selectFrequencyAxisTicks(linearMapping, 2'560.0F,
            { 0.0F, 1.0F, 1.0F }, detail::FrequencyAxisOrientation::horizontal);
        expectEquals(capacityTicks.count, detail::maximumFrequencyAxisTickCount);
        expect(!capacityTicks.candidateCapacityExceeded);
        verifyNoOverlap(linearMapping, 2'560.0F, capacityTicks, { 0.0F, 1.0F, 1.0F },
            detail::FrequencyAxisOrientation::horizontal);

        const auto impossibleTicks = detail::selectFrequencyAxisTicks(
            logarithmicMapping, 30.0F, wideMetrics, detail::FrequencyAxisOrientation::horizontal);
        expectEquals(impossibleTicks.count, std::size_t { 1 });

        const auto invalidMetrics = detail::selectFrequencyAxisTicks(logarithmicMapping, 800.0F,
            { 4.0F, 0.0F, 12.0F }, detail::FrequencyAxisOrientation::horizontal);
        expectEquals(invalidMetrics.count, std::size_t { 0 });

        beginTest("Spectrum decibel ticks use accepted steps and 28-point label spacing");

        expectEquals(detail::chooseSpectrumDecibelTickStep(-96.0F, 0.0F, 448.0F), 6);
        expectEquals(detail::chooseSpectrumDecibelTickStep(-96.0F, 0.0F, 224.0F), 12);
        expectEquals(detail::chooseSpectrumDecibelTickStep(-96.0F, 0.0F, 112.0F), 24);
        expectEquals(detail::chooseSpectrumDecibelTickStep(-96.0F, 0.0F, 56.0F), 48);

        const auto ordinaryDecibelTicks = detail::makeSpectrumDecibelTicks(-90.0F, 0.0F, 300.0F);
        expectEquals(ordinaryDecibelTicks.candidateStep, 12);
        expectEquals(ordinaryDecibelTicks.displayedStep, 12);
        expectEquals(ordinaryDecibelTicks.count, std::size_t { 8 });
        expectEquals(ordinaryDecibelTicks.values[0], -84);
        expectEquals(ordinaryDecibelTicks.values[ordinaryDecibelTicks.count - 1], 0);

        const auto shortAxisTicks = detail::makeSpectrumDecibelTicks(-180.0F, 12.0F, 100.0F);
        expectEquals(shortAxisTicks.candidateStep, 48);
        expect(shortAxisTicks.displayedStep > shortAxisTicks.candidateStep);
        for (std::size_t index = 1; index < shortAxisTicks.count; ++index) {
            const auto spacing = 100.0F
                * static_cast<float>(
                    shortAxisTicks.values[index] - shortAxisTicks.values[index - 1])
                / 192.0F;
            expect(spacing >= detail::minimumSpectrumDecibelLabelSpacing);
        }

        constexpr std::array floors { -180.0F, -150.0F, -90.0F, -36.0F };
        constexpr std::array ceilings { -24.0F, 0.0F, 12.0F };
        constexpr std::array axisLengths { 56.0F, 112.0F, 224.0F, 448.0F };
        for (const auto floor : floors) {
            for (const auto ceiling : ceilings) {
                if (ceiling - floor < 24.0F)
                    continue;

                for (const auto axisLength : axisLengths) {
                    const auto ticks = detail::makeSpectrumDecibelTicks(floor, ceiling, axisLength);
                    expect(std::find(detail::spectrumDecibelTickSteps.begin(),
                               detail::spectrumDecibelTickSteps.end(), ticks.candidateStep)
                        != detail::spectrumDecibelTickSteps.end());
                    for (std::size_t index = 1; index < ticks.count; ++index) {
                        expect(ticks.values[index - 1] < ticks.values[index]);
                        const auto spacing = axisLength
                            * static_cast<float>(ticks.values[index] - ticks.values[index - 1])
                            / (ceiling - floor);
                        expect(spacing >= detail::minimumSpectrumDecibelLabelSpacing - 0.0001F);
                    }
                }
            }
        }

        expectEquals(
            detail::makeSpectrumDecibelTicks(-90.0F, -90.0F, 300.0F).count, std::size_t { 0 });
        expectEquals(detail::makeSpectrumDecibelTicks(-90.0F, 0.0F, 0.0F).count, std::size_t { 0 });

        beginTest("Spectrum slope and sRGB conversion preserve their presentation references");

        expectWithinAbsoluteError(
            detail::spectrumSlopeCompensationDecibels(1'000.0F, 6.0F), 0.0F, 1.0e-6F);
        expectWithinAbsoluteError(
            detail::spectrumSlopeCompensationDecibels(2'000.0F, 6.0F), 6.0F, 1.0e-6F);
        expectWithinAbsoluteError(
            detail::spectrumSlopeCompensationDecibels(500.0F, 6.0F), -6.0F, 1.0e-6F);

        beginTest("Spectrum clips non-bin-centred endpoints using power interpolation");

        constexpr std::array endpointFftSizes { 1024U, 2048U, 4096U, 8192U, 16384U };
        for (const auto fftSize : endpointFftSizes) {
            const auto binFrequency = 48'000.0 / static_cast<double>(fftSize);
            const auto binCount = static_cast<std::size_t>(fftSize / 2U) + 1;
            const auto lowerEndpoint
                = detail::spectrumFrequencyInterpolation(20.0, binFrequency, binCount);
            const auto upperEndpoint
                = detail::spectrumFrequencyInterpolation(20'000.0, binFrequency, binCount);
            expect(lowerEndpoint.valid);
            expect(upperEndpoint.valid);
            expectWithinAbsoluteError((static_cast<double>(lowerEndpoint.lowerBin)
                                          + static_cast<double>(lowerEndpoint.upperBinWeight))
                    * binFrequency,
                20.0, 1.0e-4);
            expectWithinAbsoluteError((static_cast<double>(upperEndpoint.lowerBin)
                                          + static_cast<double>(upperEndpoint.upperBinWeight))
                    * binFrequency,
                20'000.0, 1.0e-4);
        }

        const auto exactBin
            = detail::spectrumFrequencyInterpolation(23.4375, 23.4375, std::size_t { 1025 });
        expect(exactBin.valid);
        expectEquals(exactBin.lowerBin, std::size_t { 1 });
        expectEquals(exactBin.upperBin, std::size_t { 1 });
        expectWithinAbsoluteError(exactBin.upperBinWeight, 0.0F, 0.0F);
        expect(!detail::spectrumFrequencyInterpolation(-1.0, 10.0, 100).valid);
        expect(!detail::spectrumFrequencyInterpolation(1'001.0, 10.0, 100).valid);
        expect(!detail::spectrumFrequencyInterpolation(20.0, 0.0, 100).valid);

        const auto powerInterpolated
            = detail::interpolateSpectrumPowerDecibels(-20.0F, -40.0F, 0.5F);
        const auto expectedPowerInterpolation = static_cast<float>(
            10.0 * std::log10((std::pow(10.0, -2.0) + std::pow(10.0, -4.0)) * 0.5));
        expectWithinAbsoluteError(powerInterpolated, expectedPowerInterpolation, 1.0e-5F);
        expect(std::abs(powerInterpolated - (-30.0F)) > 1.0F);
        expectWithinAbsoluteError(
            detail::interpolateSpectrumPowerDecibels(-20.0F, -40.0F, 0.0F), -20.0F, 0.0F);
        expectWithinAbsoluteError(
            detail::interpolateSpectrumPowerDecibels(-20.0F, -40.0F, 1.0F), -40.0F, 0.0F);

        const auto compensatedSilence
            = detail::sanitiseSpectrumAnalysisDecibels(minimumSpectrumDecibels)
            + detail::spectrumSlopeCompensationDecibels(20'000.0F, 6.0F);
        expect(compensatedSilence < -150.0F);
        expectWithinAbsoluteError(
            detail::sanitiseSpectrumAnalysisDecibels(std::numeric_limits<float>::quiet_NaN()),
            minimumSpectrumDecibels, 1.0e-6F);
        expectWithinAbsoluteError(
            detail::sanitiseSpectrumAnalysisDecibels(std::numeric_limits<float>::infinity()),
            minimumSpectrumDecibels, 1.0e-6F);
        expectWithinAbsoluteError(
            detail::sanitiseSpectrumAnalysisDecibels(-140.0F), -140.0F, 1.0e-6F);
        expectWithinAbsoluteError(detail::sanitiseSpectrumAnalysisDecibels(24.0F), 24.0F, 1.0e-6F);
        expectWithinAbsoluteError(detail::srgbComponentToLinear(0.0F), 0.0F, 1.0e-7F);
        expectWithinAbsoluteError(detail::srgbComponentToLinear(1.0F), 1.0F, 1.0e-7F);
        expectWithinAbsoluteError(detail::srgbComponentToLinear(0.5F), 0.214041F, 1.0e-5F);

        beginTest("Peak/RMS ticks use the accepted fixed scale and bounded label selection");

        constexpr std::array expectedMeterTicks { -60, -48, -36, -24, -12, -6, 0, 3 };
        expect(detail::peakRmsMajorDecibelTicks == expectedMeterTicks);
        expectEquals(detail::mapPeakRmsDecibelsToUnit(-60.0F), 0.0F);
        expectWithinAbsoluteError(
            detail::mapPeakRmsDecibelsToUnit(-6.0F), 54.0F / 63.0F, 0.000001F);
        expectWithinAbsoluteError(detail::mapPeakRmsDecibelsToUnit(0.0F), 60.0F / 63.0F, 0.000001F);
        expectEquals(detail::mapPeakRmsDecibelsToUnit(3.0F), 1.0F);
        expectEquals(detail::mapPeakRmsDecibelsToUnit(-120.0F), 0.0F);
        expectEquals(detail::mapPeakRmsDecibelsToUnit(12.0F), 1.0F);
        expectEquals(
            detail::mapPeakRmsDecibelsToUnit(std::numeric_limits<float>::quiet_NaN()), 0.0F);

        const auto roomyMeterLabels = detail::selectPeakRmsTickLabels(512.0F, 10.0F);
        expect(std::all_of(roomyMeterLabels.visible.begin(), roomyMeterLabels.visible.end(),
            [](const auto visible) { return visible; }));

        const auto compactMeterLabels = detail::selectPeakRmsTickLabels(100.0F, 10.0F);
        expect(compactMeterLabels.visible.front());
        expect(compactMeterLabels.visible.back());
        expect(
            std::count(compactMeterLabels.visible.begin(), compactMeterLabels.visible.end(), true)
            < static_cast<int>(compactMeterLabels.visible.size()));
        auto previousMeterLabelEnd = -2.0F;
        for (std::size_t index = 0; index < compactMeterLabels.visible.size(); ++index) {
            if (!compactMeterLabels.visible[index])
                continue;

            const auto centre = detail::mapPeakRmsDecibelsToUnit(
                                    static_cast<float>(detail::peakRmsMajorDecibelTicks[index]))
                * 100.0F;
            const auto start = std::clamp(centre - 5.0F, 0.0F, 90.0F);
            expect(start >= previousMeterLabelEnd + 2.0F - 0.0001F);
            previousMeterLabelEnd = start + 10.0F;
        }
        const auto invalidMeterLabels = detail::selectPeakRmsTickLabels(0.0F, 10.0F);
        expect(std::none_of(invalidMeterLabels.visible.begin(), invalidMeterLabels.visible.end(),
            [](const auto visible) { return visible; }));

        beginTest("Peak/RMS readouts preserve measurement floor and colour thresholds");

        const auto silentReadout = detail::classifyPeakRmsReadout(minimumDisplayDecibels);
        expect(silentReadout.kind == detail::PeakRmsReadout::Kind::minusInfinity);
        expect(detail::classifyPeakRmsReadout(-121.0F).kind
            == detail::PeakRmsReadout::Kind::minusInfinity);
        expect(detail::classifyPeakRmsReadout(std::numeric_limits<float>::infinity()).kind
            == detail::PeakRmsReadout::Kind::minusInfinity);

        const auto belowVisibleFloor = detail::classifyPeakRmsReadout(-119.9F);
        expect(belowVisibleFloor.kind == detail::PeakRmsReadout::Kind::decibelTenths);
        expectEquals(belowVisibleFloor.decibelTenths, -1'199);
        expect(belowVisibleFloor.levelRange == detail::PeakRmsLevelRange::cyan);

        const auto roundedReadout = detail::classifyPeakRmsReadout(-12.34F);
        expectEquals(roundedReadout.decibelTenths, -123);
        expect(roundedReadout.levelRange == detail::PeakRmsLevelRange::cyan);
        expect(
            detail::classifyPeakRmsReadout(-6.001F).levelRange == detail::PeakRmsLevelRange::cyan);
        expect(
            detail::classifyPeakRmsReadout(-6.0F).levelRange == detail::PeakRmsLevelRange::amber);
        expect(
            detail::classifyPeakRmsReadout(-0.001F).levelRange == detail::PeakRmsLevelRange::amber);
        expect(detail::classifyPeakRmsReadout(0.0F).levelRange == detail::PeakRmsLevelRange::red);
        const auto doubledFloatReadout = detail::classifyPeakRmsReadout(20.0F * std::log10(2.0F));
        expectEquals(doubledFloatReadout.decibelTenths, 60);
        expect(doubledFloatReadout.levelRange == detail::PeakRmsLevelRange::red);
        const auto twelveDecibelReadout = detail::classifyPeakRmsReadout(12.0F);
        expectEquals(twelveDecibelReadout.decibelTenths, 120);
        expect(twelveDecibelReadout.levelRange == detail::PeakRmsLevelRange::red);
        const auto maximumFloatDecibels = 20.0F * std::log10(std::numeric_limits<float>::max());
        expectEquals(detail::classifyPeakRmsReadout(maximumFloatDecibels).decibelTenths,
            detail::maximumFiniteFloatPeakRmsReadoutTenths);
        expectEquals(
            detail::classifyPeakRmsReadout(std::numeric_limits<float>::max()).decibelTenths,
            detail::maximumFiniteFloatPeakRmsReadoutTenths);

        beginTest("Peak/RMS layout is mono-aware, responsive, and shares CLEAR hit geometry");

        const auto stereoMeterLayout
            = detail::calculatePeakRmsPanelLayout(240.0F, 280.0F, 24.0F, 2, 10.0F, 24.0F, 42.0F);
        expectEquals(stereoMeterLayout.channelCount, std::size_t { 2 });
        expect(stereoMeterLayout.showTickLabels);
        expect(stereoMeterLayout.showReadouts);
        expect(stereoMeterLayout.channelTracks[0].right < stereoMeterLayout.channelTracks[1].left);
        expectWithinAbsoluteError(stereoMeterLayout.channelTracks[0].width(),
            stereoMeterLayout.channelTracks[1].width(), 0.0001F);
        expect(stereoMeterLayout.scaleTop > stereoMeterLayout.scaleBottom);
        expect(stereoMeterLayout.clearHitBounds.width()
            >= stereoMeterLayout.clearVisualBounds.width());
        expect(stereoMeterLayout.clearHitBounds.height() >= 24.0F);
        expect(stereoMeterLayout.clearHitBounds.contains(
            (stereoMeterLayout.clearVisualBounds.left + stereoMeterLayout.clearVisualBounds.right)
                * 0.5F,
            (stereoMeterLayout.clearVisualBounds.bottom + stereoMeterLayout.clearVisualBounds.top)
                * 0.5F));

        const auto monoMeterLayout
            = detail::calculatePeakRmsPanelLayout(240.0F, 280.0F, 24.0F, 1, 10.0F, 24.0F, 42.0F);
        expectEquals(monoMeterLayout.channelCount, std::size_t { 1 });
        expect(monoMeterLayout.channelTracks[0].width() > 0.0F);
        expectEquals(monoMeterLayout.channelTracks[1].width(), 0.0F);
        const auto monoTrackCentre
            = (monoMeterLayout.channelTracks[0].left + monoMeterLayout.channelTracks[0].right)
            * 0.5F;
        const auto monoGroupCentre
            = ((monoMeterLayout.tickLineLeft + 4.0F) + monoMeterLayout.tickLineRight) * 0.5F;
        expectWithinAbsoluteError(monoTrackCentre, monoGroupCentre, 0.0001F);

        const auto narrowMeterLayout
            = detail::calculatePeakRmsPanelLayout(90.0F, 100.0F, 18.0F, 2, 10.0F, 24.0F, 42.0F);
        expectEquals(narrowMeterLayout.channelCount, std::size_t { 2 });
        expect(!narrowMeterLayout.showReadouts);
        constexpr auto unscaledOverWidth = 32.0F;
        constexpr auto preferredMeterTextScale = 0.78F;
        for (std::size_t channel = 0; channel < narrowMeterLayout.channelCount; ++channel) {
            const auto& track = narrowMeterLayout.channelTracks[channel];
            const auto& column = narrowMeterLayout.channelColumns[channel];
            expect(track.left >= 0.0F);
            expect(track.right <= 90.0F);
            expect(track.bottom >= 0.0F);
            expect(track.top <= 100.0F);

            const auto overScale = detail::fitPeakRmsTextScale(
                column.width() - 2.0F, unscaledOverWidth, preferredMeterTextScale);
            const auto fittedOverWidth = unscaledOverWidth * overScale;
            const auto fittedOverLeft = column.left + ((column.width() - fittedOverWidth) * 0.5F);
            expect(overScale > 0.0F && overScale <= preferredMeterTextScale);
            expect(fittedOverLeft >= column.left);
            expect(fittedOverLeft + fittedOverWidth <= column.right + 0.0001F);
        }
        expect(
            narrowMeterLayout.channelColumns[0].right <= narrowMeterLayout.channelColumns[1].left);
        expectEquals(detail::fitPeakRmsTextScale(0.0F, 32.0F, 0.78F), 0.0F);

        const auto invalidMeterLayout
            = detail::calculatePeakRmsPanelLayout(240.0F, 280.0F, 24.0F, 3, 10.0F, 24.0F, 42.0F);
        expectEquals(invalidMeterLayout.channelCount, std::size_t { 0 });
        expectEquals(invalidMeterLayout.channelTracks[0].width(), 0.0F);
        expectEquals(
            detail::calculatePeakRmsPanelLayout(0.0F, 280.0F, 24.0F, 2, 10.0F, 24.0F, 42.0F)
                .channelCount,
            std::size_t { 0 });
        expect(!detail::PeakRmsLogicalRect { }.contains(0.0F, 0.0F));
        const auto hiddenClearLayout
            = detail::calculatePeakRmsPanelLayout(60.0F, 100.0F, 18.0F, 2, 10.0F, 24.0F, 42.0F);
        expectEquals(hiddenClearLayout.clearHitBounds.width(), 0.0F);
        expect(!hiddenClearLayout.clearHitBounds.contains(0.0F, 0.0F));

        beginTest("Loudness mapping and readouts preserve readiness, silence, and finite values");

        expectEquals(detail::mapLoudnessLufsToUnit(-60.0F), 0.0F);
        expectEquals(detail::mapLoudnessLufsToUnit(-30.0F), 0.5F);
        expectEquals(detail::mapLoudnessLufsToUnit(0.0F), 1.0F);
        expectEquals(detail::mapLoudnessLufsToUnit(-90.0F), 0.0F);
        expectEquals(detail::mapLoudnessLufsToUnit(12.0F), 1.0F);
        expectEquals(detail::mapLoudnessLufsToUnit(std::numeric_limits<float>::quiet_NaN()), 0.0F);

        expect(detail::classifyLoudnessReadout(-std::numeric_limits<double>::infinity(), false).kind
            == detail::LoudnessReadout::Kind::emDash);
        expect(detail::classifyLoudnessReadout(-std::numeric_limits<double>::infinity(), true).kind
            == detail::LoudnessReadout::Kind::minusInfinity);
        expect(detail::classifyLoudnessReadout(std::numeric_limits<double>::quiet_NaN(), true).kind
            == detail::LoudnessReadout::Kind::emDash);
        expect(detail::classifyLoudnessReadout(std::numeric_limits<double>::infinity(), true).kind
            == detail::LoudnessReadout::Kind::emDash);
        expect(detail::classifyLoudnessReadout(-23.0, false).kind
                == detail::LoudnessReadout::Kind::emDash,
            "A finite Integrated value invalidated by capacity overflow must render as not ready");
        const auto finiteLoudness = detail::classifyLoudnessReadout(-23.46, true);
        expect(finiteLoudness.kind == detail::LoudnessReadout::Kind::lufsTenths);
        expectEquals(finiteLoudness.lufsTenths, -235);
        const auto belowVisualFloor = detail::classifyLoudnessReadout(-80.0, true);
        expect(belowVisualFloor.kind == detail::LoudnessReadout::Kind::lufsTenths);
        expectEquals(belowVisualFloor.lufsTenths, -800);
        expectEquals(detail::classifyLoudnessReadout(4.2, true).lufsTenths, 42);
        expectEquals(detail::classifyLoudnessReadout(10'000.0, true).lufsTenths,
            detail::maximumCachedLoudnessReadoutTenths);
        expectEquals(
            detail::formatLoudnessAccessibilityReading(-23.46, true), juce::String("-23.5 LUFS"));
        expectEquals(
            detail::formatLoudnessAccessibilityReading(-0.01, true), juce::String("0.0 LUFS"));
        expectEquals(
            detail::formatLoudnessAccessibilityReading(4.2, true), juce::String("+4.2 LUFS"));
        expectEquals(
            detail::formatLoudnessAccessibilityReading(-23.0, false), juce::String("not ready"));
        expectEquals(detail::formatLoudnessAccessibilityReading(
                         -std::numeric_limits<double>::infinity(), true),
            juce::String("minus infinity"));

        beginTest("Loudness layout keeps a slim centred bar and responsive readout rows");

        const auto checkLoudnessLayout = [this](const float width, const float height) {
            const auto layout
                = detail::calculateLoudnessPanelLayout(width, height, 22.0F, 10.0F, 42.0F);
            const auto expectInside
                = [this, width, height](const detail::PeakRmsLogicalRect& rect) {
                      expect(rect.left >= 0.0F);
                      expect(rect.bottom >= 0.0F);
                      expect(rect.right <= width + 0.0001F);
                      expect(rect.top <= height + 0.0001F);
                  };
            expect(layout.trackBounds.width() > 0.0F);
            expect(layout.trackBounds.width() <= 16.0F);
            expectWithinAbsoluteError(
                (layout.trackBounds.left + layout.trackBounds.right) * 0.5F, width * 0.5F, 0.0001F);
            expect(layout.showMomentaryText);
            expect(layout.showSecondaryText);
            expect(layout.momentaryTextBounds.bottom > layout.trackBounds.top);
            expect(layout.shortTermTextBounds.top < layout.trackBounds.bottom);
            expect(layout.integratedTextBounds.top < layout.shortTermTextBounds.bottom);
            expect(layout.resetHitBounds.height() >= 24.0F);
            expect(layout.resetHitBounds.contains(
                (layout.resetVisualBounds.left + layout.resetVisualBounds.right) * 0.5F,
                (layout.resetVisualBounds.bottom + layout.resetVisualBounds.top) * 0.5F));
            expectInside(layout.trackBounds);
            expectInside(layout.resetVisualBounds);
            expectInside(layout.resetHitBounds);
            expectInside(layout.momentaryTextBounds);
            expectInside(layout.shortTermTextBounds);
            expectInside(layout.integratedTextBounds);
        };

        const auto defaultTiles = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, 720.0, 420.0 }, DashboardLayout::defaultSplits);
        const auto defaultLoudnessTile = defaultTiles[DashboardPanel::loudness];
        checkLoudnessLayout(static_cast<float>(defaultLoudnessTile.width),
            static_cast<float>(defaultLoudnessTile.height));
        constexpr DashboardLayoutSplits minimumLoudnessSplits { 26, 36, 28, 42 };
        const auto minimumTiles = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, 720.0, 420.0 }, minimumLoudnessSplits);
        const auto minimumLoudnessTile = minimumTiles[DashboardPanel::loudness];
        checkLoudnessLayout(static_cast<float>(minimumLoudnessTile.width),
            static_cast<float>(minimumLoudnessTile.height));

        const auto invalidLoudnessLayout = detail::calculateLoudnessPanelLayout(
            std::numeric_limits<float>::quiet_NaN(), 180.0F, 22.0F, 10.0F, 42.0F);
        expectEquals(invalidLoudnessLayout.trackBounds.width(), 0.0F);
        expectEquals(invalidLoudnessLayout.resetHitBounds.width(), 0.0F);

        beginTest("Stereo field coordinates remain fixed at full scale without edge clamping");

        expectWithinAbsoluteError(
            detail::mapStereoFieldCoordinate(-1.0F, 10.0F, 110.0F), 10.0F, 0.0001F);
        expectWithinAbsoluteError(
            detail::mapStereoFieldCoordinate(0.0F, 10.0F, 110.0F), 60.0F, 0.0001F);
        expectWithinAbsoluteError(
            detail::mapStereoFieldCoordinate(1.0F, 10.0F, 110.0F), 110.0F, 0.0001F);
        const auto quietSignalPosition = detail::mapStereoFieldCoordinate(0.05F, 10.0F, 110.0F);
        expectWithinAbsoluteError(quietSignalPosition, 62.5F, 0.0001F);
        expect(std::abs(quietSignalPosition - 60.0F) < 3.0F,
            "A quiet field coordinate must remain near the fixed full-scale centre");
        expectWithinAbsoluteError(
            detail::mapStereoFieldCoordinate(2.0F, 10.0F, 110.0F), 160.0F, 0.0001F);
        expectEquals(detail::mapStereoFieldCoordinate(0.5F, 4.0F, 4.0F), 0.0F);

        beginTest("Stereo correlation clamps, rounds, and uses three semantic colour ranges");

        expectEquals(detail::mapStereoCorrelationToUnit(-2.0F), 0.0F);
        expectEquals(detail::mapStereoCorrelationToUnit(0.0F), 0.5F);
        expectEquals(detail::mapStereoCorrelationToUnit(2.0F), 1.0F);
        const auto positiveCorrelation = detail::classifyStereoCorrelationReadout(0.126F, true);
        expect(positiveCorrelation.available);
        expectEquals(positiveCorrelation.hundredths, 13);
        expect(positiveCorrelation.colourRange == detail::StereoCorrelationColourRange::cyan);
        const auto negativeCorrelation = detail::classifyStereoCorrelationReadout(-0.126F, true);
        expectEquals(negativeCorrelation.hundredths, -13);
        expect(negativeCorrelation.colourRange == detail::StereoCorrelationColourRange::amber);
        expect(detail::classifyStereoCorrelationReadout(0.05F, true).colourRange
            == detail::StereoCorrelationColourRange::neutral);
        expect(detail::classifyStereoCorrelationReadout(-0.05F, true).colourRange
            == detail::StereoCorrelationColourRange::neutral);
        expectEquals(detail::classifyStereoCorrelationReadout(4.0F, true).hundredths, 100);
        expectEquals(detail::classifyStereoCorrelationReadout(-4.0F, true).hundredths, -100);
        expect(!detail::classifyStereoCorrelationReadout(0.5F, false).available);
        expect(
            !detail::classifyStereoCorrelationReadout(std::numeric_limits<float>::quiet_NaN(), true)
                .available);

        beginTest("Stereo point opacity fades across the latest 250 milliseconds");

        expectWithinAbsoluteError(detail::stereoFieldPointAgeOpacity(0.0F, 0.0), 1.0F, 0.0001F);
        expectWithinAbsoluteError(detail::stereoFieldPointAgeOpacity(0.25F, 0.0625), 0.5F, 0.0001F);
        expectWithinAbsoluteError(detail::stereoFieldPointAgeOpacity(0.0F, 0.125), 0.5F, 0.0001F);
        expectEquals(detail::stereoFieldPointAgeOpacity(1.0F, 0.0), 0.0F);
        expectEquals(detail::stereoFieldPointAgeOpacity(0.5F, 0.250), 0.0F);
        expectWithinAbsoluteError(detail::stereoFieldPointAgeOpacity(0.5F, -1.0), 0.5F, 0.0001F);
        expectEquals(
            detail::stereoFieldPointAgeOpacity(std::numeric_limits<float>::infinity(), 0.0), 0.0F);

        beginTest("Stereo panel layout preserves a square scope and responsive correlation strip");

        const auto stereoLayout
            = detail::calculateStereoFieldPanelLayout(240.0F, 280.0F, 24.0F, 10.0F);
        expect(stereoLayout.scopeBounds.width() > 0.0F);
        expectWithinAbsoluteError(
            stereoLayout.scopeBounds.width(), stereoLayout.scopeBounds.height(), 0.0001F);
        expect(stereoLayout.scopeBounds.left >= 0.0F && stereoLayout.scopeBounds.right <= 240.0F);
        expect(stereoLayout.scopeBounds.bottom >= 0.0F && stereoLayout.scopeBounds.top <= 280.0F);
        expect(stereoLayout.correlationTrackBounds.width() > 0.0F);
        expect(stereoLayout.showCorrelationReadout);

        const auto compactStereoLayout
            = detail::calculateStereoFieldPanelLayout(60.0F, 70.0F, 18.0F, 10.0F);
        expect(compactStereoLayout.scopeBounds.width() > 0.0F);
        expectWithinAbsoluteError(compactStereoLayout.scopeBounds.width(),
            compactStereoLayout.scopeBounds.height(), 0.0001F);
        expect(!compactStereoLayout.showCorrelationReadout);
        expect(compactStereoLayout.correlationTrackBounds.left >= 0.0F);
        expect(compactStereoLayout.correlationTrackBounds.right <= 60.0F);
        expect(compactStereoLayout.scopeBounds.top <= 70.0F);

        const auto invalidStereoLayout
            = detail::calculateStereoFieldPanelLayout(0.0F, 280.0F, 24.0F, 10.0F);
        expectEquals(invalidStereoLayout.scopeBounds.width(), 0.0F);
        expectEquals(invalidStereoLayout.correlationTrackBounds.width(), 0.0F);
        expectEquals(detail::calculateStereoFieldPanelLayout(
                         std::numeric_limits<float>::quiet_NaN(), 280.0F, 24.0F, 10.0F)
                         .scopeBounds.width(),
            0.0F);

        beginTest("Spectrum CLEAR uses a visible control and a twenty-four-point hit target");

        const auto spectrumClearLayout
            = detail::calculateSpectrumClearLayout(320.0F, 180.0F, 22.0F);
        expect(spectrumClearLayout.visualBounds.width() > 0.0F);
        expect(spectrumClearLayout.hitBounds.height() >= 24.0F);
        expect(spectrumClearLayout.hitBounds.contains(
            (spectrumClearLayout.visualBounds.left + spectrumClearLayout.visualBounds.right) * 0.5F,
            (spectrumClearLayout.visualBounds.bottom + spectrumClearLayout.visualBounds.top)
                * 0.5F));
        const auto unavailableSpectrumClear
            = detail::calculateSpectrumClearLayout(80.0F, 180.0F, 22.0F);
        expectEquals(unavailableSpectrumClear.visualBounds.width(), 0.0F);

        beginTest("Axis and meter geometry remain within the fixed Metal vertex buffer");

        expectEquals(detail::MetalVisualizationGeometryLimits::maximumGeneratedVertices,
            std::size_t { 60'586 });
        expect(detail::MetalVisualizationGeometryLimits::maximumGeneratedVertices
            <= detail::MetalVisualizationGeometryLimits::vertexCapacity);
        expectEquals(
            detail::MetalVisualizationGeometryLimits::vertexCapacity, std::size_t { 65'536 });
        expectEquals(detail::MetalVisualizationGeometryLimits::maximumSpectrumVertices,
            (6 * maximumSpectrumBinCount) + 30);
        expectEquals(
            detail::MetalVisualizationGeometryLimits::maximumStereoVertices, std::size_t { 84 });
        expectEquals(
            detail::MetalVisualizationGeometryLimits::maximumLoudnessVertices, std::size_t { 78 });

        beginTest("Spectrum frame metadata accepts only supported FFT storage bounds");

        VisualizationFrame spectrumMetadata;
        constexpr std::array supportedFftSizes {
            std::uint32_t { 1024 },
            std::uint32_t { 2048 },
            std::uint32_t { 4096 },
            std::uint32_t { 8192 },
            std::uint32_t { 16384 },
        };
        for (const auto supportedFftSize : supportedFftSizes) {
            spectrumMetadata.spectrumFftSize = supportedFftSize;
            spectrumMetadata.spectrumBinCount = (supportedFftSize / 2) + 1;
            expect(detail::hasSupportedSpectrumMetadata(spectrumMetadata));
        }

        spectrumMetadata.spectrumFftSize = 1536;
        spectrumMetadata.spectrumBinCount = 769;
        expect(!detail::hasSupportedSpectrumMetadata(spectrumMetadata));
        spectrumMetadata.spectrumFftSize = 16384;
        spectrumMetadata.spectrumBinCount = maximumSpectrumBinCount - 1;
        expect(!detail::hasSupportedSpectrumMetadata(spectrumMetadata));
        spectrumMetadata = { };
        expect(!detail::hasSupportedSpectrumMetadata(spectrumMetadata));

        beginTest("Telemetry reset publishes immediately while rendering is paused");

        StubVisualizationDataSource dataSource;
        MetalVisualization visualization(dataSource);
        expect(!visualization.isEffectivelyRendering());

        auto effectiveActivityChanges = 0;
        visualization.setEffectiveActivityCallback([&](bool) { ++effectiveActivityChanges; });
        visualization.setRenderingActive(false);
        visualization.setRenderingActive(true);
        visualization.setRenderingActive(true);
        expectEquals(effectiveActivityChanges, 0,
            "Detached render requests are not effective-activity transitions");
        visualization.setLoudnessSettings({ -18.5F });

        const auto beforeReset = visualization.getRenderTelemetry();
        visualization.resetRenderTelemetry();
        const auto afterReset = visualization.getRenderTelemetry();

        expectEquals(afterReset.epoch, beforeReset.epoch + 1);
        expect(!afterReset.resetPending);
        expectEquals(afterReset.displayLinkCallbacks, std::uint64_t { 0 });
        expectEquals(afterReset.presentedFrames, std::uint64_t { 0 });
        expectEquals(afterReset.gpuTimingSamples, std::uint64_t { 0 });
        expectEquals(afterReset.presentationLatenessSamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencySamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencyTotalTimingSamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencyTotalTimingUnavailableSamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencyComponentTimingSamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencyComponentTimingUnavailableSamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencyHistoryDiscardedSamples, std::uint64_t { 0 });
        expectEquals(afterReset.frameLatencyHistoryCount, std::size_t { 0 });
        expect(afterReset.spectrogramColumnsRead == 0);
        expect(afterReset.spectrogramColumnsUploaded == 0);
        expect(afterReset.spectrogramColumnsRejected == 0);
        expect(afterReset.spectrogramGapColumns == 0);
        expect(afterReset.spectrogramHistoryClears == 0);
        expect(afterReset.spectrogramTextureReallocations == 0);
        expect(afterReset.spectrogramTextureAllocationFailures == 0);
        expect(afterReset.spectrogramUploadBackpressureDrops == 0);
        expect(afterReset.spectrogramUploadDeferrals == 0);
        expect(afterReset.spectrogramScrollClockInitializations == 0);
        expect(afterReset.spectrogramScrollUnderrunFrames == 0);
        expectWithinAbsoluteError(afterReset.spectrogramScrollHeadOffsetColumns,
            beforeReset.spectrogramScrollHeadOffsetColumns, 0.000001);
        expect(afterReset.spectrogramUploadCommands == 0);
        expect(afterReset.spectrogramUploadBytes == 0);
        expect(afterReset.spectrogramLastColumnSequence == 0);
        expect(afterReset.lastStereoSequence == 0);
        expect(afterReset.stereoPointInstancesPrepared == 0);
        expect(afterReset.stereoPointDrawCalls == 0);
        expect(afterReset.stereoLastPointCount == beforeReset.stereoLastPointCount);
        expectWithinAbsoluteError(
            afterReset.stereoCorrelation, beforeReset.stereoCorrelation, 0.000001);
        expect(afterReset.stereoCorrelationValid == beforeReset.stereoCorrelationValid);
        expect(afterReset.stereoMono == beforeReset.stereoMono);
        expect(afterReset.lastLoudnessSequence == 0);
        expect(afterReset.loudnessMeasurementCapturedFrameEnd
            == beforeReset.loudnessMeasurementCapturedFrameEnd);
        expect(afterReset.loudnessIntegratedCapturedFrameEnd
            == beforeReset.loudnessIntegratedCapturedFrameEnd);
        expectWithinAbsoluteError(
            afterReset.loudnessMomentaryLufs, beforeReset.loudnessMomentaryLufs, 0.000001);
        expectWithinAbsoluteError(
            afterReset.loudnessShortTermLufs, beforeReset.loudnessShortTermLufs, 0.000001);
        expectWithinAbsoluteError(
            afterReset.loudnessIntegratedLufs, beforeReset.loudnessIntegratedLufs, 0.000001);
        expectWithinAbsoluteError(
            afterReset.loudnessReferenceLufs, beforeReset.loudnessReferenceLufs, 0.000001);
        expect(afterReset.loudnessMomentaryValid == beforeReset.loudnessMomentaryValid);
        expect(afterReset.loudnessShortTermValid == beforeReset.loudnessShortTermValid);
        expect(afterReset.loudnessIntegratedValid == beforeReset.loudnessIntegratedValid);
        expect(afterReset.metalAvailable == beforeReset.metalAvailable);
        expect(afterReset.renderingRequested == beforeReset.renderingRequested);
        expect(afterReset.effectivelyRendering == beforeReset.effectivelyRendering);

        visualization.resetRenderTelemetry();
        const auto afterSecondReset = visualization.getRenderTelemetry();
        expectEquals(afterSecondReset.epoch, afterReset.epoch + 1);
        expect(!afterSecondReset.resetPending);

        beginTest("Dashboard splits are published as one validated snapshot");

        expect(visualization.getDashboardLayoutSplits() == DashboardLayout::defaultSplits);

        constexpr DashboardLayoutSplits validSplits { 14, 24, 16, 36 };
        visualization.setDashboardLayoutSplits(validSplits);
        expect(visualization.getDashboardLayoutSplits() == validSplits);

        visualization.setDashboardLayoutSplits({ 22, 36, 35, 36 });
        expect(visualization.getDashboardLayoutSplits() == DashboardLayout::defaultSplits);

        beginTest("Dashboard layout editing is opt-in and preserves valid working splits");

        expect(!visualization.isDashboardLayoutEditing());
        visualization.setDashboardLayoutEditCancelCallback([] { });
        visualization.setDashboardLayoutEditing(true);
        expect(visualization.isDashboardLayoutEditing());

        constexpr DashboardLayoutSplits editingSplits { 26, 40, 20, 38 };
        visualization.setDashboardLayoutSplits(editingSplits);
        expect(visualization.getDashboardLayoutSplits() == editingSplits);

        visualization.setDashboardLayoutEditing(false);
        expect(!visualization.isDashboardLayoutEditing());
        expect(visualization.getDashboardLayoutSplits() == editingSplits);
        visualization.setDashboardLayoutEditCancelCallback({ });

        beginTest("Spectrum render settings publish one coherent presentation snapshot");

        visualization.setSpectrumSettings({ -180.0F, 12.0F, 4.5F, 0.65F, 0.50F, 0x123456U });
        const auto renderSettings = visualization.getSpectrumSettings();
        expectWithinAbsoluteError(renderSettings.floorDecibels, -180.0F, 0.001F);
        expectWithinAbsoluteError(renderSettings.ceilingDecibels, 12.0F, 0.001F);
        expectWithinAbsoluteError(renderSettings.slopeDecibelsPerOctave, 4.5F, 0.0001F);
        expectWithinAbsoluteError(renderSettings.frequencySpacing, 0.65F, 0.0001F);
        expectWithinAbsoluteError(renderSettings.fillOpacity, 0.50F, 0.0001F);
        expect(renderSettings.traceColourRgb == 0x123456U);

        visualization.setSpectrumSettings({ -40.0F, -24.0F, 3.0F, 1.0F, 0.0F, 0xffabcdefU });
        const auto minimumSpanSettings = visualization.getSpectrumSettings();
        expectWithinAbsoluteError(minimumSpanSettings.floorDecibels, -48.0F, 0.001F);
        expectWithinAbsoluteError(minimumSpanSettings.ceilingDecibels, -24.0F, 0.001F);
        expectWithinAbsoluteError(minimumSpanSettings.fillOpacity, 0.0F, 0.0001F);
        expect(minimumSpanSettings.traceColourRgb == 0xabcdefU);

        const auto notFinite = std::numeric_limits<float>::quiet_NaN();
        visualization.setSpectrumSettings(
            { notFinite, notFinite, notFinite, notFinite, notFinite });
        const auto defaultedSettings = visualization.getSpectrumSettings();
        expectWithinAbsoluteError(defaultedSettings.floorDecibels, -90.0F, 0.001F);
        expectWithinAbsoluteError(defaultedSettings.ceilingDecibels, 0.0F, 0.001F);
        expectWithinAbsoluteError(defaultedSettings.slopeDecibelsPerOctave, 0.0F, 0.0001F);
        expectWithinAbsoluteError(defaultedSettings.frequencySpacing, 1.0F, 0.0001F);
        expectWithinAbsoluteError(defaultedSettings.fillOpacity, 0.18F, 0.0001F);
        expect(defaultedSettings.traceColourRgb == 0x55c7e8U);

        beginTest("Spectrogram render settings sanitize and publish one coherent snapshot");

        visualization.setSpectrogramSettings({ detail::SpectrogramRenderPalette::viridis, 1.25F,
            -144.0F, -6.0F, 30, detail::SpectrogramRenderHistoryMode::overwrite, 120 });
        const auto spectrogramSettings = visualization.getSpectrogramSettings();
        expect(spectrogramSettings.palette == detail::SpectrogramRenderPalette::viridis);
        expectWithinAbsoluteError(spectrogramSettings.colorResponse, 1.25F, 0.0001F);
        expectWithinAbsoluteError(spectrogramSettings.colorFloorDecibels, -144.0F, 0.001F);
        expectWithinAbsoluteError(spectrogramSettings.colorCeilingDecibels, -6.0F, 0.001F);
        expectEquals(spectrogramSettings.historyDurationSeconds, 30);
        expect(spectrogramSettings.historyMode == detail::SpectrogramRenderHistoryMode::overwrite);
        expectEquals(spectrogramSettings.requestedSliceRateHz, 120);

        visualization.setSpectrogramSettings(
            { static_cast<detail::SpectrogramRenderPalette>(255), 9.0F, -36.0F, -24.0F, 999,
                static_cast<detail::SpectrogramRenderHistoryMode>(255), -99 });
        const auto boundedSpectrogramSettings = visualization.getSpectrogramSettings();
        expect(boundedSpectrogramSettings.palette == detail::SpectrogramRenderPalette::blueFire);
        expectWithinAbsoluteError(boundedSpectrogramSettings.colorResponse, 2.0F, 0.0001F);
        expectWithinAbsoluteError(boundedSpectrogramSettings.colorFloorDecibels, -48.0F, 0.001F);
        expectWithinAbsoluteError(boundedSpectrogramSettings.colorCeilingDecibels, -24.0F, 0.001F);
        expectEquals(boundedSpectrogramSettings.historyDurationSeconds, 60);
        expect(
            boundedSpectrogramSettings.historyMode == detail::SpectrogramRenderHistoryMode::scroll);
        expectEquals(boundedSpectrogramSettings.requestedSliceRateHz, 15);

        visualization.setSpectrogramSettings({ detail::SpectrogramRenderPalette::inferno, notFinite,
            notFinite, notFinite, 10, detail::SpectrogramRenderHistoryMode::scroll, 60 });
        const auto defaultedSpectrogramSettings = visualization.getSpectrogramSettings();
        expect(defaultedSpectrogramSettings.palette == detail::SpectrogramRenderPalette::inferno);
        expectWithinAbsoluteError(defaultedSpectrogramSettings.colorResponse, 0.0F, 0.0001F);
        expectWithinAbsoluteError(defaultedSpectrogramSettings.colorFloorDecibels, -120.0F, 0.001F);
        expectWithinAbsoluteError(defaultedSpectrogramSettings.colorCeilingDecibels, 0.0F, 0.001F);

        const auto discardsBeforePresentationChange = dataSource.spectrogramDiscardCount;
        dataSource.pendingSpectrogramColumns = 3;
        visualization.setSpectrogramSettings({ detail::SpectrogramRenderPalette::grayscale, 0.5F,
            -110.0F, -2.0F, 10, detail::SpectrogramRenderHistoryMode::overwrite, 60 });
        expectEquals(dataSource.spectrogramDiscardCount, discardsBeforePresentationChange);
        expectEquals(dataSource.pendingSpectrogramColumns, 3);

        visualization.setSpectrogramSettings({ detail::SpectrogramRenderPalette::grayscale, 0.5F,
            -110.0F, -2.0F, 20, detail::SpectrogramRenderHistoryMode::overwrite, 60 });
        expectEquals(dataSource.spectrogramDiscardCount, discardsBeforePresentationChange + 1);
        expectEquals(dataSource.pendingSpectrogramColumns, 0);

        beginTest("Loudness reference settings clamp and snap to half-LU steps");

        visualization.setLoudnessSettings({ -17.8F });
        expectWithinAbsoluteError(
            visualization.getLoudnessSettings().referenceLufs, -18.0F, 0.0001F);
        visualization.setLoudnessSettings({ -100.0F });
        expectWithinAbsoluteError(
            visualization.getLoudnessSettings().referenceLufs, -36.0F, 0.0001F);
        visualization.setLoudnessSettings({ 100.0F });
        expectWithinAbsoluteError(
            visualization.getLoudnessSettings().referenceLufs, -9.0F, 0.0001F);
        visualization.setLoudnessSettings({ notFinite });
        expectWithinAbsoluteError(
            visualization.getLoudnessSettings().referenceLufs, -23.0F, 0.0001F);

        visualization.setEffectiveActivityCallback({ });
    }
};

static MetalVisualizationTests metalVisualizationTests;
} // namespace
} // namespace audio_insight
