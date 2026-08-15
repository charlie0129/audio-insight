// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MetalVisualization.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include <os/lock.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace audio_insight::detail {
class MetalRenderBackend;
}

@interface AIAudioInsightMetalView : MTKView <CAMetalDisplayLinkDelegate> {
@private
    audio_insight::detail::MetalRenderBackend* renderBackend;
    NSWindow* observedWindow;
    CAMetalDisplayLink* metalDisplayLink;
}

- (void)attachRenderBackend:(audio_insight::detail::MetalRenderBackend*)backend;
- (void)detachRenderBackend;
- (void)setDisplayLinkPaused:(BOOL)shouldBePaused;
- (void)setDisplayLinkMaximumFramesPerSecond:(NSInteger)maximumFramesPerSecond;
- (BOOL)hasDisplayLink;

@end

namespace audio_insight::detail {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t renderBufferCount = 3;
constexpr std::size_t maximumVertexCount = 8'192;

// The fixed capacity covers five filled/bordered/header-divided tiles, every
// accepted grid line, the maximum FFT trace, and both live meter channels. Keep
// this proof beside the allocation so a future geometry builder cannot quietly
// rely on the bounds checks to truncate a frame.
constexpr std::size_t maximumShellVertices = dashboardPanelCount * 36;
constexpr std::size_t maximumGridVertices = (10 + 16) * 6;
constexpr std::size_t maximumSpectrumVertices = 2 * spectrumBinCount;
constexpr std::size_t maximumMeterVertices = 2 * 3 * 6;
static_assert(
    maximumShellVertices + maximumGridVertices + maximumSpectrumVertices + maximumMeterVertices
    <= maximumVertexCount);

constexpr float minimumSpectrumFrequency = 20.0F;
constexpr float maximumSpectrumFrequency = 20'000.0F;
constexpr float minimumMeterDecibels = -60.0F;
constexpr float maximumMeterDecibels = 3.0F;
constexpr float minimumAllowedSpectrumFloor = -160.0F;
constexpr float maximumAllowedSpectrumCeiling = 24.0F;
constexpr float minimumSpectrumRange = 6.0F;

struct MetalVertex {
    simd_float2 position;
    simd_float4 colour;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

struct AtomicRenderTelemetry {
    std::uint64_t epoch = 1;
    std::atomic<std::uint64_t> displayLinkCallbacks { 0 };
    std::atomic<std::uint64_t> submittedFrames { 0 };
    std::atomic<std::uint64_t> completedFrames { 0 };
    std::atomic<std::uint64_t> gpuTimingSamples { 0 };
    std::atomic<std::uint64_t> gpuTimingUnavailableSamples { 0 };
    std::atomic<std::uint64_t> commandBufferFailures { 0 };
    std::atomic<std::uint64_t> presentationCallbacks { 0 };
    std::atomic<std::uint64_t> presentedFrames { 0 };
    std::atomic<std::uint64_t> presentationLatenessSamples { 0 };
    std::atomic<std::uint64_t> presentationLatenessUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> presentationHistoryDiscardedTimestamps { 0 };
    std::atomic<std::uint64_t> frameLatencySamples { 0 };
    std::atomic<std::uint64_t> frameLatencyTotalTimingSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyTotalTimingUnavailableSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyComponentTimingSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyComponentTimingUnavailableSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyHistoryDiscardedSamples { 0 };
    std::atomic<std::uint64_t> presentationsAfterTarget { 0 };
    std::atomic<std::uint64_t> skippedPresentations { 0 };
    std::atomic<std::uint64_t> gpuBackpressureDrops { 0 };
    std::atomic<std::uint64_t> drawableUnavailableDrops { 0 };
    std::atomic<std::uint64_t> callbackHostDelaySamples { 0 };
    std::atomic<std::uint64_t> callbackHostDelayUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> callbackAlreadyLateHostDelays { 0 };
    std::atomic<std::uint64_t> cpuCommitLatenessSamples { 0 };
    std::atomic<std::uint64_t> cpuCommitLatenessUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> cpuCommitDeadlineMisses { 0 };
    std::atomic<std::uint64_t> gpuCompletionLatenessSamples { 0 };
    std::atomic<std::uint64_t> gpuCompletionLatenessUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> gpuCompletionDeadlineMisses { 0 };
    std::atomic<std::uint64_t> analysisRequestCalls { 0 };
    std::atomic<std::uint64_t> snapshotReads { 0 };
    std::atomic<std::uint64_t> framesWithNewSnapshot { 0 };
    std::atomic<std::uint64_t> lastSpectrumSequence { 0 };

    std::atomic<std::uint64_t> lastCpuEncodeNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumCpuEncodeNanoseconds { 0 };
    std::atomic<std::uint64_t> lastGpuExecutionNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumGpuExecutionNanoseconds { 0 };
    std::atomic<std::uint64_t> lastDisplayCallbackIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetPresentationIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastCallbackHostDelayNanoseconds { 0 };
    std::atomic<std::uint64_t> lastCpuCommitLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastGpuCompletionLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetPresentationTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastProvidedDrawableAccessNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumProvidedDrawableAccessNanoseconds { 0 };

    // Command-buffer completion handlers may overlap. This lock makes their
    // related timing values and validity counters one coherent UI snapshot.
    mutable os_unfair_lock gpuTelemetryLock = OS_UNFAIR_LOCK_INIT;

    // Drawable presentation handlers are not audio callbacks and may run
    // concurrently or arrive out of timestamp order. A tiny unfair lock keeps
    // the fixed history exact without imposing any work on the audio thread.
    mutable os_unfair_lock presentedFrameHistoryLock = OS_UNFAIR_LOCK_INIT;
    PresentedFrameHistory presentedFrameHistory;
    FrameLatencyHistory frameLatencyHistory;
    std::uint64_t lastPresentationLatenessNanoseconds = 0;
    std::uint64_t lastPresentationLatenessTimestampNanoseconds = 0;
    std::uint64_t maximumPresentationLatenessNanoseconds = 0;

    std::atomic<std::uint32_t> drawableWidthPixels { 0 };
    std::atomic<std::uint32_t> drawableHeightPixels { 0 };
    std::atomic<std::uint32_t> configuredMaximumFramesPerSecond { 0 };
    std::atomic<double> backingScale { 1.0 };

