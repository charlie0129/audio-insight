// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../core/VisualizationDataSource.h"
#include "DashboardLayout.h"
#include "FrameLatencyHistory.h"
#include "PresentedFrameHistory.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace audio_insight {
enum class MetalDisplayFramePacing : std::uint8_t {
    fixedMaximum,
    adaptive,
};

namespace detail {
struct DisplayLinkFrameRateRange final {
    std::uint32_t minimumFramesPerSecond = 60;
    std::uint32_t preferredFramesPerSecond = 60;
    std::uint32_t maximumFramesPerSecond = 60;
};

/** Builds the best-effort Core Animation request for the active display. */
[[nodiscard]] DisplayLinkFrameRateRange displayLinkFrameRateRange(
    int displayMaximumFramesPerSecond, MetalDisplayFramePacing framePacing) noexcept;

inline constexpr float maximumFrequencyAxisFrequencyHz = 20'000.0F;
inline constexpr std::size_t maximumFrequencyAxisCandidateCount = 320;
inline constexpr std::size_t maximumFrequencyAxisTickCount = 64;
inline constexpr std::size_t maximumFrequencyAxisLabelGlyphs = 10;
inline constexpr std::size_t frequencyAxisLabelStorage = maximumFrequencyAxisLabelGlyphs + 1;

inline constexpr std::size_t maximumSpectrogramHistoryColumnCount = 8'192;
inline constexpr std::size_t maximumSpectrogramColumnsDrainedPerFrame = 32;

enum class SpectrogramRenderPalette : std::uint8_t {
    blueFire,
    inferno,
    viridis,
    grayscale,
};

enum class SpectrogramRenderHistoryMode : std::uint8_t {
    scroll,
    overwrite,
};

struct SpectrogramPaletteColour final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
};

struct SpectrogramHistorySignature final {
    std::uint64_t captureGeneration = 0;
    std::uint64_t fftGeneration = 0;
    std::uint64_t mappingGeneration = 0;
    std::uint64_t resetEpoch = 0;
    std::uint32_t fftSize = 0;
    std::uint32_t rowCount = 0;
    std::uint32_t columnCount = 0;
    std::uint32_t requestedSliceRateHz = 0;
    double sampleRate = 0.0;
};

struct SpectrogramHistoryTransition final {
    bool clear = false;
    bool reallocate = false;
};

struct SpectrogramRingAdvance final {
    std::uint32_t writeColumn = 0;
    std::uint64_t gapColumnCount = 0;
    bool accepted = false;
    bool discardedPreviousSpan = false;
};

struct SpectrogramScrollClockUpdate final {
    float headOffsetColumns = 0.0F;
    bool valid = false;
    bool initializedThisUpdate = false;
    bool underrun = false;
};

/**
    Advances the Scroll-mode display head from display target time rather than
    from discrete FFT-column arrivals.

    The first column is held one timeline slot ahead of the display head. At a
    120 Hz display and the default 60 Hz FFT cadence this produces two evenly
    spaced half-column movements instead of one whole-column jump. Ordinary
    later column bursts move only the stored-data frontier; an exhausted
    cushion or expired display head deliberately starts a fresh anchor.
*/
class SpectrogramScrollClock final {
public:
    void reset() noexcept;

    [[nodiscard]] SpectrogramScrollClockUpdate update(std::uint64_t latestTimelineSlot,
        std::uint32_t requestedSliceRateHz, double targetPresentationTimestampSeconds,
        std::uint32_t historyColumnCount) noexcept;

private:
    double headOffsetColumns_ = 0.0;
    double previousTargetPresentationTimestampSeconds_ = 0.0;
    std::uint64_t latestObservedTimelineSlot_ = 0;
    std::uint32_t requestedSliceRateHz_ = 0;
    bool initialized_ = false;
};

