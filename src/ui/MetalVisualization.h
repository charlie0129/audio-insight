// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../core/VisualizationDataSource.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdint>
#include <memory>

namespace audio_insight {
/** A point-in-time copy of the Metal renderer's non-real-time telemetry. */
struct MetalRenderTelemetry {
    // A callback is attributed to host delay only when it begins after the
    // CAMetalDisplayLink update's targetTimestamp. CPU and GPU deadline misses
    // are independently compared with targetPresentationTimestamp. The
    // configured screen maximum is never used to infer a missed deadline.
    std::uint64_t epoch = 0;
    std::uint64_t displayLinkCallbacks = 0;
    std::uint64_t submittedFrames = 0;
    std::uint64_t completedFrames = 0;
    std::uint64_t commandBufferFailures = 0;
    std::uint64_t presentationCallbacks = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t presentationsAfterTarget = 0;
    std::uint64_t skippedPresentations = 0;
    std::uint64_t gpuBackpressureDrops = 0;
    std::uint64_t drawableUnavailableDrops = 0;
    std::uint64_t callbackAlreadyLateHostDelays = 0;
    std::uint64_t cpuCommitDeadlineMisses = 0;
    std::uint64_t gpuCompletionDeadlineMisses = 0;
    std::uint64_t analysisRequestCalls = 0;
    std::uint64_t snapshotReads = 0;
    std::uint64_t framesWithNewSnapshot = 0;
    std::uint64_t lastSpectrumSequence = 0;

    std::uint64_t lastCpuEncodeNanoseconds = 0;
    std::uint64_t maximumCpuEncodeNanoseconds = 0;
    std::uint64_t lastGpuExecutionNanoseconds = 0;
    std::uint64_t maximumGpuExecutionNanoseconds = 0;
    std::uint64_t lastDisplayCallbackIntervalNanoseconds = 0;
    std::uint64_t lastTargetIntervalNanoseconds = 0;
    std::uint64_t lastTargetPresentationIntervalNanoseconds = 0;
    std::uint64_t lastPresentedFrameIntervalNanoseconds = 0;
    std::uint64_t lastPresentedHostTimestampNanoseconds = 0;
    std::uint64_t lastPresentationLatenessNanoseconds = 0;
    std::uint64_t maximumPresentationLatenessNanoseconds = 0;
    std::uint64_t lastCallbackHostDelayNanoseconds = 0;
    std::uint64_t lastCpuCommitLatenessNanoseconds = 0;
    std::uint64_t lastGpuCompletionLatenessNanoseconds = 0;
    std::uint64_t lastTargetTimestampNanoseconds = 0;
    std::uint64_t lastTargetPresentationTimestampNanoseconds = 0;
    std::uint64_t lastProvidedDrawableAccessNanoseconds = 0;
    std::uint64_t maximumProvidedDrawableAccessNanoseconds = 0;

    std::uint32_t drawableWidthPixels = 0;
    std::uint32_t drawableHeightPixels = 0;
    std::uint32_t configuredMaximumFramesPerSecond = 0;
    double backingScale = 1.0;

    bool metalAvailable = false;
    bool renderingRequested = false;
    bool effectivelyRendering = false;
    bool resetPending = false;
};

/** Thread-safe render settings corresponding to the spectrum APVTS values. */
struct SpectrumRenderSettings {
    float floorDecibels = -90.0F;
    float ceilingDecibels = 0.0F;
    float smoothing = 0.65F;
};

/**
    A JUCE component containing a paused native MTKView whose CAMetalLayer is
    driven by CAMetalDisplayLink.

    setRenderingActive() is the editor lifecycle boundary. It must be called on
    the JUCE message thread. Native visibility, window occlusion, application
    attachment, and Metal availability are additionally considered before the
    renderer or its VisualizationDataSource is activated.
*/
class MetalVisualization final : public juce::NSViewComponent {
public:
    explicit MetalVisualization(VisualizationDataSource& dataSource);
    ~MetalVisualization() override;

    /** Requests active rendering and analysis while the editor is visible. */
    void setRenderingActive(bool shouldRender);

    [[nodiscard]] bool isRenderingRequested() const noexcept;
    [[nodiscard]] bool isEffectivelyRendering() const noexcept;
    [[nodiscard]] bool isMetalAvailable() const noexcept;
    [[nodiscard]] juce::String getInitializationError() const;

    /**
        Updates the render range and temporal smoothing without requiring the
        message thread. smoothing is normalized: zero is immediate and one is
        the strongest smoothing.
    */
    void setSpectrumSettings(SpectrumRenderSettings settings) noexcept;
    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept;

    [[nodiscard]] MetalRenderTelemetry getRenderTelemetry() const noexcept;
    void resetRenderTelemetry();

    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetalVisualization)
};
} // namespace audio_insight
