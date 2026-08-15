// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/AnalyzerSettingsPanel.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>

namespace audio_insight {
namespace {
juce::Component* findDescendantWithId(juce::Component& component, const juce::String& componentId)
{
    if (component.getComponentID() == componentId)
        return &component;

    for (auto* child : component.getChildren()) {
        if (auto* match = findDescendantWithId(*child, componentId))
            return match;
    }

    return nullptr;
}
} // namespace

class AnalyzerSettingsPanelTests final : public juce::UnitTest {
public:
    AnalyzerSettingsPanelTests() : UnitTest("Analyzer settings panel", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Fresh controls expose responsive normalized smoothing");
        {
            auto callbackCount = 0;
            AnalyzerConfiguration lastPublished;
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(),
                [&](const AnalyzerConfiguration& configuration) {
                    ++callbackCount;
                    lastPublished = configuration;
                },
                [] { });
            panel.setBounds(0, 0, 360, 720);

            auto* floor
                = dynamic_cast<juce::Slider*>(findDescendantWithId(panel, "settingsSpectrumFloor"));
            auto* averaging = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumSmooth"));
            auto* frequencySpacing = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsFrequencySpacing"));
            expect(floor != nullptr);
            expect(averaging != nullptr);
            expect(frequencySpacing != nullptr);
            if (floor == nullptr || averaging == nullptr || frequencySpacing == nullptr)
                return;

            expect(!floor->isScrollWheelEnabled());
            expect(!averaging->isScrollWheelEnabled());
            expect(!frequencySpacing->isScrollWheelEnabled());
            expectWithinAbsoluteError(
                averaging->getValue(), 0.01 + (0.99 * std::sqrt((75.0 - 25.0) / 425.0)), 0.005);
            expectWithinAbsoluteError(frequencySpacing->getInterval(), 0.0, 1.0e-12);
            floor->setValue(-110.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 1);
            expectWithinAbsoluteError(lastPublished.spectrum.floorDb, -110.0, 1.0e-12);

            averaging->setValue(0.01, juce::sendNotificationSync);
            expectEquals(callbackCount, 2);
            expectWithinAbsoluteError(averaging->getValue(), 0.01, 1.0e-12);
            expectWithinAbsoluteError(lastPublished.spectrum.temporalAveraging.milliseconds,
                TemporalAveragingSettings::minimumMilliseconds, 1.0e-12);

            averaging->setValue(0.30, juce::sendNotificationSync);
            expectEquals(callbackCount, 3);
            constexpr auto normalizedTime = (0.30 - 0.01) / 0.99;
            expectWithinAbsoluteError(lastPublished.spectrum.temporalAveraging.milliseconds,
                25.0 + (425.0 * normalizedTime * normalizedTime), 1.0e-9);

            frequencySpacing->setValue(0.401234, juce::sendNotificationSync);
            expectEquals(callbackCount, 4);
            expectWithinAbsoluteError(
                lastPublished.sharedAnalysis.frequencySpacing, 0.401234, 1.0e-12);
        }

        beginTest("Spectrum reset restores every Spectrum default in one publication");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.spectrum.floorDb = -140.0;
            configuration.spectrum.ceilingDb = 6.0;
            configuration.spectrum.temporalAveraging.enabled = false;
            configuration.spectrum.temporalAveraging.milliseconds = 500.0;
            configuration.spectrum.slope = SpectrumSlope::db6PerOctave;
            configuration.spectrum.peakHoldMode = SpectrumPeakHoldMode::infinite;
            configuration.spectrum.finitePeakHoldSeconds = 9.0;
            configuration.spectrum.traceColor = SrgbColor::fromPackedRgb(0x123456U);
            configuration.spectrum.fillOpacity = 0.48;

            auto callbackCount = 0;
            AnalyzerConfiguration lastPublished;
            AnalyzerSettingsPanel panel(
                configuration,
                [&](const AnalyzerConfiguration& published) {
                    ++callbackCount;
                    lastPublished = published;
                },
                [] { });
            panel.setBounds(0, 0, 360, 720);

            auto* reset
                = dynamic_cast<juce::Button*>(findDescendantWithId(panel, "settingsSpectrumReset"));
            expect(reset != nullptr);
            if (reset == nullptr)
                return;

            reset->onClick();
            expectEquals(callbackCount, 1);
            expectWithinAbsoluteError(lastPublished.spectrum.floorDb, -90.0, 1.0e-12);
            expectWithinAbsoluteError(lastPublished.spectrum.ceilingDb, 0.0, 1.0e-12);
            expect(lastPublished.spectrum.temporalAveraging.enabled);
            expectWithinAbsoluteError(
                lastPublished.spectrum.temporalAveraging.milliseconds, 75.0, 1.0e-12);
            expect(lastPublished.spectrum.slope == SpectrumSlope::flat);
            expect(lastPublished.spectrum.peakHoldMode == SpectrumPeakHoldMode::off);
            expectWithinAbsoluteError(lastPublished.spectrum.finitePeakHoldSeconds, 2.0, 1.0e-12);
            expect(lastPublished.spectrum.traceColor == SpectrumSettings::defaultTraceColor);
            expectWithinAbsoluteError(lastPublished.spectrum.fillOpacity, 0.18, 1.0e-12);
        }