/** Keeps fallback display-link timestamps in the presentation-target time domain. */
class SpectrogramPresentationTimebase final {
public:
    void reset() noexcept;
    [[nodiscard]] double presentationTime(double callbackHostTimeSeconds,
        double targetTimestampSeconds, double targetPresentationTimestampSeconds) noexcept;

private:
    double presentationLeadFromTargetSeconds_ = 0.0;
    double presentationLeadFromCallbackSeconds_ = 0.0;
    bool hasTargetLead_ = false;
    bool hasCallbackLead_ = false;
};

/** Fixed-capacity logical state for the renderer-owned circular history texture. */
class SpectrogramHistoryRing final {
public:
    void configure(std::uint32_t columnCount) noexcept;
    void clear() noexcept;

    [[nodiscard]] SpectrogramRingAdvance append(
        std::uint64_t timelineSlot, std::uint64_t sequence) noexcept;
    [[nodiscard]] std::optional<std::uint32_t> physicalColumnForScreenColumn(
        std::uint32_t screenColumn, SpectrogramRenderHistoryMode mode) const noexcept;

    [[nodiscard]] std::uint32_t columnCount() const noexcept
    {
        return columnCount_;
    }
    [[nodiscard]] std::uint32_t nextWriteColumn() const noexcept
    {
        return nextWriteColumn_;
    }
    [[nodiscard]] std::uint32_t timelineSpan() const noexcept
    {
        return timelineSpan_;
    }
    [[nodiscard]] std::uint64_t latestTimelineSlot() const noexcept
    {
        return hasTimeline_ ? lastTimelineSlot_ : 0;
    }
    [[nodiscard]] bool isColumnValid(std::uint32_t physicalColumn) const noexcept;
    [[nodiscard]] const std::array<std::uint8_t, maximumSpectrogramHistoryColumnCount>&
    validity() const noexcept
    {
        return validity_;
    }

private:
    void appendGap() noexcept;

    std::array<std::uint8_t, maximumSpectrogramHistoryColumnCount> validity_ { };
    std::uint32_t columnCount_ = 0;
    std::uint32_t nextWriteColumn_ = 0;
    std::uint32_t timelineSpan_ = 0;
    std::uint64_t lastTimelineSlot_ = 0;
    std::uint64_t lastSequence_ = 0;
    bool hasTimeline_ = false;
};

struct SpectrogramUploadCompletion final {
    std::uint64_t transaction = 0;
    bool succeeded = false;
};

/** Serializes upload-bearing command buffers without blocking the display callback. */
class SpectrogramUploadGate final {
public:
    [[nodiscard]] bool isUploadInFlight() const noexcept
    {
        return uploadInFlight_.load(std::memory_order_acquire);
    }

    void beginUpload(std::uint64_t transaction) noexcept
    {
        jassert(transaction != 0);
        jassert(!uploadInFlight_.load(std::memory_order_relaxed));
        jassert(completedTransaction_.load(std::memory_order_relaxed) == 0);
        inFlightTransaction_.store(transaction, std::memory_order_relaxed);
        uploadInFlight_.store(true, std::memory_order_release);
    }

    void completeUpload(std::uint64_t transaction, bool succeeded) noexcept
    {
        jassert(transaction != 0);
        jassert(inFlightTransaction_.load(std::memory_order_relaxed) == transaction);
        completionSucceeded_.store(succeeded, std::memory_order_relaxed);
        completedTransaction_.store(transaction, std::memory_order_release);
        inFlightTransaction_.store(0, std::memory_order_relaxed);

        // The release of the gate follows completion publication. A callback
        // which observes the open gate with acquire semantics must therefore
        // also observe and consume that result before starting another upload.
        uploadInFlight_.store(false, std::memory_order_release);
    }

    [[nodiscard]] std::optional<SpectrogramUploadCompletion> consumeCompletion() noexcept
    {
        if (isUploadInFlight())
            return std::nullopt;

        const auto transaction = completedTransaction_.exchange(0, std::memory_order_acq_rel);
        if (transaction == 0)
            return std::nullopt;

        return SpectrogramUploadCompletion { transaction,
            completionSucceeded_.load(std::memory_order_acquire) };
    }

private:
    std::atomic<bool> uploadInFlight_ { false };
    std::atomic<std::uint64_t> inFlightTransaction_ { 0 };
    std::atomic<std::uint64_t> completedTransaction_ { 0 };
    std::atomic<bool> completionSucceeded_ { false };
};

