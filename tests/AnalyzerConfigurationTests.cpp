// SPDX-License-Identifier: AGPL-3.0-or-later

#include "state/AnalyzerConfiguration.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <limits>

namespace audio_insight {
namespace {
bool propertiesAreCanonicalStrings(const juce::ValueTree& tree)
{
    for (auto index = 0; index < tree.getNumProperties(); ++index) {
        if (!tree.getProperty(tree.getPropertyName(index)).isString())
            return false;
    }

    for (const auto& child : tree) {
        if (!propertiesAreCanonicalStrings(child))
            return false;
    }

    return true;
}

bool configurationsMatch(
    const AnalyzerConfiguration& first, const AnalyzerConfiguration& second) noexcept
{
    constexpr auto epsilon = 1.0e-12;
    const auto near
        = [](const double left, const double right) { return std::abs(left - right) <= epsilon; };

    return first.sharedAnalysis.fftSize == second.sharedAnalysis.fftSize
        && first.sharedAnalysis.window == second.sharedAnalysis.window
        && first.sharedAnalysis.requestedFftSliceRateHz
        == second.sharedAnalysis.requestedFftSliceRateHz
        && near(first.sharedAnalysis.frequencySpacing, second.sharedAnalysis.frequencySpacing)
        && near(first.spectrum.floorDb, second.spectrum.floorDb)
        && near(first.spectrum.ceilingDb, second.spectrum.ceilingDb)
        && near(first.spectrum.temporalAveraging.attackMilliseconds,
            second.spectrum.temporalAveraging.attackMilliseconds)
        && near(first.spectrum.temporalAveraging.releaseMilliseconds,
            second.spectrum.temporalAveraging.releaseMilliseconds)
        && first.spectrum.slope == second.spectrum.slope
        && first.spectrum.peakHoldMode == second.spectrum.peakHoldMode
        && near(first.spectrum.finitePeakHoldSeconds, second.spectrum.finitePeakHoldSeconds)
        && first.spectrum.traceColor == second.spectrum.traceColor
        && near(first.spectrum.fillOpacity, second.spectrum.fillOpacity)
        && first.spectrogram.palette == second.spectrogram.palette
        && near(first.spectrogram.colorResponse, second.spectrogram.colorResponse)
        && near(first.spectrogram.colorFloorDb, second.spectrogram.colorFloorDb)
        && near(first.spectrogram.colorCeilingDb, second.spectrogram.colorCeilingDb)
        && first.spectrogram.historyDurationSeconds == second.spectrogram.historyDurationSeconds
        && first.spectrogram.historyMode == second.spectrogram.historyMode
        && near(first.loudness.referenceLufs, second.loudness.referenceLufs);
}

class AnalyzerConfigurationTests final : public juce::UnitTest {
public:
    AnalyzerConfigurationTests() : UnitTest("Analyzer configuration", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Compiled defaults match the accepted analyzer design");
        {
            const auto configuration = AnalyzerConfigurationCodec::defaults();

            expectEquals(configuration.sharedAnalysis.fftSize, 8192);
            expect(configuration.sharedAnalysis.window == FftWindow::fiveTermFlatTop);
            expectEquals(configuration.sharedAnalysis.requestedFftSliceRateHz, 60);
            expectWithinAbsoluteError(configuration.sharedAnalysis.frequencySpacing, 0.8, 1.0e-12);

            expectWithinAbsoluteError(configuration.spectrum.floorDb, -90.0, 1.0e-12);
            expectWithinAbsoluteError(configuration.spectrum.ceilingDb, 0.0, 1.0e-12);
            expectWithinAbsoluteError(SpectrumSettings::minimumVisibleSpanDb, 24.0, 1.0e-12);
            expectWithinAbsoluteError(
                configuration.spectrum.temporalAveraging.attackMilliseconds, 0.0, 1.0e-12);
            expectWithinAbsoluteError(
                configuration.spectrum.temporalAveraging.releaseMilliseconds, 250.0, 1.0e-12);
            expect(configuration.spectrum.slope == SpectrumSlope::flat);
            expect(configuration.spectrum.peakHoldMode == SpectrumPeakHoldMode::off);
            expectWithinAbsoluteError(configuration.spectrum.finitePeakHoldSeconds, 2.0, 1.0e-12);
            expectEquals(static_cast<int>(configuration.spectrum.traceColor.packedRgb()),
                static_cast<int>(0x55c7e8));
            expectWithinAbsoluteError(configuration.spectrum.fillOpacity, 0.18, 1.0e-12);

            expect(configuration.spectrogram.palette == SpectrogramPalette::blueFire);
            expectWithinAbsoluteError(configuration.spectrogram.colorResponse, 0.0, 1.0e-12);
            expectWithinAbsoluteError(configuration.spectrogram.colorFloorDb, -120.0, 1.0e-12);
            expectWithinAbsoluteError(configuration.spectrogram.colorCeilingDb, 0.0, 1.0e-12);
            expectWithinAbsoluteError(SpectrogramSettings::minimumColorSpanDb, 24.0, 1.0e-12);
            expectEquals(configuration.spectrogram.historyDurationSeconds, 10);
            expect(configuration.spectrogram.historyMode == SpectrogramHistoryMode::scroll);
            expectWithinAbsoluteError(configuration.loudness.referenceLufs, -23.0, 1.0e-12);
        }

        beginTest("Sanitization enforces choices, ranges, steps, and both minimum spans");
        {
            auto input = AnalyzerConfigurationCodec::defaults();
            input.sharedAnalysis.fftSize = 1000;
            input.sharedAnalysis.window = static_cast<FftWindow>(99);
            input.sharedAnalysis.requestedFftSliceRateHz = 90;
            input.sharedAnalysis.frequencySpacing = -0.25;
            input.spectrum.floorDb = 100.0;
            input.spectrum.ceilingDb = -100.0;
            input.spectrum.temporalAveraging.attackMilliseconds = 2.0;
            input.spectrum.temporalAveraging.releaseMilliseconds = 4000.0;
            input.spectrum.slope = static_cast<SpectrumSlope>(99);
            input.spectrum.peakHoldMode = static_cast<SpectrumPeakHoldMode>(99);
            input.spectrum.finitePeakHoldSeconds = 0.1;
            input.spectrum.fillOpacity = 0.9;
            input.spectrogram.palette = static_cast<SpectrogramPalette>(99);
            input.spectrogram.colorResponse = -3.0;
            input.spectrogram.colorFloorDb = 50.0;
            input.spectrogram.colorCeilingDb = -50.0;
            input.spectrogram.historyDurationSeconds = 15;
            input.spectrogram.historyMode = static_cast<SpectrogramHistoryMode>(99);
            input.loudness.referenceLufs = -22.74;

            const auto sanitized = AnalyzerConfigurationCodec::sanitize(input);
            expectEquals(sanitized.sharedAnalysis.fftSize, 8192);
            expect(sanitized.sharedAnalysis.window == FftWindow::fiveTermFlatTop);
            expectEquals(sanitized.sharedAnalysis.requestedFftSliceRateHz, 60);
            expectWithinAbsoluteError(sanitized.sharedAnalysis.frequencySpacing, 0.0, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrum.floorDb, -48.0, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrum.ceilingDb, -24.0, 1.0e-12);
            expectWithinAbsoluteError(
                sanitized.spectrum.temporalAveraging.attackMilliseconds, 5.0, 1.0e-12);
            expectWithinAbsoluteError(
                sanitized.spectrum.temporalAveraging.releaseMilliseconds, 2000.0, 1.0e-12);
            expect(sanitized.spectrum.slope == SpectrumSlope::flat);
            expect(sanitized.spectrum.peakHoldMode == SpectrumPeakHoldMode::off);
            expectWithinAbsoluteError(sanitized.spectrum.finitePeakHoldSeconds, 0.25, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrum.fillOpacity, 0.5, 1.0e-12);
            expect(sanitized.spectrogram.palette == SpectrogramPalette::blueFire);
            expectWithinAbsoluteError(sanitized.spectrogram.colorResponse, -2.0, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrogram.colorFloorDb, -48.0, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrogram.colorCeilingDb, -24.0, 1.0e-12);
            expectEquals(sanitized.spectrogram.historyDurationSeconds, 10);
            expect(sanitized.spectrogram.historyMode == SpectrogramHistoryMode::scroll);
            expectWithinAbsoluteError(sanitized.loudness.referenceLufs, -22.5, 1.0e-12);
        }

        beginTest("Non-finite values use documented defaults");
        {
            auto input = AnalyzerConfigurationCodec::defaults();
            input.sharedAnalysis.frequencySpacing = std::numeric_limits<double>::quiet_NaN();
            input.spectrum.floorDb = std::numeric_limits<double>::infinity();
            input.spectrum.temporalAveraging.attackMilliseconds
                = std::numeric_limits<double>::quiet_NaN();
            input.spectrum.temporalAveraging.releaseMilliseconds
                = std::numeric_limits<double>::quiet_NaN();
            input.spectrum.fillOpacity = -std::numeric_limits<double>::infinity();
            input.spectrogram.colorResponse = std::numeric_limits<double>::quiet_NaN();
            input.loudness.referenceLufs = std::numeric_limits<double>::infinity();

            const auto sanitized = AnalyzerConfigurationCodec::sanitize(input);
            expectWithinAbsoluteError(sanitized.sharedAnalysis.frequencySpacing, 0.8, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrum.floorDb, -90.0, 1.0e-12);
            expectWithinAbsoluteError(
                sanitized.spectrum.temporalAveraging.attackMilliseconds, 0.0, 1.0e-12);
            expectWithinAbsoluteError(
                sanitized.spectrum.temporalAveraging.releaseMilliseconds, 250.0, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrum.fillOpacity, 0.18, 1.0e-12);
            expectWithinAbsoluteError(sanitized.spectrogram.colorResponse, 0.0, 1.0e-12);
            expectWithinAbsoluteError(sanitized.loudness.referenceLufs, -23.0, 1.0e-12);
        }

        beginTest("The complete current schema round-trips through ValueTree XML");
        {
            auto expected = AnalyzerConfigurationCodec::defaults();
            expected.sharedAnalysis.fftSize = 16384;
            expected.sharedAnalysis.window = FftWindow::fiveTermFlatTop;
            expected.sharedAnalysis.requestedFftSliceRateHz = 120;
            expected.sharedAnalysis.frequencySpacing = 0.35;
            expected.spectrum.floorDb = -132.0;
            expected.spectrum.ceilingDb = 6.0;
            expected.spectrum.temporalAveraging.attackMilliseconds = 125.5;
            expected.spectrum.temporalAveraging.releaseMilliseconds = 1250.5;
            expected.spectrum.slope = SpectrumSlope::db4Point5PerOctave;
            expected.spectrum.peakHoldMode = SpectrumPeakHoldMode::infinite;
            expected.spectrum.finitePeakHoldSeconds = 9.25;
            expected.spectrum.traceColor = { 1U, 2U, 3U };
            expected.spectrum.fillOpacity = 0.49;
            expected.spectrogram.palette = SpectrogramPalette::grayscale;
            expected.spectrogram.colorResponse = 1.25;
            expected.spectrogram.colorFloorDb = -160.0;
            expected.spectrogram.colorCeilingDb = -6.0;
            expected.spectrogram.historyDurationSeconds = 60;
            expected.spectrogram.historyMode = SpectrogramHistoryMode::overwrite;
            expected.loudness.referenceLufs = -14.5;

            const auto encoded = AnalyzerConfigurationCodec::encode(expected);
            expect(encoded.hasType(AnalyzerConfigurationCodec::treeType()));
            expectEquals(encoded.getNumProperties(), 1);
            expectEquals(encoded.getNumChildren(), 4);
            expect(!encoded.getChildWithName("Display").isValid());
            expectEquals(encoded.getChildWithName("SharedAnalysis").getNumProperties(), 4);
            expectEquals(encoded.getChildWithName("Spectrum").getNumProperties(), 9);
            expectEquals(encoded.getChildWithName("Spectrogram").getNumProperties(), 6);
            expectEquals(encoded.getChildWithName("Loudness").getNumProperties(), 1);
            expect(!encoded.getChildWithName("PeakRms").isValid());
            expect(!encoded.getChildWithName("Stereo").isValid());
            expect(!encoded.getChildWithName("Spectrum").hasProperty("minimumVisibleSpanDb"));
            expect(!encoded.getChildWithName("Spectrogram").hasProperty("minimumColorSpanDb"));
            expect(propertiesAreCanonicalStrings(encoded));

            const auto decoded = AnalyzerConfigurationCodec::decode(encoded);
            expect(decoded.has_value());
            if (decoded.has_value())
                expect(configurationsMatch(*decoded, expected));

            const auto xml = encoded.createXml();
            expect(xml != nullptr);
            if (xml != nullptr) {
                const auto fromXml = juce::ValueTree::fromXml(*xml);
                const auto decodedFromXml = AnalyzerConfigurationCodec::decode(fromXml);
                expect(decodedFromXml.has_value());
                if (decodedFromXml.has_value())
                    expect(configurationsMatch(*decodedFromXml, expected));
            }
        }

        beginTest("Every accepted discrete choice survives the codec");
        {
            constexpr std::array fftSizes { 1024, 2048, 4096, 8192, 16384 };
            constexpr std::array windows { FftWindow::rectangular, FftWindow::periodicHann,
                FftWindow::fourTermBlackmanHarris, FftWindow::fiveTermFlatTop };
            constexpr std::array fftSliceRates { 15, 30, 60, 120 };
            constexpr std::array slopes { SpectrumSlope::flat, SpectrumSlope::db3PerOctave,
                SpectrumSlope::db4Point5PerOctave, SpectrumSlope::db6PerOctave };
            constexpr std::array peakHoldModes { SpectrumPeakHoldMode::off,
                SpectrumPeakHoldMode::finite, SpectrumPeakHoldMode::infinite };
            constexpr std::array palettes { SpectrogramPalette::blueFire,
                SpectrogramPalette::inferno, SpectrogramPalette::viridis,
                SpectrogramPalette::grayscale };
            constexpr std::array historyDurations { 2, 5, 10, 20, 30, 60 };
            constexpr std::array historyModes { SpectrogramHistoryMode::scroll,
                SpectrogramHistoryMode::overwrite };
            for (const auto choice : fftSizes) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.sharedAnalysis.fftSize = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expectEquals(decoded->sharedAnalysis.fftSize, choice);
            }
            for (const auto choice : windows) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.sharedAnalysis.window = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expect(decoded->sharedAnalysis.window == choice);
            }
            for (const auto choice : fftSliceRates) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.sharedAnalysis.requestedFftSliceRateHz = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expectEquals(decoded->sharedAnalysis.requestedFftSliceRateHz, choice);
            }
            for (const auto choice : slopes) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.spectrum.slope = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expect(decoded->spectrum.slope == choice);
            }
            for (const auto choice : peakHoldModes) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.spectrum.peakHoldMode = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expect(decoded->spectrum.peakHoldMode == choice);
            }
            for (const auto choice : palettes) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.spectrogram.palette = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expect(decoded->spectrogram.palette == choice);
            }
            for (const auto choice : historyDurations) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.spectrogram.historyDurationSeconds = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expectEquals(decoded->spectrogram.historyDurationSeconds, choice);
            }
            for (const auto choice : historyModes) {
                auto configuration = AnalyzerConfigurationCodec::defaults();
                configuration.spectrogram.historyMode = choice;
                const auto decoded = AnalyzerConfigurationCodec::decode(
                    AnalyzerConfigurationCodec::encode(configuration));
                expect(decoded.has_value());
                if (decoded.has_value())
                    expect(decoded->spectrogram.historyMode == choice);
            }
        }

        beginTest("Recognized partial state defaults and sanitizes individual settings");
        {
            juce::ValueTree partial(AnalyzerConfigurationCodec::treeType());
            partial.setProperty("version", "3", nullptr);
            juce::ValueTree shared("SharedAnalysis");
            shared.setProperty("frequencySpacing", "2.5", nullptr);
            shared.setProperty("fftSize", "not-an-integer", nullptr);
            partial.addChild(shared, -1, nullptr);
            juce::ValueTree spectrum("Spectrum");
            spectrum.setProperty("floorDb", "not-a-number", nullptr);
            spectrum.setProperty("attackAveragingMilliseconds", "not-a-number", nullptr);
            spectrum.setProperty("slope", "unknown", nullptr);
            spectrum.setProperty("traceColorSrgb", "16777216", nullptr);
            partial.addChild(spectrum, -1, nullptr);
            juce::ValueTree spectrogram("Spectrogram");
            spectrogram.setProperty("historyDurationSeconds", "7", nullptr);
            spectrogram.setProperty("palette", "unknown", nullptr);
            partial.addChild(spectrogram, -1, nullptr);

            const auto decoded = AnalyzerConfigurationCodec::decode(partial);
            expect(decoded.has_value());
            if (decoded.has_value()) {
                expectEquals(decoded->sharedAnalysis.fftSize, 8192);
                expectWithinAbsoluteError(decoded->sharedAnalysis.frequencySpacing, 1.0, 1.0e-12);
                expectWithinAbsoluteError(decoded->spectrum.floorDb, -90.0, 1.0e-12);
                expectWithinAbsoluteError(
                    decoded->spectrum.temporalAveraging.attackMilliseconds, 0.0, 1.0e-12);
                expectWithinAbsoluteError(
                    decoded->spectrum.temporalAveraging.releaseMilliseconds, 250.0, 1.0e-12);
                expect(decoded->spectrum.slope == SpectrumSlope::flat);
                expect(decoded->spectrum.traceColor == SpectrumSettings::defaultTraceColor);
                expectEquals(decoded->spectrogram.historyDurationSeconds, 10);
                expect(decoded->spectrogram.palette == SpectrogramPalette::blueFire);
                expectWithinAbsoluteError(decoded->loudness.referenceLufs, -23.0, 1.0e-12);
            }
        }

        beginTest("Wrong types, versions, unknown fields, and duplicate sections are rejected");
        {
            expect(!AnalyzerConfigurationCodec::decode({ }).has_value());
            expect(!AnalyzerConfigurationCodec::decode(juce::ValueTree("WrongType")).has_value());

            auto missingVersion = AnalyzerConfigurationCodec::encode({ });
            missingVersion.removeProperty("version", nullptr);
            expect(!AnalyzerConfigurationCodec::decode(missingVersion).has_value());

            auto unknownVersion = AnalyzerConfigurationCodec::encode({ });
            unknownVersion.setProperty("version", "4", nullptr);
            expect(!AnalyzerConfigurationCodec::decode(unknownVersion).has_value());

            auto oldVersion = AnalyzerConfigurationCodec::encode({ });
            oldVersion.setProperty("version", "2", nullptr);
            expect(!AnalyzerConfigurationCodec::decode(oldVersion).has_value());

            auto nonIntegerVersion = AnalyzerConfigurationCodec::encode({ });
            nonIntegerVersion.setProperty("version", "1.0", nullptr);
            expect(!AnalyzerConfigurationCodec::decode(nonIntegerVersion).has_value());

            auto extraRootProperty = AnalyzerConfigurationCodec::encode({ });
            extraRootProperty.setProperty("unexpected", "1", nullptr);
            expect(!AnalyzerConfigurationCodec::decode(extraRootProperty).has_value());

            auto extraSectionProperty = AnalyzerConfigurationCodec::encode({ });
            extraSectionProperty.getChildWithName("Spectrum")
                .setProperty("unexpected", "1", nullptr);
            expect(!AnalyzerConfigurationCodec::decode(extraSectionProperty).has_value());

            auto unknownSection = AnalyzerConfigurationCodec::encode({ });
            unknownSection.addChild(juce::ValueTree("PeakRms"), -1, nullptr);
            expect(!AnalyzerConfigurationCodec::decode(unknownSection).has_value());

            auto duplicateSection = AnalyzerConfigurationCodec::encode({ });
            duplicateSection.addChild(juce::ValueTree("Spectrum"), -1, nullptr);
            expect(!AnalyzerConfigurationCodec::decode(duplicateSection).has_value());

            auto nestedSection = AnalyzerConfigurationCodec::encode({ });
            nestedSection.getChildWithName("Spectrum")
                .addChild(juce::ValueTree("Nested"), -1, nullptr);
            expect(!AnalyzerConfigurationCodec::decode(nestedSection).has_value());

            expect(configurationsMatch(AnalyzerConfigurationCodec::decodeOrDefault(unknownVersion),
                AnalyzerConfigurationCodec::defaults()));
        }
    }
};

AnalyzerConfigurationTests analyzerConfigurationTests;
} // namespace
} // namespace audio_insight
