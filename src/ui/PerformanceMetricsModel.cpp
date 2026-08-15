// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PerformanceMetricsModel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

namespace audio_insight {
namespace {
constexpr double nanosecondsPerMillisecond = 1'000'000.0;
constexpr double nanosecondsPerSecond = 1'000'000'000.0;

std::string formatUnsigned(const std::uint64_t value)
{
    char buffer[32] { };
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    return result.ec == std::errc { } ? std::string(buffer, result.ptr)
                                      : std::string("unavailable");
}

std::string formatFinite(const double value, const int decimalPlaces)
{
    if (!std::isfinite(value))
        return "unavailable";

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(decimalPlaces) << value;
    return stream.str();
}

std::string formatRawDouble(const double value)
{
    if (!std::isfinite(value))
        return "unavailable";

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

PerformanceMetricRow rawUnsigned(
    std::string fieldName, std::string label, const std::uint64_t value, std::string unit = { })
{
    const auto formatted = formatUnsigned(value);
    return { std::move(fieldName), std::move(label), formatted, unit, formatted, std::move(unit),
        PerformanceMetricKind::raw };
}

PerformanceMetricRow rawBoolean(std::string fieldName, std::string label, const bool value)
{
    const auto formatted = value ? std::string("true") : std::string("false");
    return { std::move(fieldName), std::move(label), formatted, { }, formatted, { },
        PerformanceMetricKind::raw };
}

PerformanceMetricRow rawDouble(std::string fieldName, std::string label, const double value,
    std::string unit, const int decimalPlaces)
{
    return { std::move(fieldName), std::move(label), formatFinite(value, decimalPlaces), unit,
        formatRawDouble(value), std::move(unit), PerformanceMetricKind::raw };
}

PerformanceMetricRow rawDuration(
    std::string fieldName, std::string label, const std::uint64_t nanoseconds)
{
    return { std::move(fieldName), std::move(label),
        formatFinite(static_cast<double>(nanoseconds) / nanosecondsPerMillisecond, 3), "ms",
        formatUnsigned(nanoseconds), "ns", PerformanceMetricKind::raw };
}

PerformanceMetricRow rawTimestamp(
    std::string fieldName, std::string label, const std::uint64_t nanoseconds)
{
    return { std::move(fieldName), std::move(label),
        formatFinite(static_cast<double>(nanoseconds) / nanosecondsPerSecond, 6), "s monotonic",
        formatUnsigned(nanoseconds), "ns monotonic", PerformanceMetricKind::raw };
}

std::size_t boundedPresentedHistoryCount(const MetalRenderTelemetry& telemetry) noexcept
{
    return std::min(telemetry.presentedFrameIntervalHistoryCount,
        telemetry.presentedFrameIntervalHistory.size());
}

std::string presentedHistorySummary(const MetalRenderTelemetry& telemetry)
{
    const auto count = boundedPresentedHistoryCount(telemetry);
    if (count == 0)
        return "0 samples";

    const auto firstSequence = telemetry.presentedFrameIntervalHistory[0].sequence;
    const auto lastSequence = telemetry.presentedFrameIntervalHistory[count - 1].sequence;
    return formatUnsigned(count) + " samples, sequence " + formatUnsigned(firstSequence) + "-"
        + formatUnsigned(lastSequence);
}

std::string presentedHistoryRawValue(const MetalRenderTelemetry& telemetry)
{
    const auto count = boundedPresentedHistoryCount(telemetry);
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << '[';

    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            stream << ", ";

        const auto& sample = telemetry.presentedFrameIntervalHistory[index];
        stream << sample.sequence << ':' << sample.nanoseconds;
    }

    stream << ']';
    return stream.str();
}

PerformanceMetricRow rawPresentedHistory(const MetalRenderTelemetry& telemetry)
{
    return { "metal.presentedFrameIntervalHistory", "Presented-frame interval history",
        presentedHistorySummary(telemetry), { }, { }, "sequence:nanoseconds",
        PerformanceMetricKind::raw };
}

std::size_t boundedFrameLatencyHistoryCount(const MetalRenderTelemetry& telemetry) noexcept
{
    return std::min(telemetry.frameLatencyHistoryCount, telemetry.frameLatencyHistory.size());
}

std::string frameLatencyHistorySummary(const MetalRenderTelemetry& telemetry)
{
    const auto count = boundedFrameLatencyHistoryCount(telemetry);
    if (count == 0)
        return "0 samples";

    const auto firstSequence = telemetry.frameLatencyHistory[0].sequence;
    const auto lastSequence = telemetry.frameLatencyHistory[count - 1].sequence;
    return formatUnsigned(count) + " samples, first sequence " + formatUnsigned(firstSequence)
        + ", last sequence " + formatUnsigned(lastSequence);
}

constexpr std::string_view frameLatencyHistoryRawUnit
    = "sequence:presented_host_timestamp_nanoseconds:cpu_encode_nanoseconds:"
      "submit_queue_wait_nanoseconds:gpu_execution_nanoseconds:compositor_wait_nanoseconds:"
      "total_nanoseconds:total_valid:components_valid";

std::string frameLatencyHistoryRawValue(const MetalRenderTelemetry& telemetry)
{
    const auto count = boundedFrameLatencyHistoryCount(telemetry);
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << '[';

    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            stream << ", ";

        const auto& sample = telemetry.frameLatencyHistory[index];
        stream << sample.sequence << ':' << sample.presentedHostTimestampNanoseconds << ':'
               << sample.cpuEncodeNanoseconds << ':' << sample.submitQueueWaitNanoseconds << ':'
               << sample.gpuExecutionNanoseconds << ':' << sample.compositorWaitNanoseconds << ':'
               << sample.totalNanoseconds << ':' << (sample.totalValid ? "true" : "false") << ':'
               << (sample.componentsValid ? "true" : "false");
    }

