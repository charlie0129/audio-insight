// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PerformanceMetricsModel.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace audio_insight {
namespace {
struct AggregateInitializer {
    template <typename Value> constexpr operator Value() const noexcept;
};

template <typename Aggregate, std::size_t... Indices>
consteval bool canAggregateInitialise(std::index_sequence<Indices...>)
{
    return requires { Aggregate { (static_cast<void>(Indices), AggregateInitializer { })... }; };
}

template <typename Aggregate, std::size_t FieldCount = 0>
consteval std::size_t aggregateFieldCount()
{
    static_assert(FieldCount < 128, "Telemetry aggregate unexpectedly large");

    if constexpr (canAggregateInitialise<Aggregate>(std::make_index_sequence<FieldCount + 1> { }))
        return aggregateFieldCount<Aggregate, FieldCount + 1>();
    else
        return FieldCount;
}

// These guards force a deliberate field-name mapping update whenever a raw
// telemetry aggregate gains or loses a member. Nested aggregates are guarded
// separately because the view model expands them into individual rows.
static_assert(aggregateFieldCount<PresentedFrameIntervalSample>() == 2);
static_assert(aggregateFieldCount<FrameLatencySample>() == 9);
static_assert(aggregateFieldCount<MetalRenderTelemetry>() == 86);
static_assert(aggregateFieldCount<StereoSampleCapture::Telemetry>() == 9);
static_assert(aggregateFieldCount<StereoMeterAccumulator::Telemetry>() == 7);
static_assert(aggregateFieldCount<SharedAnalysisScheduler::Counters>() == 3);
static_assert(aggregateFieldCount<AnalysisTelemetry>() == 59);

constexpr std::array<std::string_view, 161> expectedRawFieldNames {
    "metal.epoch",
    "metal.displayLinkCallbacks",
    "metal.submittedFrames",
    "metal.completedFrames",
    "metal.gpuTimingSamples",
    "metal.gpuTimingUnavailableSamples",
    "metal.commandBufferFailures",
    "metal.presentationCallbacks",
    "metal.presentedFrames",
    "metal.presentationLatenessSamples",
    "metal.presentationLatenessUnclassifiableSamples",
    "metal.presentationHistoryDiscardedTimestamps",
    "metal.frameLatencySamples",
    "metal.frameLatencyTotalTimingSamples",
    "metal.frameLatencyTotalTimingUnavailableSamples",
    "metal.frameLatencyComponentTimingSamples",
    "metal.frameLatencyComponentTimingUnavailableSamples",
    "metal.frameLatencyHistoryDiscardedSamples",
    "metal.presentationsAfterTarget",
    "metal.skippedPresentations",
    "metal.gpuBackpressureDrops",
    "metal.drawableUnavailableDrops",
    "metal.callbackHostDelaySamples",
    "metal.callbackHostDelayUnclassifiableSamples",
    "metal.callbackAlreadyLateHostDelays",
    "metal.cpuCommitLatenessSamples",
    "metal.cpuCommitLatenessUnclassifiableSamples",
    "metal.cpuCommitDeadlineMisses",
    "metal.gpuCompletionLatenessSamples",
    "metal.gpuCompletionLatenessUnclassifiableSamples",
    "metal.gpuCompletionDeadlineMisses",
    "metal.analysisRequestCalls",
    "metal.snapshotReads",
    "metal.framesWithNewSnapshot",
    "metal.lastSpectrumSequence",
    "metal.spectrogramColumnsRead",
    "metal.spectrogramColumnsUploaded",
    "metal.spectrogramColumnsRejected",
    "metal.spectrogramGapColumns",
    "metal.spectrogramHistoryClears",
    "metal.spectrogramTextureReallocations",
    "metal.spectrogramTextureAllocationFailures",
    "metal.spectrogramUploadBackpressureDrops",
    "metal.spectrogramUploadCommands",
    "metal.spectrogramUploadBytes",
    "metal.spectrogramLastColumnSequence",
    "metal.lastStereoSequence",
    "metal.stereoPointInstancesPrepared",
    "metal.stereoPointDrawCalls",
    "metal.lastCpuEncodeNanoseconds",
    "metal.maximumCpuEncodeNanoseconds",
    "metal.lastGpuExecutionNanoseconds",
    "metal.maximumGpuExecutionNanoseconds",
    "metal.lastDisplayCallbackIntervalNanoseconds",
    "metal.lastTargetIntervalNanoseconds",
    "metal.lastTargetPresentationIntervalNanoseconds",
    "metal.lastPresentedFrameIntervalNanoseconds",
    "metal.lastPresentedHostTimestampNanoseconds",
    "metal.lastPresentationLatenessNanoseconds",
    "metal.maximumPresentationLatenessNanoseconds",
    "metal.lastCallbackHostDelayNanoseconds",
    "metal.lastCpuCommitLatenessNanoseconds",
    "metal.lastGpuCompletionLatenessNanoseconds",
    "metal.lastTargetTimestampNanoseconds",
    "metal.lastTargetPresentationTimestampNanoseconds",
    "metal.lastProvidedDrawableAccessNanoseconds",
    "metal.maximumProvidedDrawableAccessNanoseconds",
    "metal.presentedFrameIntervalHistory",
    "metal.presentedFrameIntervalHistoryCount",
    "metal.frameLatencyHistory",
    "metal.frameLatencyHistoryCount",
    "metal.drawableWidthPixels",
    "metal.drawableHeightPixels",
    "metal.configuredMaximumFramesPerSecond",
    "metal.spectrogramTextureRows",
    "metal.spectrogramTextureColumns",
    "metal.spectrogramTextureBytes",
    "metal.stereoLastPointCount",
    "metal.backingScale",
    "metal.stereoCorrelation",
    "metal.metalAvailable",
    "metal.renderingRequested",
    "metal.effectivelyRendering",
    "metal.resetPending",
    "metal.stereoCorrelationValid",
    "metal.stereoMono",
    "analysis.capture.attemptedChunks",
    "analysis.capture.publishedChunks",
    "analysis.capture.reclaimedReadyChunks",
    "analysis.capture.droppedIncomingChunks",
    "analysis.capture.consumerDiscontinuities",
    "analysis.capture.lastAttemptedSequence",
    "analysis.capture.capturedFrames",
    "analysis.capture.readyHighWaterMark",
    "analysis.capture.readySlots",
    "analysis.meters.attemptedBlocks",
    "analysis.meters.publishedBlocks",
    "analysis.meters.coalescedBlocks",
    "analysis.meters.droppedBlocks",
    "analysis.meters.consumerDiscontinuities",
    "analysis.meters.readyHighWaterMark",
    "analysis.meters.readySlots",
    "analysis.stereoFieldProcessedChunks",
    "analysis.stereoFieldProcessedFrames",
    "analysis.stereoFieldSelectedPoints",
    "analysis.stereoFieldHistoryResets",
    "analysis.stereoFieldInvalidChunks",
    "analysis.stereoCorrelationProcessedSamples",
    "analysis.stereoCorrelationPublishedEndpoints",
    "analysis.stereoCorrelationConsumedEndpoints",
    "analysis.stereoCorrelationStateResets",
    "analysis.stereoCapturedFrameEnd",
    "analysis.stereoSequence",
    "analysis.stereoFieldPointCount",
    "analysis.stereoPointStrideFrames",
    "analysis.stereoFieldValid",
    "analysis.stereoCorrelationValid",
    "analysis.stereoMono",
    "analysis.scheduler.submitted",
    "analysis.scheduler.executed",
    "analysis.scheduler.cancelled",
    "analysis.fftConfigurationChanges",
    "analysis.spectrumTemporalConfigurationChanges",
    "analysis.spectrogramTransformsOffered",
    "analysis.spectrogramColumnsMapped",
    "analysis.spectrogramMappingFailures",
    "analysis.spectrogramColumnsPublished",
    "analysis.spectrogramColumnsReclaimed",
    "analysis.spectrogramColumnsDropped",
    "analysis.spectrogramColumnsConsumed",
    "analysis.spectrogramColumnsDiscarded",
    "analysis.spectrogramMappingChanges",
    "analysis.spectrogramCapturedFrameEnd",
    "analysis.spectrogramMappingGeneration",
    "analysis.fftGeneration",
    "analysis.configuredFftSize",
    "analysis.configuredFftWindow",
    "analysis.requestedFftSliceRateHz",
    "analysis.spectrogramRowCount",
    "analysis.spectrogramQueueReadyHighWaterMark",
    "analysis.spectrogramQueueReadyColumns",
    "analysis.jobsStarted",
    "analysis.jobsCompleted",
    "analysis.jobsStopped",
    "analysis.ignoredGenerationChunks",
    "analysis.publishedFrames",
    "analysis.droppedFramePublications",
    "analysis.lastJobNanoseconds",
    "analysis.maximumJobNanoseconds",
    "analysis.spectrumTransforms",
    "analysis.lastJobSpectrumTransforms",
    "analysis.maximumJobSpectrumTransforms",
    "analysis.backlogDiscardedFrames",
    "analysis.spectrumCapturedFrameEnd",
    "analysis.meterCapturedFrameEnd",
    "analysis.latestCaptureRevision",
    "analysis.lastAnalyzedCaptureRevision",
    "analysis.emptyAnalysisRequestsAvoided",
    "analysis.staleFramesPublished",
    "analysis.peakRmsUserResets",
    "analysis.spectrumUserClears",
};

const PerformanceMetricRate* findRate(
    const PerformanceMetricsViewModel& view, const std::string_view sourceFieldName)
{
    const auto match = std::find_if(view.derived.rates.begin(), view.derived.rates.end(),
        [sourceFieldName](const auto& rate) { return rate.sourceFieldName == sourceFieldName; });
    return match != view.derived.rates.end() ? &*match : nullptr;
}

const PerformanceMetricRow* findRawRow(
    const PerformanceMetricsViewModel& view, const std::string_view fieldName)
{
    for (const auto& section : view.sections) {
        const auto match
            = std::find_if(section.rows.begin(), section.rows.end(), [fieldName](const auto& row) {
                  return row.kind == PerformanceMetricKind::raw && row.fieldName == fieldName;
              });
        if (match != section.rows.end())
            return &*match;
    }

    return nullptr;
}

void expectFiniteStatistics(juce::UnitTest& test, const FrameIntervalStatistics& statistics)
{
    test.expect(std::isfinite(statistics.latestMilliseconds));
    test.expect(std::isfinite(statistics.minimumMilliseconds));
    test.expect(std::isfinite(statistics.meanMilliseconds));
    test.expect(std::isfinite(statistics.percentile95Milliseconds));
    test.expect(std::isfinite(statistics.maximumMilliseconds));
    test.expect(std::isfinite(statistics.standardDeviationMilliseconds));
    test.expect(std::isfinite(statistics.equivalentHertz));
}

class PerformanceMetricsModelTests final : public juce::UnitTest {
public:
    PerformanceMetricsModelTests() : UnitTest("Performance metrics model", "audio-insight")
    {
    }