    std::atomic<bool> metalAvailable { false };
    std::atomic<bool> renderingRequested { false };
    std::atomic<bool> effectivelyRendering { false };
};

template <typename Integer>
void updateMaximum(std::atomic<Integer>& destination, Integer candidate) noexcept
{
    auto previous = destination.load(std::memory_order_relaxed);

    while (candidate > previous
        && !destination.compare_exchange_weak(
            previous, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) { }
}

std::uint64_t nanosecondsBetween(Clock::time_point start, Clock::time_point end) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

std::uint64_t hostTimeNanoseconds(CFTimeInterval hostTime) noexcept
{
    if (!std::isfinite(hostTime) || hostTime <= 0.0)
        return 0;

    const auto nanoseconds = hostTime * 1'000'000'000.0;
    const auto bounded = std::min<double>(
        nanoseconds, static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
    return static_cast<std::uint64_t>(std::llround(bounded));
}

std::uint64_t positiveHostTimeDifference(CFTimeInterval later, CFTimeInterval earlier) noexcept
{
    if (!std::isfinite(later) || !std::isfinite(earlier) || later <= 0.0 || earlier <= 0.0
        || later <= earlier) {
        return 0;
    }

    return hostTimeNanoseconds(later - earlier);
}

std::uint32_t roundedPixelDimension(CGFloat value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0;

    const auto bounded = std::min<double>(value, std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(std::llround(bounded));
}

float sanitiseDecibels(float value, float minimum, float maximum) noexcept
{
    if (!std::isfinite(value))
        return minimum;

    return std::clamp(value, minimum, maximum);
}

float smoothingCoefficient(double elapsedSeconds, double timeConstantSeconds) noexcept
{
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0)
        return 1.0F;

    return static_cast<float>(1.0 - std::exp(-elapsedSeconds / timeConstantSeconds));
}

struct RenderBufferSlot {
    // Even values are free. The following odd value identifies a particular
    // in-flight admission, allowing late callbacks to release only their own.
    std::atomic<std::uint64_t> admissionState { 0 };
    id<MTLBuffer> vertexBuffer = nil;

    ~RenderBufferSlot()
    {
        [vertexBuffer release];
    }
};

struct SharedRenderState {
    std::array<RenderBufferSlot, renderBufferCount> slots;
};

struct VertexRange {
    std::size_t start = 0;
    std::size_t count = 0;
};

struct VertexBatches {
    VertexRange shell;
    VertexRange spectrumGrid;
    VertexRange spectrum;
    VertexRange peakRms;
};

struct RenderRect {
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;

    [[nodiscard]] float width() const noexcept
    {
        return std::max(0.0F, right - left);
    }

    [[nodiscard]] float height() const noexcept
    {
        return std::max(0.0F, top - bottom);
    }
};

struct RenderBufferAdmission {
    std::size_t index = renderBufferCount;
    std::uint64_t busyState = 0;
};

SpectrumRenderSettings sanitiseSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    if (!std::isfinite(settings.floorDecibels))
        settings.floorDecibels = SpectrumRenderSettings { }.floorDecibels;

    if (!std::isfinite(settings.ceilingDecibels))
        settings.ceilingDecibels = SpectrumRenderSettings { }.ceilingDecibels;

    if (!std::isfinite(settings.smoothing))
        settings.smoothing = SpectrumRenderSettings { }.smoothing;

    settings.floorDecibels = std::clamp(settings.floorDecibels, minimumAllowedSpectrumFloor,
        maximumAllowedSpectrumCeiling - minimumSpectrumRange);
    settings.ceilingDecibels = std::clamp(settings.ceilingDecibels,
        settings.floorDecibels + minimumSpectrumRange, maximumAllowedSpectrumCeiling);
    settings.smoothing = std::clamp(settings.smoothing, 0.0F, 1.0F);
    return settings;
}

std::uint64_t packSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    settings = sanitiseSpectrumSettings(settings);
    const auto floorValue = static_cast<std::uint16_t>(
        std::lround((settings.floorDecibels - minimumAllowedSpectrumFloor) * 100.0F));
    const auto ceilingValue = static_cast<std::uint16_t>(
        std::lround((settings.ceilingDecibels - minimumAllowedSpectrumFloor) * 100.0F));
    const auto smoothingValue
        = static_cast<std::uint16_t>(std::lround(settings.smoothing * 65'535.0F));

    return static_cast<std::uint64_t>(floorValue)
        | (static_cast<std::uint64_t>(ceilingValue) << 16U)
        | (static_cast<std::uint64_t>(smoothingValue) << 32U);
}

SpectrumRenderSettings unpackSpectrumSettings(std::uint64_t packed) noexcept
{
    const auto floorValue = static_cast<std::uint16_t>(packed & 0xffffU);
    const auto ceilingValue = static_cast<std::uint16_t>((packed >> 16U) & 0xffffU);
    const auto smoothingValue = static_cast<std::uint16_t>((packed >> 32U) & 0xffffU);

    return { minimumAllowedSpectrumFloor + (static_cast<float>(floorValue) * 0.01F),
        minimumAllowedSpectrumFloor + (static_cast<float>(ceilingValue) * 0.01F),
        static_cast<float>(smoothingValue) / 65'535.0F };
}

std::uint32_t packDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
{
    splits = DashboardLayout::validOrDefault(splits);
    return static_cast<std::uint32_t>(splits.horizontal)
        | (static_cast<std::uint32_t>(splits.upper) << 8U)
        | (static_cast<std::uint32_t>(splits.lowerLeft) << 16U)
        | (static_cast<std::uint32_t>(splits.lowerRight) << 24U);
}

DashboardLayoutSplits unpackDashboardLayoutSplits(const std::uint32_t packed) noexcept
{
    const DashboardLayoutSplits splits { static_cast<int>(packed & 0xffU),
        static_cast<int>((packed >> 8U) & 0xffU), static_cast<int>((packed >> 16U) & 0xffU),
        static_cast<int>((packed >> 24U) & 0xffU) };
    return DashboardLayout::validOrDefault(splits);
}

RenderRect toRenderRect(const DashboardLogicalBounds& bounds, const float logicalHeight) noexcept
{
    return { static_cast<float>(bounds.x), logicalHeight - static_cast<float>(bounds.bottom()),
        static_cast<float>(bounds.right()), logicalHeight - static_cast<float>(bounds.y) };
}

RenderRect insetRenderRect(
    const RenderRect& bounds, const float horizontal, const float bottom, const float top) noexcept
{
    const auto insetX = std::min(horizontal, bounds.width() * 0.5F);
    const auto insetBottom = std::min(bottom, bounds.height());
    const auto insetTop = std::min(top, std::max(0.0F, bounds.height() - insetBottom));
    return { bounds.left + insetX, bounds.bottom + insetBottom, bounds.right - insetX,
        bounds.top - insetTop };
}

MTLScissorRect makeScissorRect(const DashboardLogicalBounds& bounds, const CGSize logicalSize,
    const NSUInteger drawableWidth, const NSUInteger drawableHeight) noexcept
{
    if (logicalSize.width <= 0.0 || logicalSize.height <= 0.0 || drawableWidth == 0
        || drawableHeight == 0) {
        return { 0, 0, 0, 0 };
    }

    const auto xScale = static_cast<double>(drawableWidth) / logicalSize.width;
    const auto yScale = static_cast<double>(drawableHeight) / logicalSize.height;
    const auto clampHorizontal = [drawableWidth](const double value) noexcept {
        return std::clamp(value, 0.0, static_cast<double>(drawableWidth));
    };
    const auto clampVertical = [drawableHeight](const double value) noexcept {
        return std::clamp(value, 0.0, static_cast<double>(drawableHeight));
    };
    const auto left = clampHorizontal(std::floor(bounds.x * xScale));
    const auto top = clampVertical(std::floor(bounds.y * yScale));
    const auto right = clampHorizontal(std::ceil(bounds.right() * xScale));
    const auto bottom = clampVertical(std::ceil(bounds.bottom() * yScale));

    return { static_cast<NSUInteger>(left), static_cast<NSUInteger>(top),
        static_cast<NSUInteger>(std::max(0.0, right - left)),
        static_cast<NSUInteger>(std::max(0.0, bottom - top)) };
}

void releaseRenderBuffer(
    const std::shared_ptr<SharedRenderState>& state, RenderBufferAdmission admission) noexcept
{
    if (admission.index >= state->slots.size())
        return;

    auto expected = admission.busyState;
    state->slots[admission.index].admissionState.compare_exchange_strong(
        expected, admission.busyState + 1, std::memory_order_release, std::memory_order_relaxed);
}

void recordSkippedPresentation(const std::shared_ptr<FrameLatencySubmission>& submission,
    const std::shared_ptr<AtomicRenderTelemetry>& telemetry) noexcept
{
    os_unfair_lock_lock(&telemetry->presentedFrameHistoryLock);

    if (submission->classifySkipped() == PresentationOutcomeTransition::newlySkipped)
        telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);

    os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
}

void recordFrameLatencySample(const std::shared_ptr<AtomicRenderTelemetry>& telemetry,
    const FrameLatencySample& sample) noexcept
{
    os_unfair_lock_lock(&telemetry->presentedFrameHistoryLock);
    telemetry->frameLatencySamples.fetch_add(1, std::memory_order_relaxed);

    if (sample.totalValid) {
        telemetry->frameLatencyTotalTimingSamples.fetch_add(1, std::memory_order_relaxed);
    } else {
        telemetry->frameLatencyTotalTimingUnavailableSamples.fetch_add(
            1, std::memory_order_relaxed);
    }

    if (sample.componentsValid) {
        telemetry->frameLatencyComponentTimingSamples.fetch_add(1, std::memory_order_relaxed);
    } else {
        telemetry->frameLatencyComponentTimingUnavailableSamples.fetch_add(
            1, std::memory_order_relaxed);
    }

    if (!telemetry->frameLatencyHistory.record(sample))
        telemetry->frameLatencyHistoryDiscardedSamples.fetch_add(1, std::memory_order_relaxed);

    os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
}

void copyGpuTelemetry(
    const AtomicRenderTelemetry& source, MetalRenderTelemetry& destination) noexcept
{
    os_unfair_lock_lock(&source.gpuTelemetryLock);
    destination.completedFrames = source.completedFrames.load(std::memory_order_relaxed);
    destination.gpuTimingSamples = source.gpuTimingSamples.load(std::memory_order_relaxed);
    destination.gpuTimingUnavailableSamples
        = source.gpuTimingUnavailableSamples.load(std::memory_order_relaxed);
    destination.gpuCompletionLatenessSamples
        = source.gpuCompletionLatenessSamples.load(std::memory_order_relaxed);
    destination.gpuCompletionLatenessUnclassifiableSamples
        = source.gpuCompletionLatenessUnclassifiableSamples.load(std::memory_order_relaxed);
    destination.gpuCompletionDeadlineMisses
        = source.gpuCompletionDeadlineMisses.load(std::memory_order_relaxed);
    destination.lastGpuExecutionNanoseconds
        = source.lastGpuExecutionNanoseconds.load(std::memory_order_relaxed);
    destination.maximumGpuExecutionNanoseconds
        = source.maximumGpuExecutionNanoseconds.load(std::memory_order_relaxed);
    destination.lastGpuCompletionLatenessNanoseconds
        = source.lastGpuCompletionLatenessNanoseconds.load(std::memory_order_relaxed);
    os_unfair_lock_unlock(&source.gpuTelemetryLock);
}

void copyPresentedFrameTelemetry(
    const AtomicRenderTelemetry& source, MetalRenderTelemetry& destination) noexcept
{
    os_unfair_lock_lock(&source.presentedFrameHistoryLock);
    destination.presentedFrameIntervalHistoryCount
        = source.presentedFrameHistory.snapshotIntervals(destination.presentedFrameIntervalHistory);
    destination.lastPresentedHostTimestampNanoseconds
        = source.presentedFrameHistory.latestTimestampNanoseconds();
    destination.lastPresentedFrameIntervalNanoseconds
        = source.presentedFrameHistory.latestIntervalNanoseconds();
    destination.lastPresentationLatenessNanoseconds = source.lastPresentationLatenessNanoseconds;
    destination.maximumPresentationLatenessNanoseconds
        = source.maximumPresentationLatenessNanoseconds;
    destination.presentedFrames = source.presentedFrames.load(std::memory_order_relaxed);
    destination.presentationLatenessSamples
        = source.presentationLatenessSamples.load(std::memory_order_relaxed);
    destination.presentationLatenessUnclassifiableSamples
        = source.presentationLatenessUnclassifiableSamples.load(std::memory_order_relaxed);
    destination.presentationHistoryDiscardedTimestamps
        = source.presentationHistoryDiscardedTimestamps.load(std::memory_order_relaxed);
    destination.frameLatencySamples = source.frameLatencySamples.load(std::memory_order_relaxed);
    destination.frameLatencyTotalTimingSamples
        = source.frameLatencyTotalTimingSamples.load(std::memory_order_relaxed);
    destination.frameLatencyTotalTimingUnavailableSamples
        = source.frameLatencyTotalTimingUnavailableSamples.load(std::memory_order_relaxed);
    destination.frameLatencyComponentTimingSamples
        = source.frameLatencyComponentTimingSamples.load(std::memory_order_relaxed);
    destination.frameLatencyComponentTimingUnavailableSamples
        = source.frameLatencyComponentTimingUnavailableSamples.load(std::memory_order_relaxed);
    destination.frameLatencyHistoryDiscardedSamples
        = source.frameLatencyHistoryDiscardedSamples.load(std::memory_order_relaxed);
    destination.frameLatencyHistoryCount
        = source.frameLatencyHistory.snapshot(destination.frameLatencyHistory);
    destination.presentationsAfterTarget
        = source.presentationsAfterTarget.load(std::memory_order_relaxed);
    os_unfair_lock_unlock(&source.presentedFrameHistoryLock);
}

void recordPresentedDrawable(const std::shared_ptr<FrameLatencySubmission>& submission,
    const std::shared_ptr<AtomicRenderTelemetry>& telemetry, id<MTLDrawable> drawable,
    CFTimeInterval targetPresentationTimestamp, const std::uint64_t presentationSequence) noexcept
{
    telemetry->presentationCallbacks.fetch_add(1, std::memory_order_relaxed);

    const auto presentedTime = drawable.presentedTime;
    const auto presentedNanoseconds = hostTimeNanoseconds(presentedTime);
    FrameLatencySample latencySample;

    if (submission->recordPresentation(presentedNanoseconds, latencySample))
        recordFrameLatencySample(telemetry, latencySample);

    if (!std::isfinite(presentedTime) || presentedTime <= 0.0) {
        recordSkippedPresentation(submission, telemetry);
        return;
    }

    const auto hasTargetPresentationTimestamp
        = std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0;
    const auto presentationLateness = hasTargetPresentationTimestamp
        ? positiveHostTimeDifference(presentedTime, targetPresentationTimestamp)
        : 0;
    os_unfair_lock_lock(&telemetry->presentedFrameHistoryLock);
    const auto outcomeTransition = submission->classifyPresented();

    if (outcomeTransition == PresentationOutcomeTransition::none) {
        os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
        return;
    }

    if (outcomeTransition == PresentationOutcomeTransition::upgradedSkippedToPresented)
        telemetry->skippedPresentations.fetch_sub(1, std::memory_order_relaxed);

    const auto historyAccepted = telemetry->presentedFrameHistory.recordPresentation(
        presentationSequence, presentedNanoseconds);

    if (hasTargetPresentationTimestamp) {
        telemetry->maximumPresentationLatenessNanoseconds
            = std::max(telemetry->maximumPresentationLatenessNanoseconds, presentationLateness);
        telemetry->presentationLatenessSamples.fetch_add(1, std::memory_order_relaxed);

        if (presentedNanoseconds > telemetry->lastPresentationLatenessTimestampNanoseconds) {
            telemetry->lastPresentationLatenessTimestampNanoseconds = presentedNanoseconds;
            telemetry->lastPresentationLatenessNanoseconds = presentationLateness;
        }
    } else {
        telemetry->presentationLatenessUnclassifiableSamples.fetch_add(
            1, std::memory_order_relaxed);
    }

    if (!historyAccepted)
        telemetry->presentationHistoryDiscardedTimestamps.fetch_add(1, std::memory_order_relaxed);

    if (hasTargetPresentationTimestamp && presentationLateness != 0)
        telemetry->presentationsAfterTarget.fetch_add(1, std::memory_order_relaxed);

    telemetry->presentedFrames.fetch_add(1, std::memory_order_relaxed);
    os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
}

const char* metalShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Vertex
{
    float2 position;
    float4 colour;
};

struct RasterVertex
{
    float4 position [[position]];
    float4 colour;
};

vertex RasterVertex audioInsightVertex(const device Vertex* vertices [[buffer(0)]],
                                       uint vertexId [[vertex_id]])
{
    RasterVertex output;
    output.position = float4(vertices[vertexId].position, 0.0, 1.0);
    output.colour = vertices[vertexId].colour;
    return output;
}

fragment half4 audioInsightFragment(RasterVertex input [[stage_in]])
{
    return half4(input.colour);
}
)metal";
} // namespace

class MetalRenderBackend {
public:
    MetalRenderBackend(VisualizationDataSource& sourceToUse, AIAudioInsightMetalView* viewToUse)
        : source(sourceToUse), view(viewToUse), sharedState(std::make_shared<SharedRenderState>())
    {
        callbackTelemetry = std::make_shared<AtomicRenderTelemetry>();
        std::atomic_store_explicit(
            &publishedTelemetry, callbackTelemetry, std::memory_order_release);
        setSpectrumSettings({ });
        setDashboardLayoutSplits(DashboardLayout::defaultSplits);
        initialiseMetal();
        callbackTelemetry->metalAvailable.store(metalReady, std::memory_order_relaxed);
    }

