// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MetalVisualization.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

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
constexpr std::size_t maximumGridVertices = 160;
constexpr std::size_t maximumMeterVertices = 48;
constexpr std::size_t maximumVertexCount
    = (2 * spectrumBinCount) + maximumGridVertices + maximumMeterVertices;

constexpr float minimumSpectrumFrequency = 20.0F;
constexpr float maximumSpectrumFrequency = 20'000.0F;
constexpr float minimumMeterDecibels = -60.0F;
constexpr float maximumMeterDecibels = 12.0F;
constexpr float minimumAllowedSpectrumFloor = -160.0F;
constexpr float maximumAllowedSpectrumCeiling = 24.0F;
constexpr float minimumSpectrumRange = 6.0F;

struct MetalVertex {
    simd_float2 position;
    simd_float4 colour;
};

struct AtomicRenderTelemetry {
    std::uint64_t epoch = 1;
    std::atomic<std::uint64_t> displayLinkCallbacks { 0 };
    std::atomic<std::uint64_t> submittedFrames { 0 };
    std::atomic<std::uint64_t> completedFrames { 0 };
    std::atomic<std::uint64_t> commandBufferFailures { 0 };
    std::atomic<std::uint64_t> presentationCallbacks { 0 };
    std::atomic<std::uint64_t> presentedFrames { 0 };
    std::atomic<std::uint64_t> presentationsAfterTarget { 0 };
    std::atomic<std::uint64_t> skippedPresentations { 0 };
    std::atomic<std::uint64_t> gpuBackpressureDrops { 0 };
    std::atomic<std::uint64_t> drawableUnavailableDrops { 0 };
    std::atomic<std::uint64_t> callbackAlreadyLateHostDelays { 0 };
    std::atomic<std::uint64_t> cpuCommitDeadlineMisses { 0 };
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
    std::atomic<std::uint64_t> lastPresentedFrameIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastPresentedHostTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastPresentationLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumPresentationLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastCallbackHostDelayNanoseconds { 0 };
    std::atomic<std::uint64_t> lastCpuCommitLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastGpuCompletionLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetPresentationTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastProvidedDrawableAccessNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumProvidedDrawableAccessNanoseconds { 0 };

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
    std::atomic<std::uint64_t> classifiedAdmission { 0 };
    id<MTLBuffer> vertexBuffer = nil;

    ~RenderBufferSlot()
    {
        [vertexBuffer release];
    }
};

struct SharedRenderState {
    std::array<RenderBufferSlot, renderBufferCount> slots;
};