    stream << ']';
    return stream.str();
}

PerformanceMetricRow rawFrameLatencyHistory(const MetalRenderTelemetry& telemetry)
{
    return { "metal.frameLatencyHistory", "Per-frame latency history",
        frameLatencyHistorySummary(telemetry), { }, { }, std::string(frameLatencyHistoryRawUnit),
        PerformanceMetricKind::raw };
}

void appendRawSections(
    const PerformanceMetricsSnapshot& snapshot, std::vector<PerformanceMetricGroup>& sections)
{
    const auto& metal = snapshot.metal;
    const auto& analysis = snapshot.analysis;

    PerformanceMetricGroup status { "Renderer status and surface", { } };
    status.rows.emplace_back(rawUnsigned("metal.epoch", "Telemetry epoch", metal.epoch));
    status.rows.emplace_back(rawUnsigned(
        "metal.drawableWidthPixels", "Drawable width", metal.drawableWidthPixels, "px"));
    status.rows.emplace_back(rawUnsigned(
        "metal.drawableHeightPixels", "Drawable height", metal.drawableHeightPixels, "px"));
    status.rows.emplace_back(rawUnsigned("metal.configuredMaximumFramesPerSecond",
        "Configured maximum frame rate", metal.configuredMaximumFramesPerSecond, "Hz"));
    status.rows.emplace_back(
        rawDouble("metal.backingScale", "Backing scale", metal.backingScale, "x", 2));
    status.rows.emplace_back(
        rawBoolean("metal.metalAvailable", "Metal available", metal.metalAvailable));
    status.rows.emplace_back(
        rawBoolean("metal.renderingRequested", "Rendering requested", metal.renderingRequested));
    status.rows.emplace_back(rawBoolean(
        "metal.effectivelyRendering", "Effectively rendering", metal.effectivelyRendering));
    status.rows.emplace_back(
        rawBoolean("metal.resetPending", "Telemetry reset pending", metal.resetPending));
    sections.emplace_back(std::move(status));

    PerformanceMetricGroup frameFlow { "Renderer frame flow", { } };
    frameFlow.rows.emplace_back(rawUnsigned(
        "metal.displayLinkCallbacks", "Display-link callbacks", metal.displayLinkCallbacks));
    frameFlow.rows.emplace_back(
        rawUnsigned("metal.submittedFrames", "Submitted frames", metal.submittedFrames));
    frameFlow.rows.emplace_back(
        rawUnsigned("metal.completedFrames", "Completed frames", metal.completedFrames));
    frameFlow.rows.emplace_back(
        rawUnsigned("metal.gpuTimingSamples", "Valid GPU timing samples", metal.gpuTimingSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.gpuTimingUnavailableSamples",
        "Unavailable GPU timing samples", metal.gpuTimingUnavailableSamples));
    frameFlow.rows.emplace_back(rawUnsigned(
        "metal.commandBufferFailures", "Command-buffer failures", metal.commandBufferFailures));
    frameFlow.rows.emplace_back(rawUnsigned(
        "metal.presentationCallbacks", "Presentation callbacks", metal.presentationCallbacks));
    frameFlow.rows.emplace_back(
        rawUnsigned("metal.presentedFrames", "Presented frames", metal.presentedFrames));
    frameFlow.rows.emplace_back(rawUnsigned("metal.presentationLatenessSamples",
        "Valid presentation-lateness samples", metal.presentationLatenessSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.presentationLatenessUnclassifiableSamples",
        "Unclassifiable presentation-lateness samples",
        metal.presentationLatenessUnclassifiableSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.presentationHistoryDiscardedTimestamps",
        "History timestamps discarded", metal.presentationHistoryDiscardedTimestamps));
    frameFlow.rows.emplace_back(rawUnsigned(
        "metal.frameLatencySamples", "Frame-latency samples", metal.frameLatencySamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.frameLatencyTotalTimingSamples",
        "Valid total frame-latency timings", metal.frameLatencyTotalTimingSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.frameLatencyTotalTimingUnavailableSamples",
        "Unavailable total frame-latency timings",
        metal.frameLatencyTotalTimingUnavailableSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.frameLatencyComponentTimingSamples",
        "Valid frame-latency component timings", metal.frameLatencyComponentTimingSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.frameLatencyComponentTimingUnavailableSamples",
        "Unavailable frame-latency component timings",
        metal.frameLatencyComponentTimingUnavailableSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.frameLatencyHistoryDiscardedSamples",
        "Frame-latency history samples discarded", metal.frameLatencyHistoryDiscardedSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.presentationsAfterTarget",
        "Presentations after target", metal.presentationsAfterTarget));
    frameFlow.rows.emplace_back(rawUnsigned(
        "metal.skippedPresentations", "Skipped presentations", metal.skippedPresentations));
    frameFlow.rows.emplace_back(rawUnsigned(
        "metal.gpuBackpressureDrops", "Render-buffer admission drops", metal.gpuBackpressureDrops));
    frameFlow.rows.emplace_back(rawUnsigned("metal.drawableUnavailableDrops",
        "Drawable-unavailable drops", metal.drawableUnavailableDrops));
    frameFlow.rows.emplace_back(rawUnsigned("metal.callbackHostDelaySamples",
        "Valid callback host-delay samples", metal.callbackHostDelaySamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.callbackHostDelayUnclassifiableSamples",
        "Unclassifiable callback host-delay samples",
        metal.callbackHostDelayUnclassifiableSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.callbackAlreadyLateHostDelays",
        "Callbacks already late (host)", metal.callbackAlreadyLateHostDelays));
    frameFlow.rows.emplace_back(rawUnsigned("metal.cpuCommitLatenessSamples",
        "Valid CPU commit-lateness samples", metal.cpuCommitLatenessSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.cpuCommitLatenessUnclassifiableSamples",
        "Unclassifiable CPU commit-lateness samples",
        metal.cpuCommitLatenessUnclassifiableSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.cpuCommitDeadlineMisses",
        "CPU commit deadline misses", metal.cpuCommitDeadlineMisses));
    frameFlow.rows.emplace_back(rawUnsigned("metal.gpuCompletionLatenessSamples",
        "Valid GPU completion-lateness samples", metal.gpuCompletionLatenessSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.gpuCompletionLatenessUnclassifiableSamples",
        "Unclassifiable GPU completion-lateness samples",
        metal.gpuCompletionLatenessUnclassifiableSamples));
    frameFlow.rows.emplace_back(rawUnsigned("metal.gpuCompletionDeadlineMisses",
        "GPU completion deadline misses", metal.gpuCompletionDeadlineMisses));
    sections.emplace_back(std::move(frameFlow));

    PerformanceMetricGroup handoff { "Renderer snapshot handoff", { } };
    handoff.rows.emplace_back(rawUnsigned(
        "metal.analysisRequestCalls", "Analysis request calls", metal.analysisRequestCalls));
    handoff.rows.emplace_back(
        rawUnsigned("metal.snapshotReads", "Snapshot reads", metal.snapshotReads));
    handoff.rows.emplace_back(rawUnsigned(
        "metal.framesWithNewSnapshot", "Frames with a new snapshot", metal.framesWithNewSnapshot));
    handoff.rows.emplace_back(rawUnsigned(
        "metal.lastSpectrumSequence", "Last spectrum sequence", metal.lastSpectrumSequence));
    sections.emplace_back(std::move(handoff));

    PerformanceMetricGroup spectrogramRender { "Renderer Spectrogram history", { } };
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramColumnsRead",
        "Columns read from analysis", metal.spectrogramColumnsRead, "columns"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramColumnsUploaded",
        "Columns uploaded", metal.spectrogramColumnsUploaded, "columns"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramColumnsRejected",
        "Columns rejected", metal.spectrogramColumnsRejected, "columns"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramGapColumns",
        "Black gap columns inserted", metal.spectrogramGapColumns, "columns"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramHistoryClears",
        "Logical history clears", metal.spectrogramHistoryClears, "clears"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramTextureReallocations",
        "History texture reallocations", metal.spectrogramTextureReallocations, "allocations"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramTextureAllocationFailures",
        "History texture allocation failures", metal.spectrogramTextureAllocationFailures,
        "failures"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramUploadBackpressureDrops",
        "Callbacks skipped for an outstanding history upload",
        metal.spectrogramUploadBackpressureDrops, "drops"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramUploadCommands",
        "History column copy commands", metal.spectrogramUploadCommands, "commands"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramUploadBytes",
        "History upload bytes", metal.spectrogramUploadBytes, "bytes"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramLastColumnSequence",
        "Last accepted column sequence", metal.spectrogramLastColumnSequence));
    spectrogramRender.rows.emplace_back(rawUnsigned(
        "metal.spectrogramTextureRows", "History texture rows", metal.spectrogramTextureRows));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramTextureColumns",
        "History texture columns", metal.spectrogramTextureColumns));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramTextureBytes",
        "History texture allocation", metal.spectrogramTextureBytes, "bytes"));
    sections.emplace_back(std::move(spectrogramRender));

    PerformanceMetricGroup stereoRender { "Renderer Stereo field", { } };
    stereoRender.rows.emplace_back(rawUnsigned(
        "metal.lastStereoSequence", "Last accepted Stereo sequence", metal.lastStereoSequence));
    stereoRender.rows.emplace_back(rawUnsigned("metal.stereoPointInstancesPrepared",
        "Point instances prepared", metal.stereoPointInstancesPrepared, "instances"));
    stereoRender.rows.emplace_back(rawUnsigned(
        "metal.stereoPointDrawCalls", "Point draw calls", metal.stereoPointDrawCalls, "calls"));
    stereoRender.rows.emplace_back(rawUnsigned(
        "metal.stereoLastPointCount", "Last point count", metal.stereoLastPointCount, "points"));
    stereoRender.rows.emplace_back(
        rawDouble("metal.stereoCorrelation", "Correlation", metal.stereoCorrelation, { }, 3));
    stereoRender.rows.emplace_back(rawBoolean(
        "metal.stereoCorrelationValid", "Correlation valid", metal.stereoCorrelationValid));
    stereoRender.rows.emplace_back(rawBoolean("metal.stereoMono", "Mono input", metal.stereoMono));
    sections.emplace_back(std::move(stereoRender));

    PerformanceMetricGroup timing { "Renderer timing", { } };
    timing.rows.emplace_back(rawDuration(
        "metal.lastCpuEncodeNanoseconds", "Last CPU encode", metal.lastCpuEncodeNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.maximumCpuEncodeNanoseconds", "Maximum CPU encode",
        metal.maximumCpuEncodeNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastGpuExecutionNanoseconds", "Last GPU execution",
        metal.lastGpuExecutionNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.maximumGpuExecutionNanoseconds",
        "Maximum GPU execution", metal.maximumGpuExecutionNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastDisplayCallbackIntervalNanoseconds",
        "Last display callback interval", metal.lastDisplayCallbackIntervalNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastTargetIntervalNanoseconds",
        "Last target callback interval", metal.lastTargetIntervalNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastTargetPresentationIntervalNanoseconds",
        "Last target presentation interval", metal.lastTargetPresentationIntervalNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastPresentedFrameIntervalNanoseconds",
        "Last presented-frame interval", metal.lastPresentedFrameIntervalNanoseconds));
    timing.rows.emplace_back(rawTimestamp("metal.lastPresentedHostTimestampNanoseconds",
        "Last presented host timestamp", metal.lastPresentedHostTimestampNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastPresentationLatenessNanoseconds",
        "Last valid presentation lateness", metal.lastPresentationLatenessNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.maximumPresentationLatenessNanoseconds",
        "Maximum presentation lateness", metal.maximumPresentationLatenessNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastCallbackHostDelayNanoseconds",
        "Last callback host delay", metal.lastCallbackHostDelayNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastCpuCommitLatenessNanoseconds",
        "Last CPU commit lateness", metal.lastCpuCommitLatenessNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastGpuCompletionLatenessNanoseconds",
        "Last GPU completion lateness", metal.lastGpuCompletionLatenessNanoseconds));
    timing.rows.emplace_back(rawTimestamp("metal.lastTargetTimestampNanoseconds",
        "Last target callback timestamp", metal.lastTargetTimestampNanoseconds));
    timing.rows.emplace_back(rawTimestamp("metal.lastTargetPresentationTimestampNanoseconds",
        "Last target presentation timestamp", metal.lastTargetPresentationTimestampNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.lastProvidedDrawableAccessNanoseconds",
        "Last provided-drawable access", metal.lastProvidedDrawableAccessNanoseconds));
    timing.rows.emplace_back(rawDuration("metal.maximumProvidedDrawableAccessNanoseconds",
        "Maximum provided-drawable access", metal.maximumProvidedDrawableAccessNanoseconds));
    timing.rows.emplace_back(rawPresentedHistory(metal));
    timing.rows.emplace_back(rawUnsigned("metal.presentedFrameIntervalHistoryCount",
        "Presented-frame history count", metal.presentedFrameIntervalHistoryCount, "samples"));
    timing.rows.emplace_back(rawFrameLatencyHistory(metal));
    timing.rows.emplace_back(rawUnsigned("metal.frameLatencyHistoryCount",
        "Frame-latency history count", metal.frameLatencyHistoryCount, "samples"));
    sections.emplace_back(std::move(timing));

    PerformanceMetricGroup capture { "Analysis sample capture", { } };
    capture.rows.emplace_back(rawUnsigned("analysis.capture.attemptedChunks", "Attempted chunks",
        analysis.capture.attemptedChunks, "chunks"));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.publishedChunks", "Published chunks",
        analysis.capture.publishedChunks, "chunks"));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.reclaimedReadyChunks",
        "Reclaimed ready chunks", analysis.capture.reclaimedReadyChunks, "chunks"));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.droppedIncomingChunks",
        "Dropped incoming chunks", analysis.capture.droppedIncomingChunks, "chunks"));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.consumerDiscontinuities",
        "Consumer discontinuities", analysis.capture.consumerDiscontinuities));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.lastAttemptedSequence",
        "Last attempted sequence", analysis.capture.lastAttemptedSequence));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.capturedFrames", "Captured frames",
        analysis.capture.capturedFrames, "frames"));
    capture.rows.emplace_back(rawUnsigned("analysis.capture.readyHighWaterMark",
        "Ready-slot high-water mark", analysis.capture.readyHighWaterMark, "slots"));
    capture.rows.emplace_back(rawUnsigned(
        "analysis.capture.readySlots", "Ready slots", analysis.capture.readySlots, "slots"));
    sections.emplace_back(std::move(capture));

    PerformanceMetricGroup meters { "Analysis meter handoff", { } };
    meters.rows.emplace_back(rawUnsigned("analysis.meters.attemptedBlocks", "Attempted blocks",
        analysis.meters.attemptedBlocks, "blocks"));
    meters.rows.emplace_back(rawUnsigned("analysis.meters.publishedBlocks", "Published blocks",
        analysis.meters.publishedBlocks, "blocks"));
    meters.rows.emplace_back(rawUnsigned("analysis.meters.coalescedBlocks", "Coalesced blocks",
        analysis.meters.coalescedBlocks, "blocks"));
    meters.rows.emplace_back(rawUnsigned("analysis.meters.droppedBlocks", "Dropped blocks",
        analysis.meters.droppedBlocks, "blocks"));
    meters.rows.emplace_back(rawUnsigned("analysis.meters.consumerDiscontinuities",
        "Consumer discontinuities", analysis.meters.consumerDiscontinuities));
    meters.rows.emplace_back(rawUnsigned("analysis.meters.readyHighWaterMark",
        "Ready-slot high-water mark", analysis.meters.readyHighWaterMark, "slots"));
    meters.rows.emplace_back(rawUnsigned(
        "analysis.meters.readySlots", "Ready slots", analysis.meters.readySlots, "slots"));
    sections.emplace_back(std::move(meters));

    PerformanceMetricGroup scheduler { "Analysis scheduler", { } };
    scheduler.rows.emplace_back(rawUnsigned(
        "analysis.scheduler.submitted", "Submitted requests", analysis.scheduler.submitted));
    scheduler.rows.emplace_back(
        rawUnsigned("analysis.scheduler.executed", "Executed jobs", analysis.scheduler.executed));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.cancelled",
        "Coalesced or cancelled requests", analysis.scheduler.cancelled));
    sections.emplace_back(std::move(scheduler));

    PerformanceMetricGroup fftConfiguration { "FFT analysis configuration", { } };
    fftConfiguration.rows.emplace_back(rawUnsigned("analysis.fftConfigurationChanges",
        "FFT configuration changes", analysis.fftConfigurationChanges, "changes"));
    fftConfiguration.rows.emplace_back(rawUnsigned("analysis.spectrumTemporalConfigurationChanges",
        "Spectrum temporal configuration changes", analysis.spectrumTemporalConfigurationChanges,
        "changes"));
    fftConfiguration.rows.emplace_back(
        rawUnsigned("analysis.fftGeneration", "FFT generation", analysis.fftGeneration));
    fftConfiguration.rows.emplace_back(rawUnsigned("analysis.configuredFftSize",
        "Configured FFT size", analysis.configuredFftSize, "samples"));
    fftConfiguration.rows.emplace_back(rawUnsigned("analysis.configuredFftWindow",
        "Configured FFT window enum", analysis.configuredFftWindow));
    fftConfiguration.rows.emplace_back(rawUnsigned("analysis.requestedFftSliceRateHz",
        "Requested FFT slice rate", analysis.requestedFftSliceRateHz, "Hz"));
    sections.emplace_back(std::move(fftConfiguration));

    PerformanceMetricGroup spectrogramAnalysis { "Spectrogram mapping and column handoff", { } };
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramTransformsOffered",
        "Raw transforms offered", analysis.spectrogramTransformsOffered, "transforms"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramColumnsMapped",
        "Columns mapped", analysis.spectrogramColumnsMapped, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramMappingFailures",
        "Column mapping failures", analysis.spectrogramMappingFailures, "failures"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramColumnsPublished",
        "Columns published", analysis.spectrogramColumnsPublished, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramColumnsReclaimed",
        "Ready columns reclaimed", analysis.spectrogramColumnsReclaimed, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramColumnsDropped",
        "Incoming columns dropped", analysis.spectrogramColumnsDropped, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramColumnsConsumed",
        "Columns consumed", analysis.spectrogramColumnsConsumed, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramColumnsDiscarded",
        "Queued columns discarded", analysis.spectrogramColumnsDiscarded, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramMappingChanges",
        "Frequency-mapping changes", analysis.spectrogramMappingChanges, "changes"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramCapturedFrameEnd",
        "Latest column captured-frame endpoint", analysis.spectrogramCapturedFrameEnd, "frames"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramMappingGeneration",
        "Frequency-mapping generation", analysis.spectrogramMappingGeneration));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.spectrogramRowCount", "Mapped frequency rows", analysis.spectrogramRowCount));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramQueueReadyHighWaterMark",
        "Ready-column high-water mark", analysis.spectrogramQueueReadyHighWaterMark, "columns"));
    spectrogramAnalysis.rows.emplace_back(rawUnsigned("analysis.spectrogramQueueReadyColumns",
        "Ready columns", analysis.spectrogramQueueReadyColumns, "columns"));
    sections.emplace_back(std::move(spectrogramAnalysis));

    PerformanceMetricGroup stereoAnalysis { "Stereo analysis and correlation", { } };
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoFieldProcessedChunks",
        "Vectorscope chunks processed", analysis.stereoFieldProcessedChunks, "chunks"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoFieldProcessedFrames",
        "Vectorscope source frames processed", analysis.stereoFieldProcessedFrames, "frames"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoFieldSelectedPoints",
        "Vectorscope points selected", analysis.stereoFieldSelectedPoints, "points"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoFieldHistoryResets",
        "Vectorscope history resets", analysis.stereoFieldHistoryResets, "resets"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoFieldInvalidChunks",
        "Invalid vectorscope chunks", analysis.stereoFieldInvalidChunks, "chunks"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoCorrelationProcessedSamples",
        "Correlation samples processed", analysis.stereoCorrelationProcessedSamples, "samples"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoCorrelationPublishedEndpoints",
        "Correlation endpoints published", analysis.stereoCorrelationPublishedEndpoints,
        "endpoints"));
    stereoAnalysis.rows.emplace_back(
        rawUnsigned("analysis.stereoCorrelationConsumedEndpoints", "Correlation endpoints consumed",
            analysis.stereoCorrelationConsumedEndpoints, "endpoints"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoCorrelationStateResets",
        "Correlation state resets", analysis.stereoCorrelationStateResets, "resets"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoCapturedFrameEnd",
        "Stereo captured-frame endpoint", analysis.stereoCapturedFrameEnd, "frames"));
    stereoAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.stereoSequence", "Stereo snapshot sequence", analysis.stereoSequence));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoFieldPointCount",
        "Current vectorscope point count", analysis.stereoFieldPointCount, "points"));
    stereoAnalysis.rows.emplace_back(rawUnsigned("analysis.stereoPointStrideFrames",
        "Vectorscope point stride", analysis.stereoPointStrideFrames, "frames"));
    stereoAnalysis.rows.emplace_back(rawBoolean(
        "analysis.stereoFieldValid", "Vectorscope field valid", analysis.stereoFieldValid));
    stereoAnalysis.rows.emplace_back(rawBoolean(
        "analysis.stereoCorrelationValid", "Correlation valid", analysis.stereoCorrelationValid));
    stereoAnalysis.rows.emplace_back(
        rawBoolean("analysis.stereoMono", "Mono input", analysis.stereoMono));
    sections.emplace_back(std::move(stereoAnalysis));

    PerformanceMetricGroup jobs { "Analysis jobs and publication", { } };
    jobs.rows.emplace_back(
        rawUnsigned("analysis.jobsStarted", "Jobs started", analysis.jobsStarted));
    jobs.rows.emplace_back(
        rawUnsigned("analysis.jobsCompleted", "Jobs completed", analysis.jobsCompleted));
    jobs.rows.emplace_back(
        rawUnsigned("analysis.jobsStopped", "Jobs stopped", analysis.jobsStopped));
    jobs.rows.emplace_back(rawUnsigned("analysis.ignoredGenerationChunks",
        "Ignored generation chunks", analysis.ignoredGenerationChunks, "chunks"));
    jobs.rows.emplace_back(rawUnsigned(
        "analysis.publishedFrames", "Published visualization frames", analysis.publishedFrames));
    jobs.rows.emplace_back(rawUnsigned("analysis.droppedFramePublications",
        "Dropped frame publications", analysis.droppedFramePublications));
    jobs.rows.emplace_back(rawDuration(
        "analysis.lastJobNanoseconds", "Last job duration", analysis.lastJobNanoseconds));
    jobs.rows.emplace_back(rawDuration(
        "analysis.maximumJobNanoseconds", "Maximum job duration", analysis.maximumJobNanoseconds));
    jobs.rows.emplace_back(rawUnsigned("analysis.spectrumTransforms", "Spectrum transforms",
        analysis.spectrumTransforms, "transforms"));
    jobs.rows.emplace_back(rawUnsigned("analysis.lastJobSpectrumTransforms",
        "Last-job spectrum transforms", analysis.lastJobSpectrumTransforms, "transforms"));
    jobs.rows.emplace_back(
        rawUnsigned("analysis.maximumJobSpectrumTransforms", "Maximum per-job spectrum transforms",
            analysis.maximumJobSpectrumTransforms, "transforms"));
    jobs.rows.emplace_back(rawUnsigned("analysis.backlogDiscardedFrames",
        "Backlog frames discarded for freshness", analysis.backlogDiscardedFrames, "frames"));
    sections.emplace_back(std::move(jobs));

    PerformanceMetricGroup freshness { "Analysis freshness and lifecycle", { } };
    freshness.rows.emplace_back(rawUnsigned("analysis.spectrumCapturedFrameEnd",
        "Spectrum captured-frame endpoint", analysis.spectrumCapturedFrameEnd, "frames"));
    freshness.rows.emplace_back(rawUnsigned("analysis.meterCapturedFrameEnd",
        "Meter captured-frame endpoint", analysis.meterCapturedFrameEnd, "frames"));
    freshness.rows.emplace_back(rawUnsigned("analysis.latestCaptureRevision",
        "Latest capture revision", analysis.latestCaptureRevision));
    freshness.rows.emplace_back(rawUnsigned("analysis.lastAnalyzedCaptureRevision",
        "Last analyzed capture revision", analysis.lastAnalyzedCaptureRevision));
    freshness.rows.emplace_back(rawUnsigned("analysis.emptyAnalysisRequestsAvoided",
        "Empty analysis requests avoided", analysis.emptyAnalysisRequestsAvoided));
    freshness.rows.emplace_back(rawUnsigned("analysis.staleFramesPublished",
        "Stale-clear frames published", analysis.staleFramesPublished));
    freshness.rows.emplace_back(rawUnsigned(
        "analysis.peakRmsUserResets", "Peak/RMS user resets", analysis.peakRmsUserResets));
    freshness.rows.emplace_back(rawUnsigned(
        "analysis.spectrumUserClears", "Spectrum user clears", analysis.spectrumUserClears));
    sections.emplace_back(std::move(freshness));
}