    ~MetalRenderBackend()
    {
        shutdown();
        [pipelineState release];
        [commandQueue release];
    }

    void attachNativeView()
    {
        assertMessageThread();

        if (view == nil)
            return;

        view.delegate = nil;
        view.paused = YES;
        view.enableSetNeedsDisplay = NO;
        view.autoResizeDrawable = YES;
        view.framebufferOnly = YES;
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        view.clearColor = MTLClearColorMake(0.018, 0.024, 0.035, 1.0);

        if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
            auto* layer = static_cast<CAMetalLayer*>(view.layer);
            layer.maximumDrawableCount = renderBufferCount;
            layer.allowsNextDrawableTimeout = YES;
            layer.displaySyncEnabled = YES;
            layer.presentsWithTransaction = NO;
        }

        [view attachRenderBackend:this];

        if (![view hasDisplayLink]) {
            metalReady = false;
            initializationError = "CAMetalDisplayLink could not attach to the Metal layer.";
            callbackTelemetry->metalAvailable.store(false, std::memory_order_relaxed);
        }

        nativeViewStateChanged();
    }

    void shutdown() noexcept
    {
        assertMessageThread();

        if (hasShutDown.exchange(true, std::memory_order_acq_rel))
            return;

        requestedActive.store(false, std::memory_order_relaxed);
        loadPublishedTelemetry()->renderingRequested.store(false, std::memory_order_relaxed);
        setEffectiveActive(false);

        if (view != nil) {
            view.paused = YES;
            view.delegate = nil;
            [view detachRenderBackend];
        }

        view = nil;
    }

    void setRequestedActive(bool shouldBeActive)
    {
        assertMessageThread();

        if (hasShutDown.load(std::memory_order_acquire))
            return;

        requestedActive.store(shouldBeActive, std::memory_order_relaxed);
        loadPublishedTelemetry()->renderingRequested.store(
            shouldBeActive, std::memory_order_relaxed);
        refreshEffectiveActivity();
    }

    void setJuceShowing(bool shouldBeShowing)
    {
        assertMessageThread();
        juceShowing.store(shouldBeShowing, std::memory_order_relaxed);
        refreshEffectiveActivity();
    }

