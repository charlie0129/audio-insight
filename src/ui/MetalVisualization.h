// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../core/VisualizationDataSource.h"
#include "DashboardLayout.h"
#include "FrameLatencyHistory.h"
#include "PresentedFrameHistory.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace audio_insight {
namespace detail {
inline constexpr std::size_t frequencyAxisTickCandidateCount = 10;
inline constexpr std::array<float, frequencyAxisTickCandidateCount> frequencyAxisTickCandidates {
    20.0F,
    50.0F,
    100.0F,
    200.0F,
    500.0F,
    1'000.0F,
    2'000.0F,
    5'000.0F,
    10'000.0F,
    20'000.0F,
};
inline constexpr std::size_t maximumFrequencyAxisTickCount = frequencyAxisTickCandidateCount + 1;
inline constexpr std::size_t maximumFrequencyAxisLabelGlyphs = 10;
inline constexpr std::size_t frequencyAxisLabelStorage = maximumFrequencyAxisLabelGlyphs + 1;

inline constexpr std::array<int, 4> spectrumDecibelTickSteps { 6, 12, 24, 48 };
inline constexpr float minimumSpectrumDecibelLabelSpacing = 28.0F;
inline constexpr int minimumCachedDecibelTick = -180;
inline constexpr int maximumCachedDecibelTick = 12;
inline constexpr int cachedDecibelTickInterval = 6;
inline constexpr std::size_t cachedDecibelTickCount
    = static_cast<std::size_t>(
          (maximumCachedDecibelTick - minimumCachedDecibelTick) / cachedDecibelTickInterval)
    + 1;

inline constexpr float peakRmsMinimumDecibels = -60.0F;
inline constexpr float peakRmsMaximumDecibels = 3.0F;
inline constexpr std::array<int, 8> peakRmsMajorDecibelTicks {
    -60,
    -48,
    -36,
    -24,
    -12,
    -6,
    0,
    3,
};
inline constexpr std::size_t maximumPeakRmsReadoutGlyphs = 6;
// 20 * log10(maximum finite float) rounds to approximately +770.6 dB.
inline constexpr int maximumFiniteFloatPeakRmsReadoutTenths = 7'706;

enum class PeakRmsLevelRange : std::uint8_t {
    cyan,
    amber,
    red,
};

struct PeakRmsReadout final {
    enum class Kind : std::uint8_t {
        minusInfinity,
        decibelTenths,
    };

    Kind kind = Kind::minusInfinity;
    int decibelTenths = -1'199;
    PeakRmsLevelRange levelRange = PeakRmsLevelRange::cyan;
};

struct PeakRmsLogicalRect final {
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;

    [[nodiscard]] constexpr float width() const noexcept
    {
        return right > left ? right - left : 0.0F;
    }

    [[nodiscard]] constexpr float height() const noexcept
    {
        return top > bottom ? top - bottom : 0.0F;
    }

    [[nodiscard]] constexpr bool contains(float x, float y) const noexcept
    {
        return right > left && top > bottom && x >= left && x <= right && y >= bottom && y <= top;
    }
};

struct PeakRmsPanelLayout final {
    std::array<PeakRmsLogicalRect, 2> channelColumns { };
    std::array<PeakRmsLogicalRect, 2> channelTracks { };
    PeakRmsLogicalRect clearVisualBounds;
    PeakRmsLogicalRect clearHitBounds;
    float scaleBottom = 0.0F;
    float scaleTop = 0.0F;
    float tickLabelRight = 0.0F;
    float tickLineLeft = 0.0F;
    float tickLineRight = 0.0F;
    float channelLabelBottom = 0.0F;
    float readoutBottom = 0.0F;
    float overBottom = 0.0F;
    std::size_t channelCount = 0;
    bool showTickLabels = false;
    bool showReadouts = false;
};

struct PeakRmsTickLabelSelection final {
    std::array<bool, peakRmsMajorDecibelTicks.size()> visible { };
};

/** The shared Spectrum/Spectrogram frequency-coordinate transform. */
struct FrequencyAxisMapping final {
    float minimumFrequencyHz = 20.0F;
    float maximumFrequencyHz = 20'000.0F;
    float spacing = 1.0F;
};

struct FrequencyAxisTickSelection final {
    struct Tick final {
        float frequencyHz = 0.0F;
        std::size_t candidateIndex = frequencyAxisTickCandidateCount;
        bool usesUpperEndpointLabel = false;
    };