    void runTest() override
    {
        testCase("Exact presented-frame history produces 120 Hz statistics", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 7;
            snapshot.metal.presentedFrameIntervalHistoryCount
                = presentedFrameIntervalHistoryCapacity;

            for (std::size_t index = 0; index < presentedFrameIntervalHistoryCapacity; ++index) {
                snapshot.metal.presentedFrameIntervalHistory[index] = {
                    index + 1,
                    index % 2 == 0 ? std::uint64_t { 8'333'333 } : std::uint64_t { 8'333'334 },
                };
            }

            snapshot.metal.lastPresentedFrameIntervalNanoseconds = 8'333'334;
            const auto view = model.update(snapshot, 10.0);
            const auto& statistics = view.derived.frameIntervals.presentedFrames;

            expect(statistics.available);
            expectEquals(statistics.sampleCount, presentedFrameIntervalHistoryCapacity);
            expectWithinAbsoluteError(statistics.latestMilliseconds, 8.333334, 1.0e-9);
            expectWithinAbsoluteError(statistics.minimumMilliseconds, 8.333333, 1.0e-9);
            expectWithinAbsoluteError(statistics.meanMilliseconds, 8.3333335, 1.0e-9);
            expectWithinAbsoluteError(statistics.percentile95Milliseconds, 8.333334, 1.0e-9);
            expectWithinAbsoluteError(statistics.maximumMilliseconds, 8.333334, 1.0e-9);
            expectWithinAbsoluteError(statistics.standardDeviationMilliseconds, 0.0000005, 1.0e-9);
            expectWithinAbsoluteError(statistics.equivalentHertz, 120.0, 1.0e-4);
            expect(view.report.find("metal.presentedFrameIntervalHistory = [1:8333333")
                != std::string::npos);
            expect(view.report.find("240:8333334") != std::string::npos);
            expect(
                view.report.find("statistics.metal.presentedFrameIntervals.minimumMilliseconds = "
                                 "8.3333329999999997 ms")
                != std::string::npos);
            expect(view.report.find(
                       "statistics.metal.presentedFrameIntervals.standardDeviationMilliseconds = ")
                != std::string::npos);
        });