        beginTest("Shared reset restores disabled and live Shared settings together");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.sharedAnalysis.fftSize = 16384;
            configuration.sharedAnalysis.window = FftWindow::fiveTermFlatTop;
            configuration.sharedAnalysis.requestedFftSliceRateHz = 120;
            configuration.sharedAnalysis.frequencySpacing = 0.25;

            AnalyzerConfiguration lastPublished;
            auto callbackCount = 0;
            AnalyzerSettingsPanel panel(
                configuration,
                [&](const AnalyzerConfiguration& published) {
                    ++callbackCount;
                    lastPublished = published;
                },
                [] { });
            panel.setBounds(0, 0, 360, 720);

            auto* reset
                = dynamic_cast<juce::Button*>(findDescendantWithId(panel, "settingsSharedReset"));
            expect(reset != nullptr);
            if (reset == nullptr)
                return;

            reset->onClick();
            expectEquals(callbackCount, 1);
            expectEquals(lastPublished.sharedAnalysis.fftSize, 4096);
            expect(lastPublished.sharedAnalysis.window == FftWindow::periodicHann);
            expectEquals(lastPublished.sharedAnalysis.requestedFftSliceRateHz, 60);
            expectWithinAbsoluteError(lastPublished.sharedAnalysis.frequencySpacing, 1.0, 1.0e-12);
        }

        beginTest("Unavailable analyzer sections are visible but cannot accept changes");
        {
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(), [](const AnalyzerConfiguration&) { },
                [] { });
            panel.setBounds(0, 0, 720, 420);

            const auto expectUnavailable = [this, &panel](const juce::String& componentId) {
                const auto* component = findDescendantWithId(panel, componentId);
                expect(component != nullptr);
                expect(component != nullptr && !component->isEnabled());
            };

            expectUnavailable("settingsSpectrogramUnavailable");
            expectUnavailable("settingsSpectrumSlope");
            expectUnavailable("settingsSpectrumPeakHoldMode");
            expectUnavailable("settingsSpectrumPeakHoldDuration");
            expectUnavailable("settingsSpectrumTraceColor");
            expectUnavailable("settingsSpectrumFillOpacity");
            expectUnavailable("settingsSpectrogramPalette");
            expectUnavailable("settingsSpectrogramColorResponse");
            expectUnavailable("settingsSpectrogramColorFloor");
            expectUnavailable("settingsSpectrogramColorCeiling");
            expectUnavailable("settingsSpectrogramHistory");
            expectUnavailable("settingsSpectrogramHistoryMode");
            expectUnavailable("settingsStereoUnavailable");
            expectUnavailable("settingsLoudnessUnavailable");
            expectUnavailable("settingsLoudnessReference");
            expectUnavailable("settingsSpectrogramReset");
        }

        beginTest("Disabled advanced controls reflect every saved presentation value");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.spectrum.slope = SpectrumSlope::db4Point5PerOctave;
            configuration.spectrum.peakHoldMode = SpectrumPeakHoldMode::infinite;
            configuration.spectrum.finitePeakHoldSeconds = 5.25;
            configuration.spectrum.traceColor = SrgbColor::fromPackedRgb(0x12ab34U);
            configuration.spectrum.fillOpacity = 0.42;
            configuration.spectrogram.colorResponse = -1.25;
            configuration.spectrogram.colorFloorDb = -150.0;
            configuration.spectrogram.colorCeilingDb = 6.0;
            configuration.spectrogram.historyMode = SpectrogramHistoryMode::overwrite;

            AnalyzerSettingsPanel panel(
                configuration, [](const AnalyzerConfiguration&) { }, [] { });
            panel.setBounds(0, 0, 360, 720);

            auto* slope = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrumSlope"));
            auto* peakHoldMode = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrumPeakHoldMode"));
            auto* peakHoldDuration = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumPeakHoldDuration"));
            auto* traceColor = dynamic_cast<juce::TextEditor*>(
                findDescendantWithId(panel, "settingsSpectrumTraceColor"));
            auto* fillOpacity = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumFillOpacity"));
            auto* colorResponse = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorResponse"));
            auto* colorFloor = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorFloor"));
            auto* colorCeiling = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorCeiling"));
            auto* historyMode = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistoryMode"));

            const std::array<juce::Component*, 9> controls { slope, peakHoldMode, peakHoldDuration,
                traceColor, fillOpacity, colorResponse, colorFloor, colorCeiling, historyMode };
            for (const auto* control : controls) {
                expect(control != nullptr);
                expect(control != nullptr && !control->isEnabled());
            }

            if (slope != nullptr)
                expectEquals(slope->getText(), juce::String("+4.5 dB/oct"));
            if (peakHoldMode != nullptr)
                expectEquals(peakHoldMode->getText(), juce::String("Infinite"));
            if (peakHoldDuration != nullptr)
                expectWithinAbsoluteError(peakHoldDuration->getValue(), 5.25, 1.0e-12);
            if (traceColor != nullptr)
                expectEquals(traceColor->getText(), juce::String("#12AB34"));
            if (fillOpacity != nullptr)
                expectWithinAbsoluteError(fillOpacity->getValue(), 42.0, 1.0e-12);
            if (colorResponse != nullptr)
                expectWithinAbsoluteError(colorResponse->getValue(), -1.25, 1.0e-12);
            if (colorFloor != nullptr)
                expectWithinAbsoluteError(colorFloor->getValue(), -150.0, 1.0e-12);
            if (colorCeiling != nullptr)
                expectWithinAbsoluteError(colorCeiling->getValue(), 6.0, 1.0e-12);
            if (historyMode != nullptr)
                expectEquals(historyMode->getText(), juce::String("Overwrite"));
        }

        beginTest("External state refresh does not publish and Escape requests close");
        {
            auto callbackCount = 0;
            auto closeCount = 0;
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(),
                [&](const AnalyzerConfiguration&) { ++callbackCount; }, [&] { ++closeCount; });
            panel.setBounds(0, 0, 360, 720);

            auto replacement = AnalyzerConfigurationCodec::defaults();
            replacement.sharedAnalysis.fftSize = 8192;
            replacement.sharedAnalysis.window = FftWindow::fiveTermFlatTop;
            replacement.sharedAnalysis.requestedFftSliceRateHz = 120;
            replacement.sharedAnalysis.frequencySpacing = 0.5;
            replacement.spectrum.temporalAveraging.milliseconds = 125.0;
            replacement.spectrogram.palette = SpectrogramPalette::viridis;
            replacement.spectrogram.historyDurationSeconds = 30;
            replacement.loudness.referenceLufs = -14.5;
            panel.setConfiguration(replacement);
            expectEquals(callbackCount, 0);
            expectWithinAbsoluteError(
                panel.getConfiguration().spectrum.temporalAveraging.milliseconds, 125.0, 1.0e-12);

            auto* fftSize
                = dynamic_cast<juce::ComboBox*>(findDescendantWithId(panel, "settingsFftSize"));
            auto* window
                = dynamic_cast<juce::ComboBox*>(findDescendantWithId(panel, "settingsFftWindow"));
            auto* fftRate = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsFftSliceRate"));
            auto* spacing = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsFrequencySpacing"));
            auto* palette = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramPalette"));
            auto* history = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramHistory"));
            auto* reference = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsLoudnessReference"));
            expect(fftSize != nullptr && fftSize->getText() == "8192");
            expect(window != nullptr && window->getText() == "Flat-top");
            expect(fftRate != nullptr && fftRate->getText() == "120 Hz");
            expect(spacing != nullptr && std::abs(spacing->getValue() - 0.5) < 1.0e-12);
            expect(palette != nullptr && palette->getText() == "Viridis");
            expect(history != nullptr);
            if (history != nullptr)
                expectWithinAbsoluteError(history->getValue(), 30.0, 1.0e-12);
            expect(reference != nullptr);
            if (reference != nullptr)
                expectWithinAbsoluteError(reference->getValue(), -14.5, 1.0e-12);

            expect(panel.keyPressed(juce::KeyPress { juce::KeyPress::escapeKey }));
            expectEquals(closeCount, 1);
        }

        beginTest("Keyboard traversal and accessibility names include enabled settings");
        {
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(), [](const AnalyzerConfiguration&) { },
                [] { });
            panel.setBounds(0, 0, 360, 720);

            auto* close = findDescendantWithId(panel, "settingsClose");
            auto* floor = findDescendantWithId(panel, "settingsSpectrumFloor");
            auto* smooth = findDescendantWithId(panel, "settingsSpectrumSmooth");
            auto* reset = findDescendantWithId(panel, "settingsSpectrumReset");
            expect(close != nullptr);
            expect(floor != nullptr);
            expect(smooth != nullptr);
            expect(reset != nullptr);

            auto traverser = panel.createKeyboardFocusTraverser();
            auto* current = close;
            auto reachedFloor = false;
            auto reachedSmooth = false;
            for (auto step = 0; step < 12 && current != nullptr; ++step) {
                current = traverser->getNextComponent(current);
                reachedFloor = reachedFloor || current == floor;
                reachedSmooth = reachedSmooth || current == smooth;
            }
            expect(reachedFloor);
            expect(reachedSmooth);

            if (reset != nullptr) {
                auto handler = reset->createAccessibilityHandler();
                expect(handler != nullptr);
                if (handler != nullptr)
                    expect(handler->getTitle().containsIgnoreCase("Spectrum"));
            }
            if (smooth != nullptr) {
                auto handler = smooth->createAccessibilityHandler();
                expect(handler != nullptr);
                if (handler != nullptr)
                    expect(handler->getTitle().containsIgnoreCase("smooth"));
            }
        }
    }
};

AnalyzerSettingsPanelTests analyzerSettingsPanelTests;
} // namespace audio_insight
