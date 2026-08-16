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

PerformanceMetricRow rawDisplayFramePacing(const MetalDisplayFramePacing framePacing)
{
    auto value = std::string { "unknown" };
    auto rawValue = value;
    switch (framePacing) {
    case MetalDisplayFramePacing::fixedMaximum:
        value = "Fixed maximum";
        rawValue = "fixedMaximum";
        break;
    case MetalDisplayFramePacing::adaptive:
        value = "Adaptive";
        rawValue = "adaptive";
        break;
    }

    return { "metal.configuredDisplayFramePacing", "Configured display frame pacing",
        std::move(value), { }, std::move(rawValue), { }, PerformanceMetricKind::raw };
}

PerformanceMetricRow rawDouble(std::string fieldName, std::string label, const double value,
    std::string unit, const int decimalPlaces)
{
    return { std::move(fieldName), std::move(label), formatFinite(value, decimalPlaces), unit,
        formatRawDouble(value), std::move(unit), PerformanceMetricKind::raw };
}

PerformanceMetricRow rawLoudness(
    std::string fieldName, std::string label, const double value, const bool valid)
{
    auto display = std::string("not-ready");
    auto raw = display;
    if (valid) {
        if (std::isinf(value) && std::signbit(value)) {
            display = "-infinity";
            raw = display;
        } else if (std::isfinite(value)) {
            display = formatFinite(value, 1);
            raw = formatRawDouble(value);
        } else {
            display = "invalid";
            raw = display;
        }
    }

    return { std::move(fieldName), std::move(label), std::move(display), "LUFS", std::move(raw),
        "LUFS", PerformanceMetricKind::raw };
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

std::string audioCallbackBlockPrefix(const std::uint32_t blockSizeFrames)
{
    return "analysis.audioCallback.block" + formatUnsigned(blockSizeFrames);
}

std::uint64_t audioCallbackHistogramSampleCount(
    const AudioCallbackBlockTelemetry& telemetry) noexcept
{
    auto count = std::uint64_t { 0 };
    for (const auto bucket : telemetry.durationHistogram) {
        count = bucket > std::numeric_limits<std::uint64_t>::max() - count
            ? std::numeric_limits<std::uint64_t>::max()
            : count + bucket;
    }
    return count;
}

std::string audioCallbackHistogramRawValue(const AudioCallbackBlockTelemetry& telemetry)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << '[';
    for (auto index = std::size_t { 0 }; index < telemetry.durationHistogram.size(); ++index) {
        if (index != 0)
            stream << ", ";
        stream << index << ':' << telemetry.durationHistogram[index];
    }
    stream << ']';
    return stream.str();
}

constexpr std::string_view audioCallbackHistogramRawUnit
    = "bucket_index:count; regular buckets are 1 us [index,index+1); "
      "bucket 1024 is >=1024 us";

PerformanceMetricRow rawAudioCallbackHistogram(
    const std::uint32_t blockSizeFrames, const AudioCallbackBlockTelemetry& telemetry)
{
    const auto sampleCount = audioCallbackHistogramSampleCount(telemetry);
    const auto overflow = telemetry.durationHistogram[audioCallbackDurationHistogramOverflowBucket];
    return { audioCallbackBlockPrefix(blockSizeFrames) + ".durationHistogram", "Duration histogram",
        formatUnsigned(sampleCount) + " samples; " + formatUnsigned(overflow) + " overflow", { },
        { }, std::string(audioCallbackHistogramRawUnit), PerformanceMetricKind::raw };
}

