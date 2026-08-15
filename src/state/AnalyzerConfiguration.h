// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/SpectrumAnalysisConfiguration.h"
#include "core/SpectrumTemporalConfiguration.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>

namespace audio_insight {
/** A renderer-independent sRGB trace colour with no implicit alpha. */
struct SrgbColor final {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    [[nodiscard]] constexpr std::uint32_t packedRgb() const noexcept
    {
        return (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(green) << 8U)
            | static_cast<std::uint32_t>(blue);
    }

    [[nodiscard]] static constexpr SrgbColor fromPackedRgb(const std::uint32_t rgb) noexcept
    {
        return { static_cast<std::uint8_t>((rgb >> 16U) & 0xffU),
            static_cast<std::uint8_t>((rgb >> 8U) & 0xffU),
            static_cast<std::uint8_t>(rgb & 0xffU) };
    }

    constexpr bool operator==(const SrgbColor&) const noexcept = default;
};

enum class SpectrumSlope {
    flat,
    db3PerOctave,
    db4Point5PerOctave,
    db6PerOctave,
};

enum class SpectrogramPalette {
    blueFire,
    inferno,
    viridis,
    grayscale,
};

enum class SpectrogramHistoryMode {
    scroll,
    overwrite,
};

struct SharedAnalysisSettings final {
    static constexpr int defaultFftSize = 4096;
    static constexpr int defaultFftSliceRateHz = 60;
    static constexpr double minimumFrequencySpacing = 0.0;
    static constexpr double maximumFrequencySpacing = 1.0;
    static constexpr double defaultFrequencySpacing = 1.0;

    int fftSize = defaultFftSize;
    FftWindow window = FftWindow::periodicHann;
    int requestedFftSliceRateHz = defaultFftSliceRateHz;
    double frequencySpacing = defaultFrequencySpacing;
};

struct TemporalAveragingSettings final {
    static constexpr double minimumMilliseconds = 25.0;
    static constexpr double maximumMilliseconds = 2000.0;
    static constexpr double defaultMilliseconds = 75.0;

    bool enabled = true;
    double milliseconds = defaultMilliseconds;
};

struct SpectrumSettings final {
    static constexpr double minimumFloorDb = -180.0;
    static constexpr double maximumFloorDb = -36.0;
    static constexpr double defaultFloorDb = -90.0;
    static constexpr double minimumCeilingDb = -24.0;
    static constexpr double maximumCeilingDb = 12.0;
    static constexpr double defaultCeilingDb = 0.0;
    static constexpr double minimumVisibleSpanDb = 24.0;
    static constexpr double minimumPeakHoldSeconds = 0.25;
    static constexpr double maximumPeakHoldSeconds = 10.0;
    static constexpr double defaultFinitePeakHoldSeconds = 2.0;
    static constexpr double minimumFillOpacity = 0.0;
    static constexpr double maximumFillOpacity = 0.5;
    static constexpr double defaultFillOpacity = 0.18;
    static constexpr SrgbColor defaultTraceColor { 0x55U, 0xc7U, 0xe8U };

    double floorDb = defaultFloorDb;
    double ceilingDb = defaultCeilingDb;
    TemporalAveragingSettings temporalAveraging;
    SpectrumSlope slope = SpectrumSlope::flat;
    SpectrumPeakHoldMode peakHoldMode = SpectrumPeakHoldMode::off;
    double finitePeakHoldSeconds = defaultFinitePeakHoldSeconds;
    SrgbColor traceColor = defaultTraceColor;
    double fillOpacity = defaultFillOpacity;
};

struct SpectrogramSettings final {
    static constexpr double minimumColorResponse = -2.0;
    static constexpr double maximumColorResponse = 2.0;
    static constexpr double defaultColorResponse = 0.0;
    static constexpr double minimumColorFloorDb = -180.0;
    static constexpr double maximumColorFloorDb = -36.0;
    static constexpr double defaultColorFloorDb = -120.0;
    static constexpr double minimumColorCeilingDb = -24.0;
    static constexpr double maximumColorCeilingDb = 12.0;
    static constexpr double defaultColorCeilingDb = 0.0;
    static constexpr double minimumColorSpanDb = 24.0;
    static constexpr int defaultHistoryDurationSeconds = 10;

    SpectrogramPalette palette = SpectrogramPalette::blueFire;
    double colorResponse = defaultColorResponse;
    double colorFloorDb = defaultColorFloorDb;
    double colorCeilingDb = defaultColorCeilingDb;
    int historyDurationSeconds = defaultHistoryDurationSeconds;
    SpectrogramHistoryMode historyMode = SpectrogramHistoryMode::scroll;
};

struct LoudnessSettings final {
    static constexpr double minimumReferenceLufs = -36.0;
    static constexpr double maximumReferenceLufs = -9.0;
    static constexpr double defaultReferenceLufs = -23.0;

    double referenceLufs = defaultReferenceLufs;
};

/**
    Complete non-automatable analyzer configuration for one plugin instance.

    This is a renderer- and JUCE-state-independent value after construction. A
    sanitized value can later be published by value or behind a const shared
    pointer as an immutable render/analysis configuration snapshot.

    Peak/RMS and Stereo do not appear here because the accepted initial design
    gives those panels no persistent adjustable settings.
*/
struct AnalyzerConfiguration final {
    SharedAnalysisSettings sharedAnalysis;
    SpectrumSettings spectrum;
    SpectrogramSettings spectrogram;
    LoudnessSettings loudness;
};

using AnalyzerConfigurationSnapshot = AnalyzerConfiguration;

/** Values read from the pre-configuration Spectrum compatibility shims. */
struct LegacySpectrumSettings final {
    std::optional<double> floorDb;
    std::optional<double> ceilingDb;
    std::optional<double> normalizedSmoothing;
};

/**
    Strict versioned ValueTree boundary for per-instance analyzer state.

    This codec may allocate and interact with reference-counted ValueTrees. It
    must not be called from the audio callback. Decode accepts only its exact
    tree type, current integer schema version, known child types, and known
    properties. Within a recognized schema, missing or malformed setting values
    fall back to their documented defaults and numeric values are sanitized.
*/
class AnalyzerConfigurationCodec final {
public:
    static constexpr int schemaVersion = 1;

    [[nodiscard]] static const juce::Identifier& treeType();
    [[nodiscard]] static AnalyzerConfiguration defaults() noexcept;
    [[nodiscard]] static AnalyzerConfiguration sanitize(
        AnalyzerConfiguration configuration) noexcept;

    [[nodiscard]] static juce::ValueTree encode(const AnalyzerConfiguration& configuration);

    /**
        Decodes a recognized current-schema tree.

        A disengaged result means that the tree type, version, or structural
        shape is not this schema. A present but malformed current subtree should
        therefore be passed to decodeOrDefault rather than treated as legacy;
        legacy migration is only for state with no analyzer subtree at all.
    */
    [[nodiscard]] static std::optional<AnalyzerConfiguration> decode(const juce::ValueTree& tree);
    [[nodiscard]] static AnalyzerConfiguration decodeOrDefault(const juce::ValueTree& tree);

    [[nodiscard]] static TemporalAveragingSettings migrateLegacySmoothing(
        double normalizedSmoothing) noexcept;
    [[nodiscard]] static AnalyzerConfiguration migrateLegacy(
        const LegacySpectrumSettings& legacy) noexcept;
};
} // namespace audio_insight