    [[nodiscard]] bool isRenderingRequested() const noexcept
    {
        return requestedActive.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isEffectivelyRendering() const noexcept
    {
        return effectiveActive.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isMetalAvailable() const noexcept
    {
        return metalReady;
    }

    [[nodiscard]] juce::String getInitializationError() const
    {
        return initializationError;
    }

    void setEffectiveActivityCallback(MetalVisualization::EffectiveActivityCallback callback)
    {
        assertMessageThread();
        effectiveActivityCallback = std::move(callback);
    }

    void setSpectrumSettings(SpectrumRenderSettings settings) noexcept
    {
        packedSpectrumSettings.store(packSpectrumSettings(settings), std::memory_order_release);
    }

    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept
    {
        return unpackSpectrumSettings(packedSpectrumSettings.load(std::memory_order_acquire));
    }

    void setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
    {
        packedDashboardLayoutSplits.store(
            packDashboardLayoutSplits(splits), std::memory_order_release);
    }

    [[nodiscard]] DashboardLayoutSplits getDashboardLayoutSplits() const noexcept
    {
        return unpackDashboardLayoutSplits(
            packedDashboardLayoutSplits.load(std::memory_order_acquire));
    }

    [[nodiscard]] MetalRenderTelemetry getTelemetry() const noexcept
    {
        const auto telemetry = loadPublishedTelemetry();
        MetalRenderTelemetry result;

        result.epoch = telemetry->epoch;
        result.displayLinkCallbacks
            = telemetry->displayLinkCallbacks.load(std::memory_order_relaxed);
        result.submittedFrames = telemetry->submittedFrames.load(std::memory_order_relaxed);
        result.commandBufferFailures
            = telemetry->commandBufferFailures.load(std::memory_order_relaxed);
        result.presentationCallbacks
            = telemetry->presentationCallbacks.load(std::memory_order_relaxed);
        result.skippedPresentations
            = telemetry->skippedPresentations.load(std::memory_order_relaxed);
        result.gpuBackpressureDrops
            = telemetry->gpuBackpressureDrops.load(std::memory_order_relaxed);
        result.drawableUnavailableDrops
            = telemetry->drawableUnavailableDrops.load(std::memory_order_relaxed);
        result.callbackHostDelaySamples
            = telemetry->callbackHostDelaySamples.load(std::memory_order_relaxed);
        result.callbackHostDelayUnclassifiableSamples
            = telemetry->callbackHostDelayUnclassifiableSamples.load(std::memory_order_relaxed);
        result.callbackAlreadyLateHostDelays
            = telemetry->callbackAlreadyLateHostDelays.load(std::memory_order_relaxed);
        result.cpuCommitLatenessSamples
            = telemetry->cpuCommitLatenessSamples.load(std::memory_order_relaxed);
        result.cpuCommitLatenessUnclassifiableSamples
            = telemetry->cpuCommitLatenessUnclassifiableSamples.load(std::memory_order_relaxed);
        result.cpuCommitDeadlineMisses
            = telemetry->cpuCommitDeadlineMisses.load(std::memory_order_relaxed);
        result.analysisRequestCalls
            = telemetry->analysisRequestCalls.load(std::memory_order_relaxed);
        result.snapshotReads = telemetry->snapshotReads.load(std::memory_order_relaxed);
        result.framesWithNewSnapshot
            = telemetry->framesWithNewSnapshot.load(std::memory_order_relaxed);
        result.lastSpectrumSequence
            = telemetry->lastSpectrumSequence.load(std::memory_order_relaxed);
        result.lastCpuEncodeNanoseconds
            = telemetry->lastCpuEncodeNanoseconds.load(std::memory_order_relaxed);
        result.maximumCpuEncodeNanoseconds
            = telemetry->maximumCpuEncodeNanoseconds.load(std::memory_order_relaxed);
        result.lastDisplayCallbackIntervalNanoseconds
            = telemetry->lastDisplayCallbackIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetIntervalNanoseconds
            = telemetry->lastTargetIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetPresentationIntervalNanoseconds
            = telemetry->lastTargetPresentationIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastCallbackHostDelayNanoseconds
            = telemetry->lastCallbackHostDelayNanoseconds.load(std::memory_order_relaxed);
        result.lastCpuCommitLatenessNanoseconds
            = telemetry->lastCpuCommitLatenessNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetTimestampNanoseconds
            = telemetry->lastTargetTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetPresentationTimestampNanoseconds
            = telemetry->lastTargetPresentationTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastProvidedDrawableAccessNanoseconds
            = telemetry->lastProvidedDrawableAccessNanoseconds.load(std::memory_order_relaxed);
        result.maximumProvidedDrawableAccessNanoseconds
            = telemetry->maximumProvidedDrawableAccessNanoseconds.load(std::memory_order_relaxed);
        copyGpuTelemetry(*telemetry, result);
        copyPresentedFrameTelemetry(*telemetry, result);
        result.drawableWidthPixels = telemetry->drawableWidthPixels.load(std::memory_order_relaxed);
        result.drawableHeightPixels
            = telemetry->drawableHeightPixels.load(std::memory_order_relaxed);
        result.configuredMaximumFramesPerSecond
            = telemetry->configuredMaximumFramesPerSecond.load(std::memory_order_relaxed);
        result.backingScale = telemetry->backingScale.load(std::memory_order_relaxed);
        result.metalAvailable = telemetry->metalAvailable.load(std::memory_order_relaxed);
        result.renderingRequested = telemetry->renderingRequested.load(std::memory_order_relaxed);
        result.effectivelyRendering
            = telemetry->effectivelyRendering.load(std::memory_order_relaxed);
        result.resetPending
            = requestedTelemetryEpoch.load(std::memory_order_acquire) != result.epoch;
        return result;
    }

    void resetTelemetry()
    {
        assertMessageThread();
        queueTelemetryReset();

        // An active display link installs the replacement at the next callback
        // boundary. Without callbacks there is no reason to leave the reset
        // pending: the message thread is already a safe boundary and in-flight
        // completion/presentation handlers retain the old telemetry object.
        if (!effectiveActive.load(std::memory_order_acquire) || !metalReady || view == nil)
            applyPendingTelemetryAtSafeBoundary();
    }

    void nativeViewStateChanged()
    {
        assertMessageThread();

        if (hasShutDown.load(std::memory_order_acquire) || view == nil)
            return;

        updateScreenProperties();
        refreshEffectiveActivity();
    }

    void displayLinkUpdate(CAMetalDisplayLinkUpdate* update)
    {
        assertMessageThread();

        if (!effectiveActive.load(std::memory_order_relaxed) || view == nil || update == nil
            || !metalReady) {
            return;
        }

        const auto callbackTime = Clock::now();
        const auto callbackHostTime = CACurrentMediaTime();
        applyPendingTelemetryAtSafeBoundary();
        const auto telemetry = callbackTelemetry;
        const auto targetTimestamp = update.targetTimestamp;
        const auto targetPresentationTimestamp = update.targetPresentationTimestamp;
        const auto presentationSequence = recordDisplayLinkCallback(
            *telemetry, callbackHostTime, targetTimestamp, targetPresentationTimestamp);

        const auto admission = acquireRenderBuffer();

        if (admission.index >= renderBufferCount) {
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::shared_ptr<FrameLatencySubmission> submission;

        try {
            // Presentation may trail GPU completion by several display periods,
            // so this small state must outlive reusable vertex-buffer admission.
            submission = std::make_shared<FrameLatencySubmission>(
                presentationSequence, hostTimeNanoseconds(callbackHostTime));
        } catch (...) {
            releaseRenderBuffer(sharedState, admission);
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto& slot = sharedState->slots[admission.index];
        const auto drawableAccessStart = Clock::now();
        id<CAMetalDrawable> drawable = update.drawable;
        const auto drawableAccessNanoseconds
            = nanosecondsBetween(drawableAccessStart, Clock::now());
        telemetry->lastProvidedDrawableAccessNanoseconds.store(
            drawableAccessNanoseconds, std::memory_order_relaxed);
        updateMaximum(
            telemetry->maximumProvidedDrawableAccessNanoseconds, drawableAccessNanoseconds);

        if (drawable == nil || drawable.texture == nil) {
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->drawableUnavailableDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        telemetry->drawableWidthPixels.store(
            roundedPixelDimension(static_cast<CGFloat>(drawable.texture.width)),
            std::memory_order_relaxed);
        telemetry->drawableHeightPixels.store(
            roundedPixelDimension(static_cast<CGFloat>(drawable.texture.height)),
            std::memory_order_relaxed);
        updateBackingScale();

        source.requestAnalysis();
        telemetry->analysisRequestCalls.fetch_add(1, std::memory_order_relaxed);

        VisualizationFrame incomingFrame;

        if (source.copyLatestVisualizationFrame(incomingFrame)) {
            telemetry->snapshotReads.fetch_add(1, std::memory_order_relaxed);
            acceptSnapshot(incomingFrame, *telemetry);
        }

        const auto spectrumSettings = getSpectrumSettings();
        updateSmoothedDisplayValues(callbackTime, spectrumSettings);

        const auto boundsSize = view.bounds.size;
        const auto dashboardLayout = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, static_cast<double>(boundsSize.width),
                static_cast<double>(boundsSize.height) },
            getDashboardLayoutSplits());

        auto* vertices = static_cast<MetalVertex*>(slot.vertexBuffer.contents);
        const auto batches
            = populateVertices(vertices, boundsSize, dashboardLayout, spectrumSettings);

        auto* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* colourAttachment = descriptor.colorAttachments[0];
        colourAttachment.texture = drawable.texture;
        colourAttachment.loadAction = MTLLoadActionClear;
        colourAttachment.storeAction = MTLStoreActionStore;
        colourAttachment.clearColor = MTLClearColorMake(0.018, 0.024, 0.035, 1.0);

        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];

        if (commandBuffer == nil) {
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:descriptor];

        if (encoder == nil) {
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->drawableUnavailableDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        encoder.label = @"Audio Insight visualization";
        [encoder setRenderPipelineState:pipelineState];
        [encoder setVertexBuffer:slot.vertexBuffer offset:0 atIndex:0];

        if (batches.shell.count != 0)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.shell.start
                        vertexCount:batches.shell.count];

        const auto spectrumScissor = makeScissorRect(dashboardLayout[DashboardPanel::spectrum],
            boundsSize, drawable.texture.width, drawable.texture.height);

        if (spectrumScissor.width != 0 && spectrumScissor.height != 0) {
            [encoder setScissorRect:spectrumScissor];

            if (batches.spectrumGrid.count != 0)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:batches.spectrumGrid.start
                            vertexCount:batches.spectrumGrid.count];

            if (batches.spectrum.count >= 2)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:batches.spectrum.start
                            vertexCount:batches.spectrum.count];
        }

        const auto meterScissor = makeScissorRect(dashboardLayout[DashboardPanel::peakRms],
            boundsSize, drawable.texture.width, drawable.texture.height);

        if (meterScissor.width != 0 && meterScissor.height != 0 && batches.peakRms.count != 0) {
            [encoder setScissorRect:meterScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.peakRms.start
                        vertexCount:batches.peakRms.count];
        }

        [encoder endEncoding];

        auto presentationTelemetry = telemetry;
        [drawable addPresentedHandler:^(id<MTLDrawable> presentedDrawable) {
            recordPresentedDrawable(submission, presentationTelemetry, presentedDrawable,
                targetPresentationTimestamp, presentationSequence);
        }];

        auto completionState = sharedState;
        auto completionTelemetry = telemetry;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            // The GPU has finished reading this submission's shared vertex
            // buffer. Presentation may be delayed by several refresh periods,
            // so it must not retain renderer admission.
            releaseRenderBuffer(completionState, admission);

            const auto gpuStart = completedBuffer.GPUStartTime;
            const auto gpuEnd = completedBuffer.GPUEndTime;
            const auto commandSucceeded = completedBuffer.status != MTLCommandBufferStatusError;
            const auto hasGpuTiming = commandSucceeded && std::isfinite(gpuStart)
                && std::isfinite(gpuEnd) && gpuEnd >= gpuStart && gpuStart > 0.0;
            FrameLatencySample latencySample;

            if (submission->recordGpuCompletion(hasGpuTiming ? hostTimeNanoseconds(gpuStart) : 0,
                    hasGpuTiming ? hostTimeNanoseconds(gpuEnd) : 0, latencySample)) {
                recordFrameLatencySample(completionTelemetry, latencySample);
            }

            if (!commandSucceeded) {
                completionTelemetry->commandBufferFailures.fetch_add(1, std::memory_order_relaxed);
                recordSkippedPresentation(submission, completionTelemetry);
                return;
            }

            const auto hasTargetPresentationTimestamp
                = std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0;

            os_unfair_lock_lock(&completionTelemetry->gpuTelemetryLock);
            if (hasGpuTiming) {
                const auto gpuNanoseconds = positiveHostTimeDifference(gpuEnd, gpuStart);
                completionTelemetry->lastGpuExecutionNanoseconds.store(
                    gpuNanoseconds, std::memory_order_relaxed);
                updateMaximum(completionTelemetry->maximumGpuExecutionNanoseconds, gpuNanoseconds);
                completionTelemetry->gpuTimingSamples.fetch_add(1, std::memory_order_relaxed);

                if (hasTargetPresentationTimestamp) {
                    const auto completionLateness
                        = positiveHostTimeDifference(gpuEnd, targetPresentationTimestamp);
                    completionTelemetry->lastGpuCompletionLatenessNanoseconds.store(
                        completionLateness, std::memory_order_relaxed);
                    completionTelemetry->gpuCompletionLatenessSamples.fetch_add(
                        1, std::memory_order_relaxed);

                    if (completionLateness != 0)
                        completionTelemetry->gpuCompletionDeadlineMisses.fetch_add(
                            1, std::memory_order_relaxed);
                } else {
                    completionTelemetry->gpuCompletionLatenessUnclassifiableSamples.fetch_add(
                        1, std::memory_order_relaxed);
                }
            } else {
                completionTelemetry->gpuTimingUnavailableSamples.fetch_add(
                    1, std::memory_order_relaxed);
                completionTelemetry->gpuCompletionLatenessUnclassifiableSamples.fetch_add(
                    1, std::memory_order_relaxed);
            }

            completionTelemetry->completedFrames.fetch_add(1, std::memory_order_relaxed);
            os_unfair_lock_unlock(&completionTelemetry->gpuTelemetryLock);
        }];