void appendRate(std::vector<PerformanceMetricRate>& rates, std::string sourceFieldName,
    std::string label, std::string unit, const std::uint64_t current, const std::uint64_t previous,
    const double elapsedSeconds, const bool baselineIsValid)
{
    PerformanceMetricRate rate { std::move(sourceFieldName), std::move(label), std::move(unit) };

    if (baselineIsValid && current >= previous) {
        const auto delta = current - previous;
        const auto value = static_cast<double>(delta) / elapsedSeconds;
        if (std::isfinite(value)) {
            rate.value = value;
            rate.counterDelta = delta;
            rate.sampleIntervalSeconds = elapsedSeconds;
            rate.available = true;
        }
    }

    rates.emplace_back(std::move(rate));
}

void appendSummedRate(std::vector<PerformanceMetricRate>& rates, std::string sourceFieldName,
    std::string label, std::string unit, const std::uint64_t currentFirst,
    const std::uint64_t previousFirst, const std::uint64_t currentSecond,
    const std::uint64_t previousSecond, const double elapsedSeconds, const bool baselineIsValid)
{
    PerformanceMetricRate rate { std::move(sourceFieldName), std::move(label), std::move(unit) };

    if (baselineIsValid && currentFirst >= previousFirst && currentSecond >= previousSecond) {
        const auto firstDelta = currentFirst - previousFirst;
        const auto secondDelta = currentSecond - previousSecond;

        if (firstDelta <= std::numeric_limits<std::uint64_t>::max() - secondDelta) {
            const auto delta = firstDelta + secondDelta;
            const auto value = static_cast<double>(delta) / elapsedSeconds;
            if (std::isfinite(value)) {
                rate.value = value;
                rate.counterDelta = delta;
                rate.sampleIntervalSeconds = elapsedSeconds;
                rate.available = true;
            }
        }
    }

    rates.emplace_back(std::move(rate));
}