        testCase("Per-frame latency history preserves exact component values", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 3;
            snapshot.metal.frameLatencyHistoryCount = 2;
            snapshot.metal.frameLatencyHistory[0]
                = { 17, 1'234'567'890, 101, 202, 303, 404, 1'010, true, true };
            snapshot.metal.frameLatencyHistory[1]
                = { 19, 1'234'567'999, 111, 0, 0, 0, 999, true, false };

            const auto view = model.update(snapshot, 1.0, false);
            expect(view.report.empty());

            auto foundDeferredHistory = false;
            for (const auto& section : view.sections) {
                for (const auto& row : section.rows) {
                    if (row.fieldName != "metal.frameLatencyHistory")
                        continue;

                    foundDeferredHistory = true;
                    expect(row.rawValue.empty());
                    expectEquals(row.rawUnit,
                        std::string("sequence:presented_host_timestamp_nanoseconds:"
                                    "cpu_encode_nanoseconds:submit_queue_wait_nanoseconds:"
                                    "gpu_execution_nanoseconds:compositor_wait_nanoseconds:"
                                    "total_nanoseconds:total_valid:components_valid"));
                }
            }
            expect(foundDeferredHistory);

            const auto report = PerformanceMetricsModel::buildCopyReport(view, snapshot);
            expect(report.find("metal.frameLatencyHistory = "
                               "[17:1234567890:101:202:303:404:1010:true:true, "
                               "19:1234567999:111:0:0:0:999:true:false] "
                               "sequence:presented_host_timestamp_nanoseconds:"
                               "cpu_encode_nanoseconds:submit_queue_wait_nanoseconds:"
                               "gpu_execution_nanoseconds:compositor_wait_nanoseconds:"
                               "total_nanoseconds:total_valid:components_valid")
                != std::string::npos);
        });