        submission->setCpuReadyTimestamp(hostTimeNanoseconds(CACurrentMediaTime()));
        [commandBuffer commit];

        // CAMetalDisplayLink owns the drawable's presentation timing. Timed presentation APIs
        // assert for drawables supplied by its update callback.
        [drawable present];

        const auto commitHostTime = CACurrentMediaTime();
        const auto hasCpuCommitLateness = std::isfinite(commitHostTime) && commitHostTime > 0.0
            && std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0;

        if (hasCpuCommitLateness) {
            const auto commitLateness
                = positiveHostTimeDifference(commitHostTime, targetPresentationTimestamp);
            telemetry->lastCpuCommitLatenessNanoseconds.store(
                commitLateness, std::memory_order_relaxed);
            telemetry->cpuCommitLatenessSamples.fetch_add(1, std::memory_order_relaxed);

            if (commitLateness != 0)
                telemetry->cpuCommitDeadlineMisses.fetch_add(1, std::memory_order_relaxed);
        } else {
            telemetry->cpuCommitLatenessUnclassifiableSamples.fetch_add(
                1, std::memory_order_relaxed);
        }

        const auto cpuNanoseconds = nanosecondsBetween(callbackTime, Clock::now());
        telemetry->lastCpuEncodeNanoseconds.store(cpuNanoseconds, std::memory_order_relaxed);
        updateMaximum(telemetry->maximumCpuEncodeNanoseconds, cpuNanoseconds);
        telemetry->submittedFrames.fetch_add(1, std::memory_order_relaxed);
    }