void buildRates(const PerformanceMetricsSnapshot& current,
    const PerformanceMetricsSnapshot& previous, const double elapsedSeconds,
    const bool baselineIsValid, std::vector<PerformanceMetricRate>& rates)
{
    const auto addMetal = [&](std::string fieldName, std::string label, std::string unit,
                              const std::uint64_t currentValue, const std::uint64_t previousValue) {
        appendRate(rates, std::move(fieldName), std::move(label), std::move(unit), currentValue,
            previousValue, elapsedSeconds, baselineIsValid);
    };

    addMetal("metal.displayLinkCallbacks", "Display-link callbacks", "callbacks/s",
        current.metal.displayLinkCallbacks, previous.metal.displayLinkCallbacks);
    addMetal("metal.submittedFrames", "Submitted frames", "frames/s", current.metal.submittedFrames,
        previous.metal.submittedFrames);
    addMetal("metal.completedFrames", "Completed frames", "frames/s", current.metal.completedFrames,
        previous.metal.completedFrames);
    addMetal("metal.gpuTimingSamples", "Valid GPU timing samples", "samples/s",
        current.metal.gpuTimingSamples, previous.metal.gpuTimingSamples);
    addMetal("metal.gpuTimingUnavailableSamples", "Unavailable GPU timing samples", "samples/s",
        current.metal.gpuTimingUnavailableSamples, previous.metal.gpuTimingUnavailableSamples);
    addMetal("metal.commandBufferFailures", "Command-buffer failures", "failures/s",
        current.metal.commandBufferFailures, previous.metal.commandBufferFailures);
    addMetal("metal.presentationCallbacks", "Presentation callbacks", "callbacks/s",
        current.metal.presentationCallbacks, previous.metal.presentationCallbacks);
    addMetal("metal.presentedFrames", "Presented frames", "frames/s", current.metal.presentedFrames,
        previous.metal.presentedFrames);
    addMetal("metal.presentationLatenessSamples", "Valid presentation-lateness samples",
        "samples/s", current.metal.presentationLatenessSamples,
        previous.metal.presentationLatenessSamples);
    addMetal("metal.presentationLatenessUnclassifiableSamples",
        "Unclassifiable presentation-lateness samples", "samples/s",
        current.metal.presentationLatenessUnclassifiableSamples,
        previous.metal.presentationLatenessUnclassifiableSamples);
    addMetal("metal.presentationHistoryDiscardedTimestamps", "History timestamps discarded",
        "timestamps/s", current.metal.presentationHistoryDiscardedTimestamps,
        previous.metal.presentationHistoryDiscardedTimestamps);
    addMetal("metal.frameLatencySamples", "Frame-latency samples", "samples/s",
        current.metal.frameLatencySamples, previous.metal.frameLatencySamples);
    addMetal("metal.frameLatencyTotalTimingSamples", "Valid total frame-latency timings",
        "samples/s", current.metal.frameLatencyTotalTimingSamples,
        previous.metal.frameLatencyTotalTimingSamples);
    addMetal("metal.frameLatencyTotalTimingUnavailableSamples",
        "Unavailable total frame-latency timings", "samples/s",
        current.metal.frameLatencyTotalTimingUnavailableSamples,
        previous.metal.frameLatencyTotalTimingUnavailableSamples);
    addMetal("metal.frameLatencyComponentTimingSamples", "Valid frame-latency component timings",
        "samples/s", current.metal.frameLatencyComponentTimingSamples,
        previous.metal.frameLatencyComponentTimingSamples);
    addMetal("metal.frameLatencyComponentTimingUnavailableSamples",
        "Unavailable frame-latency component timings", "samples/s",
        current.metal.frameLatencyComponentTimingUnavailableSamples,
        previous.metal.frameLatencyComponentTimingUnavailableSamples);
    addMetal("metal.frameLatencyHistoryDiscardedSamples", "Frame-latency history samples discarded",
        "samples/s", current.metal.frameLatencyHistoryDiscardedSamples,
        previous.metal.frameLatencyHistoryDiscardedSamples);
    addMetal("metal.presentationsAfterTarget", "Presentations after target", "events/s",
        current.metal.presentationsAfterTarget, previous.metal.presentationsAfterTarget);
    addMetal("metal.skippedPresentations", "Skipped presentations", "events/s",
        current.metal.skippedPresentations, previous.metal.skippedPresentations);
    addMetal("metal.gpuBackpressureDrops", "Render-buffer admission drops", "drops/s",
        current.metal.gpuBackpressureDrops, previous.metal.gpuBackpressureDrops);
    addMetal("metal.drawableUnavailableDrops", "Drawable-unavailable drops", "drops/s",
        current.metal.drawableUnavailableDrops, previous.metal.drawableUnavailableDrops);
    addMetal("metal.callbackHostDelaySamples", "Valid callback host-delay samples", "samples/s",
        current.metal.callbackHostDelaySamples, previous.metal.callbackHostDelaySamples);
    addMetal("metal.callbackHostDelayUnclassifiableSamples",
        "Unclassifiable callback host-delay samples", "samples/s",
        current.metal.callbackHostDelayUnclassifiableSamples,
        previous.metal.callbackHostDelayUnclassifiableSamples);
    addMetal("metal.callbackAlreadyLateHostDelays", "Callbacks already late (host)", "events/s",
        current.metal.callbackAlreadyLateHostDelays, previous.metal.callbackAlreadyLateHostDelays);
    addMetal("metal.cpuCommitLatenessSamples", "Valid CPU commit-lateness samples", "samples/s",
        current.metal.cpuCommitLatenessSamples, previous.metal.cpuCommitLatenessSamples);
    addMetal("metal.cpuCommitLatenessUnclassifiableSamples",
        "Unclassifiable CPU commit-lateness samples", "samples/s",
        current.metal.cpuCommitLatenessUnclassifiableSamples,
        previous.metal.cpuCommitLatenessUnclassifiableSamples);
    addMetal("metal.cpuCommitDeadlineMisses", "CPU commit deadline misses", "misses/s",
        current.metal.cpuCommitDeadlineMisses, previous.metal.cpuCommitDeadlineMisses);
    addMetal("metal.gpuCompletionLatenessSamples", "Valid GPU completion-lateness samples",
        "samples/s", current.metal.gpuCompletionLatenessSamples,
        previous.metal.gpuCompletionLatenessSamples);
    addMetal("metal.gpuCompletionLatenessUnclassifiableSamples",
        "Unclassifiable GPU completion-lateness samples", "samples/s",
        current.metal.gpuCompletionLatenessUnclassifiableSamples,
        previous.metal.gpuCompletionLatenessUnclassifiableSamples);
    addMetal("metal.gpuCompletionDeadlineMisses", "GPU completion deadline misses", "misses/s",
        current.metal.gpuCompletionDeadlineMisses, previous.metal.gpuCompletionDeadlineMisses);
    addMetal("metal.analysisRequestCalls", "Analysis request calls", "requests/s",
        current.metal.analysisRequestCalls, previous.metal.analysisRequestCalls);
    addMetal("metal.snapshotReads", "Snapshot reads", "snapshots/s", current.metal.snapshotReads,
        previous.metal.snapshotReads);
    addMetal("metal.framesWithNewSnapshot", "Frames with new snapshots", "frames/s",
        current.metal.framesWithNewSnapshot, previous.metal.framesWithNewSnapshot);
    addMetal("metal.spectrogramColumnsRead", "Spectrogram columns read", "columns/s",
        current.metal.spectrogramColumnsRead, previous.metal.spectrogramColumnsRead);
    addMetal("metal.spectrogramColumnsUploaded", "Spectrogram columns uploaded", "columns/s",
        current.metal.spectrogramColumnsUploaded, previous.metal.spectrogramColumnsUploaded);
    addMetal("metal.spectrogramColumnsRejected", "Spectrogram columns rejected", "columns/s",
        current.metal.spectrogramColumnsRejected, previous.metal.spectrogramColumnsRejected);
    addMetal("metal.spectrogramGapColumns", "Spectrogram black gaps", "columns/s",
        current.metal.spectrogramGapColumns, previous.metal.spectrogramGapColumns);
    addMetal("metal.spectrogramHistoryClears", "Spectrogram history clears", "clears/s",
        current.metal.spectrogramHistoryClears, previous.metal.spectrogramHistoryClears);
    addMetal("metal.spectrogramTextureReallocations", "Spectrogram texture reallocations",
        "allocations/s", current.metal.spectrogramTextureReallocations,
        previous.metal.spectrogramTextureReallocations);
    addMetal("metal.spectrogramTextureAllocationFailures",
        "Spectrogram texture allocation failures", "failures/s",
        current.metal.spectrogramTextureAllocationFailures,
        previous.metal.spectrogramTextureAllocationFailures);
    addMetal("metal.spectrogramUploadBackpressureDrops", "Spectrogram upload-backpressure drops",
        "drops/s", current.metal.spectrogramUploadBackpressureDrops,
        previous.metal.spectrogramUploadBackpressureDrops);
    addMetal("metal.spectrogramUploadCommands", "Spectrogram column copy commands", "commands/s",
        current.metal.spectrogramUploadCommands, previous.metal.spectrogramUploadCommands);
    addMetal("metal.spectrogramUploadBytes", "Spectrogram upload throughput", "bytes/s",
        current.metal.spectrogramUploadBytes, previous.metal.spectrogramUploadBytes);
    addMetal("metal.stereoPointInstancesPrepared", "Stereo point instances prepared", "instances/s",
        current.metal.stereoPointInstancesPrepared, previous.metal.stereoPointInstancesPrepared);
    addMetal("metal.stereoPointDrawCalls", "Stereo point draw calls", "calls/s",
        current.metal.stereoPointDrawCalls, previous.metal.stereoPointDrawCalls);

    const auto& capture = current.analysis.capture;
    const auto& previousCapture = previous.analysis.capture;
    appendRate(rates, "analysis.capture.attemptedChunks", "Attempted sample chunks", "chunks/s",
        capture.attemptedChunks, previousCapture.attemptedChunks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.capture.publishedChunks", "Published sample chunks", "chunks/s",
        capture.publishedChunks, previousCapture.publishedChunks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.capture.reclaimedReadyChunks", "Reclaimed ready chunks", "chunks/s",
        capture.reclaimedReadyChunks, previousCapture.reclaimedReadyChunks, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.capture.droppedIncomingChunks", "Dropped incoming chunks",
        "chunks/s", capture.droppedIncomingChunks, previousCapture.droppedIncomingChunks,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.capture.consumerDiscontinuities", "Capture discontinuities",
        "events/s", capture.consumerDiscontinuities, previousCapture.consumerDiscontinuities,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.capture.capturedFrames", "Captured audio frames", "frames/s",
        capture.capturedFrames, previousCapture.capturedFrames, elapsedSeconds, baselineIsValid);
    appendSummedRate(rates, "analysis.capture.lostChunks", "Lost sample chunks", "chunks/s",
        capture.reclaimedReadyChunks, previousCapture.reclaimedReadyChunks,
        capture.droppedIncomingChunks, previousCapture.droppedIncomingChunks, elapsedSeconds,
        baselineIsValid);

    const auto& meters = current.analysis.meters;
    const auto& previousMeters = previous.analysis.meters;
    appendRate(rates, "analysis.meters.attemptedBlocks", "Attempted meter blocks", "blocks/s",
        meters.attemptedBlocks, previousMeters.attemptedBlocks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.meters.publishedBlocks", "Published meter blocks", "blocks/s",
        meters.publishedBlocks, previousMeters.publishedBlocks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.meters.coalescedBlocks", "Coalesced meter blocks", "blocks/s",
        meters.coalescedBlocks, previousMeters.coalescedBlocks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.meters.droppedBlocks", "Dropped meter blocks", "blocks/s",
        meters.droppedBlocks, previousMeters.droppedBlocks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.meters.consumerDiscontinuities", "Meter discontinuities",
        "events/s", meters.consumerDiscontinuities, previousMeters.consumerDiscontinuities,
        elapsedSeconds, baselineIsValid);

    appendRate(rates, "analysis.stereoFieldProcessedChunks", "Vectorscope chunks processed",
        "chunks/s", current.analysis.stereoFieldProcessedChunks,
        previous.analysis.stereoFieldProcessedChunks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoFieldProcessedFrames", "Vectorscope source frames processed",
        "frames/s", current.analysis.stereoFieldProcessedFrames,
        previous.analysis.stereoFieldProcessedFrames, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoFieldSelectedPoints", "Vectorscope points selected",
        "points/s", current.analysis.stereoFieldSelectedPoints,
        previous.analysis.stereoFieldSelectedPoints, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoFieldHistoryResets", "Vectorscope history resets", "resets/s",
        current.analysis.stereoFieldHistoryResets, previous.analysis.stereoFieldHistoryResets,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoFieldInvalidChunks", "Invalid vectorscope chunks", "chunks/s",
        current.analysis.stereoFieldInvalidChunks, previous.analysis.stereoFieldInvalidChunks,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoCorrelationProcessedSamples", "Correlation samples processed",
        "samples/s", current.analysis.stereoCorrelationProcessedSamples,
        previous.analysis.stereoCorrelationProcessedSamples, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoCorrelationPublishedEndpoints",
        "Correlation endpoints published", "endpoints/s",
        current.analysis.stereoCorrelationPublishedEndpoints,
        previous.analysis.stereoCorrelationPublishedEndpoints, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoCorrelationConsumedEndpoints",
        "Correlation endpoints consumed", "endpoints/s",
        current.analysis.stereoCorrelationConsumedEndpoints,
        previous.analysis.stereoCorrelationConsumedEndpoints, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoCorrelationStateResets", "Correlation state resets",
        "resets/s", current.analysis.stereoCorrelationStateResets,
        previous.analysis.stereoCorrelationStateResets, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.stereoSequence", "Stereo snapshot updates", "updates/s",
        current.analysis.stereoSequence, previous.analysis.stereoSequence, elapsedSeconds,
        baselineIsValid);

    const auto& scheduler = current.analysis.scheduler;
    const auto& previousScheduler = previous.analysis.scheduler;
    appendRate(rates, "analysis.scheduler.submitted", "Submitted analysis requests", "requests/s",
        scheduler.submitted, previousScheduler.submitted, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.scheduler.executed", "Executed analysis jobs", "jobs/s",
        scheduler.executed, previousScheduler.executed, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.scheduler.cancelled", "Coalesced or cancelled requests",
        "requests/s", scheduler.cancelled, previousScheduler.cancelled, elapsedSeconds,
        baselineIsValid);

    appendRate(rates, "analysis.jobsStarted", "Jobs started", "jobs/s",
        current.analysis.jobsStarted, previous.analysis.jobsStarted, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.jobsCompleted", "Jobs completed", "jobs/s",
        current.analysis.jobsCompleted, previous.analysis.jobsCompleted, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.jobsStopped", "Jobs stopped", "jobs/s",
        current.analysis.jobsStopped, previous.analysis.jobsStopped, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.ignoredGenerationChunks", "Ignored generation chunks", "chunks/s",
        current.analysis.ignoredGenerationChunks, previous.analysis.ignoredGenerationChunks,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.publishedFrames", "Published visualization frames", "frames/s",
        current.analysis.publishedFrames, previous.analysis.publishedFrames, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.droppedFramePublications", "Dropped frame publications", "drops/s",
        current.analysis.droppedFramePublications, previous.analysis.droppedFramePublications,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrumTransforms", "Spectrum transforms", "transforms/s",
        current.analysis.spectrumTransforms, previous.analysis.spectrumTransforms, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.backlogDiscardedFrames", "Backlog frames discarded", "frames/s",
        current.analysis.backlogDiscardedFrames, previous.analysis.backlogDiscardedFrames,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.emptyAnalysisRequestsAvoided", "Empty requests avoided",
        "requests/s", current.analysis.emptyAnalysisRequestsAvoided,
        previous.analysis.emptyAnalysisRequestsAvoided, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.staleFramesPublished", "Stale-clear frames published", "frames/s",
        current.analysis.staleFramesPublished, previous.analysis.staleFramesPublished,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.peakRmsUserResets", "Peak/RMS user resets", "resets/s",
        current.analysis.peakRmsUserResets, previous.analysis.peakRmsUserResets, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.spectrumUserClears", "Spectrum user clears", "clears/s",
        current.analysis.spectrumUserClears, previous.analysis.spectrumUserClears, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.fftConfigurationChanges", "FFT configuration changes", "changes/s",
        current.analysis.fftConfigurationChanges, previous.analysis.fftConfigurationChanges,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrumTemporalConfigurationChanges",
        "Spectrum temporal configuration changes", "changes/s",
        current.analysis.spectrumTemporalConfigurationChanges,
        previous.analysis.spectrumTemporalConfigurationChanges, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramTransformsOffered", "Spectrogram raw transforms offered",
        "transforms/s", current.analysis.spectrogramTransformsOffered,
        previous.analysis.spectrogramTransformsOffered, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramColumnsMapped", "Spectrogram columns mapped",
        "columns/s", current.analysis.spectrogramColumnsMapped,
        previous.analysis.spectrogramColumnsMapped, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramMappingFailures", "Spectrogram mapping failures",
        "failures/s", current.analysis.spectrogramMappingFailures,
        previous.analysis.spectrogramMappingFailures, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramColumnsPublished", "Spectrogram columns published",
        "columns/s", current.analysis.spectrogramColumnsPublished,
        previous.analysis.spectrogramColumnsPublished, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramColumnsReclaimed", "Spectrogram columns reclaimed",
        "columns/s", current.analysis.spectrogramColumnsReclaimed,
        previous.analysis.spectrogramColumnsReclaimed, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramColumnsDropped", "Spectrogram columns dropped",
        "columns/s", current.analysis.spectrogramColumnsDropped,
        previous.analysis.spectrogramColumnsDropped, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramColumnsConsumed", "Spectrogram columns consumed",
        "columns/s", current.analysis.spectrogramColumnsConsumed,
        previous.analysis.spectrogramColumnsConsumed, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramColumnsDiscarded", "Spectrogram columns discarded",
        "columns/s", current.analysis.spectrogramColumnsDiscarded,
        previous.analysis.spectrogramColumnsDiscarded, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.spectrogramMappingChanges", "Spectrogram mapping changes",
        "changes/s", current.analysis.spectrogramMappingChanges,
        previous.analysis.spectrogramMappingChanges, elapsedSeconds, baselineIsValid);
}

