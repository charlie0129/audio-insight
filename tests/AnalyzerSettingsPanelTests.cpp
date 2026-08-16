// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/AnalyzerSettingsPanel.h"

#include <juce_core/juce_core.h>

#include <algorithm>
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
        beginTest("Attack and release use physical time with Off sentinels and log spacing");
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

            expect(findDescendantWithId(panel, "settingsDisplayFramePacing") == nullptr);
            expect(findDescendantWithId(panel, "settingsDisplayReset") == nullptr);

            auto* floor
                = dynamic_cast<juce::Slider*>(findDescendantWithId(panel, "settingsSpectrumFloor"));
            auto* attack = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumAttack"));
            auto* release = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumRelease"));
            auto* frequencySpacing = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsFrequencySpacing"));
            expect(floor != nullptr);
            expect(attack != nullptr);
            expect(release != nullptr);
            expect(frequencySpacing != nullptr);
            if (floor == nullptr || attack == nullptr || release == nullptr
                || frequencySpacing == nullptr) {
                return;
            }

            expect(!floor->isScrollWheelEnabled());
            expect(!attack->isScrollWheelEnabled());
            expect(!release->isScrollWheelEnabled());
            expect(!frequencySpacing->isScrollWheelEnabled());
            expectWithinAbsoluteError(attack->getValue(), 0.0, 1.0e-12);
            expectWithinAbsoluteError(release->getValue(), 250.0, 1.0e-12);
            expectEquals(attack->getTextFromValue(0.0), juce::String("Off"));
            expectEquals(release->getTextFromValue(250.0), juce::String("250 ms"));
            auto attackAccessibility = attack->createAccessibilityHandler();
            expect(attackAccessibility != nullptr);
            expect(attackAccessibility != nullptr
                && attackAccessibility->getValueInterface() != nullptr);
            if (attackAccessibility != nullptr
                && attackAccessibility->getValueInterface() != nullptr) {
                expectEquals(attackAccessibility->getValueInterface()->getCurrentValueAsString(),
                    juce::String("Off"));
            }

            const auto attackStart = attack->valueToProportionOfLength(
                TemporalAveragingSettings::minimumAttackMilliseconds);
            const auto attackMidpoint = (attackStart + 1.0) * 0.5;
            expectWithinAbsoluteError(attack->proportionOfLengthToValue(attackMidpoint),
                std::sqrt(TemporalAveragingSettings::minimumAttackMilliseconds
                    * TemporalAveragingSettings::maximumAttackMilliseconds),
                1.0e-9);
            const auto releaseStart = release->valueToProportionOfLength(
                TemporalAveragingSettings::minimumReleaseMilliseconds);
            const auto releaseMidpoint = (releaseStart + 1.0) * 0.5;
            expectWithinAbsoluteError(release->proportionOfLengthToValue(releaseMidpoint),
                std::sqrt(TemporalAveragingSettings::minimumReleaseMilliseconds
                    * TemporalAveragingSettings::maximumReleaseMilliseconds),
                1.0e-9);
            expectWithinAbsoluteError(frequencySpacing->getInterval(), 0.0, 1.0e-12);

            constexpr std::array<const char*, 3> markIds { "settingsFrequencySpacingMark1",
                "settingsFrequencySpacingMark2", "settingsFrequencySpacingMark3" };
            constexpr std::array<const char*, 3> markText { "0.25", "0.5", "0.75" };
            auto previousMarkX = -1;
            for (auto index = std::size_t { 0 }; index < markIds.size(); ++index) {
                auto* mark
                    = dynamic_cast<juce::Label*>(findDescendantWithId(panel, markIds[index]));
                expect(mark != nullptr);
                if (mark == nullptr)
                    continue;

                expectEquals(mark->getText(), juce::String(markText[index]));
                expect(mark->isVisible());
                expect(mark->getBounds().getCentreX() > previousMarkX);
                previousMarkX = mark->getBounds().getCentreX();
            }

            floor->setValue(-110.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 1);
            expectWithinAbsoluteError(lastPublished.spectrum.floorDb, -110.0, 1.0e-12);

            attack->setValue(25.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 2);
            expectWithinAbsoluteError(
                lastPublished.spectrum.temporalAveraging.attackMilliseconds, 25.0, 1.0e-12);

            release->setValue(625.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 3);
            expectWithinAbsoluteError(
                lastPublished.spectrum.temporalAveraging.releaseMilliseconds, 625.0, 1.0e-12);

            frequencySpacing->setValue(0.401234, juce::sendNotificationSync);
            expectEquals(callbackCount, 4);
            expectWithinAbsoluteError(
                lastPublished.sharedAnalysis.frequencySpacing, 0.401234, 1.0e-12);
        }

        beginTest("Shared FFT controls publish their selected analysis choices");
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

            auto* fftSizeControl
                = dynamic_cast<juce::ComboBox*>(findDescendantWithId(panel, "settingsFftSize"));
            auto* windowControl
                = dynamic_cast<juce::ComboBox*>(findDescendantWithId(panel, "settingsFftWindow"));
            auto* rateControl = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsFftSliceRate"));
            expect(fftSizeControl != nullptr && fftSizeControl->isEnabled());
            expect(windowControl != nullptr && windowControl->isEnabled());
            expect(rateControl != nullptr && rateControl->isEnabled());
            if (fftSizeControl == nullptr || windowControl == nullptr || rateControl == nullptr)
                return;

            fftSizeControl->setSelectedId(1, juce::sendNotificationSync);
            expectEquals(callbackCount, 1);
            expectEquals(lastPublished.sharedAnalysis.fftSize, 1024);

            windowControl->setSelectedId(3, juce::sendNotificationSync);
            expectEquals(callbackCount, 2);
            expect(lastPublished.sharedAnalysis.window == FftWindow::fourTermBlackmanHarris);

            rateControl->setSelectedId(4, juce::sendNotificationSync);
            expectEquals(callbackCount, 3);
            expectEquals(lastPublished.sharedAnalysis.requestedFftSliceRateHz, 120);
        }

        beginTest("Spectrum reset restores every Spectrum default in one publication");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.spectrum.floorDb = -140.0;
            configuration.spectrum.ceilingDb = 6.0;
            configuration.spectrum.temporalAveraging.attackMilliseconds = 125.0;
            configuration.spectrum.temporalAveraging.releaseMilliseconds = 500.0;
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
            expectWithinAbsoluteError(
                lastPublished.spectrum.temporalAveraging.attackMilliseconds, 0.0, 1.0e-12);
            expectWithinAbsoluteError(
                lastPublished.spectrum.temporalAveraging.releaseMilliseconds, 250.0, 1.0e-12);
            expect(lastPublished.spectrum.slope == SpectrumSlope::flat);
            expect(lastPublished.spectrum.peakHoldMode == SpectrumPeakHoldMode::off);
            expectWithinAbsoluteError(lastPublished.spectrum.finitePeakHoldSeconds, 2.0, 1.0e-12);
            expect(lastPublished.spectrum.traceColor == SpectrumSettings::defaultTraceColor);
            expectWithinAbsoluteError(lastPublished.spectrum.fillOpacity, 0.18, 1.0e-12);
        }

        beginTest("Shared reset restores every Shared analysis setting together");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.sharedAnalysis.fftSize = 16384;
            configuration.sharedAnalysis.window = FftWindow::periodicHann;
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
            expectEquals(lastPublished.sharedAnalysis.fftSize, 8192);
            expect(lastPublished.sharedAnalysis.window == FftWindow::fiveTermFlatTop);
            expectEquals(lastPublished.sharedAnalysis.requestedFftSliceRateHz, 60);
            expectWithinAbsoluteError(lastPublished.sharedAnalysis.frequencySpacing, 0.8, 1.0e-12);
        }

        beginTest("Every implemented analyzer section exposes only its supported controls");
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

            const auto expectAvailable = [this, &panel](const juce::String& componentId) {
                const auto* component = findDescendantWithId(panel, componentId);
                expect(component != nullptr);
                expect(component != nullptr && component->isEnabled());
            };

            expectAvailable("settingsSpectrumAttack");
            expectAvailable("settingsSpectrumRelease");
            expectAvailable("settingsSpectrumSlope");
            expectAvailable("settingsSpectrumPeakHoldMode");
            expectAvailable("settingsSpectrumTraceColor");
            expectAvailable("settingsSpectrumFillOpacity");
            expectAvailable("settingsSpectrogramStatus");
            expectAvailable("settingsSpectrogramPalette");
            expectAvailable("settingsSpectrogramColorResponse");
            expectAvailable("settingsSpectrogramColorFloor");
            expectAvailable("settingsSpectrogramColorCeiling");
            expectAvailable("settingsSpectrogramHistory");
            expectAvailable("settingsSpectrogramHistoryMode");
            expectAvailable("settingsSpectrogramReset");
            expectAvailable("settingsStereoStatus");
            expectAvailable("settingsLoudnessStatus");
            expectAvailable("settingsLoudnessReference");
            expectAvailable("settingsLoudnessReset");

            const auto* peakHoldDuration
                = findDescendantWithId(panel, "settingsSpectrumPeakHoldDuration");
            expect(peakHoldDuration != nullptr);
            expect(peakHoldDuration != nullptr && !peakHoldDuration->isEnabled());
            expect(peakHoldDuration != nullptr && !peakHoldDuration->getWantsKeyboardFocus());

            expectUnavailable("settingsStereoReset");

            const auto* stereoStatus = dynamic_cast<const juce::Label*>(
                findDescendantWithId(panel, "settingsStereoStatus"));
            expect(stereoStatus != nullptr);
            expect(stereoStatus != nullptr
                && stereoStatus->getText().containsIgnoreCase("no adjustable settings"));

            auto* stereoReset
                = dynamic_cast<juce::Button*>(findDescendantWithId(panel, "settingsStereoReset"));
            expect(stereoReset != nullptr
                && stereoReset->getTooltip().containsIgnoreCase("no settings to reset"));

            auto* history = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistory"));
            expect(history != nullptr);
            if (history != nullptr) {
                expectEquals(history->getNumItems(), 6);
                expectEquals(history->getItemText(0), juce::String("2 s"));
                expectEquals(history->getItemText(1), juce::String("5 s"));
                expectEquals(history->getItemText(2), juce::String("10 s"));
                expectEquals(history->getItemText(3), juce::String("20 s"));
                expectEquals(history->getItemText(4), juce::String("30 s"));
                expectEquals(history->getItemText(5), juce::String("60 s"));
            }
        }

        beginTest("Spectrum and Spectrogram controls reflect saved presentation values");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.spectrum.slope = SpectrumSlope::db4Point5PerOctave;
            configuration.spectrum.peakHoldMode = SpectrumPeakHoldMode::infinite;
            configuration.spectrum.finitePeakHoldSeconds = 5.25;
            configuration.spectrum.traceColor = SrgbColor::fromPackedRgb(0x12ab34U);
            configuration.spectrum.fillOpacity = 0.42;
            configuration.spectrogram.palette = SpectrogramPalette::grayscale;
            configuration.spectrogram.colorResponse = -1.25;
            configuration.spectrogram.colorFloorDb = -150.0;
            configuration.spectrogram.colorCeilingDb = 6.0;
            configuration.spectrogram.historyDurationSeconds = 20;
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
            auto* palette = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramPalette"));
            auto* colorResponse = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorResponse"));
            auto* colorFloor = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorFloor"));
            auto* colorCeiling = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorCeiling"));
            auto* history = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistory"));
            auto* historyMode = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistoryMode"));

            const std::array<juce::Component*, 10> enabledControls { slope, peakHoldMode,
                traceColor, fillOpacity, palette, colorResponse, colorFloor, colorCeiling, history,
                historyMode };
            for (const auto* control : enabledControls) {
                expect(control != nullptr);
                expect(control != nullptr && control->isEnabled());
            }

            const std::array<juce::Component*, 1> disabledControls { peakHoldDuration };
            for (const auto* control : disabledControls) {
                expect(control != nullptr);
                expect(control != nullptr && !control->isEnabled());
            }

            if (slope != nullptr)
                expectEquals(slope->getText(), juce::String("+4.5 dB/oct"));
            if (peakHoldMode != nullptr)
                expectEquals(peakHoldMode->getText(), juce::String("Infinite"));
            if (peakHoldDuration != nullptr)
                expectWithinAbsoluteError(peakHoldDuration->getValue(), 5.25, 1.0e-12);
            if (traceColor != nullptr) {
                expectEquals(traceColor->getText(), juce::String("#12AB34"));
                expect(!traceColor->isReadOnly());
            }
            if (fillOpacity != nullptr)
                expectWithinAbsoluteError(fillOpacity->getValue(), 42.0, 1.0e-12);
            if (palette != nullptr)
                expectEquals(palette->getText(), juce::String("Grayscale"));
            if (colorResponse != nullptr)
                expectWithinAbsoluteError(colorResponse->getValue(), -1.25, 1.0e-12);
            if (colorFloor != nullptr)
                expectWithinAbsoluteError(colorFloor->getValue(), -150.0, 1.0e-12);
            if (colorCeiling != nullptr)
                expectWithinAbsoluteError(colorCeiling->getValue(), 6.0, 1.0e-12);
            if (history != nullptr)
                expectEquals(history->getText(), juce::String("20 s"));
            if (historyMode != nullptr)
                expectEquals(historyMode->getText(), juce::String("Overwrite"));
        }

        beginTest("Each Spectrum presentation edit publishes once and colour validates on commit");
        {
            auto callbackCount = 0;
            AnalyzerConfiguration lastPublished;
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(),
                [&](const AnalyzerConfiguration& published) {
                    ++callbackCount;
                    lastPublished = published;
                },
                [] { });
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

            expect(slope != nullptr);
            expect(peakHoldMode != nullptr);
            expect(peakHoldDuration != nullptr);
            expect(traceColor != nullptr);
            expect(fillOpacity != nullptr);
            if (slope == nullptr || peakHoldMode == nullptr || peakHoldDuration == nullptr
                || traceColor == nullptr || fillOpacity == nullptr) {
                return;
            }

            slope->setSelectedId(3, juce::sendNotificationSync);
            expectEquals(callbackCount, 1);
            expect(lastPublished.spectrum.slope == SpectrumSlope::db4Point5PerOctave);

            peakHoldMode->setSelectedId(2, juce::sendNotificationSync);
            expectEquals(callbackCount, 2);
            expect(lastPublished.spectrum.peakHoldMode == SpectrumPeakHoldMode::finite);
            expect(peakHoldDuration->isEnabled());
            expect(peakHoldDuration->getWantsKeyboardFocus());
            expectWithinAbsoluteError(peakHoldDuration->getValue(), 2.0, 1.0e-12);

            peakHoldDuration->setValue(3.5, juce::sendNotificationSync);
            expectEquals(callbackCount, 3);
            expectWithinAbsoluteError(lastPublished.spectrum.finitePeakHoldSeconds, 3.5, 1.0e-12);

            fillOpacity->setValue(41.2, juce::sendNotificationSync);
            expectEquals(callbackCount, 4);
            expectWithinAbsoluteError(lastPublished.spectrum.fillOpacity, 0.412, 1.0e-12);

            traceColor->setText("#12ab34", false);
            expect(static_cast<bool>(traceColor->onReturnKey));
            if (traceColor->onReturnKey)
                traceColor->onReturnKey();
            expectEquals(callbackCount, 5);
            expect(lastPublished.spectrum.traceColor == SrgbColor::fromPackedRgb(0x12ab34U));
            expectEquals(traceColor->getText(), juce::String("#12AB34"));

            expect(static_cast<bool>(traceColor->onFocusLost));
            if (traceColor->onFocusLost)
                traceColor->onFocusLost();
            expectEquals(callbackCount, 5);

            traceColor->setText("bad", false);
            if (traceColor->onFocusLost)
                traceColor->onFocusLost();
            expectEquals(callbackCount, 5);
            expectEquals(traceColor->getText(), juce::String("#12AB34"));

            peakHoldMode->setSelectedId(3, juce::sendNotificationSync);
            expectEquals(callbackCount, 6);
            expect(lastPublished.spectrum.peakHoldMode == SpectrumPeakHoldMode::infinite);
            expect(!peakHoldDuration->isEnabled());
            expect(!peakHoldDuration->getWantsKeyboardFocus());
        }

        beginTest("Each Spectrogram edit publishes immediately and history choices are discrete");
        {
            auto callbackCount = 0;
            AnalyzerConfiguration lastPublished;
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(),
                [&](const AnalyzerConfiguration& published) {
                    ++callbackCount;
                    lastPublished = published;
                },
                [] { });
            panel.setBounds(0, 0, 360, 720);

            auto* palette = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramPalette"));
            auto* colorResponse = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorResponse"));
            auto* colorFloor = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorFloor"));
            auto* colorCeiling = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorCeiling"));
            auto* history = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistory"));
            auto* historyMode = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistoryMode"));

            expect(palette != nullptr);
            expect(colorResponse != nullptr);
            expect(colorFloor != nullptr);
            expect(colorCeiling != nullptr);
            expect(history != nullptr);
            expect(historyMode != nullptr);
            if (palette == nullptr || colorResponse == nullptr || colorFloor == nullptr
                || colorCeiling == nullptr || history == nullptr || historyMode == nullptr) {
                return;
            }

            expect(!colorResponse->isScrollWheelEnabled());
            expect(!colorFloor->isScrollWheelEnabled());
            expect(!colorCeiling->isScrollWheelEnabled());

            palette->setSelectedId(2, juce::sendNotificationSync);
            expectEquals(callbackCount, 1);
            expect(lastPublished.spectrogram.palette == SpectrogramPalette::inferno);

            colorResponse->setValue(-1.25, juce::sendNotificationSync);
            expectEquals(callbackCount, 2);
            expectWithinAbsoluteError(lastPublished.spectrogram.colorResponse, -1.25, 1.0e-12);

            colorFloor->setValue(-150.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 3);
            expectWithinAbsoluteError(lastPublished.spectrogram.colorFloorDb, -150.0, 1.0e-12);

            colorCeiling->setValue(6.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 4);
            expectWithinAbsoluteError(lastPublished.spectrogram.colorCeilingDb, 6.0, 1.0e-12);

            constexpr std::array historyChoiceIds { 1, 2, 4, 5, 6, 3 };
            constexpr std::array historyDurations { 2, 5, 20, 30, 60, 10 };
            for (auto index = std::size_t { 0 }; index < historyChoiceIds.size(); ++index) {
                history->setSelectedId(historyChoiceIds[index], juce::sendNotificationSync);
                expectEquals(callbackCount, static_cast<int>(index) + 5);
                expectEquals(
                    lastPublished.spectrogram.historyDurationSeconds, historyDurations[index]);
            }

            historyMode->setSelectedId(2, juce::sendNotificationSync);
            expectEquals(callbackCount, 11);
            expect(lastPublished.spectrogram.historyMode == SpectrogramHistoryMode::overwrite);
        }

        beginTest("Spectrogram reset restores every Spectrogram default in one publication");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.spectrogram.palette = SpectrogramPalette::grayscale;
            configuration.spectrogram.colorResponse = 1.5;
            configuration.spectrogram.colorFloorDb = -160.0;
            configuration.spectrogram.colorCeilingDb = 6.0;
            configuration.spectrogram.historyDurationSeconds = 60;
            configuration.spectrogram.historyMode = SpectrogramHistoryMode::overwrite;

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

            auto* reset = dynamic_cast<juce::Button*>(
                findDescendantWithId(panel, "settingsSpectrogramReset"));
            auto* palette = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramPalette"));
            auto* colorResponse = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorResponse"));
            auto* colorFloor = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorFloor"));
            auto* colorCeiling = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorCeiling"));
            auto* history = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistory"));
            auto* historyMode = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistoryMode"));
            expect(reset != nullptr);
            if (reset == nullptr)
                return;

            reset->onClick();
            expectEquals(callbackCount, 1);
            expect(lastPublished.spectrogram.palette == SpectrogramPalette::blueFire);
            expectWithinAbsoluteError(lastPublished.spectrogram.colorResponse, 0.0, 1.0e-12);
            expectWithinAbsoluteError(lastPublished.spectrogram.colorFloorDb, -120.0, 1.0e-12);
            expectWithinAbsoluteError(lastPublished.spectrogram.colorCeilingDb, 0.0, 1.0e-12);
            expectEquals(lastPublished.spectrogram.historyDurationSeconds, 10);
            expect(lastPublished.spectrogram.historyMode == SpectrogramHistoryMode::scroll);

            expect(palette != nullptr && palette->getText() == "Blue Fire");
            expect(colorResponse != nullptr);
            if (colorResponse != nullptr)
                expectWithinAbsoluteError(colorResponse->getValue(), 0.0, 1.0e-12);
            expect(colorFloor != nullptr);
            if (colorFloor != nullptr)
                expectWithinAbsoluteError(colorFloor->getValue(), -120.0, 1.0e-12);
            expect(colorCeiling != nullptr);
            if (colorCeiling != nullptr)
                expectWithinAbsoluteError(colorCeiling->getValue(), 0.0, 1.0e-12);
            expect(history != nullptr && history->getText() == "10 s");
            expect(historyMode != nullptr && historyMode->getText() == "Scroll");
        }

        beginTest("Loudness reference edits and section reset publish presentation state");
        {
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.loudness.referenceLufs = -14.5;

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

            auto* reference = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsLoudnessReference"));
            auto* reset
                = dynamic_cast<juce::Button*>(findDescendantWithId(panel, "settingsLoudnessReset"));
            expect(reference != nullptr);
            expect(reset != nullptr);
            if (reference == nullptr || reset == nullptr)
                return;

            expect(!reference->isScrollWheelEnabled());
            reference->setValue(-18.0, juce::sendNotificationSync);
            expectEquals(callbackCount, 1);
            expectWithinAbsoluteError(lastPublished.loudness.referenceLufs, -18.0, 1.0e-12);

            reset->onClick();
            expectEquals(callbackCount, 2);
            expectWithinAbsoluteError(lastPublished.loudness.referenceLufs, -23.0, 1.0e-12);
            expectWithinAbsoluteError(reference->getValue(), -23.0, 1.0e-12);
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
            replacement.spectrum.temporalAveraging.attackMilliseconds = 125.0;
            replacement.spectrum.temporalAveraging.releaseMilliseconds = 500.0;
            replacement.spectrum.slope = SpectrumSlope::db3PerOctave;
            replacement.spectrum.peakHoldMode = SpectrumPeakHoldMode::finite;
            replacement.spectrum.finitePeakHoldSeconds = 4.25;
            replacement.spectrum.traceColor = SrgbColor::fromPackedRgb(0xfedcbaU);
            replacement.spectrum.fillOpacity = 0.27;
            replacement.spectrogram.palette = SpectrogramPalette::viridis;
            replacement.spectrogram.colorResponse = -0.75;
            replacement.spectrogram.colorFloorDb = -144.0;
            replacement.spectrogram.colorCeilingDb = 6.0;
            replacement.spectrogram.historyDurationSeconds = 30;
            replacement.spectrogram.historyMode = SpectrogramHistoryMode::overwrite;
            replacement.loudness.referenceLufs = -14.5;
            panel.setConfiguration(replacement);
            expectEquals(callbackCount, 0);
            expectWithinAbsoluteError(
                panel.getConfiguration().spectrum.temporalAveraging.attackMilliseconds, 125.0,
                1.0e-12);
            expectWithinAbsoluteError(
                panel.getConfiguration().spectrum.temporalAveraging.releaseMilliseconds, 500.0,
                1.0e-12);

            auto* fftSizeControl
                = dynamic_cast<juce::ComboBox*>(findDescendantWithId(panel, "settingsFftSize"));
            auto* window
                = dynamic_cast<juce::ComboBox*>(findDescendantWithId(panel, "settingsFftWindow"));
            auto* fftRate = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsFftSliceRate"));
            auto* spacing = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsFrequencySpacing"));
            auto* attack = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumAttack"));
            auto* release = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrumRelease"));
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
            auto* palette = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramPalette"));
            auto* colorResponse = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorResponse"));
            auto* colorFloor = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorFloor"));
            auto* colorCeiling = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsSpectrogramColorCeiling"));
            auto* history = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistory"));
            auto* historyMode = dynamic_cast<juce::ComboBox*>(
                findDescendantWithId(panel, "settingsSpectrogramHistoryMode"));
            auto* reference = dynamic_cast<juce::Slider*>(
                findDescendantWithId(panel, "settingsLoudnessReference"));
            expect(fftSizeControl != nullptr && fftSizeControl->getText() == "8192");
            expect(window != nullptr && window->getText() == "Flat-top");
            expect(fftRate != nullptr && fftRate->getText() == "120 Hz");
            expect(spacing != nullptr && std::abs(spacing->getValue() - 0.5) < 1.0e-12);
            expect(attack != nullptr);
            if (attack != nullptr)
                expectWithinAbsoluteError(attack->getValue(), 125.0, 1.0e-12);
            expect(release != nullptr);
            if (release != nullptr)
                expectWithinAbsoluteError(release->getValue(), 500.0, 1.0e-12);
            expect(slope != nullptr && slope->getText() == "+3 dB/oct");
            expect(peakHoldMode != nullptr && peakHoldMode->getText() == "Finite");
            expect(peakHoldDuration != nullptr && peakHoldDuration->isEnabled());
            if (peakHoldDuration != nullptr)
                expectWithinAbsoluteError(peakHoldDuration->getValue(), 4.25, 1.0e-12);
            expect(traceColor != nullptr && traceColor->getText() == "#FEDCBA");
            expect(fillOpacity != nullptr);
            if (fillOpacity != nullptr)
                expectWithinAbsoluteError(fillOpacity->getValue(), 27.0, 1.0e-12);
            expect(palette != nullptr && palette->getText() == "Viridis");
            expect(colorResponse != nullptr);
            if (colorResponse != nullptr)
                expectWithinAbsoluteError(colorResponse->getValue(), -0.75, 1.0e-12);
            expect(colorFloor != nullptr);
            if (colorFloor != nullptr)
                expectWithinAbsoluteError(colorFloor->getValue(), -144.0, 1.0e-12);
            expect(colorCeiling != nullptr);
            if (colorCeiling != nullptr)
                expectWithinAbsoluteError(colorCeiling->getValue(), 6.0, 1.0e-12);
            expect(history != nullptr && history->getText() == "30 s");
            expect(historyMode != nullptr && historyMode->getText() == "Overwrite");
            expect(reference != nullptr);
            if (reference != nullptr)
                expectWithinAbsoluteError(reference->getValue(), -14.5, 1.0e-12);

            if (traceColor != nullptr) {
                traceColor->setText("#010203", false);
                expect(static_cast<bool>(traceColor->onEscapeKey));
                if (traceColor->onEscapeKey)
                    traceColor->onEscapeKey();
                expectEquals(callbackCount, 0);
                expectEquals(closeCount, 1);
                expectEquals(traceColor->getText(), juce::String("#FEDCBA"));
            }

            expect(panel.keyPressed(juce::KeyPress { juce::KeyPress::escapeKey }));
            expectEquals(closeCount, traceColor != nullptr ? 2 : 1);
        }

        beginTest("Keyboard traversal and accessibility names include enabled settings");
        {
            AnalyzerSettingsPanel panel(
                AnalyzerConfigurationCodec::defaults(), [](const AnalyzerConfiguration&) { },
                [] { });
            panel.setBounds(0, 0, 360, 720);

            auto traverser = panel.createKeyboardFocusTraverser();
            expect(traverser != nullptr);
            if (traverser == nullptr)
                return;

            struct ExpectedControl {
                const char* componentId;
                const char* accessibleTitle;
            };

            constexpr std::array expectedControls {
                ExpectedControl { "settingsClose", "Close" },
                ExpectedControl { "settingsSharedReset", "Shared analysis" },
                ExpectedControl { "settingsFftSize", "FFT size" },
                ExpectedControl { "settingsFftWindow", "FFT window" },
                ExpectedControl { "settingsFftSliceRate", "FFT slice rate" },
                ExpectedControl { "settingsFrequencySpacing", "Frequency spacing" },
                ExpectedControl { "settingsSpectrumReset", "Spectrum" },
                ExpectedControl { "settingsSpectrumFloor", "Spectrum floor" },
                ExpectedControl { "settingsSpectrumCeiling", "Spectrum ceiling" },
                ExpectedControl { "settingsSpectrumAttack", "Spectrum attack" },
                ExpectedControl { "settingsSpectrumRelease", "Spectrum release" },
                ExpectedControl { "settingsSpectrumSlope", "Spectrum slope compensation" },
                ExpectedControl { "settingsSpectrumPeakHoldMode", "Spectrum peak hold mode" },
                ExpectedControl { "settingsSpectrumTraceColor", "Spectrum trace colour" },
                ExpectedControl { "settingsSpectrumFillOpacity", "Spectrum fill opacity" },
                ExpectedControl { "settingsSpectrogramReset", "Spectrogram" },
                ExpectedControl { "settingsSpectrogramPalette", "Spectrogram palette" },
                ExpectedControl {
                    "settingsSpectrogramColorResponse", "Spectrogram colour response" },
                ExpectedControl { "settingsSpectrogramColorFloor", "Spectrogram colour floor" },
                ExpectedControl { "settingsSpectrogramColorCeiling", "Spectrogram colour ceiling" },
                ExpectedControl { "settingsSpectrogramHistory", "Spectrogram history duration" },
                ExpectedControl { "settingsSpectrogramHistoryMode", "Spectrogram history mode" },
                ExpectedControl { "settingsLoudnessReset", "Loudness" },
                ExpectedControl { "settingsLoudnessReference", "Loudness reference" },
            };

            const auto focusComponents = traverser->getAllComponents(&panel);
            auto traversedSettingIndex = std::size_t { 0 };
            for (const auto* focusComponent : focusComponents) {
                const auto componentId = focusComponent->getComponentID();
                if (!componentId.startsWith("settings"))
                    continue;

                expect(traversedSettingIndex < expectedControls.size(),
                    juce::String("Unexpected settings control in the Tab chain: ") + componentId);
                if (traversedSettingIndex < expectedControls.size()) {
                    expectEquals(componentId,
                        juce::String(expectedControls[traversedSettingIndex].componentId));
                }
                ++traversedSettingIndex;
            }
            expectEquals(traversedSettingIndex, expectedControls.size());

            auto previousPosition = focusComponents.cbegin();
            auto hasPreviousPosition = false;

            for (const auto& expectedControl : expectedControls) {
                auto* control = findDescendantWithId(panel, expectedControl.componentId);
                expect(control != nullptr,
                    juce::String("Missing focus control: ") + expectedControl.componentId);
                if (control == nullptr)
                    continue;

                expect(control->isEnabled(),
                    juce::String("Enabled focus control is disabled: ")
                        + expectedControl.componentId);
                expect(control->getWantsKeyboardFocus(),
                    juce::String("Control does not request keyboard focus: ")
                        + expectedControl.componentId);

                const auto position
                    = std::find(focusComponents.cbegin(), focusComponents.cend(), control);
                expect(position != focusComponents.cend(),
                    juce::String("Control is absent from the Tab chain: ")
                        + expectedControl.componentId);
                if (position != focusComponents.cend() && hasPreviousPosition) {
                    expect(previousPosition < position,
                        juce::String("Control is out of Tab order: ")
                            + expectedControl.componentId);
                }
                if (position != focusComponents.cend()) {
                    previousPosition = position;
                    hasPreviousPosition = true;
                }

                auto handler = control->createAccessibilityHandler();
                expect(handler != nullptr,
                    juce::String("Missing accessibility handler: ") + expectedControl.componentId);
                if (handler != nullptr) {
                    expect(handler->getTitle().containsIgnoreCase(expectedControl.accessibleTitle),
                        juce::String("Unexpected accessibility title for ")
                            + expectedControl.componentId + ": " + handler->getTitle());
                }
            }

            auto finiteConfiguration = AnalyzerConfigurationCodec::defaults();
            finiteConfiguration.spectrum.peakHoldMode = SpectrumPeakHoldMode::finite;
            panel.setConfiguration(finiteConfiguration);

            auto* duration = findDescendantWithId(panel, "settingsSpectrumPeakHoldDuration");
            expect(duration != nullptr);
            expect(duration != nullptr && duration->isEnabled());
            expect(duration != nullptr && duration->getWantsKeyboardFocus());
            if (duration != nullptr) {
                const auto finiteFocusComponents = traverser->getAllComponents(&panel);
                const auto durationPosition = std::find(
                    finiteFocusComponents.cbegin(), finiteFocusComponents.cend(), duration);
                const auto modePosition
                    = std::find(finiteFocusComponents.cbegin(), finiteFocusComponents.cend(),
                        findDescendantWithId(panel, "settingsSpectrumPeakHoldMode"));
                const auto colourPosition
                    = std::find(finiteFocusComponents.cbegin(), finiteFocusComponents.cend(),
                        findDescendantWithId(panel, "settingsSpectrumTraceColor"));
                expect(durationPosition != finiteFocusComponents.cend());
                expect(modePosition < durationPosition);
                expect(durationPosition < colourPosition);

                auto handler = duration->createAccessibilityHandler();
                expect(handler != nullptr);
                if (handler != nullptr) {
                    expect(handler->getTitle().containsIgnoreCase(
                        "Spectrum finite peak-hold duration"));
                }
            }
        }
    }
};

AnalyzerSettingsPanelTests analyzerSettingsPanelTests;
} // namespace audio_insight