private:
    VisualizationDataSource& source;
    AIAudioInsightMetalView* view = nil;
    std::shared_ptr<SharedRenderState> sharedState;
    std::shared_ptr<AtomicRenderTelemetry> callbackTelemetry;
    std::shared_ptr<AtomicRenderTelemetry> publishedTelemetry;
    std::shared_ptr<AtomicRenderTelemetry> pendingTelemetry;

    id<MTLCommandQueue> commandQueue = nil;
    id<MTLRenderPipelineState> pipelineState = nil;
    juce::String initializationError;
    bool metalReady = false;
    std::atomic<std::uint64_t> packedSpectrumSettings { 0 };
    std::atomic<std::uint32_t> packedDashboardLayoutSplits { 0 };
    std::atomic<std::uint64_t> requestedTelemetryEpoch { 1 };

    std::atomic<bool> requestedActive { false };
    std::atomic<bool> juceShowing { false };
    std::atomic<bool> effectiveActive { false };
    std::atomic<bool> hasShutDown { false };
    MetalVisualization::EffectiveActivityCallback effectiveActivityCallback;

    VisualizationFrame targetFrame;
    std::array<float, spectrumBinCount> displayedSpectrum { };
    std::array<float, 2> displayedPeak { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> displayedRms { minimumDisplayDecibels, minimumDisplayDecibels };
    std::uint64_t lastSpectrumSequence = 0;
    std::uint64_t lastCapturedFrameEnd = 0;
    std::uint64_t lastGeneration = 0;
    bool hasDisplayFrame = false;

    Clock::time_point previousSmoothingTime;
    CFTimeInterval previousDisplayCallbackHostTime = 0.0;
    CFTimeInterval previousTargetTimestamp = 0.0;
    CFTimeInterval previousTargetPresentationTimestamp = 0.0;

    void initialiseMetal()
    {
        if (view == nil || view.device == nil) {
            initializationError = "Metal is not available on this system.";
            return;
        }

        commandQueue = [view.device newCommandQueue];

        if (commandQueue == nil) {
            initializationError = "Metal could not create a command queue.";
            return;
        }

        NSError* libraryError = nil;
        auto* sourceString = [NSString stringWithUTF8String:metalShaderSource];
        id<MTLLibrary> library = [view.device newLibraryWithSource:sourceString
                                                           options:nil
                                                             error:&libraryError];

        if (library == nil) {
            initializationError = libraryError != nil
                ? juce::String::fromUTF8(libraryError.localizedDescription.UTF8String)
                : juce::String("Metal could not compile the visualization shader.");
            return;
        }

        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"audioInsightVertex"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"audioInsightFragment"];

        if (vertexFunction == nil || fragmentFunction == nil) {
            initializationError = "Metal could not load the visualization shader functions.";
            [vertexFunction release];
            [fragmentFunction release];
            [library release];
            return;
        }

        auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.label = @"Audio Insight visualization pipeline";
        descriptor.vertexFunction = vertexFunction;
        descriptor.fragmentFunction = fragmentFunction;
        auto* colourAttachment = descriptor.colorAttachments[0];
        colourAttachment.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        colourAttachment.blendingEnabled = YES;
        colourAttachment.rgbBlendOperation = MTLBlendOperationAdd;
        colourAttachment.alphaBlendOperation = MTLBlendOperationAdd;
        colourAttachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        colourAttachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        colourAttachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        colourAttachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        NSError* pipelineError = nil;
        pipelineState = [view.device newRenderPipelineStateWithDescriptor:descriptor
                                                                    error:&pipelineError];

        [descriptor release];
        [vertexFunction release];
        [fragmentFunction release];
        [library release];

        if (pipelineState == nil) {
            initializationError = pipelineError != nil
                ? juce::String::fromUTF8(pipelineError.localizedDescription.UTF8String)
                : juce::String("Metal could not create the visualization pipeline.");
            return;
        }

        for (auto& slot : sharedState->slots) {
            slot.vertexBuffer =
                [view.device newBufferWithLength:maximumVertexCount * sizeof(MetalVertex)
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];

            if (slot.vertexBuffer == nil) {
                initializationError = "Metal could not allocate the visualization buffers.";
                return;
            }
        }

        metalReady = true;
    }

    static void assertMessageThread() noexcept
    {
        auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
        juce::ignoreUnused(messageManager);
    }

    [[nodiscard]] std::shared_ptr<AtomicRenderTelemetry> loadPublishedTelemetry() const noexcept
    {
        return std::atomic_load_explicit(&publishedTelemetry, std::memory_order_acquire);
    }

    void queueTelemetryReset()
    {
        auto replacement = std::make_shared<AtomicRenderTelemetry>();
        const auto epoch = requestedTelemetryEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        replacement->epoch = epoch;

        auto pending = std::atomic_load_explicit(&pendingTelemetry, std::memory_order_acquire);

        for (;;) {
            if (requestedTelemetryEpoch.load(std::memory_order_acquire) != epoch)
                return;

            if (pending != nullptr && pending->epoch >= epoch)
                return;

            if (std::atomic_compare_exchange_weak_explicit(&pendingTelemetry, &pending, replacement,
                    std::memory_order_release, std::memory_order_acquire)) {
                return;
            }
        }
    }

    static void copyPersistentTelemetry(
        const AtomicRenderTelemetry& sourceTelemetry, AtomicRenderTelemetry& destination) noexcept
    {
        destination.drawableWidthPixels.store(
            sourceTelemetry.drawableWidthPixels.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.drawableHeightPixels.store(
            sourceTelemetry.drawableHeightPixels.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.configuredMaximumFramesPerSecond.store(
            sourceTelemetry.configuredMaximumFramesPerSecond.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.backingScale.store(sourceTelemetry.backingScale.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.metalAvailable.store(
            sourceTelemetry.metalAvailable.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.renderingRequested.store(
            sourceTelemetry.renderingRequested.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.effectivelyRendering.store(
            sourceTelemetry.effectivelyRendering.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    void resetTelemetryTimingAtCallbackBoundary() noexcept
    {
        previousDisplayCallbackHostTime = 0.0;
        previousTargetTimestamp = 0.0;
        previousTargetPresentationTimestamp = 0.0;
    }

    void resetRendererStateWhileDisplayLinkIsPaused() noexcept
    {
        targetFrame = { };
        displayedSpectrum.fill(minimumDisplayDecibels);
        displayedPeak.fill(minimumDisplayDecibels);
        displayedRms.fill(minimumDisplayDecibels);
        lastGeneration = 0;
        lastSpectrumSequence = 0;
        lastCapturedFrameEnd = 0;
        hasDisplayFrame = false;
        previousSmoothingTime = { };
        resetTelemetryTimingAtCallbackBoundary();
    }

    void applyPendingTelemetryAtSafeBoundary()
    {
        if (auto replacement = std::atomic_exchange_explicit(&pendingTelemetry,
                std::shared_ptr<AtomicRenderTelemetry> { }, std::memory_order_acq_rel);
            replacement != nullptr && replacement->epoch > callbackTelemetry->epoch) {
            copyPersistentTelemetry(*loadPublishedTelemetry(), *replacement);
            callbackTelemetry = std::move(replacement);
            std::atomic_store_explicit(
                &publishedTelemetry, callbackTelemetry, std::memory_order_release);
            resetTelemetryTimingAtCallbackBoundary();
        }
    }

    void setEffectiveActive(bool shouldBeActive)
    {
        assertMessageThread();

        if (effectiveActive.load(std::memory_order_acquire) == shouldBeActive)
            return;

        if (shouldBeActive) {
            // Keep the link paused until the coordinator has advanced its
            // generation and renderer state has been reset.
            source.setVisualizationActive(true);
            resetRendererStateWhileDisplayLinkIsPaused();
            queueTelemetryReset();
            applyPendingTelemetryAtSafeBoundary();
            effectiveActive.store(true, std::memory_order_release);
            loadPublishedTelemetry()->effectivelyRendering.store(true, std::memory_order_relaxed);

            if (view != nil)
                [view setDisplayLinkPaused:NO];

            notifyEffectiveActivityChanged(true);
            return;
        }

        if (view != nil)
            [view setDisplayLinkPaused:YES];

        effectiveActive.store(false, std::memory_order_release);
        loadPublishedTelemetry()->effectivelyRendering.store(false, std::memory_order_relaxed);
        applyPendingTelemetryAtSafeBoundary();
        source.setVisualizationActive(false);
        notifyEffectiveActivityChanged(false);
    }

    void refreshEffectiveActivity()
    {
        if (hasShutDown.load(std::memory_order_acquire) || view == nil) {
            setEffectiveActive(false);
            return;
        }

        auto* window = view.window;
        const auto windowIsVisible = window != nil && window.visible && !window.miniaturized
            && (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
        const auto nativeViewIsVisible = !view.hiddenOrHasHiddenAncestor;
        const auto shouldRender = requestedActive.load(std::memory_order_relaxed)
            && juceShowing.load(std::memory_order_relaxed) && metalReady && windowIsVisible
            && nativeViewIsVisible;

        setEffectiveActive(shouldRender);
    }

    void notifyEffectiveActivityChanged(const bool isActive) noexcept
    {
        if (!effectiveActivityCallback)
            return;

        try {
            effectiveActivityCallback(isActive);
        } catch (...) {
            // A diagnostics/lifecycle observer must never destabilize its host.
        }
    }

    void updateScreenProperties()
    {
        if (view == nil)
            return;

        auto* screen = view.window.screen;
        const auto maximumFps
            = screen != nil ? std::max<NSInteger>(1, screen.maximumFramesPerSecond) : 60;
        [view setDisplayLinkMaximumFramesPerSecond:maximumFps];
        loadPublishedTelemetry()->configuredMaximumFramesPerSecond.store(
            static_cast<std::uint32_t>(maximumFps), std::memory_order_relaxed);
        updateBackingScale();
    }

    void updateBackingScale()
    {
        if (view == nil)
            return;

        const auto backingSize = [view convertSizeToBacking:NSMakeSize(1.0, 1.0)];
        auto scale = static_cast<double>(backingSize.width);

        if ((!std::isfinite(scale) || scale <= 0.0) && view.window != nil)
            scale = static_cast<double>(view.window.backingScaleFactor);

        if (!std::isfinite(scale) || scale <= 0.0)
            scale = 1.0;

        loadPublishedTelemetry()->backingScale.store(scale, std::memory_order_relaxed);
    }

    RenderBufferAdmission acquireRenderBuffer() noexcept
    {
        for (std::size_t index = 0; index < sharedState->slots.size(); ++index) {
            auto expected
                = sharedState->slots[index].admissionState.load(std::memory_order_relaxed);

            if ((expected & 1U) == 0U
                && sharedState->slots[index].admissionState.compare_exchange_strong(
                    expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                return { index, expected + 1 };
            }
        }

        return { };
    }

    std::uint64_t recordDisplayLinkCallback(AtomicRenderTelemetry& telemetry,
        CFTimeInterval callbackHostTime, CFTimeInterval targetTimestamp,
        CFTimeInterval targetPresentationTimestamp) noexcept
    {
        const auto presentationSequence
            = telemetry.displayLinkCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        telemetry.lastTargetTimestampNanoseconds.store(
            hostTimeNanoseconds(targetTimestamp), std::memory_order_relaxed);
        telemetry.lastTargetPresentationTimestampNanoseconds.store(
            hostTimeNanoseconds(targetPresentationTimestamp), std::memory_order_relaxed);

        if (previousDisplayCallbackHostTime > 0.0)
            telemetry.lastDisplayCallbackIntervalNanoseconds.store(
                positiveHostTimeDifference(callbackHostTime, previousDisplayCallbackHostTime),
                std::memory_order_relaxed);

        if (previousTargetTimestamp > 0.0)
            telemetry.lastTargetIntervalNanoseconds.store(
                positiveHostTimeDifference(targetTimestamp, previousTargetTimestamp),
                std::memory_order_relaxed);

        if (previousTargetPresentationTimestamp > 0.0)
            telemetry.lastTargetPresentationIntervalNanoseconds.store(
                positiveHostTimeDifference(
                    targetPresentationTimestamp, previousTargetPresentationTimestamp),
                std::memory_order_relaxed);

        const auto hasCallbackHostDelay = std::isfinite(callbackHostTime) && callbackHostTime > 0.0
            && std::isfinite(targetTimestamp) && targetTimestamp > 0.0;

        if (hasCallbackHostDelay) {
            const auto callbackHostDelay
                = positiveHostTimeDifference(callbackHostTime, targetTimestamp);
            telemetry.lastCallbackHostDelayNanoseconds.store(
                callbackHostDelay, std::memory_order_relaxed);
            telemetry.callbackHostDelaySamples.fetch_add(1, std::memory_order_relaxed);

            if (callbackHostDelay != 0)
                telemetry.callbackAlreadyLateHostDelays.fetch_add(1, std::memory_order_relaxed);
        } else {
            telemetry.callbackHostDelayUnclassifiableSamples.fetch_add(
                1, std::memory_order_relaxed);
        }

        previousDisplayCallbackHostTime = callbackHostTime;
        previousTargetTimestamp = targetTimestamp;
        previousTargetPresentationTimestamp = targetPresentationTimestamp;
        return presentationSequence;
    }

    void acceptSnapshot(const VisualizationFrame& incoming, AtomicRenderTelemetry& telemetry)
    {
        const auto generationChanged = incoming.generation != lastGeneration;
        const auto isNew = !hasDisplayFrame || generationChanged
            || incoming.spectrumSequence != lastSpectrumSequence
            || incoming.capturedFrameEnd != lastCapturedFrameEnd;

        if (!isNew)
            return;

        targetFrame = incoming;
        lastGeneration = incoming.generation;
        lastSpectrumSequence = incoming.spectrumSequence;
        lastCapturedFrameEnd = incoming.capturedFrameEnd;
        telemetry.framesWithNewSnapshot.fetch_add(1, std::memory_order_relaxed);
        telemetry.lastSpectrumSequence.store(lastSpectrumSequence, std::memory_order_relaxed);

        if (!hasDisplayFrame || generationChanged) {
            const auto settings = getSpectrumSettings();

            for (std::size_t index = 0; index < displayedSpectrum.size(); ++index)
                displayedSpectrum[index] = sanitiseDecibels(targetFrame.spectrumDecibels[index],
                    settings.floorDecibels, settings.ceilingDecibels);

            for (std::size_t channel = 0; channel < 2; ++channel) {
                displayedPeak[channel] = sanitiseDecibels(
                    targetFrame.peakDecibels[channel], minimumMeterDecibels, maximumMeterDecibels);
                displayedRms[channel] = sanitiseDecibels(
                    targetFrame.rmsDecibels[channel], minimumMeterDecibels, maximumMeterDecibels);
            }

            hasDisplayFrame = true;
        }
    }

    void updateSmoothedDisplayValues(Clock::time_point now, SpectrumRenderSettings settings)
    {
        if (!hasDisplayFrame)
            return;

        const auto elapsed = previousSmoothingTime != Clock::time_point { }
            ? std::chrono::duration<double>(now - previousSmoothingTime).count()
            : 1.0 / 60.0;
        previousSmoothingTime = now;

        const auto smoothingShape = settings.smoothing * settings.smoothing;
        const auto spectrumRise = settings.smoothing <= 0.0F
            ? 1.0F
            : smoothingCoefficient(elapsed, 0.004 + (0.096 * smoothingShape));
        const auto spectrumFall = settings.smoothing <= 0.0F
            ? 1.0F
            : smoothingCoefficient(elapsed, 0.015 + (0.435 * smoothingShape));
        const auto meterRise = smoothingCoefficient(elapsed, 0.025);
        const auto meterFall = smoothingCoefficient(elapsed, 0.100);

        for (std::size_t index = 0; index < displayedSpectrum.size(); ++index) {
            const auto target = sanitiseDecibels(targetFrame.spectrumDecibels[index],
                settings.floorDecibels, settings.ceilingDecibels);
            const auto coefficient
                = target >= displayedSpectrum[index] ? spectrumRise : spectrumFall;
            displayedSpectrum[index] += coefficient * (target - displayedSpectrum[index]);
        }

        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto peakTarget = sanitiseDecibels(
                targetFrame.peakDecibels[channel], minimumMeterDecibels, maximumMeterDecibels);
            const auto rmsTarget = sanitiseDecibels(
                targetFrame.rmsDecibels[channel], minimumMeterDecibels, maximumMeterDecibels);
            const auto peakCoefficient = peakTarget >= displayedPeak[channel] ? 1.0F : meterFall;
            const auto rmsCoefficient = rmsTarget >= displayedRms[channel] ? meterRise : meterFall;
            displayedPeak[channel] += peakCoefficient * (peakTarget - displayedPeak[channel]);
            displayedRms[channel] += rmsCoefficient * (rmsTarget - displayedRms[channel]);
        }
    }

    VertexBatches populateVertices(MetalVertex* vertices, CGSize logicalSize,
        const DashboardTileLayout& dashboardLayout, SpectrumRenderSettings settings) const noexcept
    {
        VertexBatches batches;

        if (vertices == nullptr || logicalSize.width < 2.0 || logicalSize.height < 2.0)
            return batches;

        std::size_t cursor = 0;
        const auto width = static_cast<float>(logicalSize.width);
        const auto height = static_cast<float>(logicalSize.height);

        const auto pointToClip = [width, height](float x, float y) noexcept {
            return simd_make_float2((2.0F * x / width) - 1.0F, (2.0F * y / height) - 1.0F);
        };

        const auto appendVertex = [&](float x, float y, simd_float4 colour) noexcept {
            jassert(cursor < maximumVertexCount);

            if (cursor >= maximumVertexCount)
                return;

            vertices[cursor++] = { pointToClip(x, y), colour };
        };

        const auto appendQuad
            = [&](float left, float bottom, float right, float top, simd_float4 colour) noexcept {
                  if (right <= left || top <= bottom)
                      return;

                  const auto bottomLeft = pointToClip(left, bottom);
                  const auto bottomRight = pointToClip(right, bottom);
                  const auto topLeft = pointToClip(left, top);
                  const auto topRight = pointToClip(right, top);

                  jassert(cursor + 6 <= maximumVertexCount);

                  if (cursor + 6 > maximumVertexCount)
                      return;

                  vertices[cursor++] = { bottomLeft, colour };
                  vertices[cursor++] = { bottomRight, colour };
                  vertices[cursor++] = { topLeft, colour };
                  vertices[cursor++] = { topLeft, colour };
                  vertices[cursor++] = { bottomRight, colour };
                  vertices[cursor++] = { topRight, colour };
              };

        const auto appendBorder = [&](const RenderRect& bounds, float thickness,
                                      simd_float4 colour) noexcept {
            if (bounds.width() <= 0.0F || bounds.height() <= 0.0F)
                return;

            thickness
                = std::min(thickness, std::min(bounds.width() * 0.5F, bounds.height() * 0.5F));
            appendQuad(bounds.left, bounds.bottom, bounds.right, bounds.bottom + thickness, colour);
            appendQuad(bounds.left, bounds.top - thickness, bounds.right, bounds.top, colour);
            appendQuad(bounds.left, bounds.bottom + thickness, bounds.left + thickness,
                bounds.top - thickness, colour);
            appendQuad(bounds.right - thickness, bounds.bottom + thickness, bounds.right,
                bounds.top - thickness, colour);
        };

        const auto headerHeight = [](const RenderRect& bounds) noexcept {
            return std::min(bounds.height(), std::clamp(bounds.height() * 0.13F, 18.0F, 26.0F));
        };

        constexpr auto livePanelColour = simd_float4 { 0.025F, 0.035F, 0.052F, 1.0F };
        constexpr auto placeholderPanelColour = simd_float4 { 0.014F, 0.020F, 0.030F, 1.0F };
        constexpr auto spectrogramPanelColour = simd_float4 { 0.0F, 0.0F, 0.0F, 1.0F };
        constexpr auto panelBorderColour = simd_float4 { 0.13F, 0.17F, 0.23F, 0.90F };
        constexpr auto headerDividerColour = simd_float4 { 0.12F, 0.17F, 0.24F, 0.72F };

        batches.shell.start = cursor;

        for (std::size_t index = 0; index < dashboardPanelCount; ++index) {
            const auto panel = static_cast<DashboardPanel>(index);
            const auto bounds = toRenderRect(dashboardLayout[panel], height);
            auto fillColour = placeholderPanelColour;

            if (panel == DashboardPanel::spectrum || panel == DashboardPanel::peakRms)
                fillColour = livePanelColour;
            else if (panel == DashboardPanel::spectrogram)
                fillColour = spectrogramPanelColour;

            appendQuad(bounds.left, bounds.bottom, bounds.right, bounds.top, fillColour);
            appendBorder(bounds, 1.0F, panelBorderColour);

            const auto dividerY = bounds.top - headerHeight(bounds);
            appendQuad(bounds.left + 1.0F, dividerY - 0.5F, bounds.right - 1.0F, dividerY + 0.5F,
                headerDividerColour);
        }

        batches.shell.count = cursor - batches.shell.start;

        const auto spectrumPanel = toRenderRect(dashboardLayout[DashboardPanel::spectrum], height);
        const auto spectrumPlot
            = insetRenderRect(spectrumPanel, 10.0F, 10.0F, headerHeight(spectrumPanel) + 8.0F);
        const auto plotLeft = spectrumPlot.left;
        const auto plotRight = spectrumPlot.right;
        const auto plotBottom = spectrumPlot.bottom;
        const auto plotTop = spectrumPlot.top;

        const auto frequencyToX = [&](float frequency, float maximumFrequency) noexcept {
            const auto ratio = std::log(frequency / minimumSpectrumFrequency)
                / std::log(maximumFrequency / minimumSpectrumFrequency);
            return plotLeft + std::clamp(ratio, 0.0F, 1.0F) * (plotRight - plotLeft);
        };

        const auto decibelsToY = [&](float decibels, float minimum, float maximum) noexcept {
            const auto normalised
                = (sanitiseDecibels(decibels, minimum, maximum) - minimum) / (maximum - minimum);
            return plotBottom + normalised * (plotTop - plotBottom);
        };

        const auto nyquist = targetFrame.sampleRate > 0.0
            ? static_cast<float>(targetFrame.sampleRate * 0.5)
            : maximumSpectrumFrequency;
        const auto maximumFrequency
            = std::clamp(nyquist, minimumSpectrumFrequency + 1.0F, maximumSpectrumFrequency);
        constexpr auto gridColour = simd_float4 { 0.16F, 0.20F, 0.27F, 0.62F };
        constexpr std::array<float, 10> frequencyGrid { 20.0F, 50.0F, 100.0F, 200.0F, 500.0F,
            1'000.0F, 2'000.0F, 5'000.0F, 10'000.0F, 20'000.0F };

        batches.spectrumGrid.start = cursor;

        if (spectrumPlot.width() > 0.0F && spectrumPlot.height() > 0.0F) {
            for (const auto frequency : frequencyGrid) {
                if (frequency > maximumFrequency)
                    continue;

                const auto x = frequencyToX(frequency, maximumFrequency);
                appendQuad(x - 0.5F, plotBottom, x + 0.5F, plotTop, gridColour);
            }

            auto decibels = static_cast<float>(std::ceil(settings.floorDecibels / 20.0F) * 20.0F);

            for (std::size_t line = 0; line < 16 && decibels <= settings.ceilingDecibels;
                ++line, decibels += 20.0F) {
                const auto y
                    = decibelsToY(decibels, settings.floorDecibels, settings.ceilingDecibels);
                appendQuad(plotLeft, y - 0.5F, plotRight, y + 0.5F, gridColour);
            }
        }

        batches.spectrumGrid.count = cursor - batches.spectrumGrid.start;
        batches.spectrum.start = cursor;

        if (spectrumPlot.width() > 0.0F && spectrumPlot.height() > 0.0F && hasDisplayFrame
            && targetFrame.spectrumValid && targetFrame.sampleRate > 0.0) {
            const auto binFrequency
                = static_cast<float>(targetFrame.sampleRate / static_cast<double>(fftSize));
            const auto firstBin = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::ceil(minimumSpectrumFrequency / binFrequency)));
            const auto finalBin = std::min<std::size_t>(spectrumBinCount - 1,
                static_cast<std::size_t>(std::floor(maximumFrequency / binFrequency)));
            constexpr auto spectrumColour = simd_float4 { 0.18F, 0.90F, 0.85F, 1.0F };
            constexpr auto spectrumHalfWidthPoints = 0.75F;
            std::array<simd_float2, spectrumBinCount> spectrumPoints;
            std::size_t pointCount = 0;

            for (auto bin = firstBin; bin <= finalBin && pointCount < spectrumPoints.size();
                ++bin) {
                const auto frequency
                    = std::max(minimumSpectrumFrequency, static_cast<float>(bin) * binFrequency);
                spectrumPoints[pointCount++]
                    = simd_make_float2(frequencyToX(frequency, maximumFrequency),
                        decibelsToY(displayedSpectrum[bin], settings.floorDecibels,
                            settings.ceilingDecibels));
            }

            for (std::size_t index = 0; index < pointCount; ++index) {
                const auto previous = spectrumPoints[index == 0 ? index : index - 1];
                const auto next = spectrumPoints[index + 1 < pointCount ? index + 1 : index];
                const auto deltaX = next.x - previous.x;
                const auto deltaY = next.y - previous.y;
                const auto length = std::sqrt((deltaX * deltaX) + (deltaY * deltaY));
                const auto inverseLength = length > 0.0001F ? 1.0F / length : 0.0F;
                const auto normalX = -deltaY * inverseLength * spectrumHalfWidthPoints;
                const auto normalY = deltaX * inverseLength * spectrumHalfWidthPoints;

                appendVertex(spectrumPoints[index].x + normalX, spectrumPoints[index].y + normalY,
                    spectrumColour);
                appendVertex(spectrumPoints[index].x - normalX, spectrumPoints[index].y - normalY,
                    spectrumColour);
            }
        }

        batches.spectrum.count = cursor - batches.spectrum.start;
        batches.peakRms.start = cursor;

        const auto meterPanel = toRenderRect(dashboardLayout[DashboardPanel::peakRms], height);
        const auto meterPlot
            = insetRenderRect(meterPanel, 12.0F, 10.0F, headerHeight(meterPanel) + 8.0F);
        const auto meterGroupWidth
            = std::min(meterPlot.width(), std::clamp(meterPanel.width() * 0.38F, 44.0F, 76.0F));
        const auto meterLeft = meterPlot.left + (meterPlot.width() - meterGroupWidth) * 0.5F;
        const auto meterBottom = meterPlot.bottom;
        const auto meterTop = meterPlot.top;
        const auto channelGap
            = std::min(meterGroupWidth * 0.14F, std::clamp(meterGroupWidth * 0.08F, 4.0F, 7.0F));
        const auto channelWidth = std::max(0.0F, (meterGroupWidth - channelGap) * 0.5F);
        constexpr auto meterTrackColour = simd_float4 { 0.08F, 0.11F, 0.15F, 1.0F };
        constexpr auto rmsColour = simd_float4 { 0.10F, 0.48F, 0.62F, 0.82F };
        constexpr auto peakNormalColour = simd_float4 { 0.18F, 0.90F, 0.85F, 1.0F };
        constexpr auto peakWarningColour = simd_float4 { 1.0F, 0.72F, 0.20F, 1.0F };
        constexpr auto peakOverColour = simd_float4 { 1.0F, 0.20F, 0.12F, 1.0F };

        const auto meterDecibelsToY = [&](float decibels) noexcept {
            const auto normalised
                = (sanitiseDecibels(decibels, minimumMeterDecibels, maximumMeterDecibels)
                      - minimumMeterDecibels)
                / (maximumMeterDecibels - minimumMeterDecibels);
            return meterBottom + normalised * (meterTop - meterBottom);
        };

        if (meterPlot.width() > 0.0F && meterPlot.height() > 0.0F && channelWidth > 0.0F) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto left
                    = meterLeft + static_cast<float>(channel) * (channelWidth + channelGap);
                const auto right = left + channelWidth;
                appendQuad(left, meterBottom, right, meterTop, meterTrackColour);

                if (!hasDisplayFrame)
                    continue;

                const auto rmsTop = meterDecibelsToY(displayedRms[channel]);
                appendQuad(left + 1.0F, meterBottom + 1.0F, right - 1.0F, rmsTop, rmsColour);

                const auto peakY = meterDecibelsToY(displayedPeak[channel]);
                const auto peakWidth = channelWidth * 0.52F;
                const auto peakLeft = left + (channelWidth - peakWidth) * 0.5F;
                const auto displayedPeakColour = displayedPeak[channel] >= 0.0F
                    ? peakOverColour
                    : (displayedPeak[channel] >= -6.0F ? peakWarningColour : peakNormalColour);
                appendQuad(peakLeft, peakY - 1.0F, peakLeft + peakWidth, peakY + 1.0F,
                    displayedPeakColour);
            }
        }

        batches.peakRms.count = cursor - batches.peakRms.start;
        return batches;
    }
};
} // namespace audio_insight::detail

@implementation AIAudioInsightMetalView

- (void)attachRenderBackend:(audio_insight::detail::MetalRenderBackend*)backend
{
    renderBackend = backend;
    self.delegate = nil;
    self.paused = YES;

    if ([self.layer isKindOfClass:[CAMetalLayer class]]) {
        metalDisplayLink =
            [[CAMetalDisplayLink alloc] initWithMetalLayer:static_cast<CAMetalLayer*>(self.layer)];
        metalDisplayLink.delegate = self;
        metalDisplayLink.preferredFrameLatency = 1.0F;
        metalDisplayLink.paused = YES;
        [metalDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
}

- (void)detachRenderBackend
{
    self.paused = YES;
    self.delegate = nil;

    if (metalDisplayLink != nil) {
        metalDisplayLink.paused = YES;
        metalDisplayLink.delegate = nil;
        [metalDisplayLink invalidate];
        [metalDisplayLink release];
        metalDisplayLink = nil;
    }

    [[NSNotificationCenter defaultCenter] removeObserver:self];
    observedWindow = nil;
    renderBackend = nullptr;
}

- (void)setDisplayLinkPaused:(BOOL)shouldBePaused
{
    self.paused = YES;

    if (metalDisplayLink != nil)
        metalDisplayLink.paused = shouldBePaused;
}

- (void)setDisplayLinkMaximumFramesPerSecond:(NSInteger)maximumFramesPerSecond
{
    if (metalDisplayLink == nil)
        return;

    const auto maximum = static_cast<float>(std::max<NSInteger>(1, maximumFramesPerSecond));
    const auto minimum = std::min(60.0F, maximum);
    metalDisplayLink.preferredFrameRateRange = CAFrameRateRangeMake(minimum, maximum, maximum);
}

- (BOOL)hasDisplayLink
{
    return metalDisplayLink != nil;
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow
{
    auto* notificationCenter = [NSNotificationCenter defaultCenter];

    if (observedWindow != nil)
        [notificationCenter removeObserver:self name:nil object:observedWindow];

    [super viewWillMoveToWindow:newWindow];
    observedWindow = newWindow;

    if (observedWindow != nil) {
        const std::array<NSNotificationName, 6> notificationNames {
            NSWindowDidChangeOcclusionStateNotification, NSWindowDidChangeScreenNotification,
            NSWindowDidChangeBackingPropertiesNotification, NSWindowDidMiniaturizeNotification,
            NSWindowDidDeminiaturizeNotification, NSWindowDidResizeNotification
        };

        for (const auto notificationName : notificationNames)
            [notificationCenter addObserver:self
                                   selector:@selector(nativeVisibilityChanged:)
                                       name:notificationName
                                     object:observedWindow];
    }
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self notifyRenderBackend];
}

- (void)viewDidMoveToSuperview
{
    [super viewDidMoveToSuperview];
    [self notifyRenderBackend];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self notifyRenderBackend];
}

- (void)viewDidHide
{
    [super viewDidHide];
    [self notifyRenderBackend];
}

- (void)viewDidUnhide
{
    [super viewDidUnhide];
    [self notifyRenderBackend];
}

- (void)nativeVisibilityChanged:(NSNotification*)notification
{
    juce::ignoreUnused(notification);
    [self notifyRenderBackend];
}

- (void)notifyRenderBackend
{
    if (renderBackend != nullptr)
        renderBackend->nativeViewStateChanged();
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)displayLink
             needsUpdate:(CAMetalDisplayLinkUpdate*)update
{
    juce::ignoreUnused(displayLink);

    if (renderBackend != nullptr)
        renderBackend->displayLinkUpdate(update);
}

@end

namespace audio_insight {
struct MetalVisualization::Impl {
    Impl(MetalVisualization& componentToUse, VisualizationDataSource& dataSource)
        : component(componentToUse)
    {
        auto device = MTLCreateSystemDefaultDevice();
        nativeView = [[AIAudioInsightMetalView alloc] initWithFrame:NSZeroRect device:device];
        [device release];
        backend = std::make_unique<detail::MetalRenderBackend>(dataSource, nativeView);
        backend->attachNativeView();
        component.setView(nativeView);
        [nativeView release];
        backend->setJuceShowing(component.isShowing());
    }

    ~Impl()
    {
        backend->shutdown();
        component.setView(nullptr);
    }

    MetalVisualization& component;
    AIAudioInsightMetalView* nativeView = nil;
    std::unique_ptr<detail::MetalRenderBackend> backend;
};

MetalVisualization::MetalVisualization(VisualizationDataSource& dataSource)
    : impl(std::make_unique<Impl>(*this, dataSource))
{
    setOpaque(true);
}

MetalVisualization::~MetalVisualization() = default;

void MetalVisualization::setRenderingActive(bool shouldRender)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    impl->backend->setJuceShowing(isShowing());
    impl->backend->setRequestedActive(shouldRender);
}

bool MetalVisualization::isRenderingRequested() const noexcept
{
    return impl->backend->isRenderingRequested();
}

bool MetalVisualization::isEffectivelyRendering() const noexcept
{
    return impl->backend->isEffectivelyRendering();
}

bool MetalVisualization::isMetalAvailable() const noexcept
{
    return impl->backend->isMetalAvailable();
}

juce::String MetalVisualization::getInitializationError() const
{
    return impl->backend->getInitializationError();
}

void MetalVisualization::setEffectiveActivityCallback(EffectiveActivityCallback callback)
{
    impl->backend->setEffectiveActivityCallback(std::move(callback));
}

void MetalVisualization::setSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    impl->backend->setSpectrumSettings(settings);
}

SpectrumRenderSettings MetalVisualization::getSpectrumSettings() const noexcept
{
    return impl->backend->getSpectrumSettings();
}

void MetalVisualization::setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
{
    impl->backend->setDashboardLayoutSplits(splits);
}

DashboardLayoutSplits MetalVisualization::getDashboardLayoutSplits() const noexcept
{
    return impl->backend->getDashboardLayoutSplits();
}

MetalRenderTelemetry MetalVisualization::getRenderTelemetry() const noexcept
{
    return impl->backend->getTelemetry();
}

void MetalVisualization::resetRenderTelemetry()
{
    impl->backend->resetTelemetry();
}

void MetalVisualization::visibilityChanged()
{
    if (impl != nullptr)
        impl->backend->setJuceShowing(isShowing());
}

void MetalVisualization::parentHierarchyChanged()
{
    if (impl != nullptr)
        impl->backend->setJuceShowing(isShowing());
}
} // namespace audio_insight