void appendIntervalSample(
    std::deque<std::uint64_t>& history, const std::uint64_t nanoseconds, const std::size_t capacity)
{
    if (nanoseconds == 0)
        return;

    history.emplace_back(nanoseconds);
    while (history.size() > capacity)
        history.pop_front();
}

FrameIntervalStatistics calculateStatistics(const std::vector<std::uint64_t>& samples)
{
    FrameIntervalStatistics result;
    if (samples.empty())
        return result;

    std::vector<double> milliseconds;
    milliseconds.reserve(samples.size());
    for (const auto nanoseconds : samples) {
        if (nanoseconds != 0)
            milliseconds.emplace_back(static_cast<double>(nanoseconds) / nanosecondsPerMillisecond);
    }

    if (milliseconds.empty())
        return result;

    result.available = true;
    result.sampleCount = milliseconds.size();
    result.latestMilliseconds = milliseconds.back();

    auto sorted = milliseconds;
    std::sort(sorted.begin(), sorted.end());
    result.minimumMilliseconds = sorted.front();
    result.maximumMilliseconds = sorted.back();

    long double sum = 0.0L;
    for (const auto value : milliseconds)
        sum += static_cast<long double>(value);
    result.meanMilliseconds = static_cast<double>(sum / milliseconds.size());

    const auto percentileIndex = std::min(sorted.size() - 1,
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(sorted.size()))) - 1);
    result.percentile95Milliseconds = sorted[percentileIndex];

    long double squaredDifferenceSum = 0.0L;
    for (const auto value : milliseconds) {
        const auto difference = static_cast<long double>(value) - result.meanMilliseconds;
        squaredDifferenceSum += difference * difference;
    }
    result.standardDeviationMilliseconds
        = std::sqrt(static_cast<double>(squaredDifferenceSum / milliseconds.size()));

    if (result.meanMilliseconds > 0.0)
        result.equivalentHertz = 1'000.0 / result.meanMilliseconds;

    if (!std::isfinite(result.latestMilliseconds) || !std::isfinite(result.minimumMilliseconds)
        || !std::isfinite(result.meanMilliseconds)
        || !std::isfinite(result.percentile95Milliseconds)
        || !std::isfinite(result.maximumMilliseconds)
        || !std::isfinite(result.standardDeviationMilliseconds)
        || !std::isfinite(result.equivalentHertz)) {
        return { };
    }

    return result;
}