    std::array<Tick, maximumFrequencyAxisTickCount> ticks { };
    std::size_t count = 0;
};

struct SpectrumDecibelTicks final {
    std::array<int, cachedDecibelTickCount> values { };
    std::size_t count = 0;
    int candidateStep = spectrumDecibelTickSteps.back();
    int displayedStep = spectrumDecibelTickSteps.back();
};

/**
    Maps frequency to [0, 1] using the accepted linear/logarithmic blend.

    Spectrum uses this value from left to right. Spectrogram uses one minus
    this value in top-origin coordinates, which is the same physical mapping
    after conversion to Metal's bottom-origin logical coordinates.
*/
[[nodiscard]] float mapFrequencyToUnit(
    const FrequencyAxisMapping& mapping, float frequencyHz) noexcept;

/** Formats one bounded frequency-axis label without allocation. */
[[nodiscard]] std::size_t formatFrequencyAxisLabel(
    float frequencyHz, std::array<char, frequencyAxisLabelStorage>& destination) noexcept;

/**
    Selects premeasured nice-label candidates without overlap.

    labelExtents contains widths for a horizontal axis or heights for a
    vertical axis. Endpoint candidates are admitted first; the remaining 1/2/5
    multiples are culled against those measured intervals. No allocation or
    text formatting occurs here.
*/
[[nodiscard]] FrequencyAxisTickSelection selectFrequencyAxisTicks(
    const FrequencyAxisMapping& mapping, float axisLength,
    const std::array<float, frequencyAxisTickCandidateCount>& labelExtents,
    float upperEndpointLabelExtent, float minimumGap = 4.0F) noexcept;

[[nodiscard]] int chooseSpectrumDecibelTickStep(
    float floorDecibels, float ceilingDecibels, float axisLength) noexcept;

[[nodiscard]] SpectrumDecibelTicks makeSpectrumDecibelTicks(
    float floorDecibels, float ceilingDecibels, float axisLength) noexcept;

/** Maps the accepted fixed Peak/RMS dB scale to [0, 1]. */
[[nodiscard]] float mapPeakRmsDecibelsToUnit(float decibels) noexcept;

/** Classifies one live peak for colour and a pre-cached one-decimal readout. */
[[nodiscard]] PeakRmsReadout classifyPeakRmsReadout(float decibels) noexcept;

/** Fits a cached meter label into one layout column without enlarging it. */
[[nodiscard]] float fitPeakRmsTextScale(
    float availableWidth, float unscaledTextWidth, float preferredScale) noexcept;

/** Selects non-overlapping labels without removing any major tick geometry. */
[[nodiscard]] PeakRmsTickLabelSelection selectPeakRmsTickLabels(
    float axisLength, float labelHeight, float minimumGap = 2.0F) noexcept;

/**
    Computes the bounded local-point geometry for the Peak/RMS panel.

    The result uses a bottom-left origin and never depends on drawable pixels,
    so the same layout drives rendering, hit testing, Retina, and regular-density
    displays.
*/
[[nodiscard]] PeakRmsPanelLayout calculatePeakRmsPanelLayout(float panelWidth, float panelHeight,
    float headerHeight, std::uint32_t channelCount, float textHeight, float maximumTickLabelWidth,
    float maximumReadoutWidth) noexcept;

/** Validates the fixed-capacity Spectrum metadata before indexing renderer storage. */
[[nodiscard]] constexpr bool hasSupportedSpectrumMetadata(const VisualizationFrame& frame) noexcept
{
    const auto hasSupportedSize = frame.spectrumFftSize == 1024 || frame.spectrumFftSize == 2048
        || frame.spectrumFftSize == 4096 || frame.spectrumFftSize == 8192
        || frame.spectrumFftSize == 16384;
    return hasSupportedSize && frame.spectrumBinCount == (frame.spectrumFftSize / 2) + 1
        && frame.spectrumBinCount <= maximumSpectrumBinCount;
}

/** Compile-time proof inputs for the one bounded shared vertex buffer. */
struct MetalVisualizationGeometryLimits final {
    static constexpr std::size_t vertexCapacity = 24'576;
    static constexpr std::size_t maximumShellVertices = dashboardPanelCount * 36;
    static constexpr std::size_t maximumDashboardSplitterVertices = dashboardSplitterCount * 6;
    static constexpr std::size_t maximumGridVertices
        = ((2 * maximumFrequencyAxisTickCount) + cachedDecibelTickCount) * 6;
    static constexpr std::size_t maximumSpectrumVertices = 2 * maximumSpectrumBinCount;
    // Eight scale ticks, one five-quad CLEAR target, and four quads (track,
    // RMS, live sample peak, held sample peak) for each of two channels.
    static constexpr std::size_t maximumMeterVertices
        = (peakRmsMajorDecibelTicks.size() + 5 + (2 * 4)) * 6;
    static constexpr std::size_t maximumFixedTextGlyphs = 114;
    static constexpr std::size_t maximumDecibelLabelGlyphs = 7;
    // CLEAR, two channel labels, two OVER labels, two six-glyph readouts,
    // and every fixed scale label.
    static constexpr std::size_t maximumPeakRmsTextGlyphs = 5 + 2 + 8 + 12 + 20;
    static constexpr std::size_t maximumNumericAxisTextGlyphs
        = (2 * maximumFrequencyAxisTickCount * maximumFrequencyAxisLabelGlyphs)
        + (cachedDecibelTickCount * maximumDecibelLabelGlyphs);
    static constexpr std::size_t maximumTextVertices
        = (maximumFixedTextGlyphs + maximumNumericAxisTextGlyphs + maximumPeakRmsTextGlyphs) * 6;
    static constexpr std::size_t maximumGeneratedVertices = maximumShellVertices
        + maximumDashboardSplitterVertices + maximumGridVertices + maximumSpectrumVertices
        + maximumMeterVertices + maximumTextVertices;
};
} // namespace detail

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
    std::uint64_t gpuTimingSamples = 0;
    std::uint64_t gpuTimingUnavailableSamples = 0;
    std::uint64_t commandBufferFailures = 0;
    std::uint64_t presentationCallbacks = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t presentationLatenessSamples = 0;
    std::uint64_t presentationLatenessUnclassifiableSamples = 0;
    std::uint64_t presentationHistoryDiscardedTimestamps = 0;
    std::uint64_t frameLatencySamples = 0;
    std::uint64_t frameLatencyTotalTimingSamples = 0;
    std::uint64_t frameLatencyTotalTimingUnavailableSamples = 0;
    std::uint64_t frameLatencyComponentTimingSamples = 0;
    std::uint64_t frameLatencyComponentTimingUnavailableSamples = 0;
    std::uint64_t frameLatencyHistoryDiscardedSamples = 0;
    std::uint64_t presentationsAfterTarget = 0;
    std::uint64_t skippedPresentations = 0;
    std::uint64_t gpuBackpressureDrops = 0;
    std::uint64_t drawableUnavailableDrops = 0;
    std::uint64_t callbackHostDelaySamples = 0;
    std::uint64_t callbackHostDelayUnclassifiableSamples = 0;
    std::uint64_t callbackAlreadyLateHostDelays = 0;
    std::uint64_t cpuCommitLatenessSamples = 0;
    std::uint64_t cpuCommitLatenessUnclassifiableSamples = 0;
    std::uint64_t cpuCommitDeadlineMisses = 0;
    std::uint64_t gpuCompletionLatenessSamples = 0;
    std::uint64_t gpuCompletionLatenessUnclassifiableSamples = 0;
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

    // Chronological, oldest-to-newest intervals derived under the renderer's
    // presentation-history lock. sequence identifies the display-link callback
    // that produced the interval's right-hand frame, so gaps expose skipped
    // presentation opportunities.
    std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>
        presentedFrameIntervalHistory { };
    std::size_t presentedFrameIntervalHistoryCount = 0;

    // One sample per correlated submission/presentation callback pair, ordered
    // by actual presentation time. CPU encode ends immediately before command
    // submission; submit + queue covers command submission and driver/GPU wait.
    std::array<FrameLatencySample, frameLatencyHistoryCapacity> frameLatencyHistory { };
    std::size_t frameLatencyHistoryCount = 0;

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
    float smoothing = 0.35F;
    float frequencySpacing = 1.0F;
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
    using EffectiveActivityCallback = std::function<void(bool)>;
    using DashboardLayoutEditCancelCallback = std::function<void()>;

    explicit MetalVisualization(VisualizationDataSource& dataSource);
    ~MetalVisualization() override;

    /** Requests active rendering and analysis while the editor is visible. */
    void setRenderingActive(bool shouldRender);

    [[nodiscard]] bool isRenderingRequested() const noexcept;
    [[nodiscard]] bool isEffectivelyRendering() const noexcept;
    [[nodiscard]] bool isMetalAvailable() const noexcept;
    [[nodiscard]] juce::String getInitializationError() const;

    /**
        Installs a message-thread callback for actual rendering-state
        transitions caused by editor or native-window lifecycle changes. Pass
        an empty callback before destroying any state captured by the callback.
    */
    void setEffectiveActivityCallback(EffectiveActivityCallback callback);

    /**
        Updates the render range and temporal smoothing without requiring the
        message thread. smoothing is normalized: zero is immediate and one is
        the strongest smoothing.
    */
    void setSpectrumSettings(SpectrumRenderSettings settings) noexcept;
    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept;

    /**
        Publishes the four validated dashboard split indices. Invalid input is
        replaced atomically with the compiled default layout. These methods are
        safe to call from any thread and never allocate.
    */
    void setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept;
    [[nodiscard]] DashboardLayoutSplits getDashboardLayoutSplits() const noexcept;

    /**
        Shows native Metal splitter handles and enables pointer, keyboard, and
        accessibility editing inside the dashboard. This is a message-thread
        UI boundary; the four working split values remain available through
        getDashboardLayoutSplits().
    */
    void setDashboardLayoutEditing(bool shouldEdit);
    [[nodiscard]] bool isDashboardLayoutEditing() const noexcept;
    void setDashboardLayoutEditCancelCallback(DashboardLayoutEditCancelCallback callback);

    [[nodiscard]] MetalRenderTelemetry getRenderTelemetry() const noexcept;

    /**
        Resets renderer telemetry on the message thread. An active renderer
        switches epochs at its next display callback boundary; an inactive or
        unavailable renderer publishes the reset immediately.
    */
    void resetRenderTelemetry();

    void visibilityChanged() override;
    void parentHierarchyChanged() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetalVisualization)
};
} // namespace audio_insight