        testCase("Every frame-latency counter exposes a derived rate", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 5;
            static_cast<void>(model.update(snapshot, 10.0));

            snapshot.metal.frameLatencySamples = 20;
            snapshot.metal.frameLatencyTotalTimingSamples = 20;
            snapshot.metal.frameLatencyTotalTimingUnavailableSamples = 20;
            snapshot.metal.frameLatencyComponentTimingSamples = 20;
            snapshot.metal.frameLatencyComponentTimingUnavailableSamples = 20;
            snapshot.metal.frameLatencyHistoryDiscardedSamples = 20;
            const auto view = model.update(snapshot, 12.0);

            constexpr std::array<std::string_view, 6> rateFieldNames {
                "metal.frameLatencySamples",
                "metal.frameLatencyTotalTimingSamples",
                "metal.frameLatencyTotalTimingUnavailableSamples",
                "metal.frameLatencyComponentTimingSamples",
                "metal.frameLatencyComponentTimingUnavailableSamples",
                "metal.frameLatencyHistoryDiscardedSamples",
            };

            for (const auto fieldName : rateFieldNames) {
                const auto* rate = findRate(view, fieldName);
                expect(rate != nullptr && rate->available,
                    "Missing derived rate for a frame-latency counter");
                if (rate != nullptr)
                    expectWithinAbsoluteError(rate->value, 10.0, 1.0e-12);
            }
        });