FrameIntervalStatistics calculateStatistics(const std::deque<std::uint64_t>& samples)
{
    return calculateStatistics(std::vector<std::uint64_t>(samples.begin(), samples.end()));
}

FrameIntervalStatistics calculatePresentedStatistics(const MetalRenderTelemetry& telemetry)
{
    std::vector<std::uint64_t> samples;
    const auto count = boundedPresentedHistoryCount(telemetry);
    samples.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const auto& sample = telemetry.presentedFrameIntervalHistory[index];
        if (sample.sequence != 0 && sample.nanoseconds != 0)
            samples.emplace_back(sample.nanoseconds);
    }

    return calculateStatistics(samples);
}

PerformanceMetricRow rateRow(const PerformanceMetricRate& rate)
{
    return { "rate." + rate.sourceFieldName, rate.label,
        rate.available ? formatFinite(rate.value, 3) : std::string("unavailable"), rate.unit,
        rate.available ? formatRawDouble(rate.value) : std::string { }, rate.unit,
        PerformanceMetricKind::derivedRate };
}

void appendStatistic(std::vector<PerformanceMetricRow>& rows, std::string fieldName,
    std::string label, const double value, std::string unit, const bool available,
    const int decimalPlaces)
{
    rows.emplace_back(PerformanceMetricRow { std::move(fieldName), std::move(label),
        available ? formatFinite(value, decimalPlaces) : std::string("unavailable"), unit,
        available ? formatRawDouble(value) : std::string { }, std::move(unit),
        PerformanceMetricKind::derivedStatistic });
}