enum class SpectrogramUploadResolution : std::uint8_t {
    none,
    promote,
    clearHistory,
    stale,
};

/** Main-thread record of texture destinations masked by later frames until completion. */
class PendingSpectrogramUpload final {
public:
    void begin(std::uint64_t transaction, std::uint64_t historyRevision,
        std::uint64_t textureRevision, const std::uint32_t* destinations,
        std::size_t destinationCount) noexcept;
    [[nodiscard]] SpectrogramUploadResolution resolve(
        const std::optional<SpectrogramUploadCompletion>& completion,
        std::uint64_t currentHistoryRevision, std::uint64_t currentTextureRevision) noexcept;
    [[nodiscard]] bool isActive() const noexcept
    {
        return transaction_ != 0;
    }
    [[nodiscard]] const std::uint32_t* destinations() const noexcept
    {
        return destinations_.data();
    }
    [[nodiscard]] std::size_t destinationCount() const noexcept
    {
        return destinationCount_;
    }

private:
    std::array<std::uint32_t, maximumSpectrogramColumnsDrainedPerFrame> destinations_ { };
    std::size_t destinationCount_ = 0;
    std::uint64_t transaction_ = 0;
    std::uint64_t historyRevision_ = 0;
    std::uint64_t textureRevision_ = 0;
};

[[nodiscard]] std::uint32_t calculateSpectrogramHistoryColumnCount(
    int historyDurationSeconds, int requestedSliceRateHz) noexcept;
[[nodiscard]] std::uint64_t calculateSpectrogramTimelineSlot(
    std::uint64_t capturedFrameEnd, double sampleRate, std::uint32_t requestedSliceRateHz) noexcept;
[[nodiscard]] SpectrogramHistoryTransition spectrogramHistoryTransition(
    const std::optional<SpectrogramHistorySignature>& previous,
    const SpectrogramHistorySignature& next) noexcept;
[[nodiscard]] float spectrogramPaletteCoordinate(
    float decibels, float floorDecibels, float ceilingDecibels, float response) noexcept;
[[nodiscard]] SpectrogramPaletteColour spectrogramPaletteColour(
    SpectrogramRenderPalette palette, float coordinate) noexcept;
[[nodiscard]] float spectrogramPerceivedLuminance(const SpectrogramPaletteColour& colour) noexcept;
/** Converts the top-origin quad coordinate used by Metal vertices to low-to-high row order. */
[[nodiscard]] float spectrogramFrequencyCoordinate(float topOriginCoordinate) noexcept;
[[nodiscard]] float spectrogramLogicalPixelWidth(float backingScale) noexcept;
void copySpectrogramRenderValidity(const SpectrogramHistoryRing& ring, std::uint8_t* destination,
    std::size_t destinationSize, const std::uint32_t* maskedColumns, std::size_t maskedColumnCount,
    bool forceBlack) noexcept;
/** Maps a normalized Scroll-mode pixel to the shifted logical history column. */
[[nodiscard]] std::optional<std::uint32_t> spectrogramScrollSourceColumn(
    float normalizedHorizontalCoordinate, std::uint32_t historyColumnCount,
    float headOffsetColumns) noexcept;

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
inline constexpr float loudnessMinimumLufs = -60.0F;
inline constexpr float loudnessMaximumLufs = 0.0F;
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
inline constexpr std::size_t maximumLoudnessReadoutGlyphs = 6;
inline constexpr int minimumCachedLoudnessReadoutTenths = -9'999;
inline constexpr int maximumCachedLoudnessReadoutTenths = 9'999;

inline constexpr float stereoFieldHistorySeconds = 0.250F;
inline constexpr float stereoCorrelationNeutralThreshold = 0.05F;
inline constexpr std::size_t maximumStereoCorrelationReadoutGlyphs = 5;

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