        testCase(
            "Spectrogram upload backpressure is observable and copy commands are explicit", [this] {
                PerformanceMetricsModel model;
                PerformanceMetricsSnapshot snapshot;
                snapshot.metal.epoch = 6;
                static_cast<void>(model.update(snapshot, 20.0));

                snapshot.metal.spectrogramUploadBackpressureDrops = 9;
                snapshot.metal.spectrogramUploadCommands = 12;
                const auto view = model.update(snapshot, 22.0);

                const auto* dropRate = findRate(view, "metal.spectrogramUploadBackpressureDrops");
                expect(dropRate != nullptr && dropRate->available);
                if (dropRate != nullptr) {
                    expectWithinAbsoluteError(dropRate->value, 4.5, 1.0e-12);
                    expectEquals(
                        dropRate->label, std::string("Spectrogram upload-backpressure drops"));
                }

                const auto* commandRate = findRate(view, "metal.spectrogramUploadCommands");
                expect(commandRate != nullptr && commandRate->available);
                if (commandRate != nullptr) {
                    expectWithinAbsoluteError(commandRate->value, 6.0, 1.0e-12);
                    expectEquals(
                        commandRate->label, std::string("Spectrogram column copy commands"));
                }

                const auto* dropRow = findRawRow(view, "metal.spectrogramUploadBackpressureDrops");
                expect(dropRow != nullptr);
                if (dropRow != nullptr) {
                    expectEquals(dropRow->label,
                        std::string("Callbacks skipped for an outstanding history upload"));
                }

                const auto* commandRow = findRawRow(view, "metal.spectrogramUploadCommands");
                expect(commandRow != nullptr);
                if (commandRow != nullptr)
                    expectEquals(commandRow->label, std::string("History column copy commands"));
            });