void appendIntervalStatistics(std::vector<PerformanceMetricRow>& rows, const std::string& prefix,
    const std::string& label, const FrameIntervalStatistics& statistics)
{
    rows.emplace_back(PerformanceMetricRow { prefix + ".sampleCount", label + " sample count",
        statistics.available ? formatUnsigned(statistics.sampleCount) : std::string("unavailable"),
        "samples", statistics.available ? formatUnsigned(statistics.sampleCount) : std::string { },
        "samples", PerformanceMetricKind::derivedStatistic });
    appendStatistic(rows, prefix + ".latestMilliseconds", label + " latest",
        statistics.latestMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".minimumMilliseconds", label + " minimum",
        statistics.minimumMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".meanMilliseconds", label + " mean",
        statistics.meanMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".percentile95Milliseconds", label + " p95",
        statistics.percentile95Milliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".maximumMilliseconds", label + " maximum",
        statistics.maximumMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".standardDeviationMilliseconds", label + " standard deviation",
        statistics.standardDeviationMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".equivalentHertz", label + " equivalent rate",
        statistics.equivalentHertz, "Hz", statistics.available, 2);
}

void appendDerivedSections(PerformanceMetricsViewModel& view)
{
    PerformanceMetricGroup rates { "Derived live rates", { } };
    rates.rows.reserve(view.derived.rates.size());
    for (const auto& rate : view.derived.rates)
        rates.rows.emplace_back(rateRow(rate));
    view.sections.emplace_back(std::move(rates));

    PerformanceMetricGroup intervals { "Frame-interval statistics", { } };
    appendIntervalStatistics(intervals.rows, "statistics.metal.displayCallbackIntervals",
        "Display callbacks (UI-sampled)", view.derived.frameIntervals.displayCallbacks);
    appendIntervalStatistics(intervals.rows, "statistics.metal.targetCallbackIntervals",
        "Target callbacks (UI-sampled)", view.derived.frameIntervals.targetCallbacks);
    appendIntervalStatistics(intervals.rows, "statistics.metal.targetPresentationIntervals",
        "Target presentations (UI-sampled)", view.derived.frameIntervals.targetPresentations);
    appendIntervalStatistics(intervals.rows, "statistics.metal.presentedFrameIntervals",
        "Presented frames (exact history)", view.derived.frameIntervals.presentedFrames);
    view.sections.emplace_back(std::move(intervals));
}