std::optional<std::size_t> audioCallbackHistogramIndex(const std::string_view fieldName)
{
    for (auto index = std::size_t { 0 }; index < trackedAudioCallbackBlockSizes.size(); ++index) {
        if (fieldName
            == audioCallbackBlockPrefix(trackedAudioCallbackBlockSizes[index])
                + ".durationHistogram") {
            return index;
        }
    }
    return std::nullopt;
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
        "Active display reported maximum", metal.configuredMaximumFramesPerSecond, "Hz"));
    status.rows.emplace_back(rawDisplayFramePacing(metal.configuredDisplayFramePacing));
    status.rows.emplace_back(rawUnsigned("metal.requestedMinimumFramesPerSecond",
        "Requested minimum frame rate", metal.requestedMinimumFramesPerSecond, "Hz"));
    status.rows.emplace_back(rawUnsigned("metal.requestedPreferredFramesPerSecond",
        "Requested preferred frame rate", metal.requestedPreferredFramesPerSecond, "Hz"));
    status.rows.emplace_back(rawUnsigned("metal.requestedMaximumFramesPerSecond",
        "Requested maximum frame rate", metal.requestedMaximumFramesPerSecond, "Hz"));
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
        "Full frames skipped for an outstanding history upload",
        metal.spectrogramUploadBackpressureDrops, "drops"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramUploadDeferrals",
        "History upload deferrals", metal.spectrogramUploadDeferrals, "callbacks"));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramScrollClockInitializations",
        "Fractional Scroll clock initializations", metal.spectrogramScrollClockInitializations,
        "initializations"));
    spectrogramRender.rows.emplace_back(rawDouble("metal.spectrogramScrollHeadOffsetColumns",
        "Fractional Scroll head offset from newest accepted column",
        metal.spectrogramScrollHeadOffsetColumns, "columns", 3));
    spectrogramRender.rows.emplace_back(rawUnsigned("metal.spectrogramScrollUnderrunFrames",
        "Fractional Scroll underrun frames", metal.spectrogramScrollUnderrunFrames, "frames"));
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

    PerformanceMetricGroup loudnessRender { "Renderer Loudness", { } };
    loudnessRender.rows.emplace_back(rawUnsigned("metal.lastLoudnessSequence",
        "Last accepted Loudness sequence", metal.lastLoudnessSequence));
    loudnessRender.rows.emplace_back(rawUnsigned("metal.loudnessMeasurementCapturedFrameEnd",
        "Measurement captured-frame endpoint", metal.loudnessMeasurementCapturedFrameEnd,
        "frames"));
    loudnessRender.rows.emplace_back(rawUnsigned("metal.loudnessIntegratedCapturedFrameEnd",
        "Integrated captured-frame endpoint", metal.loudnessIntegratedCapturedFrameEnd, "frames"));
    loudnessRender.rows.emplace_back(rawLoudness("metal.loudnessMomentaryLufs",
        "Momentary Loudness", metal.loudnessMomentaryLufs, metal.loudnessMomentaryValid));
    loudnessRender.rows.emplace_back(rawLoudness("metal.loudnessShortTermLufs",
        "Short-term Loudness", metal.loudnessShortTermLufs, metal.loudnessShortTermValid));
    loudnessRender.rows.emplace_back(rawLoudness("metal.loudnessIntegratedLufs",
        "Integrated Loudness", metal.loudnessIntegratedLufs, metal.loudnessIntegratedValid));
    loudnessRender.rows.emplace_back(rawDouble("metal.loudnessReferenceLufs",
        "Presentation reference", metal.loudnessReferenceLufs, "LUFS", 1));
    loudnessRender.rows.emplace_back(rawBoolean("metal.loudnessMomentaryValid",
        "Momentary measurement valid", metal.loudnessMomentaryValid));
    loudnessRender.rows.emplace_back(rawBoolean("metal.loudnessShortTermValid",
        "Short-term measurement valid", metal.loudnessShortTermValid));
    loudnessRender.rows.emplace_back(rawBoolean("metal.loudnessIntegratedValid",
        "Integrated measurement valid", metal.loudnessIntegratedValid));
    sections.emplace_back(std::move(loudnessRender));

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

    const auto& audioCallback = analysis.audioCallback;
    PerformanceMetricGroup callbackStatus { "Audio callback status (self-timed)", { } };
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.callbackCount",
        "Callbacks", audioCallback.callbackCount, "callbacks"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.processedFrames",
        "Processed frames", audioCallback.processedFrames, "frames"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.timingSamples",
        "Valid duration samples", audioCallback.timingSamples, "samples"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.timingUnavailable",
        "Unavailable duration samples", audioCallback.timingUnavailable, "samples"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.budgetExceeded",
        "Callbacks over configured budget", audioCallback.budgetExceeded, "callbacks"));
    callbackStatus.rows.emplace_back(rawUnsigned(
        "analysis.audioCallback.untrackedBlockSizeCallbacks", "Untracked block-size callbacks",
        audioCallback.untrackedBlockSizeCallbacks, "callbacks"));
    callbackStatus.rows.emplace_back(rawUnsigned(
        "analysis.audioCallback.concurrentCallbackViolations", "Concurrent callback entries",
        audioCallback.concurrentCallbackViolations, "violations"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.clockAnomalyViolations",
        "Monotonic-clock anomalies", audioCallback.clockAnomalyViolations, "violations"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.rtSafetyViolationCount",
        "Detected bounded RT violations", audioCallback.rtSafetyViolationCount, "violations"));
    callbackStatus.rows.emplace_back(rawUnsigned("analysis.audioCallback.detectorCoverageFlags",
        "Detector coverage flags", audioCallback.detectorCoverageFlags));
    callbackStatus.rows.emplace_back(rawBoolean("analysis.audioCallback.detectorActive",
        "Bounded detector active", audioCallback.detectorActive));
    callbackStatus.rows.emplace_back(rawBoolean("analysis.audioCallback.clockAvailable",
        "Monotonic clock available", audioCallback.clockAvailable));
    callbackStatus.rows.emplace_back(rawBoolean("analysis.audioCallback.allocationDetectorActive",
        "Allocation detector active", audioCallback.allocationDetectorActive));
    callbackStatus.rows.emplace_back(rawBoolean("analysis.audioCallback.lockWaitDetectorActive",
        "Lock/wait detector active", audioCallback.lockWaitDetectorActive));
    sections.emplace_back(std::move(callbackStatus));

    for (auto index = std::size_t { 0 }; index < trackedAudioCallbackBlockSizes.size(); ++index) {
        const auto blockSize = trackedAudioCallbackBlockSizes[index];
        const auto prefix = audioCallbackBlockPrefix(blockSize);
        const auto& block = audioCallback.trackedBlocks[index];
        PerformanceMetricGroup callbackBlock {
            "Audio callback — " + formatUnsigned(blockSize) + " frames", { }
        };
        callbackBlock.rows.emplace_back(
            rawUnsigned(prefix + ".callbackCount", "Callbacks", block.callbackCount, "callbacks"));
        callbackBlock.rows.emplace_back(rawUnsigned(
            prefix + ".timingSamples", "Valid duration samples", block.timingSamples, "samples"));
        callbackBlock.rows.emplace_back(rawUnsigned(prefix + ".budgetExceeded",
            "Callbacks over budget", block.budgetExceeded, "callbacks"));
        callbackBlock.rows.emplace_back(
            rawDuration(prefix + ".budgetNanoseconds", "Duration budget", block.budgetNanoseconds));
        callbackBlock.rows.emplace_back(rawAudioCallbackHistogram(blockSize, block));
        sections.emplace_back(std::move(callbackBlock));
    }

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

    const auto& loudness = analysis.loudness;
    const auto& loudnessMeasurement = analysis.loudnessMeasurement;
    PerformanceMetricGroup loudnessAnalysis { "Loudness analysis and gating", { } };
    loudnessAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.loudness.inputChunks", "Input chunks", loudness.inputChunks, "chunks"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.loudness.inputFrames", "Input frames", loudness.inputFrames, "frames"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.measurementCompletions",
        "Measurement completions", loudness.measurementCompletions, "measurements"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationBlockCompletions",
        "Integration block completions", loudness.integrationBlockCompletions, "blocks"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.fullResets", "Full resets", loudness.fullResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.loudness.explicitResets", "Explicit resets", loudness.explicitResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.generationResets",
        "Capture-generation resets", loudness.generationResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.discontinuityResets",
        "Discontinuity resets", loudness.discontinuityResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.loudness.formatResets", "Format resets", loudness.formatResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.invalidInputResets",
        "Invalid-input resets", loudness.invalidInputResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationResets",
        "Integration-only resets", loudness.integrationResets, "resets"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.liveMeasurementClears",
        "Live-measurement clears", loudness.liveMeasurementClears, "clears"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationCapacityOverflows",
        "Integration-capacity overflows", loudness.integrationCapacityOverflows, "overflows"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationBlocksSinceReset",
        "Current integration blocks", loudness.integrationBlocksSinceReset, "blocks"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.absoluteGatedBlocks",
        "Current absolute-gated blocks", loudness.absoluteGatedBlocks, "blocks"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.relativeGatedBlocks",
        "Current relative-gated blocks", loudness.relativeGatedBlocks, "blocks"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexReservedBytes",
            "Integration index reserved storage", loudness.integrationIndexReservedBytes, "bytes"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationIndexLeafNodes",
        "Integration index leaf nodes", loudness.integrationIndexLeafNodes, "nodes"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexInternalNodes",
            "Integration index internal nodes", loudness.integrationIndexInternalNodes, "nodes"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationIndexLeafCapacity",
        "Integration index leaf capacity", loudness.integrationIndexLeafCapacity, "nodes"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexInternalCapacity",
            "Integration index internal-node capacity", loudness.integrationIndexInternalCapacity,
            "nodes"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationIndexTreeHeight",
        "Integration index tree height", loudness.integrationIndexTreeHeight, "levels"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationIndexQueries",
        "Integration index queries", loudness.integrationIndexQueries, "queries"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexLastNodeVisits", "Last query node visits",
            loudness.integrationIndexLastNodeVisits, "visits"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexMaximumNodeVisits",
            "Maximum query node visits", loudness.integrationIndexMaximumNodeVisits, "visits"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexLastAggregateReads",
            "Last query aggregate reads", loudness.integrationIndexLastAggregateReads, "reads"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.loudness.integrationIndexMaximumAggregateReads", "Maximum query aggregate reads",
        loudness.integrationIndexMaximumAggregateReads, "reads"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexLastBoundaryValueReads",
            "Last query boundary-value reads", loudness.integrationIndexLastBoundaryValueReads,
            "reads"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.integrationIndexMaximumBoundaryValueReads",
            "Maximum query boundary-value reads",
            loudness.integrationIndexMaximumBoundaryValueReads, "reads"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudness.stateSequence", "State sequence", loudness.stateSequence));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.capturedFrameEnd",
        "Latest captured-frame endpoint", loudness.capturedFrameEnd, "frames"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudness.integrationBlockCapacity",
        "Integration block capacity", loudness.integrationBlockCapacity, "blocks"));
    loudnessAnalysis.rows.emplace_back(rawBoolean("analysis.loudness.integrationCapacityExceeded",
        "Integration capacity exceeded", loudness.integrationCapacityExceeded));

    loudnessAnalysis.rows.emplace_back(
        rawLoudness("analysis.loudnessMeasurement.momentaryLufs", "Measured Momentary Loudness",
            loudnessMeasurement.momentaryLufs, loudnessMeasurement.momentaryValid));
    loudnessAnalysis.rows.emplace_back(
        rawLoudness("analysis.loudnessMeasurement.shortTermLufs", "Measured Short-term Loudness",
            loudnessMeasurement.shortTermLufs, loudnessMeasurement.shortTermValid));
    loudnessAnalysis.rows.emplace_back(
        rawLoudness("analysis.loudnessMeasurement.integratedLufs", "Measured Integrated Loudness",
            loudnessMeasurement.integratedLufs, loudnessMeasurement.integratedValid));
    loudnessAnalysis.rows.emplace_back(
        rawLoudness("analysis.loudnessMeasurement.relativeGateLufs", "Relative gate",
            loudnessMeasurement.relativeGateLufs, loudnessMeasurement.integratedValid));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudnessMeasurement.stateSequence",
        "Measurement state sequence", loudnessMeasurement.stateSequence));
    loudnessAnalysis.rows.emplace_back(rawUnsigned(
        "analysis.loudnessMeasurement.measurementCompletionCount", "Measurement completion count",
        loudnessMeasurement.measurementCompletionCount, "measurements"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudnessMeasurement.integrationBlockCount", "Integration block count",
            loudnessMeasurement.integrationBlockCount, "blocks"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudnessMeasurement.absoluteGatedBlockCount",
            "Absolute-gated block count", loudnessMeasurement.absoluteGatedBlockCount, "blocks"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudnessMeasurement.relativeGatedBlockCount",
            "Relative-gated block count", loudnessMeasurement.relativeGatedBlockCount, "blocks"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudnessMeasurement.measurementCapturedFrameEnd",
            "Measurement captured-frame endpoint", loudnessMeasurement.measurementCapturedFrameEnd,
            "frames"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudnessMeasurement.integratedCapturedFrameEnd",
            "Integrated captured-frame endpoint", loudnessMeasurement.integratedCapturedFrameEnd,
            "frames"));
    loudnessAnalysis.rows.emplace_back(
        rawUnsigned("analysis.loudnessMeasurement.integrationBlockCapacity",
            "Integration block capacity", loudnessMeasurement.integrationBlockCapacity, "blocks"));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudnessMeasurement.generation",
        "Capture generation", loudnessMeasurement.generation));
    loudnessAnalysis.rows.emplace_back(rawUnsigned("analysis.loudnessMeasurement.channelCount",
        "Channel count", loudnessMeasurement.channelCount, "channels"));
    loudnessAnalysis.rows.emplace_back(rawDouble("analysis.loudnessMeasurement.sampleRate",
        "Sample rate", loudnessMeasurement.sampleRate, "Hz", 2));
    loudnessAnalysis.rows.emplace_back(rawBoolean("analysis.loudnessMeasurement.momentaryValid",
        "Momentary valid", loudnessMeasurement.momentaryValid));
    loudnessAnalysis.rows.emplace_back(rawBoolean("analysis.loudnessMeasurement.shortTermValid",
        "Short-term valid", loudnessMeasurement.shortTermValid));
    loudnessAnalysis.rows.emplace_back(rawBoolean("analysis.loudnessMeasurement.integratedValid",
        "Integrated valid", loudnessMeasurement.integratedValid));
    loudnessAnalysis.rows.emplace_back(
        rawBoolean("analysis.loudnessMeasurement.integrationCapacityExceeded",
            "Integration capacity exceeded", loudnessMeasurement.integrationCapacityExceeded));
    sections.emplace_back(std::move(loudnessAnalysis));

    PerformanceMetricGroup scheduler { "Analysis scheduler", { } };
    scheduler.rows.emplace_back(rawUnsigned(
        "analysis.scheduler.submitted", "Submitted requests", analysis.scheduler.submitted));
    scheduler.rows.emplace_back(
        rawUnsigned("analysis.scheduler.executed", "Executed jobs", analysis.scheduler.executed));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.cancelled",
        "Coalesced or cancelled requests", analysis.scheduler.cancelled));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.queueWaitSamples",
        "Queue-wait samples", analysis.scheduler.queueWaitSamples, "samples"));
    scheduler.rows.emplace_back(rawDuration("analysis.scheduler.lastQueueWaitNanoseconds",
        "Last queue wait", analysis.scheduler.lastQueueWaitNanoseconds));
    scheduler.rows.emplace_back(rawDuration("analysis.scheduler.maximumQueueWaitNanoseconds",
        "Maximum queue wait", analysis.scheduler.maximumQueueWaitNanoseconds));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.queueWaitDeadlineMisses",
        "Queue-wait deadline misses", analysis.scheduler.queueWaitDeadlineMisses, "misses"));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.jobTurnaroundSamples",
        "Job-turnaround samples", analysis.scheduler.jobTurnaroundSamples, "samples"));
    scheduler.rows.emplace_back(rawDuration("analysis.scheduler.lastJobTurnaroundNanoseconds",
        "Last job turnaround", analysis.scheduler.lastJobTurnaroundNanoseconds));
    scheduler.rows.emplace_back(rawDuration("analysis.scheduler.maximumJobTurnaroundNanoseconds",
        "Maximum job turnaround", analysis.scheduler.maximumJobTurnaroundNanoseconds));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.jobDeadlineMisses",
        "Job-turnaround deadline misses", analysis.scheduler.jobDeadlineMisses, "misses"));
    scheduler.rows.emplace_back(rawUnsigned("analysis.scheduler.timingUnavailable",
        "Unavailable scheduler timings", analysis.scheduler.timingUnavailable, "measurements"));
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
    freshness.rows.emplace_back(rawUnsigned(
        "analysis.captureGeneration", "Active capture generation", analysis.captureGeneration));
    freshness.rows.emplace_back(rawDouble(
        "analysis.captureSampleRate", "Capture sample rate", analysis.captureSampleRate, "Hz", 2));
    freshness.rows.emplace_back(rawUnsigned("analysis.spectrumFreshnessFrames",
        "Spectrum pipeline freshness", analysis.spectrumFreshnessFrames, "frames"));
    freshness.rows.emplace_back(rawDuration("analysis.spectrumFreshnessNanoseconds",
        "Spectrum pipeline freshness", analysis.spectrumFreshnessNanoseconds));
    freshness.rows.emplace_back(rawUnsigned("analysis.peakRmsFreshnessFrames",
        "Peak/RMS pipeline freshness", analysis.peakRmsFreshnessFrames, "frames"));
    freshness.rows.emplace_back(rawDuration("analysis.peakRmsFreshnessNanoseconds",
        "Peak/RMS pipeline freshness", analysis.peakRmsFreshnessNanoseconds));
    freshness.rows.emplace_back(rawBoolean("analysis.spectrumFreshnessValid",
        "Spectrum freshness valid", analysis.spectrumFreshnessValid));
    freshness.rows.emplace_back(rawBoolean("analysis.peakRmsFreshnessValid",
        "Peak/RMS freshness valid", analysis.peakRmsFreshnessValid));
    freshness.rows.emplace_back(rawUnsigned("analysis.latestCaptureRevision",
        "Latest capture revision", analysis.latestCaptureRevision));
    freshness.rows.emplace_back(rawUnsigned("analysis.lastAnalyzedCaptureRevision",
        "Last analyzed capture revision", analysis.lastAnalyzedCaptureRevision));
    freshness.rows.emplace_back(rawUnsigned("analysis.emptyAnalysisRequestsAvoided",
        "Empty analysis requests avoided", analysis.emptyAnalysisRequestsAvoided));
    freshness.rows.emplace_back(rawUnsigned("analysis.captureBoundaryRequestsDeferred",
        "Capture-boundary requests deferred", analysis.captureBoundaryRequestsDeferred));
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
    addMetal("metal.spectrogramUploadDeferrals", "Spectrogram history upload deferrals",
        "deferrals/s", current.metal.spectrogramUploadDeferrals,
        previous.metal.spectrogramUploadDeferrals);
    addMetal("metal.spectrogramScrollClockInitializations",
        "Spectrogram fractional Scroll clock initializations", "initializations/s",
        current.metal.spectrogramScrollClockInitializations,
        previous.metal.spectrogramScrollClockInitializations);
    addMetal("metal.spectrogramScrollUnderrunFrames", "Spectrogram fractional Scroll underruns",
        "frames/s", current.metal.spectrogramScrollUnderrunFrames,
        previous.metal.spectrogramScrollUnderrunFrames);
    addMetal("metal.spectrogramUploadCommands", "Spectrogram column copy commands", "commands/s",
        current.metal.spectrogramUploadCommands, previous.metal.spectrogramUploadCommands);
    addMetal("metal.spectrogramUploadBytes", "Spectrogram upload throughput", "bytes/s",
        current.metal.spectrogramUploadBytes, previous.metal.spectrogramUploadBytes);
    addMetal("metal.stereoPointInstancesPrepared", "Stereo point instances prepared", "instances/s",
        current.metal.stereoPointInstancesPrepared, previous.metal.stereoPointInstancesPrepared);
    addMetal("metal.stereoPointDrawCalls", "Stereo point draw calls", "calls/s",
        current.metal.stereoPointDrawCalls, previous.metal.stereoPointDrawCalls);

    const auto& audioCallback = current.analysis.audioCallback;
    const auto& previousAudioCallback = previous.analysis.audioCallback;
    appendRate(rates, "analysis.audioCallback.callbackCount", "Audio callbacks", "callbacks/s",
        audioCallback.callbackCount, previousAudioCallback.callbackCount, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.audioCallback.processedFrames", "Audio callback frames", "frames/s",
        audioCallback.processedFrames, previousAudioCallback.processedFrames, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.audioCallback.timingSamples", "Audio callback timing samples",
        "samples/s", audioCallback.timingSamples, previousAudioCallback.timingSamples,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.audioCallback.timingUnavailable",
        "Unavailable audio callback timings", "samples/s", audioCallback.timingUnavailable,
        previousAudioCallback.timingUnavailable, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.audioCallback.budgetExceeded", "Audio callbacks over budget",
        "callbacks/s", audioCallback.budgetExceeded, previousAudioCallback.budgetExceeded,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.audioCallback.untrackedBlockSizeCallbacks",
        "Untracked block-size callbacks", "callbacks/s", audioCallback.untrackedBlockSizeCallbacks,
        previousAudioCallback.untrackedBlockSizeCallbacks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.audioCallback.concurrentCallbackViolations",
        "Concurrent callback entries", "violations/s", audioCallback.concurrentCallbackViolations,
        previousAudioCallback.concurrentCallbackViolations, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.audioCallback.clockAnomalyViolations",
        "Audio callback clock anomalies", "violations/s", audioCallback.clockAnomalyViolations,
        previousAudioCallback.clockAnomalyViolations, elapsedSeconds, baselineIsValid);

    for (auto index = std::size_t { 0 }; index < trackedAudioCallbackBlockSizes.size(); ++index) {
        const auto prefix = audioCallbackBlockPrefix(trackedAudioCallbackBlockSizes[index]);
        const auto& block = audioCallback.trackedBlocks[index];
        const auto& previousBlock = previousAudioCallback.trackedBlocks[index];
        appendRate(rates, prefix + ".callbackCount",
            formatUnsigned(trackedAudioCallbackBlockSizes[index]) + "-frame callbacks",
            "callbacks/s", block.callbackCount, previousBlock.callbackCount, elapsedSeconds,
            baselineIsValid);
        appendRate(rates, prefix + ".timingSamples",
            formatUnsigned(trackedAudioCallbackBlockSizes[index]) + "-frame timing samples",
            "samples/s", block.timingSamples, previousBlock.timingSamples, elapsedSeconds,
            baselineIsValid);
        appendRate(rates, prefix + ".budgetExceeded",
            formatUnsigned(trackedAudioCallbackBlockSizes[index]) + "-frame budget misses",
            "callbacks/s", block.budgetExceeded, previousBlock.budgetExceeded, elapsedSeconds,
            baselineIsValid);
    }

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

    const auto& loudness = current.analysis.loudness;
    const auto& previousLoudness = previous.analysis.loudness;
    appendRate(rates, "analysis.loudness.inputChunks", "Loudness input chunks", "chunks/s",
        loudness.inputChunks, previousLoudness.inputChunks, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.inputFrames", "Loudness input frames", "frames/s",
        loudness.inputFrames, previousLoudness.inputFrames, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.measurementCompletions",
        "Loudness measurement completions", "measurements/s", loudness.measurementCompletions,
        previousLoudness.measurementCompletions, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.integrationBlockCompletions",
        "Loudness integration block completions", "blocks/s", loudness.integrationBlockCompletions,
        previousLoudness.integrationBlockCompletions, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.fullResets", "Loudness full resets", "resets/s",
        loudness.fullResets, previousLoudness.fullResets, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.explicitResets", "Loudness explicit resets", "resets/s",
        loudness.explicitResets, previousLoudness.explicitResets, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.generationResets", "Loudness generation resets",
        "resets/s", loudness.generationResets, previousLoudness.generationResets, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.loudness.discontinuityResets", "Loudness discontinuity resets",
        "resets/s", loudness.discontinuityResets, previousLoudness.discontinuityResets,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.formatResets", "Loudness format resets", "resets/s",
        loudness.formatResets, previousLoudness.formatResets, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.invalidInputResets", "Loudness invalid-input resets",
        "resets/s", loudness.invalidInputResets, previousLoudness.invalidInputResets,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.integrationResets", "Loudness integration-only resets",
        "resets/s", loudness.integrationResets, previousLoudness.integrationResets, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.loudness.liveMeasurementClears", "Loudness live-measurement clears",
        "clears/s", loudness.liveMeasurementClears, previousLoudness.liveMeasurementClears,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.integrationCapacityOverflows",
        "Loudness integration-capacity overflows", "overflows/s",
        loudness.integrationCapacityOverflows, previousLoudness.integrationCapacityOverflows,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.loudness.integrationIndexQueries",
        "Integrated Loudness index queries", "queries/s", loudness.integrationIndexQueries,
        previousLoudness.integrationIndexQueries, elapsedSeconds, baselineIsValid);

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
    appendRate(rates, "analysis.scheduler.queueWaitSamples", "Scheduler queue-wait samples",
        "samples/s", scheduler.queueWaitSamples, previousScheduler.queueWaitSamples, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.scheduler.queueWaitDeadlineMisses",
        "Scheduler queue-wait deadline misses", "misses/s", scheduler.queueWaitDeadlineMisses,
        previousScheduler.queueWaitDeadlineMisses, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.scheduler.jobTurnaroundSamples", "Scheduler job-turnaround samples",
        "samples/s", scheduler.jobTurnaroundSamples, previousScheduler.jobTurnaroundSamples,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.scheduler.jobDeadlineMisses",
        "Scheduler job-turnaround deadline misses", "misses/s", scheduler.jobDeadlineMisses,
        previousScheduler.jobDeadlineMisses, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.scheduler.timingUnavailable", "Unavailable scheduler timings",
        "measurements/s", scheduler.timingUnavailable, previousScheduler.timingUnavailable,
        elapsedSeconds, baselineIsValid);

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
    appendRate(rates, "analysis.spectrumTransforms", "Achieved FFT slice rate", "Hz",
        current.analysis.spectrumTransforms, previous.analysis.spectrumTransforms, elapsedSeconds,
        baselineIsValid);
    appendRate(rates, "analysis.backlogDiscardedFrames", "Backlog frames discarded", "frames/s",
        current.analysis.backlogDiscardedFrames, previous.analysis.backlogDiscardedFrames,
        elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.emptyAnalysisRequestsAvoided", "Empty requests avoided",
        "requests/s", current.analysis.emptyAnalysisRequestsAvoided,
        previous.analysis.emptyAnalysisRequestsAvoided, elapsedSeconds, baselineIsValid);
    appendRate(rates, "analysis.captureBoundaryRequestsDeferred",
        "Capture-boundary requests deferred", "requests/s",
        current.analysis.captureBoundaryRequestsDeferred,
        previous.analysis.captureBoundaryRequestsDeferred, elapsedSeconds, baselineIsValid);
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