struct VertexBatches {
    std::size_t gridStart = 0;
    std::size_t gridCount = 0;
    std::size_t spectrumStart = 0;
    std::size_t spectrumCount = 0;
    std::size_t meterStart = 0;
    std::size_t meterCount = 0;
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

void releaseRenderBuffer(
    const std::shared_ptr<SharedRenderState>& state, RenderBufferAdmission admission) noexcept
{
    if (admission.index >= state->slots.size())
        return;

    auto expected = admission.busyState;
    state->slots[admission.index].admissionState.compare_exchange_strong(
        expected, admission.busyState + 1, std::memory_order_release, std::memory_order_relaxed);
}

bool classifyPresentation(
    const std::shared_ptr<SharedRenderState>& state, RenderBufferAdmission admission) noexcept
{
    if (admission.index >= state->slots.size())
        return false;

    auto expected = admission.busyState >= 2 ? admission.busyState - 2 : 0;
    return state->slots[admission.index].classifiedAdmission.compare_exchange_strong(
        expected, admission.busyState, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void recordSkippedPresentation(const std::shared_ptr<SharedRenderState>& state,
    RenderBufferAdmission admission,
    const std::shared_ptr<AtomicRenderTelemetry>& telemetry) noexcept
{
    if (classifyPresentation(state, admission))
        telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
}

void recordPresentedDrawable(const std::shared_ptr<SharedRenderState>& state,
    RenderBufferAdmission admission, const std::shared_ptr<AtomicRenderTelemetry>& telemetry,
    id<MTLDrawable> drawable, CFTimeInterval targetPresentationTimestamp) noexcept
{
    telemetry->presentationCallbacks.fetch_add(1, std::memory_order_relaxed);

    const auto presentedTime = drawable.presentedTime;

    if (!std::isfinite(presentedTime) || presentedTime <= 0.0) {
        recordSkippedPresentation(state, admission, telemetry);
        return;
    }

    if (!classifyPresentation(state, admission))
        return;

    const auto presentedNanoseconds = hostTimeNanoseconds(presentedTime);
    auto previous
        = telemetry->lastPresentedHostTimestampNanoseconds.load(std::memory_order_relaxed);

    while (presentedNanoseconds > previous
        && !telemetry->lastPresentedHostTimestampNanoseconds.compare_exchange_weak(previous,
            presentedNanoseconds, std::memory_order_relaxed, std::memory_order_relaxed)) { }

    if (presentedNanoseconds > previous && previous != 0) {
        telemetry->lastPresentedFrameIntervalNanoseconds.store(
            presentedNanoseconds - previous, std::memory_order_relaxed);
    }

    if (std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0) {
        const auto presentationLateness
            = positiveHostTimeDifference(presentedTime, targetPresentationTimestamp);
        telemetry->lastPresentationLatenessNanoseconds.store(
            presentationLateness, std::memory_order_relaxed);
        updateMaximum(telemetry->maximumPresentationLatenessNanoseconds, presentationLateness);

        if (presentationLateness != 0)
            telemetry->presentationsAfterTarget.fetch_add(1, std::memory_order_relaxed);
    }

    telemetry->presentedFrames.fetch_add(1, std::memory_order_relaxed);
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

    void setSpectrumSettings(SpectrumRenderSettings settings) noexcept
    {
        packedSpectrumSettings.store(packSpectrumSettings(settings), std::memory_order_release);
    }

    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept
    {
        return unpackSpectrumSettings(packedSpectrumSettings.load(std::memory_order_acquire));
    }

    [[nodiscard]] MetalRenderTelemetry getTelemetry() const noexcept
    {
        const auto telemetry = loadPublishedTelemetry();
        MetalRenderTelemetry result;

        result.epoch = telemetry->epoch;
        result.displayLinkCallbacks
            = telemetry->displayLinkCallbacks.load(std::memory_order_relaxed);
        result.submittedFrames = telemetry->submittedFrames.load(std::memory_order_relaxed);
        result.completedFrames = telemetry->completedFrames.load(std::memory_order_relaxed);
        result.commandBufferFailures
            = telemetry->commandBufferFailures.load(std::memory_order_relaxed);
        result.presentationCallbacks
            = telemetry->presentationCallbacks.load(std::memory_order_relaxed);
        result.presentedFrames = telemetry->presentedFrames.load(std::memory_order_relaxed);
        result.presentationsAfterTarget
            = telemetry->presentationsAfterTarget.load(std::memory_order_relaxed);
        result.skippedPresentations
            = telemetry->skippedPresentations.load(std::memory_order_relaxed);
        result.gpuBackpressureDrops
            = telemetry->gpuBackpressureDrops.load(std::memory_order_relaxed);
        result.drawableUnavailableDrops
            = telemetry->drawableUnavailableDrops.load(std::memory_order_relaxed);
        result.callbackAlreadyLateHostDelays
            = telemetry->callbackAlreadyLateHostDelays.load(std::memory_order_relaxed);
        result.cpuCommitDeadlineMisses
            = telemetry->cpuCommitDeadlineMisses.load(std::memory_order_relaxed);
        result.gpuCompletionDeadlineMisses
            = telemetry->gpuCompletionDeadlineMisses.load(std::memory_order_relaxed);
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
        result.lastGpuExecutionNanoseconds
            = telemetry->lastGpuExecutionNanoseconds.load(std::memory_order_relaxed);
        result.maximumGpuExecutionNanoseconds
            = telemetry->maximumGpuExecutionNanoseconds.load(std::memory_order_relaxed);
        result.lastDisplayCallbackIntervalNanoseconds
            = telemetry->lastDisplayCallbackIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetIntervalNanoseconds
            = telemetry->lastTargetIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetPresentationIntervalNanoseconds
            = telemetry->lastTargetPresentationIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastPresentedFrameIntervalNanoseconds
            = telemetry->lastPresentedFrameIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastPresentedHostTimestampNanoseconds
            = telemetry->lastPresentedHostTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastPresentationLatenessNanoseconds
            = telemetry->lastPresentationLatenessNanoseconds.load(std::memory_order_relaxed);
        result.maximumPresentationLatenessNanoseconds
            = telemetry->maximumPresentationLatenessNanoseconds.load(std::memory_order_relaxed);
        result.lastCallbackHostDelayNanoseconds
            = telemetry->lastCallbackHostDelayNanoseconds.load(std::memory_order_relaxed);
        result.lastCpuCommitLatenessNanoseconds
            = telemetry->lastCpuCommitLatenessNanoseconds.load(std::memory_order_relaxed);
        result.lastGpuCompletionLatenessNanoseconds
            = telemetry->lastGpuCompletionLatenessNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetTimestampNanoseconds
            = telemetry->lastTargetTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetPresentationTimestampNanoseconds
            = telemetry->lastTargetPresentationTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastProvidedDrawableAccessNanoseconds
            = telemetry->lastProvidedDrawableAccessNanoseconds.load(std::memory_order_relaxed);
        result.maximumProvidedDrawableAccessNanoseconds
            = telemetry->maximumProvidedDrawableAccessNanoseconds.load(std::memory_order_relaxed);
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
        queueTelemetryReset();
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
        applyPendingStateAtDisplayCallbackBoundary();
        const auto telemetry = callbackTelemetry;
        const auto targetTimestamp = update.targetTimestamp;
        const auto targetPresentationTimestamp = update.targetPresentationTimestamp;
        recordDisplayLinkCallback(
            *telemetry, callbackHostTime, targetTimestamp, targetPresentationTimestamp);

        const auto admission = acquireRenderBuffer();

        if (admission.index >= renderBufferCount) {
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
            recordSkippedPresentation(sharedState, admission, telemetry);
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

        auto* vertices = static_cast<MetalVertex*>(slot.vertexBuffer.contents);
        const auto batches = populateVertices(vertices, boundsSize, spectrumSettings);

        auto* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* colourAttachment = descriptor.colorAttachments[0];
        colourAttachment.texture = drawable.texture;
        colourAttachment.loadAction = MTLLoadActionClear;
        colourAttachment.storeAction = MTLStoreActionStore;
        colourAttachment.clearColor = MTLClearColorMake(0.018, 0.024, 0.035, 1.0);

        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];

        if (commandBuffer == nil) {
            recordSkippedPresentation(sharedState, admission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:descriptor];

        if (encoder == nil) {
            recordSkippedPresentation(sharedState, admission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->drawableUnavailableDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        encoder.label = @"Audio Insight visualization";
        [encoder setRenderPipelineState:pipelineState];
        [encoder setVertexBuffer:slot.vertexBuffer offset:0 atIndex:0];

        if (batches.gridCount != 0)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.gridStart
                        vertexCount:batches.gridCount];

        if (batches.spectrumCount >= 2)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                        vertexStart:batches.spectrumStart
                        vertexCount:batches.spectrumCount];

        if (batches.meterCount != 0)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.meterStart
                        vertexCount:batches.meterCount];

        [encoder endEncoding];

        auto presentationState = sharedState;
        auto presentationTelemetry = telemetry;
        [drawable addPresentedHandler:^(id<MTLDrawable> presentedDrawable) {
            recordPresentedDrawable(presentationState, admission, presentationTelemetry,
                presentedDrawable, targetPresentationTimestamp);
            releaseRenderBuffer(presentationState, admission);
        }];

        auto completionState = sharedState;
        auto completionTelemetry = telemetry;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            if (completedBuffer.status == MTLCommandBufferStatusError) {
                completionTelemetry->commandBufferFailures.fetch_add(1, std::memory_order_relaxed);
                recordSkippedPresentation(completionState, admission, completionTelemetry);
                releaseRenderBuffer(completionState, admission);
                return;
            }

            const auto gpuStart = completedBuffer.GPUStartTime;
            const auto gpuEnd = completedBuffer.GPUEndTime;

            if (gpuEnd >= gpuStart && gpuStart > 0.0) {
                const auto gpuNanoseconds = positiveHostTimeDifference(gpuEnd, gpuStart);
                completionTelemetry->lastGpuExecutionNanoseconds.store(
                    gpuNanoseconds, std::memory_order_relaxed);
                updateMaximum(completionTelemetry->maximumGpuExecutionNanoseconds, gpuNanoseconds);

                const auto completionLateness
                    = positiveHostTimeDifference(gpuEnd, targetPresentationTimestamp);
                completionTelemetry->lastGpuCompletionLatenessNanoseconds.store(
                    completionLateness, std::memory_order_relaxed);

                if (completionLateness != 0)
                    completionTelemetry->gpuCompletionDeadlineMisses.fetch_add(
                        1, std::memory_order_relaxed);
            }

            completionTelemetry->completedFrames.fetch_add(1, std::memory_order_relaxed);
        }];

        if (std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0)
            [commandBuffer presentDrawable:drawable atTime:targetPresentationTimestamp];
        else
            [commandBuffer presentDrawable:drawable];

        [commandBuffer commit];

        const auto commitHostTime = CACurrentMediaTime();
        const auto commitLateness
            = positiveHostTimeDifference(commitHostTime, targetPresentationTimestamp);
        telemetry->lastCpuCommitLatenessNanoseconds.store(
            commitLateness, std::memory_order_relaxed);

        if (commitLateness != 0)
            telemetry->cpuCommitDeadlineMisses.fetch_add(1, std::memory_order_relaxed);

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
    std::atomic<std::uint64_t> requestedTelemetryEpoch { 1 };

    std::atomic<bool> requestedActive { false };
    std::atomic<bool> juceShowing { false };
    std::atomic<bool> effectiveActive { false };
    std::atomic<bool> hasShutDown { false };

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

    void applyPendingStateAtDisplayCallbackBoundary()
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
            effectiveActive.store(true, std::memory_order_release);
            loadPublishedTelemetry()->effectivelyRendering.store(true, std::memory_order_relaxed);

            if (view != nil)
                [view setDisplayLinkPaused:NO];

            return;
        }

        if (view != nil)
            [view setDisplayLinkPaused:YES];

        effectiveActive.store(false, std::memory_order_release);
        loadPublishedTelemetry()->effectivelyRendering.store(false, std::memory_order_relaxed);
        source.setVisualizationActive(false);
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

    void recordDisplayLinkCallback(AtomicRenderTelemetry& telemetry,
        CFTimeInterval callbackHostTime, CFTimeInterval targetTimestamp,
        CFTimeInterval targetPresentationTimestamp) noexcept
    {
        telemetry.displayLinkCallbacks.fetch_add(1, std::memory_order_relaxed);
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

        const auto callbackHostDelay
            = positiveHostTimeDifference(callbackHostTime, targetTimestamp);
        telemetry.lastCallbackHostDelayNanoseconds.store(
            callbackHostDelay, std::memory_order_relaxed);

        if (callbackHostDelay != 0)
            telemetry.callbackAlreadyLateHostDelays.fetch_add(1, std::memory_order_relaxed);

        previousDisplayCallbackHostTime = callbackHostTime;
        previousTargetTimestamp = targetTimestamp;
        previousTargetPresentationTimestamp = targetPresentationTimestamp;
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

    VertexBatches populateVertices(
        MetalVertex* vertices, CGSize logicalSize, SpectrumRenderSettings settings) const noexcept
    {
        VertexBatches batches;

        if (vertices == nullptr || logicalSize.width < 2.0 || logicalSize.height < 2.0)
            return batches;

        std::size_t cursor = 0;
        const auto width = static_cast<float>(logicalSize.width);
        const auto height = static_cast<float>(logicalSize.height);
        const auto margin = std::clamp(width * 0.018F, 10.0F, 20.0F);
        const auto meterWidth = std::clamp(width * 0.075F, 54.0F, 92.0F);
        const auto meterGap = std::clamp(width * 0.012F, 8.0F, 16.0F);
        const auto plotLeft = margin;
        const auto plotRight = std::max(plotLeft + 1.0F, width - margin - meterGap - meterWidth);
        const auto plotBottom = std::clamp(height * 0.045F, 14.0F, 30.0F);
        const auto plotTop = height - std::clamp(height * 0.025F, 10.0F, 22.0F);

        const auto pointToClip = [width, height](float x, float y) noexcept {
            return simd_make_float2((2.0F * x / width) - 1.0F, (2.0F * y / height) - 1.0F);
        };

        const auto appendVertex = [&](float x, float y, simd_float4 colour) noexcept {
            if (cursor < maximumVertexCount)
                vertices[cursor++] = { pointToClip(x, y), colour };
        };

        const auto appendQuad
            = [&](float left, float bottom, float right, float top, simd_float4 colour) noexcept {
                  const auto bottomLeft = pointToClip(left, bottom);
                  const auto bottomRight = pointToClip(right, bottom);
                  const auto topLeft = pointToClip(left, top);
                  const auto topRight = pointToClip(right, top);

                  if (cursor + 6 <= maximumVertexCount) {
                      vertices[cursor++] = { bottomLeft, colour };
                      vertices[cursor++] = { bottomRight, colour };
                      vertices[cursor++] = { topLeft, colour };
                      vertices[cursor++] = { topLeft, colour };
                      vertices[cursor++] = { bottomRight, colour };
                      vertices[cursor++] = { topRight, colour };
                  }
              };

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

        batches.gridStart = cursor;

        for (const auto frequency : frequencyGrid) {
            if (frequency > maximumFrequency)
                continue;

            const auto x = frequencyToX(frequency, maximumFrequency);
            appendQuad(x - 0.5F, plotBottom, x + 0.5F, plotTop, gridColour);
        }

        auto decibels = static_cast<float>(std::ceil(settings.floorDecibels / 20.0F) * 20.0F);

        for (std::size_t line = 0; line < 16 && decibels <= settings.ceilingDecibels;
            ++line, decibels += 20.0F) {
            const auto y = decibelsToY(decibels, settings.floorDecibels, settings.ceilingDecibels);
            appendQuad(plotLeft, y - 0.5F, plotRight, y + 0.5F, gridColour);
        }

        batches.gridCount = cursor - batches.gridStart;
        batches.spectrumStart = cursor;

        if (hasDisplayFrame && targetFrame.spectrumValid && targetFrame.sampleRate > 0.0) {
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

            for (std::size_t index = 0; index < pointCount && cursor + 2 <= maximumVertexCount;
                ++index) {
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

        batches.spectrumCount = cursor - batches.spectrumStart;
        batches.meterStart = cursor;

        const auto meterLeft = plotRight + meterGap;
        const auto channelGap = std::clamp(meterWidth * 0.11F, 5.0F, 9.0F);
        const auto channelWidth = (meterWidth - channelGap) * 0.5F;
        constexpr auto meterTrackColour = simd_float4 { 0.08F, 0.11F, 0.15F, 1.0F };
        constexpr auto rmsColour = simd_float4 { 0.12F, 0.58F, 0.78F, 1.0F };
        constexpr auto peakColour = simd_float4 { 1.0F, 0.72F, 0.20F, 1.0F };

        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto left = meterLeft + static_cast<float>(channel) * (channelWidth + channelGap);
            const auto right = left + channelWidth;
            appendQuad(left, plotBottom, right, plotTop, meterTrackColour);

            if (hasDisplayFrame) {
                const auto rmsTop = decibelsToY(
                    displayedRms[channel], minimumMeterDecibels, maximumMeterDecibels);
                appendQuad(left + 1.0F, plotBottom + 1.0F, right - 1.0F, rmsTop, rmsColour);

                const auto peakY = decibelsToY(
                    displayedPeak[channel], minimumMeterDecibels, maximumMeterDecibels);
                const auto displayedPeakColour = displayedPeak[channel] > 0.0F
                    ? simd_float4 { 1.0F, 0.20F, 0.12F, 1.0F }
                    : peakColour;
                appendQuad(left, peakY - 1.0F, right, peakY + 1.0F, displayedPeakColour);
            }
        }

        batches.meterCount = cursor - batches.meterStart;
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

void MetalVisualization::setSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    impl->backend->setSpectrumSettings(settings);
}

SpectrumRenderSettings MetalVisualization::getSpectrumSettings() const noexcept
{
    return impl->backend->getSpectrumSettings();
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