        testCase("Renderer Stereo telemetry has one group and live rates", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 8;
            static_cast<void>(model.update(snapshot, 30.0));

            snapshot.metal.lastStereoSequence = 19;
            snapshot.metal.stereoPointInstancesPrepared = 480;
            snapshot.metal.stereoPointDrawCalls = 240;
            snapshot.metal.stereoLastPointCount = 2048;
            snapshot.metal.stereoCorrelation = -0.375;
            snapshot.metal.stereoCorrelationValid = true;
            snapshot.metal.stereoMono = false;
            const auto view = model.update(snapshot, 32.0);

            std::size_t stereoGroupCount = 0;
            std::set<std::string> stereoGroupFields;
            for (const auto& section : view.sections) {
                if (section.name != "Renderer Stereo field")
                    continue;

                ++stereoGroupCount;
                for (const auto& row : section.rows)
                    stereoGroupFields.emplace(row.fieldName);
            }

            const std::set<std::string> expectedStereoGroupFields {
                "metal.lastStereoSequence",
                "metal.stereoPointInstancesPrepared",
                "metal.stereoPointDrawCalls",
                "metal.stereoLastPointCount",
                "metal.stereoCorrelation",
                "metal.stereoCorrelationValid",
                "metal.stereoMono",
            };
            expectEquals(stereoGroupCount, std::size_t { 1 });
            expect(stereoGroupFields == expectedStereoGroupFields);

            const auto* instanceRate = findRate(view, "metal.stereoPointInstancesPrepared");
            expect(instanceRate != nullptr && instanceRate->available);
            if (instanceRate != nullptr) {
                expectWithinAbsoluteError(instanceRate->value, 240.0, 1.0e-12);
                expectEquals(instanceRate->label, std::string("Stereo point instances prepared"));
                expectEquals(instanceRate->unit, std::string("instances/s"));
            }

            const auto* drawRate = findRate(view, "metal.stereoPointDrawCalls");
            expect(drawRate != nullptr && drawRate->available);
            if (drawRate != nullptr) {
                expectWithinAbsoluteError(drawRate->value, 120.0, 1.0e-12);
                expectEquals(drawRate->label, std::string("Stereo point draw calls"));
                expectEquals(drawRate->unit, std::string("calls/s"));
            }

            const auto* correlationRow = findRawRow(view, "metal.stereoCorrelation");
            expect(correlationRow != nullptr);
            if (correlationRow != nullptr) {
                expectEquals(correlationRow->value, std::string("-0.375"));
                expectEquals(correlationRow->rawValue, std::string("-0.375"));
            }
        });

        testCase("Stereo analysis telemetry has one complete group and useful rates", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 9;
            static_cast<void>(model.update(snapshot, 40.0));

            snapshot.analysis.stereoFieldProcessedChunks = 12;
            snapshot.analysis.stereoFieldProcessedFrames = 24'000;
            snapshot.analysis.stereoFieldSelectedPoints = 8'000;
            snapshot.analysis.stereoFieldHistoryResets = 2;
            snapshot.analysis.stereoFieldInvalidChunks = 1;
            snapshot.analysis.stereoCorrelationProcessedSamples = 24'000;
            snapshot.analysis.stereoCorrelationPublishedEndpoints = 12;
            snapshot.analysis.stereoCorrelationConsumedEndpoints = 10;
            snapshot.analysis.stereoCorrelationStateResets = 2;
            snapshot.analysis.stereoCapturedFrameEnd = 24'000;
            snapshot.analysis.stereoSequence = 6;
            snapshot.analysis.stereoFieldPointCount = 4'000;
            snapshot.analysis.stereoPointStrideFrames = 3;
            snapshot.analysis.stereoFieldValid = true;
            snapshot.analysis.stereoCorrelationValid = true;
            snapshot.analysis.stereoMono = false;
            const auto view = model.update(snapshot, 42.0);

            std::set<std::string> fields;
            auto groupCount = std::size_t { 0 };
            for (const auto& section : view.sections) {
                if (section.name != "Stereo analysis and correlation")
                    continue;

                ++groupCount;
                for (const auto& row : section.rows)
                    fields.emplace(row.fieldName);
            }

            expectEquals(groupCount, std::size_t { 1 });
            expectEquals(fields.size(), std::size_t { 16 });
            expect(fields.contains("analysis.stereoFieldProcessedChunks"));
            expect(fields.contains("analysis.stereoCorrelationProcessedSamples"));
            expect(fields.contains("analysis.stereoSequence"));
            expect(fields.contains("analysis.stereoCorrelationValid"));
            expect(fields.contains("analysis.stereoMono"));

            const auto* frameRate = findRate(view, "analysis.stereoFieldProcessedFrames");
            expect(frameRate != nullptr && frameRate->available);
            if (frameRate != nullptr)
                expectWithinAbsoluteError(frameRate->value, 12'000.0, 1.0e-12);

            const auto* correlationRate
                = findRate(view, "analysis.stereoCorrelationProcessedSamples");
            expect(correlationRate != nullptr && correlationRate->available);
            if (correlationRate != nullptr)
                expectWithinAbsoluteError(correlationRate->value, 12'000.0, 1.0e-12);

            const auto* updateRate = findRate(view, "analysis.stereoSequence");
            expect(updateRate != nullptr && updateRate->available);
            if (updateRate != nullptr)
                expectWithinAbsoluteError(updateRate->value, 3.0, 1.0e-12);
        });

        testCase("Counter rates rebase across epochs and reject rollbacks", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 1;
            snapshot.metal.displayLinkCallbacks = 10;
            snapshot.metal.presentedFrames = 5;
            snapshot.analysis.capture.attemptedChunks = 20;

            auto view = model.update(snapshot, 100.0);
            expect(view.derived.ratesRebased);
            auto* callbackRate = findRate(view, "metal.displayLinkCallbacks");
            expect(callbackRate != nullptr);
            if (callbackRate != nullptr)
                expect(!callbackRate->available);

            snapshot.metal.displayLinkCallbacks += 240;
            snapshot.metal.presentedFrames += 238;
            snapshot.analysis.capture.attemptedChunks += 100;
            view = model.update(snapshot, 102.0);
            expect(!view.derived.ratesRebased);

            callbackRate = findRate(view, "metal.displayLinkCallbacks");
            const auto* presentedRate = findRate(view, "metal.presentedFrames");
            const auto* captureRate = findRate(view, "analysis.capture.attemptedChunks");
            expect(callbackRate != nullptr && callbackRate->available);
            expect(presentedRate != nullptr && presentedRate->available);
            expect(captureRate != nullptr && captureRate->available);

            if (callbackRate != nullptr)
                expectWithinAbsoluteError(callbackRate->value, 120.0, 1.0e-12);
            if (presentedRate != nullptr)
                expectWithinAbsoluteError(presentedRate->value, 119.0, 1.0e-12);
            if (captureRate != nullptr)
                expectWithinAbsoluteError(captureRate->value, 50.0, 1.0e-12);

            if (callbackRate != nullptr) {
                expectEquals(callbackRate->counterDelta, std::uint64_t { 240 });
                expectWithinAbsoluteError(callbackRate->sampleIntervalSeconds, 2.0, 1.0e-12);
            }

            expect(view.report.find("rate.metal.displayLinkCallbacks = 120 callbacks/s "
                                    "(counter_delta = 240; sample_interval_seconds = 2)")
                != std::string::npos);

            snapshot.metal.epoch = 2;
            snapshot.metal.displayLinkCallbacks = 1;
            snapshot.metal.presentedFrames = 1;
            snapshot.analysis.capture.attemptedChunks += 10;
            view = model.update(snapshot, 103.0);
            expect(view.derived.ratesRebased);
            callbackRate = findRate(view, "metal.displayLinkCallbacks");
            expect(callbackRate != nullptr && !callbackRate->available);

            snapshot.metal.displayLinkCallbacks = 121;
            snapshot.metal.presentedFrames = 121;
            snapshot.analysis.capture.attemptedChunks += 50;
            view = model.update(snapshot, 104.0);
            callbackRate = findRate(view, "metal.displayLinkCallbacks");
            captureRate = findRate(view, "analysis.capture.attemptedChunks");
            expect(callbackRate != nullptr && callbackRate->available);
            expect(captureRate != nullptr && captureRate->available);

            snapshot.metal.displayLinkCallbacks += 120;
            snapshot.analysis.capture.attemptedChunks = 1;
            view = model.update(snapshot, 105.0);
            callbackRate = findRate(view, "metal.displayLinkCallbacks");
            captureRate = findRate(view, "analysis.capture.attemptedChunks");
            expect(callbackRate != nullptr && callbackRate->available);
            expect(captureRate != nullptr && !captureRate->available);

            model.reset();
            view = model.update(snapshot, 106.0);
            expect(view.derived.ratesRebased);
        });

        testCase("Invalid timing input never produces NaN or infinity", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 4;
            snapshot.metal.backingScale = std::numeric_limits<double>::quiet_NaN();
            snapshot.metal.lastDisplayCallbackIntervalNanoseconds = 0;
            snapshot.metal.lastTargetIntervalNanoseconds = 0;
            snapshot.metal.lastTargetPresentationIntervalNanoseconds = 0;
            snapshot.metal.presentedFrameIntervalHistoryCount = 1;
            snapshot.metal.presentedFrameIntervalHistory[0] = { 1, 0 };

            auto view = model.update(snapshot, std::numeric_limits<double>::quiet_NaN());
            view = model.update(snapshot, std::numeric_limits<double>::infinity());

            snapshot.metal.displayLinkCallbacks = 10;
            view = model.update(snapshot, -std::numeric_limits<double>::max());
            snapshot.metal.displayLinkCallbacks = 20;
            view = model.update(snapshot, std::numeric_limits<double>::max());
            const auto* overflowedIntervalRate = findRate(view, "metal.displayLinkCallbacks");
            expect(view.derived.ratesRebased);
            expect(overflowedIntervalRate != nullptr && !overflowedIntervalRate->available);

            for (const auto& rate : view.derived.rates)
                expect(std::isfinite(rate.value));

            expectFiniteStatistics(*this, view.derived.frameIntervals.displayCallbacks);
            expectFiniteStatistics(*this, view.derived.frameIntervals.targetCallbacks);
            expectFiniteStatistics(*this, view.derived.frameIntervals.targetPresentations);
            expectFiniteStatistics(*this, view.derived.frameIntervals.presentedFrames);

            auto report = view.report;
            std::transform(
                report.begin(), report.end(), report.begin(), [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            expect(report.find(" = nan") == std::string::npos);
            expect(report.find(" = inf") == std::string::npos);
        });

        testCase("Every raw telemetry field has exactly one stable row", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            const auto view = model.update(snapshot, 1.0);

            std::set<std::string> actual;
            for (const auto& section : view.sections) {
                for (const auto& row : section.rows) {
                    if (row.kind != PerformanceMetricKind::raw)
                        continue;

                    expect(actual.emplace(row.fieldName).second, "Duplicate raw metrics row");
                }
            }

            std::set<std::string> expected;
            for (const auto fieldName : expectedRawFieldNames)
                expected.emplace(fieldName);

            expectEquals(actual.size(), expectedRawFieldNames.size());
            expect(
                actual == expected, "Raw field mapping is incomplete or contains an unknown key");

            for (const auto fieldName : expectedRawFieldNames) {
                const auto reportEntry = std::string(fieldName) + " = ";
                expect(view.report.find(reportEntry) != std::string::npos,
                    "Copyable report omitted a raw telemetry field");
            }
        });

        testCase("The copy report can be generated lazily", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 9;
            snapshot.metal.submittedFrames = 12;
            snapshot.metal.presentedFrameIntervalHistoryCount = 1;
            snapshot.metal.presentedFrameIntervalHistory[0] = { 17, 8'333'333 };
            snapshot.metal.frameLatencyHistoryCount = 1;
            snapshot.metal.frameLatencyHistory[0]
                = { 17, 20'000'000, 100, 200, 300, 400, 1'000, true, true };

            const auto view = model.update(snapshot, 2.0, false);
            expect(view.report.empty());

            std::set<std::string> deferredHistories;
            for (const auto& section : view.sections) {
                for (const auto& row : section.rows) {
                    if (row.fieldName == "metal.presentedFrameIntervalHistory"
                        || row.fieldName == "metal.frameLatencyHistory") {
                        deferredHistories.emplace(row.fieldName);
                        expect(row.rawValue.empty());
                    }
                }
            }
            expectEquals(deferredHistories.size(), std::size_t { 2 });

            const auto report = PerformanceMetricsModel::buildCopyReport(view, snapshot);
            expect(
                report.find("Audio Insight per-instance performance metrics") != std::string::npos);
            expect(report.find("metal.submittedFrames = 12") != std::string::npos);
            expect(report.find("metal.presentedFrameIntervalHistory = [17:8333333] ")
                != std::string::npos);
            expect(report.find("metal.frameLatencyHistory = "
                               "[17:20000000:100:200:300:400:1000:true:true] ")
                != std::string::npos);
        });
    }
};

static PerformanceMetricsModelTests performanceMetricsModelTests;
} // namespace
} // namespace audio_insight