std::string makeReport(
    const PerformanceMetricsViewModel& view, const PerformanceMetricsSnapshot& snapshot)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "Audio Insight per-instance performance metrics\n";
    stream << "rates_rebased = " << (view.derived.ratesRebased ? "true" : "false") << "\n";

    for (const auto& section : view.sections) {
        stream << "\n[" << section.name << "]\n";

        for (const auto& row : section.rows) {
            stream << row.fieldName << " = ";

            if (row.fieldName == "metal.presentedFrameIntervalHistory") {
                stream << presentedHistoryRawValue(snapshot.metal) << " sequence:nanoseconds";
            } else if (row.fieldName == "metal.frameLatencyHistory") {
                stream << frameLatencyHistoryRawValue(snapshot.metal) << ' '
                       << frameLatencyHistoryRawUnit;
            } else if (row.kind == PerformanceMetricKind::derivedRate) {
                const auto sourceFieldName = row.fieldName.starts_with("rate.")
                    ? row.fieldName.substr(5)
                    : std::string { };
                const auto rate = std::find_if(view.derived.rates.begin(), view.derived.rates.end(),
                    [&](const auto& candidate) {
                        return candidate.sourceFieldName == sourceFieldName;
                    });

                if (rate != view.derived.rates.end() && rate->available) {
                    stream << formatRawDouble(rate->value) << ' ' << rate->unit
                           << " (counter_delta = " << rate->counterDelta
                           << "; sample_interval_seconds = "
                           << formatRawDouble(rate->sampleIntervalSeconds) << ')';
                } else {
                    stream << "unavailable";
                    if (!row.unit.empty())
                        stream << ' ' << row.unit;
                }
            } else if (!row.rawValue.empty()) {
                stream << row.rawValue;
                if (!row.rawUnit.empty())
                    stream << ' ' << row.rawUnit;

                if (row.value != row.rawValue || row.unit != row.rawUnit) {
                    stream << " (display: " << row.value;
                    if (!row.unit.empty())
                        stream << ' ' << row.unit;
                    stream << ')';
                }
            } else {
                stream << row.value;
                if (!row.unit.empty())
                    stream << ' ' << row.unit;
            }

            stream << '\n';
        }
    }

    return stream.str();
}
} // namespace

PerformanceMetricsModel::PerformanceMetricsModel(const std::size_t intervalHistoryCapacity)
    : historyCapacity_(std::max<std::size_t>(1, intervalHistoryCapacity))
{
}

PerformanceMetricsViewModel PerformanceMetricsModel::update(
    const PerformanceMetricsSnapshot& snapshot, const double monotonicSeconds,
    const bool includeCopyReport)
{
    auto elapsedSeconds = 0.0;
    if (previousSampleTime_.has_value() && std::isfinite(*previousSampleTime_)
        && std::isfinite(monotonicSeconds)) {
        elapsedSeconds = monotonicSeconds - *previousSampleTime_;
    }

    const auto hasUsablePreviousTime = std::isfinite(elapsedSeconds) && elapsedSeconds > 0.0;
    const auto epochIsContinuous
        = previousSnapshot_.has_value() && snapshot.metal.epoch == previousSnapshot_->metal.epoch;
    const auto baselineIsValid = hasUsablePreviousTime && epochIsContinuous;
    const auto rateIntervalSeconds = baselineIsValid ? elapsedSeconds : 1.0;

    PerformanceMetricsViewModel view;
    view.derived.ratesRebased = !baselineIsValid;

    if (!baselineIsValid) {
        displayCallbackIntervals_.samples.clear();
        targetCallbackIntervals_.samples.clear();
        targetPresentationIntervals_.samples.clear();
    }

    const auto previous = previousSnapshot_.value_or(snapshot);
    buildRates(snapshot, previous, rateIntervalSeconds, baselineIsValid, view.derived.rates);

    const auto displayCallbackAdvanced = !baselineIsValid
        || snapshot.metal.displayLinkCallbacks > previous.metal.displayLinkCallbacks;
    if (displayCallbackAdvanced) {
        appendIntervalSample(displayCallbackIntervals_.samples,
            snapshot.metal.lastDisplayCallbackIntervalNanoseconds, historyCapacity_);
        appendIntervalSample(targetCallbackIntervals_.samples,
            snapshot.metal.lastTargetIntervalNanoseconds, historyCapacity_);
        appendIntervalSample(targetPresentationIntervals_.samples,
            snapshot.metal.lastTargetPresentationIntervalNanoseconds, historyCapacity_);
    }

    view.derived.frameIntervals.displayCallbacks
        = calculateStatistics(displayCallbackIntervals_.samples);
    view.derived.frameIntervals.targetCallbacks
        = calculateStatistics(targetCallbackIntervals_.samples);
    view.derived.frameIntervals.targetPresentations
        = calculateStatistics(targetPresentationIntervals_.samples);
    view.derived.frameIntervals.presentedFrames = calculatePresentedStatistics(snapshot.metal);

    appendRawSections(snapshot, view.sections);
    appendDerivedSections(view);
    if (includeCopyReport)
        view.report = makeReport(view, snapshot);

    previousSnapshot_ = snapshot;
    if (std::isfinite(monotonicSeconds))
        previousSampleTime_ = monotonicSeconds;
    else
        previousSampleTime_.reset();

    return view;
}

std::string PerformanceMetricsModel::buildCopyReport(
    const PerformanceMetricsViewModel& view, const PerformanceMetricsSnapshot& snapshot)
{
    return makeReport(view, snapshot);
}

void PerformanceMetricsModel::reset() noexcept
{
    previousSnapshot_.reset();
    previousSampleTime_.reset();
    displayCallbackIntervals_.samples.clear();
    targetCallbackIntervals_.samples.clear();
    targetPresentationIntervals_.samples.clear();
}
} // namespace audio_insight
