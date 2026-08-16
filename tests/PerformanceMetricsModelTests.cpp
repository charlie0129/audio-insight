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
static_assert(aggregateFieldCount<MetalRenderTelemetry>() == 103);
static_assert(aggregateFieldCount<StereoSampleCapture::Telemetry>() == 9);
static_assert(aggregateFieldCount<StereoMeterAccumulator::Telemetry>() == 7);
static_assert(aggregateFieldCount<AudioCallbackBlockTelemetry>() == 5);
static_assert(aggregateFieldCount<AudioCallbackTelemetry>() == 15);
static_assert(aggregateFieldCount<SharedAnalysisScheduler::Counters>() == 12);
static_assert(aggregateFieldCount<LoudnessAnalyzer::Statistics>() == 33);
static_assert(aggregateFieldCount<LoudnessMeasurement>() == 19);
static_assert(aggregateFieldCount<AnalysisTelemetry>() == 71);

constexpr auto expectedRawFieldNames = std::to_array<std::string_view>({
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
    "metal.spectrogramUploadDeferrals",
    "metal.spectrogramScrollClockInitializations",
    "metal.spectrogramScrollUnderrunFrames",
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
    "metal.requestedMinimumFramesPerSecond",
    "metal.requestedPreferredFramesPerSecond",
    "metal.requestedMaximumFramesPerSecond",
    "metal.spectrogramTextureRows",
    "metal.spectrogramTextureColumns",
    "metal.spectrogramTextureBytes",
    "metal.stereoLastPointCount",
    "metal.backingScale",
    "metal.spectrogramScrollHeadOffsetColumns",
    "metal.stereoCorrelation",
    "metal.metalAvailable",
    "metal.renderingRequested",
    "metal.effectivelyRendering",
    "metal.resetPending",
    "metal.stereoCorrelationValid",
    "metal.stereoMono",
    "metal.lastLoudnessSequence",
    "metal.loudnessMeasurementCapturedFrameEnd",
    "metal.loudnessIntegratedCapturedFrameEnd",
    "metal.loudnessMomentaryLufs",
    "metal.loudnessShortTermLufs",
    "metal.loudnessIntegratedLufs",
    "metal.loudnessReferenceLufs",
    "metal.loudnessMomentaryValid",
    "metal.loudnessShortTermValid",
    "metal.loudnessIntegratedValid",
    "analysis.audioCallback.callbackCount",
    "analysis.audioCallback.processedFrames",
    "analysis.audioCallback.timingSamples",
    "analysis.audioCallback.timingUnavailable",
    "analysis.audioCallback.budgetExceeded",
    "analysis.audioCallback.untrackedBlockSizeCallbacks",
    "analysis.audioCallback.concurrentCallbackViolations",
    "analysis.audioCallback.clockAnomalyViolations",
    "analysis.audioCallback.rtSafetyViolationCount",
    "analysis.audioCallback.detectorCoverageFlags",
    "analysis.audioCallback.detectorActive",
    "analysis.audioCallback.clockAvailable",
    "analysis.audioCallback.allocationDetectorActive",
    "analysis.audioCallback.lockWaitDetectorActive",
    "analysis.audioCallback.block64.callbackCount",
    "analysis.audioCallback.block64.timingSamples",
    "analysis.audioCallback.block64.budgetExceeded",
    "analysis.audioCallback.block64.budgetNanoseconds",
    "analysis.audioCallback.block64.durationHistogram",
    "analysis.audioCallback.block128.callbackCount",
    "analysis.audioCallback.block128.timingSamples",
    "analysis.audioCallback.block128.budgetExceeded",
    "analysis.audioCallback.block128.budgetNanoseconds",
    "analysis.audioCallback.block128.durationHistogram",
    "analysis.audioCallback.block256.callbackCount",
    "analysis.audioCallback.block256.timingSamples",
    "analysis.audioCallback.block256.budgetExceeded",
    "analysis.audioCallback.block256.budgetNanoseconds",
    "analysis.audioCallback.block256.durationHistogram",
    "analysis.audioCallback.block512.callbackCount",
    "analysis.audioCallback.block512.timingSamples",
    "analysis.audioCallback.block512.budgetExceeded",
    "analysis.audioCallback.block512.budgetNanoseconds",
    "analysis.audioCallback.block512.durationHistogram",
    "analysis.audioCallback.block1024.callbackCount",
    "analysis.audioCallback.block1024.timingSamples",
    "analysis.audioCallback.block1024.budgetExceeded",
    "analysis.audioCallback.block1024.budgetNanoseconds",
    "analysis.audioCallback.block1024.durationHistogram",
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
    "analysis.loudness.inputChunks",
    "analysis.loudness.inputFrames",
    "analysis.loudness.measurementCompletions",
    "analysis.loudness.integrationBlockCompletions",
    "analysis.loudness.fullResets",
    "analysis.loudness.explicitResets",
    "analysis.loudness.generationResets",
    "analysis.loudness.discontinuityResets",
    "analysis.loudness.formatResets",
    "analysis.loudness.invalidInputResets",
    "analysis.loudness.integrationResets",
    "analysis.loudness.liveMeasurementClears",
    "analysis.loudness.integrationCapacityOverflows",
    "analysis.loudness.integrationBlocksSinceReset",
    "analysis.loudness.absoluteGatedBlocks",
    "analysis.loudness.relativeGatedBlocks",
    "analysis.loudness.integrationIndexReservedBytes",
    "analysis.loudness.integrationIndexLeafNodes",
    "analysis.loudness.integrationIndexInternalNodes",
    "analysis.loudness.integrationIndexLeafCapacity",
    "analysis.loudness.integrationIndexInternalCapacity",
    "analysis.loudness.integrationIndexTreeHeight",
    "analysis.loudness.integrationIndexQueries",
    "analysis.loudness.integrationIndexLastNodeVisits",
    "analysis.loudness.integrationIndexMaximumNodeVisits",
    "analysis.loudness.integrationIndexLastAggregateReads",
    "analysis.loudness.integrationIndexMaximumAggregateReads",
    "analysis.loudness.integrationIndexLastBoundaryValueReads",
    "analysis.loudness.integrationIndexMaximumBoundaryValueReads",
    "analysis.loudness.stateSequence",
    "analysis.loudness.capturedFrameEnd",
    "analysis.loudness.integrationBlockCapacity",
    "analysis.loudness.integrationCapacityExceeded",
    "analysis.loudnessMeasurement.momentaryLufs",
    "analysis.loudnessMeasurement.shortTermLufs",
    "analysis.loudnessMeasurement.integratedLufs",
    "analysis.loudnessMeasurement.relativeGateLufs",
    "analysis.loudnessMeasurement.stateSequence",
    "analysis.loudnessMeasurement.measurementCompletionCount",
    "analysis.loudnessMeasurement.integrationBlockCount",
    "analysis.loudnessMeasurement.absoluteGatedBlockCount",
    "analysis.loudnessMeasurement.relativeGatedBlockCount",
    "analysis.loudnessMeasurement.measurementCapturedFrameEnd",
    "analysis.loudnessMeasurement.integratedCapturedFrameEnd",
    "analysis.loudnessMeasurement.integrationBlockCapacity",
    "analysis.loudnessMeasurement.generation",
    "analysis.loudnessMeasurement.channelCount",
    "analysis.loudnessMeasurement.sampleRate",
    "analysis.loudnessMeasurement.momentaryValid",
    "analysis.loudnessMeasurement.shortTermValid",
    "analysis.loudnessMeasurement.integratedValid",
    "analysis.loudnessMeasurement.integrationCapacityExceeded",
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
    "analysis.scheduler.queueWaitSamples",
    "analysis.scheduler.lastQueueWaitNanoseconds",
    "analysis.scheduler.maximumQueueWaitNanoseconds",
    "analysis.scheduler.queueWaitDeadlineMisses",
    "analysis.scheduler.jobTurnaroundSamples",
    "analysis.scheduler.lastJobTurnaroundNanoseconds",
    "analysis.scheduler.maximumJobTurnaroundNanoseconds",
    "analysis.scheduler.jobDeadlineMisses",
    "analysis.scheduler.timingUnavailable",
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
    "analysis.captureGeneration",
    "analysis.captureSampleRate",
    "analysis.spectrumFreshnessFrames",
    "analysis.spectrumFreshnessNanoseconds",
    "analysis.peakRmsFreshnessFrames",
    "analysis.peakRmsFreshnessNanoseconds",
    "analysis.spectrumFreshnessValid",
    "analysis.peakRmsFreshnessValid",
    "analysis.latestCaptureRevision",
    "analysis.lastAnalyzedCaptureRevision",
    "analysis.emptyAnalysisRequestsAvoided",
    "analysis.captureBoundaryRequestsDeferred",
    "analysis.staleFramesPublished",
    "analysis.peakRmsUserResets",
    "analysis.spectrumUserClears",
});
static_assert(expectedRawFieldNames.size() == 287);

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
    test.expect(std::isfinite(statistics.percentile99Milliseconds));
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
        testCase("Exact active-display request is explicit in raw telemetry", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.configuredMaximumFramesPerSecond = 144;
            snapshot.metal.requestedMinimumFramesPerSecond = 144;
            snapshot.metal.requestedPreferredFramesPerSecond = 144;
            snapshot.metal.requestedMaximumFramesPerSecond = 144;

            const auto view = model.update(snapshot, 1.0);
            const auto* displayMaximum = findRawRow(view, "metal.configuredMaximumFramesPerSecond");
            const auto* minimum = findRawRow(view, "metal.requestedMinimumFramesPerSecond");
            const auto* preferred = findRawRow(view, "metal.requestedPreferredFramesPerSecond");
            const auto* maximum = findRawRow(view, "metal.requestedMaximumFramesPerSecond");
            expect(displayMaximum != nullptr && displayMaximum->rawValue == "144");
            expect(minimum != nullptr && minimum->rawValue == "144");
            expect(preferred != nullptr && preferred->rawValue == "144");
            expect(maximum != nullptr && maximum->rawValue == "144");
            expect(view.report.find("metal.configuredMaximumFramesPerSecond = 144 Hz")
                != std::string::npos);
        });

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
            expectWithinAbsoluteError(statistics.percentile99Milliseconds, 8.333334, 1.0e-9);
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

        testCase("Audio callback histograms expose bounded p99 and deferred raw data", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 12;
            auto& block = snapshot.analysis.audioCallback.trackedBlocks[1];
            block.budgetNanoseconds = 25'000;
            block.timingSamples = 100;
            block.durationHistogram[10] = 99;
            block.durationHistogram[100] = 1;

            auto view = model.update(snapshot, 1.0, false);
            const auto& statistics = view.derived.audioCallbackBlocks[1];
            expect(statistics.blockSizeFrames == 128);
            expect(statistics.sampleCount == 100);
            expect(statistics.percentile99Available);
            expect(!statistics.percentile99Overflow);
            expect(statistics.percentile99UpperBoundNanoseconds == 11'000);
            expect(statistics.budgetResultAvailable);
            expect(statistics.budgetPassed);

            const auto* histogram
                = findRawRow(view, "analysis.audioCallback.block128.durationHistogram");
            expect(histogram != nullptr && histogram->rawValue.empty());
            expect(histogram != nullptr
                && histogram->rawUnit.find("bucket 1024 is >=1024 us") != std::string::npos);
            const auto report = PerformanceMetricsModel::buildCopyReport(view, snapshot);
            expect(report.find("analysis.audioCallback.block128.durationHistogram = [0:0")
                != std::string::npos);
            expect(report.find(", 10:99, 11:0") != std::string::npos);
            expect(report.find(", 100:1, 101:0") != std::string::npos);

            block.durationHistogram = { };
            block.durationHistogram[10] = 98;
            block.durationHistogram[audioCallbackDurationHistogramOverflowBucket] = 2;
            view = model.update(snapshot, 2.0, false);
            const auto& overflow = view.derived.audioCallbackBlocks[1];
            expect(!overflow.percentile99Available);
            expect(overflow.percentile99Overflow);
            expect(overflow.overflowSamples == 2);
            expect(overflow.budgetResultAvailable);
            expect(!overflow.budgetPassed);
        });

        testCase(
            "Scheduler and analyzer freshness histories admit zero and reset honestly", [this] {
                PerformanceMetricsModel model(8);
                PerformanceMetricsSnapshot snapshot;
                snapshot.metal.epoch = 13;
                snapshot.analysis.scheduler.queueWaitSamples = 1;
                snapshot.analysis.scheduler.lastQueueWaitNanoseconds = 0;
                snapshot.analysis.scheduler.jobTurnaroundSamples = 1;
                snapshot.analysis.scheduler.lastJobTurnaroundNanoseconds = 4'000'000;
                snapshot.analysis.captureGeneration = 7;
                snapshot.analysis.captureSampleRate = 48'000.0;
                snapshot.analysis.spectrumFreshnessValid = true;
                snapshot.analysis.spectrumFreshnessNanoseconds = 0;
                snapshot.analysis.peakRmsFreshnessValid = true;
                snapshot.analysis.peakRmsFreshnessNanoseconds = 1'000'000;

                auto view = model.update(snapshot, 10.0);
                expect(view.derived.analysisDurations.schedulerQueueWait.available);
                expect(view.derived.analysisDurations.schedulerQueueWait.sampleCount == 1);
                expect(view.derived.analysisDurations.schedulerQueueWait.latestMilliseconds == 0.0);
                expect(view.derived.analysisDurations.spectrumFreshness.available);
                expect(view.derived.analysisDurations.spectrumFreshness.sampleCount == 1);
                expect(view.derived.analysisDurations.spectrumFreshness.latestMilliseconds == 0.0);

                snapshot.analysis.scheduler.queueWaitSamples = 2;
                snapshot.analysis.scheduler.lastQueueWaitNanoseconds = 2'000'000;
                snapshot.analysis.scheduler.jobTurnaroundSamples = 2;
                snapshot.analysis.scheduler.lastJobTurnaroundNanoseconds = 6'000'000;
                snapshot.analysis.spectrumFreshnessNanoseconds = 2'000'000;
                snapshot.analysis.peakRmsFreshnessNanoseconds = 3'000'000;
                view = model.update(snapshot, 11.0);

                const auto& queue = view.derived.analysisDurations.schedulerQueueWait;
                const auto& spectrum = view.derived.analysisDurations.spectrumFreshness;
                expect(queue.sampleCount == 2);
                expectWithinAbsoluteError(queue.percentile95Milliseconds, 2.0, 1.0e-12);
                expectWithinAbsoluteError(queue.percentile99Milliseconds, 2.0, 1.0e-12);
                expect(spectrum.sampleCount == 2);
                expectWithinAbsoluteError(spectrum.minimumMilliseconds, 0.0, 1.0e-12);
                expectWithinAbsoluteError(spectrum.percentile99Milliseconds, 2.0, 1.0e-12);
                const auto* queueRate = findRate(view, "analysis.scheduler.queueWaitSamples");
                expect(queueRate != nullptr && queueRate->available);

                snapshot.analysis.captureGeneration = 8;
                snapshot.analysis.spectrumFreshnessNanoseconds = 9'000'000;
                snapshot.analysis.peakRmsFreshnessNanoseconds = 10'000'000;
                view = model.update(snapshot, 12.0);
                expect(view.derived.analysisDurations.spectrumFreshness.sampleCount == 1);
                expectWithinAbsoluteError(
                    view.derived.analysisDurations.spectrumFreshness.latestMilliseconds, 9.0,
                    1.0e-12);
                expect(view.derived.analysisDurations.peakRmsFreshness.sampleCount == 1);
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

        testCase("Fractional Spectrogram motion and upload pressure remain distinct", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 6;
            static_cast<void>(model.update(snapshot, 20.0));

            snapshot.metal.spectrogramUploadBackpressureDrops = 9;
            snapshot.metal.spectrogramUploadDeferrals = 8;
            snapshot.metal.spectrogramScrollClockInitializations = 2;
            snapshot.metal.spectrogramScrollUnderrunFrames = 6;
            snapshot.metal.spectrogramScrollHeadOffsetColumns = -0.375;
            snapshot.metal.spectrogramUploadCommands = 12;
            const auto view = model.update(snapshot, 22.0);

            const auto* dropRate = findRate(view, "metal.spectrogramUploadBackpressureDrops");
            expect(dropRate != nullptr && dropRate->available);
            if (dropRate != nullptr) {
                expectWithinAbsoluteError(dropRate->value, 4.5, 1.0e-12);
                expectEquals(dropRate->label, std::string("Spectrogram upload-backpressure drops"));
            }

            const auto* commandRate = findRate(view, "metal.spectrogramUploadCommands");
            expect(commandRate != nullptr && commandRate->available);
            if (commandRate != nullptr) {
                expectWithinAbsoluteError(commandRate->value, 6.0, 1.0e-12);
                expectEquals(commandRate->label, std::string("Spectrogram column copy commands"));
            }

            const auto* deferralRate = findRate(view, "metal.spectrogramUploadDeferrals");
            expect(deferralRate != nullptr && deferralRate->available);
            if (deferralRate != nullptr)
                expectWithinAbsoluteError(deferralRate->value, 4.0, 1.0e-12);

            const auto* initializationRate
                = findRate(view, "metal.spectrogramScrollClockInitializations");
            expect(initializationRate != nullptr && initializationRate->available);
            if (initializationRate != nullptr)
                expectWithinAbsoluteError(initializationRate->value, 1.0, 1.0e-12);

            const auto* underrunRate = findRate(view, "metal.spectrogramScrollUnderrunFrames");
            expect(underrunRate != nullptr && underrunRate->available);
            if (underrunRate != nullptr)
                expectWithinAbsoluteError(underrunRate->value, 3.0, 1.0e-12);

            const auto* dropRow = findRawRow(view, "metal.spectrogramUploadBackpressureDrops");
            expect(dropRow != nullptr);
            if (dropRow != nullptr) {
                expectEquals(dropRow->label,
                    std::string("Full frames skipped for an outstanding history upload"));
            }

            const auto* offsetRow = findRawRow(view, "metal.spectrogramScrollHeadOffsetColumns");
            expect(offsetRow != nullptr);
            if (offsetRow != nullptr) {
                expectEquals(offsetRow->value, std::string("-0.375"));
                expectEquals(offsetRow->rawValue, std::string("-0.375"));
                expectEquals(offsetRow->unit, std::string("columns"));
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

        testCase("Renderer Loudness telemetry has one exact group and readiness semantics", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 10;
            snapshot.metal.lastLoudnessSequence = 7;
            snapshot.metal.loudnessMeasurementCapturedFrameEnd = 48'000;
            snapshot.metal.loudnessIntegratedCapturedFrameEnd = 43'200;
            snapshot.metal.loudnessMomentaryLufs = -std::numeric_limits<double>::infinity();
            snapshot.metal.loudnessShortTermLufs = -std::numeric_limits<double>::infinity();
            snapshot.metal.loudnessIntegratedLufs = -14.26;
            snapshot.metal.loudnessReferenceLufs = -23.0;
            snapshot.metal.loudnessMomentaryValid = false;
            snapshot.metal.loudnessShortTermValid = true;
            snapshot.metal.loudnessIntegratedValid = true;
            auto view = model.update(snapshot, 50.0);

            std::size_t groupCount = 0;
            std::set<std::string> fields;
            for (const auto& section : view.sections) {
                if (section.name != "Renderer Loudness")
                    continue;

                ++groupCount;
                for (const auto& row : section.rows)
                    fields.emplace(row.fieldName);
            }

            const std::set<std::string> expectedFields {
                "metal.lastLoudnessSequence",
                "metal.loudnessMeasurementCapturedFrameEnd",
                "metal.loudnessIntegratedCapturedFrameEnd",
                "metal.loudnessMomentaryLufs",
                "metal.loudnessShortTermLufs",
                "metal.loudnessIntegratedLufs",
                "metal.loudnessReferenceLufs",
                "metal.loudnessMomentaryValid",
                "metal.loudnessShortTermValid",
                "metal.loudnessIntegratedValid",
            };
            expectEquals(groupCount, std::size_t { 1 });
            expect(fields == expectedFields);

            const auto* momentary = findRawRow(view, "metal.loudnessMomentaryLufs");
            const auto* shortTerm = findRawRow(view, "metal.loudnessShortTermLufs");
            const auto* integrated = findRawRow(view, "metal.loudnessIntegratedLufs");
            expect(momentary != nullptr && momentary->value == "not-ready");
            expect(shortTerm != nullptr && shortTerm->value == "-infinity");
            expect(integrated != nullptr && integrated->value == "-14.3");
            expect(findRate(view, "metal.lastLoudnessSequence") == nullptr);

            snapshot.metal.loudnessMomentaryLufs = std::numeric_limits<double>::quiet_NaN();
            snapshot.metal.loudnessMomentaryValid = true;
            view = model.update(snapshot, 51.0);
            momentary = findRawRow(view, "metal.loudnessMomentaryLufs");
            expect(momentary != nullptr && momentary->value == "invalid");
            expect(view.report.find("metal.loudnessMomentaryLufs = invalid LUFS")
                != std::string::npos);
        });

        testCase("Loudness analysis telemetry maps every field and rates only counters", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.metal.epoch = 11;
            static_cast<void>(model.update(snapshot, 60.0));

            snapshot.analysis.loudness.inputChunks = 20;
            snapshot.analysis.loudness.inputFrames = 9'600;
            snapshot.analysis.loudness.measurementCompletions = 2;
            snapshot.analysis.loudness.integrationBlockCompletions = 2;
            snapshot.analysis.loudness.fullResets = 1;
            snapshot.analysis.loudness.explicitResets = 1;
            snapshot.analysis.loudness.integrationResets = 1;
            snapshot.analysis.loudness.liveMeasurementClears = 1;
            snapshot.analysis.loudness.integrationCapacityOverflows = 1;
            snapshot.analysis.loudness.integrationBlocksSinceReset = 2;
            snapshot.analysis.loudness.absoluteGatedBlocks = 2;
            snapshot.analysis.loudness.relativeGatedBlocks = 1;
            snapshot.analysis.loudness.integrationIndexReservedBytes = 1'048'576;
            snapshot.analysis.loudness.integrationIndexLeafNodes = 2;
            snapshot.analysis.loudness.integrationIndexInternalNodes = 1;
            snapshot.analysis.loudness.integrationIndexQueries = 4;
            snapshot.analysis.loudness.integrationIndexLastNodeVisits = 3;
            snapshot.analysis.loudness.integrationIndexMaximumNodeVisits = 5;
            snapshot.analysis.loudness.stateSequence = 4;
            snapshot.analysis.loudness.capturedFrameEnd = 9'600;
            snapshot.analysis.loudness.integrationBlockCapacity = 864'000;
            snapshot.analysis.loudness.integrationCapacityExceeded = true;
            snapshot.analysis.loudnessMeasurement.momentaryLufs = -20.0;
            snapshot.analysis.loudnessMeasurement.shortTermLufs
                = -std::numeric_limits<double>::infinity();
            snapshot.analysis.loudnessMeasurement.integratedLufs = -21.25;
            snapshot.analysis.loudnessMeasurement.relativeGateLufs = -31.25;
            snapshot.analysis.loudnessMeasurement.stateSequence = 4;
            snapshot.analysis.loudnessMeasurement.measurementCompletionCount = 2;
            snapshot.analysis.loudnessMeasurement.integrationBlockCount = 2;
            snapshot.analysis.loudnessMeasurement.absoluteGatedBlockCount = 2;
            snapshot.analysis.loudnessMeasurement.relativeGatedBlockCount = 1;
            snapshot.analysis.loudnessMeasurement.measurementCapturedFrameEnd = 9'600;
            snapshot.analysis.loudnessMeasurement.integratedCapturedFrameEnd = 9'600;
            snapshot.analysis.loudnessMeasurement.integrationBlockCapacity = 864'000;
            snapshot.analysis.loudnessMeasurement.generation = 3;
            snapshot.analysis.loudnessMeasurement.channelCount = 2;
            snapshot.analysis.loudnessMeasurement.sampleRate = 48'000.0;
            snapshot.analysis.loudnessMeasurement.momentaryValid = true;
            snapshot.analysis.loudnessMeasurement.shortTermValid = true;
            snapshot.analysis.loudnessMeasurement.integratedValid = false;
            snapshot.analysis.loudnessMeasurement.integrationCapacityExceeded = true;
            const auto view = model.update(snapshot, 62.0);

            std::size_t groupCount = 0;
            std::set<std::string> fields;
            for (const auto& section : view.sections) {
                if (section.name != "Loudness analysis and gating")
                    continue;

                ++groupCount;
                for (const auto& row : section.rows)
                    fields.emplace(row.fieldName);
            }
            expectEquals(groupCount, std::size_t { 1 });
            expectEquals(fields.size(), std::size_t { 52 });
            expect(fields.contains("analysis.loudness.inputFrames"));
            expect(fields.contains("analysis.loudness.integrationBlocksSinceReset"));
            expect(fields.contains("analysis.loudness.integrationIndexReservedBytes"));
            expect(fields.contains("analysis.loudness.integrationIndexMaximumNodeVisits"));
            expect(fields.contains("analysis.loudnessMeasurement.relativeGateLufs"));
            expect(fields.contains("analysis.loudnessMeasurement.integratedValid"));
            expect(fields.contains("analysis.loudnessMeasurement.integrationCapacityExceeded"));

            const auto* frameRate = findRate(view, "analysis.loudness.inputFrames");
            const auto* completionRate = findRate(view, "analysis.loudness.measurementCompletions");
            expect(frameRate != nullptr && frameRate->available);
            expect(completionRate != nullptr && completionRate->available);
            if (frameRate != nullptr)
                expectWithinAbsoluteError(frameRate->value, 4'800.0, 1.0e-12);
            if (completionRate != nullptr)
                expectWithinAbsoluteError(completionRate->value, 1.0, 1.0e-12);
            const auto* overflowRate
                = findRate(view, "analysis.loudness.integrationCapacityOverflows");
            const auto* queryRate = findRate(view, "analysis.loudness.integrationIndexQueries");
            expect(overflowRate != nullptr && overflowRate->available);
            expect(queryRate != nullptr && queryRate->available);
            if (overflowRate != nullptr)
                expectWithinAbsoluteError(overflowRate->value, 0.5, 1.0e-12);
            if (queryRate != nullptr)
                expectWithinAbsoluteError(queryRate->value, 2.0, 1.0e-12);
            expect(findRate(view, "analysis.loudness.integrationBlocksSinceReset") == nullptr);
            expect(findRate(view, "analysis.loudness.absoluteGatedBlocks") == nullptr);
            expect(findRate(view, "analysis.loudnessMeasurement.integrationBlockCount") == nullptr);

            const auto* silentShortTerm
                = findRawRow(view, "analysis.loudnessMeasurement.shortTermLufs");
            expect(silentShortTerm != nullptr && silentShortTerm->value == "-infinity");
            const auto* overflowedIntegrated
                = findRawRow(view, "analysis.loudnessMeasurement.integratedLufs");
            expect(overflowedIntegrated != nullptr && overflowedIntegrated->value == "not-ready");
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

        testCase("Requested and achieved FFT slice rates are explicit", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.analysis.requestedFftSliceRateHz = 60;
            snapshot.analysis.spectrumTransforms = 100;

            auto view = model.update(snapshot, 10.0);
            const auto* requested = findRawRow(view, "analysis.requestedFftSliceRateHz");
            expect(requested != nullptr);
            if (requested != nullptr) {
                expectEquals(requested->label, std::string("Requested FFT slice rate"));
                expectEquals(requested->unit, std::string("Hz"));
            }

            snapshot.analysis.spectrumTransforms += 120;
            view = model.update(snapshot, 12.0);
            const auto* achieved = findRate(view, "analysis.spectrumTransforms");
            expect(achieved != nullptr && achieved->available);
            if (achieved != nullptr) {
                expectEquals(achieved->label, std::string("Achieved FFT slice rate"));
                expectEquals(achieved->unit, std::string("Hz"));
                expectWithinAbsoluteError(achieved->value, 60.0, 1.0e-12);
            }

            expect(
                view.report.find("rate.analysis.spectrumTransforms = 60 Hz") != std::string::npos);
        });

        testCase("Capture-boundary request deferrals have explicit raw and rate metrics", [this] {
            PerformanceMetricsModel model;
            PerformanceMetricsSnapshot snapshot;
            snapshot.analysis.captureBoundaryRequestsDeferred = 4;
            static_cast<void>(model.update(snapshot, 10.0));

            snapshot.analysis.captureBoundaryRequestsDeferred = 10;
            const auto view = model.update(snapshot, 12.0);
            const auto* raw = findRawRow(view, "analysis.captureBoundaryRequestsDeferred");
            expect(raw != nullptr);
            if (raw != nullptr) {
                expectEquals(raw->label, std::string("Capture-boundary requests deferred"));
                expectEquals(raw->value, std::string("10"));
                expectEquals(raw->rawValue, std::string("10"));
            }

            const auto* rate = findRate(view, "analysis.captureBoundaryRequestsDeferred");
            expect(rate != nullptr && rate->available);
            if (rate != nullptr) {
                expectEquals(rate->label, std::string("Capture-boundary requests deferred"));
                expectEquals(rate->unit, std::string("requests/s"));
                expectWithinAbsoluteError(rate->value, 3.0, 1.0e-12);
                expectEquals(rate->counterDelta, std::uint64_t { 6 });
                expectWithinAbsoluteError(rate->sampleIntervalSeconds, 2.0, 1.0e-12);
            }

            expect(view.report.find("analysis.captureBoundaryRequestsDeferred = 10")
                != std::string::npos);
            expect(view.report.find("rate.analysis.captureBoundaryRequestsDeferred = 3 requests/s "
                                    "(counter_delta = 6; sample_interval_seconds = 2)")
                != std::string::npos);
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
            expectFiniteStatistics(*this, view.derived.analysisDurations.schedulerQueueWait);
            expectFiniteStatistics(*this, view.derived.analysisDurations.schedulerJobTurnaround);
            expectFiniteStatistics(*this, view.derived.analysisDurations.spectrumFreshness);
            expectFiniteStatistics(*this, view.derived.analysisDurations.peakRmsFreshness);

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