struct LoudnessReadout final {
    enum class Kind : std::uint8_t {
        emDash,
        minusInfinity,
        lufsTenths,
    };

    Kind kind = Kind::emDash;
    int lufsTenths = 0;
};

struct LoudnessPanelLayout final {
    PeakRmsLogicalRect trackBounds;
    PeakRmsLogicalRect resetVisualBounds;
    PeakRmsLogicalRect resetHitBounds;
    PeakRmsLogicalRect momentaryTextBounds;
    PeakRmsLogicalRect shortTermTextBounds;
    PeakRmsLogicalRect integratedTextBounds;
    bool showMomentaryText = false;
    bool showSecondaryText = false;
};

struct SpectrumClearLayout final {
    PeakRmsLogicalRect visualBounds;
    PeakRmsLogicalRect hitBounds;
};

struct PeakRmsTickLabelSelection final {
    std::array<bool, peakRmsMajorDecibelTicks.size()> visible { };
};

enum class StereoCorrelationColourRange : std::uint8_t {
    cyan,
    neutral,
    amber,
};

struct StereoCorrelationReadout final {
    int hundredths = 0;
    StereoCorrelationColourRange colourRange = StereoCorrelationColourRange::neutral;
    bool available = false;
};

struct StereoFieldPanelLayout final {
    PeakRmsLogicalRect scopeBounds;
    PeakRmsLogicalRect correlationTrackBounds;
    float correlationReadoutBottom = 0.0F;
    bool showCorrelationReadout = false;
};

/** The shared Spectrum/Spectrogram frequency-coordinate transform. */
struct FrequencyAxisMapping final {
    float minimumFrequencyHz = 20.0F;
    float maximumFrequencyHz = 20'000.0F;
    float spacing = 1.0F;
};

enum class FrequencyAxisOrientation : std::uint8_t {
    horizontal,
    vertical,
};

/** Measured, scale-adjusted bounds of the monospaced axis glyphs. */
struct FrequencyAxisLabelMetrics final {
    float glyphAdvance = 0.0F;
    float glyphWidth = 0.0F;
    float glyphHeight = 0.0F;
};

struct FrequencyAxisTickSelection final {
    struct Tick final {
        float frequencyHz = 0.0F;
        std::uint32_t labelFrequencyHz = 0;
    };

    std::array<Tick, maximumFrequencyAxisTickCount> ticks { };
    std::size_t count = 0;
    std::size_t generatedCandidateCount = 0;
    bool candidateCapacityExceeded = false;
};

struct SpectrumDecibelTicks final {
    std::array<int, cachedDecibelTickCount> values { };
    std::size_t count = 0;
    int candidateStep = spectrumDecibelTickSteps.back();
    int displayedStep = spectrumDecibelTickSteps.back();
};

struct SpectrumFrequencyInterpolation final {
    std::size_t lowerBin = 0;
    std::size_t upperBin = 0;
    float upperBinWeight = 0.0F;
    bool valid = false;
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
    Generates and selects bounded nice-label candidates without overlap.

    Exact endpoints are attempted first, followed by 1/2/5 anchors. If both
    endpoints cannot coexist, the lower endpoint remains. Remaining candidates
    are generated from the current mapped length and measured glyph bounds,
    then admitted into the largest available gaps. No allocation occurs.
*/
[[nodiscard]] FrequencyAxisTickSelection selectFrequencyAxisTicks(
    const FrequencyAxisMapping& mapping, float axisLength,
    const FrequencyAxisLabelMetrics& labelMetrics, FrequencyAxisOrientation orientation,
    float minimumGap = 4.0F) noexcept;

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
    Computes the fixed-scale Stereo field square and integrated correlation strip.

