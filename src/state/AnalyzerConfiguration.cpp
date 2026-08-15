// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AnalyzerConfiguration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace audio_insight {
namespace {
const juce::Identifier versionProperty { "version" };

const juce::Identifier sharedAnalysisType { "SharedAnalysis" };
const juce::Identifier fftSizeProperty { "fftSize" };
const juce::Identifier windowProperty { "window" };
const juce::Identifier fftSliceRateProperty { "requestedFftSliceRateHz" };
const juce::Identifier frequencySpacingProperty { "frequencySpacing" };

const juce::Identifier spectrumType { "Spectrum" };
const juce::Identifier spectrumFloorProperty { "floorDb" };
const juce::Identifier spectrumCeilingProperty { "ceilingDb" };
const juce::Identifier temporalAveragingEnabledProperty { "temporalAveragingEnabled" };
const juce::Identifier temporalAveragingMillisecondsProperty { "temporalAveragingMilliseconds" };
const juce::Identifier slopeProperty { "slope" };
const juce::Identifier peakHoldModeProperty { "peakHoldMode" };
const juce::Identifier finitePeakHoldSecondsProperty { "finitePeakHoldSeconds" };
const juce::Identifier traceColorProperty { "traceColorSrgb" };
const juce::Identifier fillOpacityProperty { "fillOpacity" };

const juce::Identifier spectrogramType { "Spectrogram" };
const juce::Identifier paletteProperty { "palette" };
const juce::Identifier colorResponseProperty { "colorResponse" };
const juce::Identifier colorFloorProperty { "colorFloorDb" };
const juce::Identifier colorCeilingProperty { "colorCeilingDb" };
const juce::Identifier historyDurationProperty { "historyDurationSeconds" };
const juce::Identifier historyModeProperty { "historyMode" };

const juce::Identifier loudnessType { "Loudness" };
const juce::Identifier loudnessReferenceProperty { "referenceLufs" };

const std::array rootProperties { versionProperty };
const std::array sharedAnalysisProperties {
    fftSizeProperty,
    windowProperty,
    fftSliceRateProperty,
    frequencySpacingProperty,
};
const std::array spectrumProperties {
    spectrumFloorProperty,
    spectrumCeilingProperty,
    temporalAveragingEnabledProperty,
    temporalAveragingMillisecondsProperty,
    slopeProperty,
    peakHoldModeProperty,
    finitePeakHoldSecondsProperty,
    traceColorProperty,
    fillOpacityProperty,
};
const std::array spectrogramProperties {
    paletteProperty,
    colorResponseProperty,
    colorFloorProperty,
    colorCeilingProperty,
    historyDurationProperty,
    historyModeProperty,
};
const std::array loudnessProperties { loudnessReferenceProperty };

template <std::size_t propertyCount>
bool hasOnlyKnownProperties(
    const juce::ValueTree& tree, const std::array<juce::Identifier, propertyCount>& allowed)
{
    for (auto index = 0; index < tree.getNumProperties(); ++index) {
        const auto property = tree.getPropertyName(index);
        if (std::find(allowed.begin(), allowed.end(), property) == allowed.end())
            return false;
    }

    return true;
}

bool hasRecognizedShape(const juce::ValueTree& tree)
{
    if (!hasOnlyKnownProperties(tree, rootProperties))
        return false;

    auto sharedAnalysisCount = 0;
    auto spectrumCount = 0;
    auto spectrogramCount = 0;
    auto loudnessCount = 0;

    for (const auto& child : tree) {
        if (child.getNumChildren() != 0)
            return false;

        if (child.hasType(sharedAnalysisType)) {
            if (++sharedAnalysisCount > 1
                || !hasOnlyKnownProperties(child, sharedAnalysisProperties)) {
                return false;
            }
        } else if (child.hasType(spectrumType)) {
            if (++spectrumCount > 1 || !hasOnlyKnownProperties(child, spectrumProperties))
                return false;
        } else if (child.hasType(spectrogramType)) {
            if (++spectrogramCount > 1 || !hasOnlyKnownProperties(child, spectrogramProperties))
                return false;
        } else if (child.hasType(loudnessType)) {
            if (++loudnessCount > 1 || !hasOnlyKnownProperties(child, loudnessProperties))
                return false;
        } else {
            return false;
        }
    }

    return true;
}

std::optional<int> readInteger(const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (!tree.hasProperty(property))
        return std::nullopt;

    const auto value = tree.getProperty(property);
    juce::int64 integer = 0;
    if (value.isInt() || value.isInt64()) {
        integer = static_cast<juce::int64>(value);
    } else if (value.isString()) {
        const auto text = value.toString();
        integer = text.getLargeIntValue();
        if (text != juce::String(integer))
            return std::nullopt;
    } else {
        return std::nullopt;
    }

    if (integer < std::numeric_limits<int>::min() || integer > std::numeric_limits<int>::max())
        return std::nullopt;

    return static_cast<int>(integer);
}

std::optional<double> readNumber(const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (!tree.hasProperty(property))
        return std::nullopt;

    const auto value = tree.getProperty(property);
    auto number = 0.0;
    if (value.isInt() || value.isInt64() || value.isDouble()) {
        number = static_cast<double>(value);
    } else if (value.isString()) {
        const auto text = value.toString();
        if (text.isEmpty() || text != text.trim() || !text.containsAnyOf("0123456789"))
            return std::nullopt;

        auto cursor = text.getCharPointer();
        const auto start = cursor;
        number = juce::CharacterFunctions::readDoubleValue(cursor);
        if (cursor == start || !cursor.isEmpty())
            return std::nullopt;
    } else {
        return std::nullopt;
    }

    if (!std::isfinite(number))
        return std::nullopt;

    return number;
}

std::optional<bool> readBoolean(const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (!tree.hasProperty(property))
        return std::nullopt;

    const auto value = tree.getProperty(property);
    if (value.isBool())
        return static_cast<bool>(value);

    if (value.isString()) {
        const auto text = value.toString();
        if (text == "true")
            return true;
        if (text == "false")
            return false;
    }

    return std::nullopt;
}

std::optional<juce::String> readString(
    const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (!tree.hasProperty(property))
        return std::nullopt;

    const auto value = tree.getProperty(property);
    if (!value.isString())
        return std::nullopt;

    return value.toString();
}

juce::String encodeNumber(const double value)
{
    auto encoded = juce::String(value, 12);
    while (encoded.containsChar('.') && encoded.endsWithChar('0'))
        encoded = encoded.dropLastCharacters(1);
    if (encoded.endsWithChar('.'))
        encoded = encoded.dropLastCharacters(1);
    return encoded == "-0" ? juce::String("0") : encoded;
}

double clampFinite(
    const double value, const double minimum, const double maximum, const double fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

double quantize(const double value, const double step) noexcept
{
    return std::round(value / step) * step;
}

double sanitizeStepped(const double value, const double minimum, const double maximum,
    const double step, const double fallback) noexcept
{
    return std::clamp(
        quantize(clampFinite(value, minimum, maximum, fallback), step), minimum, maximum);
}

template <std::size_t choiceCount>
int sanitizeChoice(
    const int value, const std::array<int, choiceCount>& choices, const int fallback) noexcept
{
    return std::find(choices.begin(), choices.end(), value) != choices.end() ? value : fallback;
}

FftWindow sanitizeWindow(const FftWindow window) noexcept
{
    switch (window) {
    case FftWindow::rectangular:
    case FftWindow::periodicHann:
    case FftWindow::fourTermBlackmanHarris:
    case FftWindow::fiveTermFlatTop:
        return window;
    }

    return FftWindow::periodicHann;
}

SpectrumSlope sanitizeSlope(const SpectrumSlope slope) noexcept
{
    switch (slope) {
    case SpectrumSlope::flat:
    case SpectrumSlope::db3PerOctave:
    case SpectrumSlope::db4Point5PerOctave:
    case SpectrumSlope::db6PerOctave:
        return slope;
    }

    return SpectrumSlope::flat;
}

SpectrumPeakHoldMode sanitizePeakHoldMode(const SpectrumPeakHoldMode mode) noexcept
{
    switch (mode) {
    case SpectrumPeakHoldMode::off:
    case SpectrumPeakHoldMode::finite:
    case SpectrumPeakHoldMode::infinite:
        return mode;
    }

    return SpectrumPeakHoldMode::off;
}

SpectrogramPalette sanitizePalette(const SpectrogramPalette palette) noexcept
{
    switch (palette) {
    case SpectrogramPalette::blueFire:
    case SpectrogramPalette::inferno:
    case SpectrogramPalette::viridis:
    case SpectrogramPalette::grayscale:
        return palette;
    }

    return SpectrogramPalette::blueFire;
}

SpectrogramHistoryMode sanitizeHistoryMode(const SpectrogramHistoryMode mode) noexcept
{
    switch (mode) {
    case SpectrogramHistoryMode::scroll:
    case SpectrogramHistoryMode::overwrite:
        return mode;
    }

    return SpectrogramHistoryMode::scroll;
}

const char* tokenForWindow(const FftWindow window) noexcept
{
    switch (window) {
    case FftWindow::rectangular:
        return "rectangular";
    case FftWindow::periodicHann:
        return "periodicHann";
    case FftWindow::fourTermBlackmanHarris:
        return "fourTermBlackmanHarris";
    case FftWindow::fiveTermFlatTop:
        return "fiveTermFlatTop";
    }

    return "periodicHann";
}

std::optional<FftWindow> parseWindow(const juce::String& token)
{
    if (token == "rectangular")
        return FftWindow::rectangular;
    if (token == "periodicHann")
        return FftWindow::periodicHann;
    if (token == "fourTermBlackmanHarris")
        return FftWindow::fourTermBlackmanHarris;
    if (token == "fiveTermFlatTop")
        return FftWindow::fiveTermFlatTop;
    return std::nullopt;
}

const char* tokenForSlope(const SpectrumSlope slope) noexcept
{
    switch (slope) {
    case SpectrumSlope::flat:
        return "flat";
    case SpectrumSlope::db3PerOctave:
        return "db3PerOctave";
    case SpectrumSlope::db4Point5PerOctave:
        return "db4Point5PerOctave";
    case SpectrumSlope::db6PerOctave:
        return "db6PerOctave";
    }

    return "flat";
}

std::optional<SpectrumSlope> parseSlope(const juce::String& token)
{
    if (token == "flat")
        return SpectrumSlope::flat;
    if (token == "db3PerOctave")
        return SpectrumSlope::db3PerOctave;
    if (token == "db4Point5PerOctave")
        return SpectrumSlope::db4Point5PerOctave;
    if (token == "db6PerOctave")
        return SpectrumSlope::db6PerOctave;
    return std::nullopt;
}

const char* tokenForPeakHoldMode(const SpectrumPeakHoldMode mode) noexcept
{
    switch (mode) {
    case SpectrumPeakHoldMode::off:
        return "off";
    case SpectrumPeakHoldMode::finite:
        return "finite";
    case SpectrumPeakHoldMode::infinite:
        return "infinite";
    }

    return "off";
}

std::optional<SpectrumPeakHoldMode> parsePeakHoldMode(const juce::String& token)
{
    if (token == "off")
        return SpectrumPeakHoldMode::off;
    if (token == "finite")
        return SpectrumPeakHoldMode::finite;
    if (token == "infinite")
        return SpectrumPeakHoldMode::infinite;
    return std::nullopt;
}

const char* tokenForPalette(const SpectrogramPalette palette) noexcept
{
    switch (palette) {
    case SpectrogramPalette::blueFire:
        return "blueFire";
    case SpectrogramPalette::inferno:
        return "inferno";
    case SpectrogramPalette::viridis:
        return "viridis";
    case SpectrogramPalette::grayscale:
        return "grayscale";
    }

    return "blueFire";
}

std::optional<SpectrogramPalette> parsePalette(const juce::String& token)
{
    if (token == "blueFire")
        return SpectrogramPalette::blueFire;
    if (token == "inferno")
        return SpectrogramPalette::inferno;
    if (token == "viridis")
        return SpectrogramPalette::viridis;
    if (token == "grayscale")
        return SpectrogramPalette::grayscale;
    return std::nullopt;
}

const char* tokenForHistoryMode(const SpectrogramHistoryMode mode) noexcept
{
    switch (mode) {
    case SpectrogramHistoryMode::scroll:
        return "scroll";
    case SpectrogramHistoryMode::overwrite:
        return "overwrite";
    }

    return "scroll";
}

std::optional<SpectrogramHistoryMode> parseHistoryMode(const juce::String& token)
{
    if (token == "scroll")
        return SpectrogramHistoryMode::scroll;
    if (token == "overwrite")
        return SpectrogramHistoryMode::overwrite;
    return std::nullopt;
}

void decodeSharedAnalysis(const juce::ValueTree& tree, SharedAnalysisSettings& destination) noexcept
{
    if (const auto value = readInteger(tree, fftSizeProperty))
        destination.fftSize = *value;
    if (const auto value = readString(tree, windowProperty))
        destination.window = parseWindow(*value).value_or(FftWindow::periodicHann);
    if (const auto value = readInteger(tree, fftSliceRateProperty))
        destination.requestedFftSliceRateHz = *value;
    if (const auto value = readNumber(tree, frequencySpacingProperty))
        destination.frequencySpacing = *value;
}

void decodeSpectrum(const juce::ValueTree& tree, SpectrumSettings& destination) noexcept
{
    if (const auto value = readNumber(tree, spectrumFloorProperty))
        destination.floorDb = *value;
    if (const auto value = readNumber(tree, spectrumCeilingProperty))
        destination.ceilingDb = *value;
    if (const auto value = readBoolean(tree, temporalAveragingEnabledProperty))
        destination.temporalAveraging.enabled = *value;
    if (const auto value = readNumber(tree, temporalAveragingMillisecondsProperty))
        destination.temporalAveraging.milliseconds = *value;
    if (const auto value = readString(tree, slopeProperty))
        destination.slope = parseSlope(*value).value_or(SpectrumSlope::flat);
    if (const auto value = readString(tree, peakHoldModeProperty)) {
        destination.peakHoldMode = parsePeakHoldMode(*value).value_or(SpectrumPeakHoldMode::off);
    }
    if (const auto value = readNumber(tree, finitePeakHoldSecondsProperty))
        destination.finitePeakHoldSeconds = *value;
    if (const auto value = readInteger(tree, traceColorProperty)) {
        if (*value >= 0 && *value <= 0x00ffffff)
            destination.traceColor = SrgbColor::fromPackedRgb(static_cast<std::uint32_t>(*value));
    }
    if (const auto value = readNumber(tree, fillOpacityProperty))
        destination.fillOpacity = *value;
}

void decodeSpectrogram(const juce::ValueTree& tree, SpectrogramSettings& destination) noexcept
{
    if (const auto value = readString(tree, paletteProperty))
        destination.palette = parsePalette(*value).value_or(SpectrogramPalette::blueFire);
    if (const auto value = readNumber(tree, colorResponseProperty))
        destination.colorResponse = *value;
    if (const auto value = readNumber(tree, colorFloorProperty))
        destination.colorFloorDb = *value;
    if (const auto value = readNumber(tree, colorCeilingProperty))
        destination.colorCeilingDb = *value;
    if (const auto value = readInteger(tree, historyDurationProperty))
        destination.historyDurationSeconds = *value;
    if (const auto value = readString(tree, historyModeProperty)) {
        destination.historyMode = parseHistoryMode(*value).value_or(SpectrogramHistoryMode::scroll);
    }
}

void decodeLoudness(const juce::ValueTree& tree, LoudnessSettings& destination) noexcept
{
    if (const auto value = readNumber(tree, loudnessReferenceProperty))
        destination.referenceLufs = *value;
}
} // namespace

const juce::Identifier& AnalyzerConfigurationCodec::treeType()
{
    static const juce::Identifier type { "AnalyzerConfiguration" };
    return type;
}

AnalyzerConfiguration AnalyzerConfigurationCodec::defaults() noexcept
{
    return { };
}

AnalyzerConfiguration AnalyzerConfigurationCodec::sanitize(
    AnalyzerConfiguration configuration) noexcept
{
    constexpr std::array fftSizes { 1024, 2048, 4096, 8192, 16384 };
    constexpr std::array fftSliceRates { 15, 30, 60, 120 };
    constexpr std::array historyDurations { 2, 5, 10, 20, 30, 60 };

    auto& shared = configuration.sharedAnalysis;
    shared.fftSize
        = sanitizeChoice(shared.fftSize, fftSizes, SharedAnalysisSettings::defaultFftSize);
    shared.window = sanitizeWindow(shared.window);
    shared.requestedFftSliceRateHz = sanitizeChoice(shared.requestedFftSliceRateHz, fftSliceRates,
        SharedAnalysisSettings::defaultFftSliceRateHz);
    shared.frequencySpacing
        = clampFinite(shared.frequencySpacing, SharedAnalysisSettings::minimumFrequencySpacing,
            SharedAnalysisSettings::maximumFrequencySpacing,
            SharedAnalysisSettings::defaultFrequencySpacing);

    auto& spectrum = configuration.spectrum;
    spectrum.floorDb = sanitizeStepped(spectrum.floorDb, SpectrumSettings::minimumFloorDb,
        SpectrumSettings::maximumFloorDb, 1.0, SpectrumSettings::defaultFloorDb);
    spectrum.ceilingDb = sanitizeStepped(spectrum.ceilingDb, SpectrumSettings::minimumCeilingDb,
        SpectrumSettings::maximumCeilingDb, 1.0, SpectrumSettings::defaultCeilingDb);
    if (spectrum.ceilingDb - spectrum.floorDb < SpectrumSettings::minimumVisibleSpanDb)
        spectrum.floorDb = spectrum.ceilingDb - SpectrumSettings::minimumVisibleSpanDb;

    spectrum.temporalAveraging.milliseconds = clampFinite(spectrum.temporalAveraging.milliseconds,
        TemporalAveragingSettings::minimumMilliseconds,
        TemporalAveragingSettings::maximumMilliseconds,
        TemporalAveragingSettings::defaultMilliseconds);
    spectrum.slope = sanitizeSlope(spectrum.slope);
    spectrum.peakHoldMode = sanitizePeakHoldMode(spectrum.peakHoldMode);
    spectrum.finitePeakHoldSeconds = clampFinite(spectrum.finitePeakHoldSeconds,
        SpectrumSettings::minimumPeakHoldSeconds, SpectrumSettings::maximumPeakHoldSeconds,
        SpectrumSettings::defaultFinitePeakHoldSeconds);
    spectrum.fillOpacity = clampFinite(spectrum.fillOpacity, SpectrumSettings::minimumFillOpacity,
        SpectrumSettings::maximumFillOpacity, SpectrumSettings::defaultFillOpacity);

    auto& spectrogram = configuration.spectrogram;
    spectrogram.palette = sanitizePalette(spectrogram.palette);
    spectrogram.colorResponse
        = clampFinite(spectrogram.colorResponse, SpectrogramSettings::minimumColorResponse,
            SpectrogramSettings::maximumColorResponse, SpectrogramSettings::defaultColorResponse);
    spectrogram.colorFloorDb = sanitizeStepped(spectrogram.colorFloorDb,
        SpectrogramSettings::minimumColorFloorDb, SpectrogramSettings::maximumColorFloorDb, 1.0,
        SpectrogramSettings::defaultColorFloorDb);
    spectrogram.colorCeilingDb = sanitizeStepped(spectrogram.colorCeilingDb,
        SpectrogramSettings::minimumColorCeilingDb, SpectrogramSettings::maximumColorCeilingDb, 1.0,
        SpectrogramSettings::defaultColorCeilingDb);
    if (spectrogram.colorCeilingDb - spectrogram.colorFloorDb
        < SpectrogramSettings::minimumColorSpanDb) {
        spectrogram.colorFloorDb
            = spectrogram.colorCeilingDb - SpectrogramSettings::minimumColorSpanDb;
    }
    spectrogram.historyDurationSeconds = sanitizeChoice(spectrogram.historyDurationSeconds,
        historyDurations, SpectrogramSettings::defaultHistoryDurationSeconds);
    spectrogram.historyMode = sanitizeHistoryMode(spectrogram.historyMode);

    configuration.loudness.referenceLufs = sanitizeStepped(configuration.loudness.referenceLufs,
        LoudnessSettings::minimumReferenceLufs, LoudnessSettings::maximumReferenceLufs, 0.5,
        LoudnessSettings::defaultReferenceLufs);
    return configuration;
}

juce::ValueTree AnalyzerConfigurationCodec::encode(const AnalyzerConfiguration& configuration)
{
    const auto sanitized = sanitize(configuration);
    juce::ValueTree tree(treeType());
    tree.setProperty(versionProperty, juce::String(schemaVersion), nullptr);

    juce::ValueTree shared(sharedAnalysisType);
    shared.setProperty(fftSizeProperty, juce::String(sanitized.sharedAnalysis.fftSize), nullptr);
    shared.setProperty(windowProperty, tokenForWindow(sanitized.sharedAnalysis.window), nullptr);
    shared.setProperty(fftSliceRateProperty,
        juce::String(sanitized.sharedAnalysis.requestedFftSliceRateHz), nullptr);
    shared.setProperty(
        frequencySpacingProperty, encodeNumber(sanitized.sharedAnalysis.frequencySpacing), nullptr);
    tree.addChild(shared, -1, nullptr);

    juce::ValueTree spectrum(spectrumType);
    spectrum.setProperty(spectrumFloorProperty, encodeNumber(sanitized.spectrum.floorDb), nullptr);
    spectrum.setProperty(
        spectrumCeilingProperty, encodeNumber(sanitized.spectrum.ceilingDb), nullptr);
    spectrum.setProperty(temporalAveragingEnabledProperty,
        sanitized.spectrum.temporalAveraging.enabled ? "true" : "false", nullptr);
    spectrum.setProperty(temporalAveragingMillisecondsProperty,
        encodeNumber(sanitized.spectrum.temporalAveraging.milliseconds), nullptr);
    spectrum.setProperty(slopeProperty, tokenForSlope(sanitized.spectrum.slope), nullptr);
    spectrum.setProperty(
        peakHoldModeProperty, tokenForPeakHoldMode(sanitized.spectrum.peakHoldMode), nullptr);
    spectrum.setProperty(finitePeakHoldSecondsProperty,
        encodeNumber(sanitized.spectrum.finitePeakHoldSeconds), nullptr);
    spectrum.setProperty(traceColorProperty,
        juce::String(static_cast<int>(sanitized.spectrum.traceColor.packedRgb())), nullptr);
    spectrum.setProperty(
        fillOpacityProperty, encodeNumber(sanitized.spectrum.fillOpacity), nullptr);
    tree.addChild(spectrum, -1, nullptr);

    juce::ValueTree spectrogram(spectrogramType);
    spectrogram.setProperty(
        paletteProperty, tokenForPalette(sanitized.spectrogram.palette), nullptr);
    spectrogram.setProperty(
        colorResponseProperty, encodeNumber(sanitized.spectrogram.colorResponse), nullptr);
    spectrogram.setProperty(
        colorFloorProperty, encodeNumber(sanitized.spectrogram.colorFloorDb), nullptr);
    spectrogram.setProperty(
        colorCeilingProperty, encodeNumber(sanitized.spectrogram.colorCeilingDb), nullptr);
    spectrogram.setProperty(historyDurationProperty,
        juce::String(sanitized.spectrogram.historyDurationSeconds), nullptr);
    spectrogram.setProperty(
        historyModeProperty, tokenForHistoryMode(sanitized.spectrogram.historyMode), nullptr);
    tree.addChild(spectrogram, -1, nullptr);

    juce::ValueTree loudness(loudnessType);
    loudness.setProperty(
        loudnessReferenceProperty, encodeNumber(sanitized.loudness.referenceLufs), nullptr);
    tree.addChild(loudness, -1, nullptr);
    return tree;
}

std::optional<AnalyzerConfiguration> AnalyzerConfigurationCodec::decode(const juce::ValueTree& tree)
{
    if (!tree.isValid() || !tree.hasType(treeType()) || !hasRecognizedShape(tree))
        return std::nullopt;

    const auto version = readInteger(tree, versionProperty);
    if (!version.has_value() || *version != schemaVersion)
        return std::nullopt;

    auto configuration = defaults();
    if (const auto shared = tree.getChildWithName(sharedAnalysisType); shared.isValid())
        decodeSharedAnalysis(shared, configuration.sharedAnalysis);
    if (const auto spectrum = tree.getChildWithName(spectrumType); spectrum.isValid())
        decodeSpectrum(spectrum, configuration.spectrum);
    if (const auto spectrogram = tree.getChildWithName(spectrogramType); spectrogram.isValid())
        decodeSpectrogram(spectrogram, configuration.spectrogram);
    if (const auto loudness = tree.getChildWithName(loudnessType); loudness.isValid())
        decodeLoudness(loudness, configuration.loudness);
    return sanitize(configuration);
}

AnalyzerConfiguration AnalyzerConfigurationCodec::decodeOrDefault(const juce::ValueTree& tree)
{
    return decode(tree).value_or(defaults());
}

TemporalAveragingSettings AnalyzerConfigurationCodec::migrateLegacySmoothing(
    const double normalizedSmoothing) noexcept
{
    auto migrated = TemporalAveragingSettings { };
    if (!std::isfinite(normalizedSmoothing))
        return migrated;

    if (normalizedSmoothing <= 0.0) {
        migrated.enabled = false;
        return migrated;
    }

    constexpr auto offsetMilliseconds = 15.0;
    constexpr auto scaleMilliseconds = 435.0;
    migrated.milliseconds = std::clamp(
        offsetMilliseconds + scaleMilliseconds * normalizedSmoothing * normalizedSmoothing,
        TemporalAveragingSettings::minimumMilliseconds,
        TemporalAveragingSettings::maximumMilliseconds);

    return migrated;
}

AnalyzerConfiguration AnalyzerConfigurationCodec::migrateLegacy(
    const LegacySpectrumSettings& legacy) noexcept
{
    auto configuration = defaults();
    if (legacy.floorDb.has_value())
        configuration.spectrum.floorDb = *legacy.floorDb;
    if (legacy.ceilingDb.has_value())
        configuration.spectrum.ceilingDb = *legacy.ceilingDb;
    if (legacy.normalizedSmoothing.has_value()) {
        configuration.spectrum.temporalAveraging
            = migrateLegacySmoothing(*legacy.normalizedSmoothing);
    }

    return sanitize(configuration);
}
} // namespace audio_insight