void appendDurationSample(
    std::deque<std::uint64_t>& history, const std::uint64_t nanoseconds, const std::size_t capacity)
{
    history.emplace_back(nanoseconds);
    while (history.size() > capacity)
        history.pop_front();
}

FrameIntervalStatistics calculateStatistics(
    const std::vector<std::uint64_t>& samples, const bool zeroIsValid = false)
{
    FrameIntervalStatistics result;
    if (samples.empty())
        return result;

    std::vector<double> milliseconds;
    milliseconds.reserve(samples.size());
    for (const auto nanoseconds : samples) {
        if (zeroIsValid || nanoseconds != 0)
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

    const auto percentile95Index = std::min(sorted.size() - 1,
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(sorted.size()))) - 1);
    result.percentile95Milliseconds = sorted[percentile95Index];
    const auto percentile99Index = std::min(sorted.size() - 1,
        static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(sorted.size()))) - 1);
    result.percentile99Milliseconds = sorted[percentile99Index];

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
        || !std::isfinite(result.percentile99Milliseconds)
        || !std::isfinite(result.maximumMilliseconds)
        || !std::isfinite(result.standardDeviationMilliseconds)
        || !std::isfinite(result.equivalentHertz)) {
        return { };
    }

    return result;
}

FrameIntervalStatistics calculateStatistics(
    const std::deque<std::uint64_t>& samples, const bool zeroIsValid = false)
{
    return calculateStatistics(
        std::vector<std::uint64_t>(samples.begin(), samples.end()), zeroIsValid);
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

AudioCallbackBlockStatistics calculateAudioCallbackBlockStatistics(
    const std::uint32_t blockSizeFrames, const AudioCallbackBlockTelemetry& telemetry) noexcept
{
    AudioCallbackBlockStatistics result;
    result.blockSizeFrames = blockSizeFrames;
    result.sampleCount = audioCallbackHistogramSampleCount(telemetry);
    result.overflowSamples
        = telemetry.durationHistogram[audioCallbackDurationHistogramOverflowBucket];
    result.budgetNanoseconds = telemetry.budgetNanoseconds;
    result.budgetAvailable = telemetry.budgetNanoseconds != 0;
    if (result.sampleCount == 0)
        return result;

    const auto percentileRank = result.sampleCount - (result.sampleCount / 100);
    auto cumulative = std::uint64_t { 0 };
    auto percentileBucket = telemetry.durationHistogram.size();
    for (auto index = std::size_t { 0 }; index < telemetry.durationHistogram.size(); ++index) {
        const auto count = telemetry.durationHistogram[index];
        cumulative = count > std::numeric_limits<std::uint64_t>::max() - cumulative
            ? std::numeric_limits<std::uint64_t>::max()
            : cumulative + count;
        if (cumulative >= percentileRank) {
            percentileBucket = index;
            break;
        }
    }

    if (percentileBucket < audioCallbackDurationHistogramOverflowBucket) {
        result.percentile99Available = true;
        result.percentile99UpperBoundNanoseconds
            = (percentileBucket + 1) * audioCallbackDurationHistogramBucketNanoseconds;
        if (result.budgetAvailable) {
            result.budgetResultAvailable = true;
            result.budgetPassed
                = result.percentile99UpperBoundNanoseconds <= result.budgetNanoseconds;
        }
    } else if (percentileBucket == audioCallbackDurationHistogramOverflowBucket) {
        result.percentile99Overflow = true;
        constexpr auto overflowLowerBound = audioCallbackDurationHistogramRegularBuckets
            * audioCallbackDurationHistogramBucketNanoseconds;
        if (result.budgetAvailable && result.budgetNanoseconds < overflowLowerBound)
            result.budgetResultAvailable = true;
    }

    return result;
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
    appendStatistic(rows, prefix + ".percentile99Milliseconds", label + " p99",
        statistics.percentile99Milliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".maximumMilliseconds", label + " maximum",
        statistics.maximumMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".standardDeviationMilliseconds", label + " standard deviation",
        statistics.standardDeviationMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".equivalentHertz", label + " equivalent rate",
        statistics.equivalentHertz, "Hz", statistics.available, 2);
}

void appendDurationStatistics(std::vector<PerformanceMetricRow>& rows, const std::string& prefix,
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
    appendStatistic(rows, prefix + ".percentile99Milliseconds", label + " p99",
        statistics.percentile99Milliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".maximumMilliseconds", label + " maximum",
        statistics.maximumMilliseconds, "ms", statistics.available, 3);
    appendStatistic(rows, prefix + ".standardDeviationMilliseconds", label + " standard deviation",
        statistics.standardDeviationMilliseconds, "ms", statistics.available, 3);
}

void appendAudioCallbackStatistics(
    std::vector<PerformanceMetricRow>& rows, const AudioCallbackBlockStatistics& statistics)
{
    const auto prefix = "statistics." + audioCallbackBlockPrefix(statistics.blockSizeFrames);
    rows.emplace_back(PerformanceMetricRow { prefix + ".sampleCount", "Histogram sample count",
        formatUnsigned(statistics.sampleCount), "samples", formatUnsigned(statistics.sampleCount),
        "samples", PerformanceMetricKind::derivedStatistic });

    auto percentileDisplay = std::string("unavailable");
    auto percentileRaw = std::string { };
    if (statistics.percentile99Available) {
        percentileDisplay
            = formatFinite(static_cast<double>(statistics.percentile99UpperBoundNanoseconds)
                    / nanosecondsPerMillisecond,
                3);
        percentileRaw = formatUnsigned(statistics.percentile99UpperBoundNanoseconds);
    } else if (statistics.percentile99Overflow) {
        percentileDisplay = ">= 1.024";
        percentileRaw = ">=1024000";
    }
    rows.emplace_back(PerformanceMetricRow { prefix + ".percentile99UpperBoundNanoseconds",
        "Self-timed lifetime p99 duration upper bound", std::move(percentileDisplay), "ms",
        std::move(percentileRaw), "ns", PerformanceMetricKind::derivedStatistic });

    rows.emplace_back(PerformanceMetricRow { prefix + ".overflowSamples",
        "Histogram overflow samples", formatUnsigned(statistics.overflowSamples), "samples",
        formatUnsigned(statistics.overflowSamples), "samples",
        PerformanceMetricKind::derivedStatistic });
    const auto histogramStatus = statistics.sampleCount == 0 ? std::string("no samples")
        : statistics.percentile99Overflow                    ? std::string("p99 in overflow")
        : statistics.overflowSamples == 0 ? std::string("fully bounded")
                                          : std::string("tail overflow; p99 bounded");
    rows.emplace_back(PerformanceMetricRow { prefix + ".histogramStatus", "Histogram status",
        histogramStatus, { }, histogramStatus, { }, PerformanceMetricKind::derivedStatistic });

    const auto budgetResult = statistics.budgetResultAvailable
        ? statistics.budgetPassed ? std::string("pass") : std::string("fail")
        : std::string("unavailable");
    rows.emplace_back(
        PerformanceMetricRow { prefix + ".budgetResult", "P99 upper-bound budget result",
            budgetResult, { }, budgetResult, { }, PerformanceMetricKind::derivedStatistic });
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

    PerformanceMetricGroup callbackStatistics { "Audio callback duration statistics", { } };
    for (const auto& block : view.derived.audioCallbackBlocks)
        appendAudioCallbackStatistics(callbackStatistics.rows, block);
    view.sections.emplace_back(std::move(callbackStatistics));

    PerformanceMetricGroup analysisDurations { "Analysis latency statistics", { } };
    appendDurationStatistics(analysisDurations.rows, "statistics.analysis.scheduler.queueWait",
        "Scheduler queue wait (UI-sampled)", view.derived.analysisDurations.schedulerQueueWait);
    appendDurationStatistics(analysisDurations.rows, "statistics.analysis.scheduler.jobTurnaround",
        "Scheduler request-to-completion (UI-sampled)",
        view.derived.analysisDurations.schedulerJobTurnaround);
    appendDurationStatistics(analysisDurations.rows, "statistics.analysis.spectrumFreshness",
        "Spectrum pipeline freshness (UI-sampling edge)",
        view.derived.analysisDurations.spectrumFreshness);
    appendDurationStatistics(analysisDurations.rows, "statistics.analysis.peakRmsFreshness",
        "Peak/RMS pipeline freshness (UI-sampling edge)",
        view.derived.analysisDurations.peakRmsFreshness);
    view.sections.emplace_back(std::move(analysisDurations));
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
            } else if (const auto histogramIndex = audioCallbackHistogramIndex(row.fieldName);
                histogramIndex.has_value()) {
                stream << audioCallbackHistogramRawValue(
                    snapshot.analysis.audioCallback.trackedBlocks[*histogramIndex])
                       << ' ' << audioCallbackHistogramRawUnit;
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

    const auto schedulerHistoryIsContinuous = previousSnapshot_.has_value()
        && snapshot.analysis.scheduler.queueWaitSamples
            >= previous.analysis.scheduler.queueWaitSamples
        && snapshot.analysis.scheduler.jobTurnaroundSamples
            >= previous.analysis.scheduler.jobTurnaroundSamples;
    if (!schedulerHistoryIsContinuous) {
        schedulerQueueWaitDurations_.samples.clear();
        schedulerJobTurnaroundDurations_.samples.clear();
    }
    const auto queueWaitAdvanced = snapshot.analysis.scheduler.queueWaitSamples > 0
        && (!schedulerHistoryIsContinuous
            || snapshot.analysis.scheduler.queueWaitSamples
                > previous.analysis.scheduler.queueWaitSamples);
    if (queueWaitAdvanced) {
        appendDurationSample(schedulerQueueWaitDurations_.samples,
            snapshot.analysis.scheduler.lastQueueWaitNanoseconds, historyCapacity_);
    }
    const auto jobTurnaroundAdvanced = snapshot.analysis.scheduler.jobTurnaroundSamples > 0
        && (!schedulerHistoryIsContinuous
            || snapshot.analysis.scheduler.jobTurnaroundSamples
                > previous.analysis.scheduler.jobTurnaroundSamples);
    if (jobTurnaroundAdvanced) {
        appendDurationSample(schedulerJobTurnaroundDurations_.samples,
            snapshot.analysis.scheduler.lastJobTurnaroundNanoseconds, historyCapacity_);
    }

    if (!freshnessCaptureGeneration_.has_value()
        || *freshnessCaptureGeneration_ != snapshot.analysis.captureGeneration) {
        spectrumFreshnessDurations_.samples.clear();
        peakRmsFreshnessDurations_.samples.clear();
        freshnessCaptureGeneration_ = snapshot.analysis.captureGeneration;
    }
    if (snapshot.analysis.captureGeneration != 0) {
        if (snapshot.analysis.spectrumFreshnessValid) {
            appendDurationSample(spectrumFreshnessDurations_.samples,
                snapshot.analysis.spectrumFreshnessNanoseconds, historyCapacity_);
        }
        if (snapshot.analysis.peakRmsFreshnessValid) {
            appendDurationSample(peakRmsFreshnessDurations_.samples,
                snapshot.analysis.peakRmsFreshnessNanoseconds, historyCapacity_);
        }
    }

    view.derived.analysisDurations.schedulerQueueWait
        = calculateStatistics(schedulerQueueWaitDurations_.samples, true);
    view.derived.analysisDurations.schedulerJobTurnaround
        = calculateStatistics(schedulerJobTurnaroundDurations_.samples, true);
    view.derived.analysisDurations.spectrumFreshness
        = calculateStatistics(spectrumFreshnessDurations_.samples, true);
    view.derived.analysisDurations.peakRmsFreshness
        = calculateStatistics(peakRmsFreshnessDurations_.samples, true);
    for (auto index = std::size_t { 0 }; index < trackedAudioCallbackBlockSizes.size(); ++index) {
        view.derived.audioCallbackBlocks[index]
            = calculateAudioCallbackBlockStatistics(trackedAudioCallbackBlockSizes[index],
                snapshot.analysis.audioCallback.trackedBlocks[index]);
    }

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
    schedulerQueueWaitDurations_.samples.clear();
    schedulerJobTurnaroundDurations_.samples.clear();
    spectrumFreshnessDurations_.samples.clear();
    peakRmsFreshnessDurations_.samples.clear();
    freshnessCaptureGeneration_.reset();
}
} // namespace audio_insight