    All output is in bottom-left-origin logical points. The scope remains square,
    and optional text is removed before either primary visualization is collapsed.
*/
[[nodiscard]] StereoFieldPanelLayout calculateStereoFieldPanelLayout(
    float panelWidth, float panelHeight, float headerHeight, float textHeight) noexcept;

/** Maps one fixed full-scale coordinate without per-frame normalization or clipping. */
[[nodiscard]] float mapStereoFieldCoordinate(
    float coordinate, float minimumPosition, float maximumPosition) noexcept;

/** Maps correlation from -1..+1 to the fixed horizontal strip. */
[[nodiscard]] float mapStereoCorrelationToUnit(float correlation) noexcept;

/** Classifies and rounds the bounded two-decimal correlation readout. */
[[nodiscard]] StereoCorrelationReadout classifyStereoCorrelationReadout(
    float correlation, bool valid) noexcept;

/** Linear 250 ms point-age fade, including display time since snapshot acceptance. */
[[nodiscard]] float stereoFieldPointAgeOpacity(
    float normalizedAge, double elapsedSinceSnapshotSeconds) noexcept;

/**
    Computes the bounded local-point geometry for the Peak/RMS panel.

    The result uses a bottom-left origin and never depends on drawable pixels,
    so the same layout drives rendering, hit testing, Retina, and regular-density
    displays.
*/
[[nodiscard]] PeakRmsPanelLayout calculatePeakRmsPanelLayout(float panelWidth, float panelHeight,
    float headerHeight, std::uint32_t channelCount, float textHeight, float maximumTickLabelWidth,
    float maximumReadoutWidth) noexcept;

/** Maps the accepted fixed Loudness scale to [0, 1] for bar geometry only. */
[[nodiscard]] float mapLoudnessLufsToUnit(float lufs) noexcept;

/** Separates readiness and completed silence from bounded one-decimal presentation. */
[[nodiscard]] LoudnessReadout classifyLoudnessReadout(double lufs, bool valid) noexcept;

/** Formats one classified Loudness reading for native accessibility clients. */
[[nodiscard]] juce::String formatLoudnessAccessibilityReading(double lufs, bool valid);

/**
    Computes one responsive, bottom-left-origin Loudness tile in logical points.

    The same RESET bounds drive drawing and hit testing on regular-density and
    Retina displays. Optional readout rows disappear before the slim meter is
    collapsed.
*/
[[nodiscard]] LoudnessPanelLayout calculateLoudnessPanelLayout(float panelWidth, float panelHeight,
    float headerHeight, float textHeight, float maximumReadoutWidth) noexcept;

/** Computes the shared Spectrum CLEAR drawing and pointer bounds in logical points. */
[[nodiscard]] SpectrumClearLayout calculateSpectrumClearLayout(
    float panelWidth, float panelHeight, float headerHeight) noexcept;

/** Presentation-only slope adjustment referenced to 1 kHz. */
[[nodiscard]] float spectrumSlopeCompensationDecibels(
    float frequencyHz, float slopeDecibelsPerOctave) noexcept;

/** Locates the FFT-bin centres bracketing one visible Spectrum frequency. */
[[nodiscard]] SpectrumFrequencyInterpolation spectrumFrequencyInterpolation(
    double frequencyHz, double binFrequencyHz, std::size_t binCount) noexcept;

/** Interpolates two calibrated dB values as linear power. */
[[nodiscard]] float interpolateSpectrumPowerDecibels(
    float lowerDecibels, float upperDecibels, float upperBinWeight) noexcept;

/** Preserves calibrated analyzer dB until final presentation-range clipping. */
[[nodiscard]] float sanitiseSpectrumAnalysisDecibels(float decibels) noexcept;

/** Converts one normalized sRGB component to the linear value expected by Metal blending. */
[[nodiscard]] float srgbComponentToLinear(float component) noexcept;

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
    static constexpr std::size_t vertexCapacity = 65'536;
    static constexpr std::size_t maximumShellVertices = dashboardPanelCount * 36;
    static constexpr std::size_t maximumDashboardSplitterVertices = dashboardSplitterCount * 6;
    static constexpr std::size_t maximumGridVertices
        = ((2 * maximumFrequencyAxisTickCount) + cachedDecibelTickCount) * 6;
    // Fill, held trace, and live trace each require two vertices per FFT bin;
    // the Spectrum CLEAR target adds one fill quad plus its four border quads.
    static constexpr std::size_t maximumSpectrumVertices = (6 * maximumSpectrumBinCount) + (5 * 6);
    static constexpr std::size_t maximumSpectrogramVertices = 4 + 6;
    // Eight scale ticks, one five-quad CLEAR target, and four quads (track,
    // RMS, live sample peak, held sample peak) for each of two channels.
    static constexpr std::size_t maximumMeterVertices
        = (peakRmsMajorDecibelTicks.size() + 5 + (2 * 4)) * 6;
    // Two scope axes, four diamond edges, five correlation ticks, and
    // track/fill/marker geometry.
    // Point sprites live in a separate fixed-capacity instance buffer.
    static constexpr std::size_t maximumStereoVertices = (6 + 5 + 3) * 6;
    // Track plus border, Momentary fill, Short-term marker, reference line,
    // and the five-quad RESET target.
    static constexpr std::size_t maximumLoudnessVertices = (5 + 1 + 1 + 1 + 5) * 6;
    static constexpr std::size_t maximumFixedTextGlyphs = 61;
    static constexpr std::size_t maximumDecibelLabelGlyphs = 7;
    // CLEAR, two channel labels, two OVER labels, two six-glyph readouts,
    // and every fixed scale label.
    static constexpr std::size_t maximumPeakRmsTextGlyphs = 5 + 2 + 8 + 12 + 20;
    static constexpr std::size_t maximumSpectrumControlTextGlyphs = 5;
    static constexpr std::size_t maximumStereoTextGlyphs = maximumStereoCorrelationReadoutGlyphs;
    // RESET, M/S/I labels, and three six-glyph one-decimal readings.
    static constexpr std::size_t maximumLoudnessTextGlyphs
        = 5 + 3 + (3 * maximumLoudnessReadoutGlyphs);
    static constexpr std::size_t maximumNumericAxisTextGlyphs
        = (2 * maximumFrequencyAxisTickCount * maximumFrequencyAxisLabelGlyphs)
        + (cachedDecibelTickCount * maximumDecibelLabelGlyphs);
    static constexpr std::size_t maximumTextVertices
        = (maximumFixedTextGlyphs + maximumNumericAxisTextGlyphs + maximumPeakRmsTextGlyphs
              + maximumSpectrumControlTextGlyphs + maximumStereoTextGlyphs
              + maximumLoudnessTextGlyphs)
        * 6;
    static constexpr std::size_t maximumGeneratedVertices = maximumShellVertices
        + maximumDashboardSplitterVertices + maximumGridVertices + maximumSpectrumVertices
        + maximumSpectrogramVertices + maximumMeterVertices + maximumStereoVertices
        + maximumLoudnessVertices + maximumTextVertices;
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
    std::uint64_t spectrogramColumnsRead = 0;
    std::uint64_t spectrogramColumnsUploaded = 0;
    std::uint64_t spectrogramColumnsRejected = 0;
    std::uint64_t spectrogramGapColumns = 0;
    std::uint64_t spectrogramHistoryClears = 0;
    std::uint64_t spectrogramTextureReallocations = 0;
    std::uint64_t spectrogramTextureAllocationFailures = 0;
    std::uint64_t spectrogramUploadBackpressureDrops = 0;
    std::uint64_t spectrogramUploadDeferrals = 0;
    std::uint64_t spectrogramScrollClockInitializations = 0;
    std::uint64_t spectrogramScrollUnderrunFrames = 0;
    // One command per encoded column copy; completion success is counted
    // separately by spectrogramColumnsUploaded.
    std::uint64_t spectrogramUploadCommands = 0;
    std::uint64_t spectrogramUploadBytes = 0;
    std::uint64_t spectrogramLastColumnSequence = 0;
    std::uint64_t lastStereoSequence = 0;
    std::uint64_t stereoPointInstancesPrepared = 0;
    std::uint64_t stereoPointDrawCalls = 0;
    std::uint64_t lastLoudnessSequence = 0;
    std::uint64_t loudnessMeasurementCapturedFrameEnd = 0;
    std::uint64_t loudnessIntegratedCapturedFrameEnd = 0;

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
    // Reported by the active NSScreen; actual cadence remains timestamp-derived.
    std::uint32_t configuredMaximumFramesPerSecond = 0;
    std::uint32_t requestedMinimumFramesPerSecond = 0;
    std::uint32_t requestedPreferredFramesPerSecond = 0;
    std::uint32_t requestedMaximumFramesPerSecond = 0;
    MetalDisplayFramePacing configuredDisplayFramePacing = MetalDisplayFramePacing::fixedMaximum;
    std::uint32_t spectrogramTextureRows = 0;
    std::uint32_t spectrogramTextureColumns = 0;
    std::uint64_t spectrogramTextureBytes = 0;
    std::uint32_t stereoLastPointCount = 0;
    double backingScale = 1.0;
    double spectrogramScrollHeadOffsetColumns = 0.0;
    double stereoCorrelation = 0.0;
    double loudnessMomentaryLufs = 0.0;
    double loudnessShortTermLufs = 0.0;
    double loudnessIntegratedLufs = 0.0;
    double loudnessReferenceLufs = -23.0;

