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

class StubVisualizationDataSource final : public VisualizationDataSource {
public:
    void requestAnalysis() noexcept override
    {
    }

    void setVisualizationActive(bool) noexcept override
    {
    }

    void resetPeakRms() noexcept override
    {
    }

    [[nodiscard]] bool copyLatestVisualizationFrame(VisualizationFrame&) const noexcept override
    {
        return false;
    }
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
        for (const auto frequency : detail::frequencyAxisTickCandidates) {
            if (frequency > lowNyquistMapping.maximumFrequencyHz)
                continue;

            const auto position = detail::mapFrequencyToUnit(lowNyquistMapping, frequency);
            expect(position >= previousPosition);
            previousPosition = position;
        }

        expectEquals(detail::mapFrequencyToUnit({ 20.0F, 20.0F, 1.0F }, 20.0F), 0.0F);

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

        std::array<float, detail::frequencyAxisTickCandidateCount> compactExtents { };
        compactExtents.fill(10.0F);
        const auto lowNyquistTicks = detail::selectFrequencyAxisTicks(
            { 20.0F, 11'025.0F, 1.0F }, 2'000.0F, compactExtents, 10.0F);
        expectEquals(lowNyquistTicks.count, std::size_t { 10 });
        expectEquals(lowNyquistTicks.ticks[0].frequencyHz, 20.0F);
        expectEquals(lowNyquistTicks.ticks[lowNyquistTicks.count - 1].frequencyHz, 11'025.0F);
        expect(lowNyquistTicks.ticks[lowNyquistTicks.count - 1].usesUpperEndpointLabel);
        for (std::size_t index = 1; index < lowNyquistTicks.count; ++index)
            expect(lowNyquistTicks.ticks[index - 1].frequencyHz
                < lowNyquistTicks.ticks[index].frequencyHz);

        const auto deduplicatedEndpoint
            = detail::selectFrequencyAxisTicks(logarithmicMapping, 1'000.0F, compactExtents, 10.0F);
        expectEquals(deduplicatedEndpoint.count, detail::frequencyAxisTickCandidateCount);
        expect(!deduplicatedEndpoint.ticks[deduplicatedEndpoint.count - 1].usesUpperEndpointLabel);

        std::array<float, detail::frequencyAxisTickCandidateCount> wideExtents { };
        wideExtents.fill(30.0F);
        const detail::FrequencyAxisMapping narrowLinearMapping { 20.0F, 200.0F, 0.0F };
        const auto culledTicks
            = detail::selectFrequencyAxisTicks(narrowLinearMapping, 180.0F, wideExtents, 30.0F);
        expectEquals(culledTicks.count, std::size_t { 3 });
        expectEquals(culledTicks.ticks[0].frequencyHz, 20.0F);
        expectEquals(culledTicks.ticks[1].frequencyHz, 100.0F);
        expectEquals(culledTicks.ticks[2].frequencyHz, 200.0F);

        const auto verifyNoOverlap = [this](const detail::FrequencyAxisMapping& mapping,
                                         const float axisLength,
                                         const detail::FrequencyAxisTickSelection& selection,
                                         const auto& candidateExtents, const float endpointExtent) {
            auto previousEnd = -4.0F;
            for (std::size_t index = 0; index < selection.count; ++index) {
                const auto& tick = selection.ticks[index];
                const auto extent = tick.usesUpperEndpointLabel
                    ? endpointExtent
                    : candidateExtents[tick.candidateIndex];
                const auto centre
                    = detail::mapFrequencyToUnit(mapping, tick.frequencyHz) * axisLength;
                const auto start = std::clamp(centre - (extent * 0.5F), 0.0F, axisLength - extent);
                expect(start >= previousEnd + 4.0F - 0.0001F);
                previousEnd = start + extent;
            }
        };
        verifyNoOverlap(narrowLinearMapping, 180.0F, culledTicks, wideExtents, 30.0F);

        std::array<float, detail::frequencyAxisTickCandidateCount> verticalExtents { };
        verticalExtents.fill(12.0F);
        const auto verticalTicks
            = detail::selectFrequencyAxisTicks(logarithmicMapping, 140.0F, verticalExtents, 12.0F);
        expect(verticalTicks.count < detail::frequencyAxisTickCandidateCount);
        verifyNoOverlap(logarithmicMapping, 140.0F, verticalTicks, verticalExtents, 12.0F);

        std::array<float, detail::frequencyAxisTickCandidateCount> impossibleExtents { };
        impossibleExtents.fill(30.0F);
        const auto impossibleTicks
            = detail::selectFrequencyAxisTicks(logarithmicMapping, 30.0F, impossibleExtents, 30.0F);
        expectEquals(impossibleTicks.count, std::size_t { 1 });

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

        beginTest("Axis and meter geometry remain within the fixed Metal vertex buffer");

        expectEquals(detail::MetalVisualizationGeometryLimits::maximumGeneratedVertices,
            std::size_t { 8'406 });
        expect(detail::MetalVisualizationGeometryLimits::maximumGeneratedVertices
            <= detail::MetalVisualizationGeometryLimits::vertexCapacity);

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

        beginTest("Spectrum render settings publish the shared frequency spacing atomically");

        visualization.setSpectrumSettings({ -180.0F, 12.0F, 0.25F, 0.65F });
        const auto renderSettings = visualization.getSpectrumSettings();
        expectWithinAbsoluteError(renderSettings.floorDecibels, -180.0F, 0.001F);
        expectWithinAbsoluteError(renderSettings.ceilingDecibels, 12.0F, 0.001F);
        expectWithinAbsoluteError(renderSettings.smoothing, 0.25F, 0.0001F);
        expectWithinAbsoluteError(renderSettings.frequencySpacing, 0.65F, 0.0001F);

        visualization.setSpectrumSettings({ -40.0F, -24.0F, 0.40F, 1.0F });
        const auto minimumSpanSettings = visualization.getSpectrumSettings();
        expectWithinAbsoluteError(minimumSpanSettings.floorDecibels, -48.0F, 0.001F);
        expectWithinAbsoluteError(minimumSpanSettings.ceilingDecibels, -24.0F, 0.001F);

        const auto notFinite = std::numeric_limits<float>::quiet_NaN();
        visualization.setSpectrumSettings({ notFinite, notFinite, notFinite, notFinite });
        const auto defaultedSettings = visualization.getSpectrumSettings();
        expectWithinAbsoluteError(defaultedSettings.floorDecibels, -90.0F, 0.001F);
        expectWithinAbsoluteError(defaultedSettings.ceilingDecibels, 0.0F, 0.001F);
        expectWithinAbsoluteError(defaultedSettings.smoothing, 0.35F, 0.0001F);
        expectWithinAbsoluteError(defaultedSettings.frequencySpacing, 1.0F, 0.0001F);

        visualization.setEffectiveActivityCallback({ });
    }
};

static MetalVisualizationTests metalVisualizationTests;
} // namespace
} // namespace audio_insight