    bool metalAvailable = false;
    bool renderingRequested = false;
    bool effectivelyRendering = false;
    bool resetPending = false;
    bool stereoCorrelationValid = false;
    bool stereoMono = false;
    bool loudnessMomentaryValid = false;
    bool loudnessShortTermValid = false;
    bool loudnessIntegratedValid = false;
};

/** Coherently packed, presentation-only Spectrum settings. */
struct SpectrumRenderSettings {
    float floorDecibels = -90.0F;
    float ceilingDecibels = 0.0F;
    float slopeDecibelsPerOctave = 0.0F;
    float frequencySpacing = 1.0F;
    float fillOpacity = 0.18F;
    std::uint32_t traceColourRgb = 0x55c7e8U;
};

/** Coherently packed presentation and bounded-history settings for Spectrogram. */
struct SpectrogramRenderSettings {
    detail::SpectrogramRenderPalette palette = detail::SpectrogramRenderPalette::blueFire;
    float colorResponse = 0.0F;
    float colorFloorDecibels = -120.0F;
    float colorCeilingDecibels = 0.0F;
    int historyDurationSeconds = 10;
    detail::SpectrogramRenderHistoryMode historyMode = detail::SpectrogramRenderHistoryMode::scroll;
    int requestedSliceRateHz = 60;
};

/** Coherently packed presentation-only Loudness settings. */
struct LoudnessRenderSettings {
    float referenceLufs = -23.0F;
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
        Selects the best-effort Core Animation frame-rate request. This is a
        message-thread presentation setting and does not reset analyzer state.
    */
    void setDisplayFramePacing(MetalDisplayFramePacing framePacing) noexcept;
    [[nodiscard]] MetalDisplayFramePacing getDisplayFramePacing() const noexcept;

    /**
        Updates presentation settings without requiring the message thread.
        Analyzer temporal averaging is worker-owned and is not part of this
        renderer snapshot.
    */
    void setSpectrumSettings(SpectrumRenderSettings settings) noexcept;
    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept;

    /** Updates Spectrogram presentation/history settings as one coherent snapshot. */
    void setSpectrogramSettings(SpectrogramRenderSettings settings) noexcept;
    [[nodiscard]] SpectrogramRenderSettings getSpectrogramSettings() const noexcept;

    /** Updates the presentation-only Loudness reference as one coherent snapshot. */
    void setLoudnessSettings(LoudnessRenderSettings settings) noexcept;
    [[nodiscard]] LoudnessRenderSettings getLoudnessSettings() const noexcept;

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
