// SPDX-License-Identifier: AGPL-3.0-or-later

#include "plugin/PluginProcessor.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>

namespace audio_insight {
class PluginProcessorTests final : public juce::UnitTest {
public:
    PluginProcessorTests() : UnitTest("Plugin processor", "audio-insight")
    {
    }

    void runTest() override
    {
        testCase("Mono and stereo layouts are supported", [this] {
            PluginProcessor processor;

            juce::AudioProcessor::BusesLayout mono;
            mono.inputBuses.add(juce::AudioChannelSet::mono());
            mono.outputBuses.add(juce::AudioChannelSet::mono());
            expect(processor.isBusesLayoutSupported(mono));

            juce::AudioProcessor::BusesLayout stereo;
            stereo.inputBuses.add(juce::AudioChannelSet::stereo());
            stereo.outputBuses.add(juce::AudioChannelSet::stereo());
            expect(processor.isBusesLayoutSupported(stereo));

            juce::AudioProcessor::BusesLayout surround;
            surround.inputBuses.add(juce::AudioChannelSet::create5point1());
            surround.outputBuses.add(juce::AudioChannelSet::create5point1());
            expect(!processor.isBusesLayoutSupported(surround));
        });

        testCase("Audio is passed through unchanged", [this] {
            PluginProcessor processor;
            processor.prepareToPlay(48'000.0, 128);

            juce::AudioBuffer<float> audio(2, 128);
            juce::AudioBuffer<float> expected(2, 128);

            for (auto channel = 0; channel < audio.getNumChannels(); ++channel) {
                for (auto sample = 0; sample < audio.getNumSamples(); ++sample) {
                    const auto value = static_cast<float>(
                        std::sin((static_cast<double>(sample) + (channel * 0.25)) * 0.071));
                    audio.setSample(channel, sample, value);
                    expected.setSample(channel, sample, value);
                }
            }

            juce::MidiBuffer midi;
            processor.processBlock(audio, midi);

            // A closed editor must not pay the sample-copy/analysis cost.
            expect(processor.getAnalysisTelemetry().capture.attemptedChunks == 0);

            for (auto channel = 0; channel < audio.getNumChannels(); ++channel)
                expect(audio.getMagnitude(channel, 0, 128) > 0.0F);

            for (auto channel = 0; channel < audio.getNumChannels(); ++channel) {
                for (auto sample = 0; sample < audio.getNumSamples(); ++sample)
                    expectEquals(
                        audio.getSample(channel, sample), expected.getSample(channel, sample));
            }
        });

        testCase("An active editor forwards audio into the bounded capture path", [this] {
            PluginProcessor processor;
            processor.prepareToPlay(48'000.0, 128);
            processor.setVisualizationActive(true);

            juce::AudioBuffer<float> audio(2, 128);
            audio.clear();
            juce::MidiBuffer midi;
            processor.processBlock(audio, midi);

            const auto telemetry = processor.getAnalysisTelemetry();
            expect(telemetry.capture.attemptedChunks == 1);
            expect(telemetry.meters.attemptedBlocks == 1);
            expect(telemetry.latestCaptureRevision == 1);

            processor.setVisualizationActive(false);
        });

        testCase("Parameter state round-trips", [this] {
            PluginProcessor source;
            auto* floor = source.getParameters().getParameter("spectrumFloor");
            auto* metrics = source.getParameters().getParameter("performanceMetrics");
            expect(floor != nullptr);
            expect(metrics != nullptr);

            if (floor == nullptr || metrics == nullptr)
                return;

            expectWithinAbsoluteError(metrics->getValue(), 0.0F, 0.0001F);
            expect(!metrics->isMetaParameter());
            expect(!metrics->isAutomatable());

            floor->setValueNotifyingHost(0.375F);
            metrics->setValueNotifyingHost(1.0F);

            juce::MemoryBlock state;
            source.getStateInformation(state);
            expect(state.getSize() > 0);

            PluginProcessor restored;
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

            const auto* restoredFloor = restored.getParameters().getParameter("spectrumFloor");
            const auto* restoredMetrics
                = restored.getParameters().getParameter("performanceMetrics");
            expect(restoredFloor != nullptr);
            expect(restoredMetrics != nullptr);

            if (restoredFloor != nullptr)
                expectWithinAbsoluteError(restoredFloor->getValue(), 0.375F, 0.0001F);

            if (restoredMetrics != nullptr)
                expectWithinAbsoluteError(restoredMetrics->getValue(), 1.0F, 0.0001F);
        });

        testCase("The legacy Metal HUD state migrates to the metrics panel", [this] {
            PluginProcessor source;
            auto legacyState = source.getParameters().copyState();
            auto metricsState = legacyState.getChildWithProperty("id", "performanceMetrics");
            expect(metricsState.isValid());

            if (!metricsState.isValid())
                return;

            metricsState.setProperty("id", "metalPerformanceHud", nullptr);
            metricsState.setProperty("value", 1.0F, nullptr);

            juce::MemoryBlock state;
            if (auto xml = legacyState.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, state);

            expect(state.getSize() > 0);

            PluginProcessor restored;
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

            const auto* restoredMetrics
                = restored.getParameters().getParameter("performanceMetrics");
            expect(restoredMetrics != nullptr);

            if (restoredMetrics != nullptr)
                expectWithinAbsoluteError(restoredMetrics->getValue(), 1.0F, 0.0001F);
        });

        testCase("Current metrics state wins over and removes stale legacy state", [this] {
            PluginProcessor source;
            auto stateTree = source.getParameters().copyState();
            auto currentState = stateTree.getChildWithProperty("id", "performanceMetrics");
            expect(currentState.isValid());

            if (!currentState.isValid())
                return;

            currentState.setProperty("value", 0.0F, nullptr);
            auto legacyState = currentState.createCopy();
            legacyState.setProperty("id", "metalPerformanceHud", nullptr);
            legacyState.setProperty("value", 1.0F, nullptr);
            stateTree.addChild(legacyState, -1, nullptr);

            juce::MemoryBlock encoded;
            if (auto xml = stateTree.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, encoded);

            PluginProcessor restored;
            restored.setStateInformation(encoded.getData(), static_cast<int>(encoded.getSize()));

            const auto* restoredMetrics
                = restored.getParameters().getParameter("performanceMetrics");
            expect(restoredMetrics != nullptr);
            if (restoredMetrics != nullptr)
                expectWithinAbsoluteError(restoredMetrics->getValue(), 0.0F, 0.0001F);

            juce::MemoryBlock roundTripped;
            restored.getStateInformation(roundTripped);
            auto roundTrippedXml = juce::AudioProcessor::getXmlFromBinary(
                roundTripped.getData(), static_cast<int>(roundTripped.getSize()));
            expect(roundTrippedXml != nullptr);
            if (roundTrippedXml != nullptr)
                expect(!roundTrippedXml->toString().contains("metalPerformanceHud"));
        });

        testCase("Only one of several legacy metrics states is migrated", [this] {
            PluginProcessor source;
            auto stateTree = source.getParameters().copyState();
            auto firstLegacyState = stateTree.getChildWithProperty("id", "performanceMetrics");
            expect(firstLegacyState.isValid());

            if (!firstLegacyState.isValid())
                return;

            firstLegacyState.setProperty("id", "metalPerformanceHud", nullptr);
            firstLegacyState.setProperty("value", 1.0F, nullptr);
            auto duplicateLegacyState = firstLegacyState.createCopy();
            duplicateLegacyState.setProperty("value", 0.0F, nullptr);
            stateTree.addChild(duplicateLegacyState, -1, nullptr);

            juce::MemoryBlock encoded;
            if (auto xml = stateTree.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, encoded);

            PluginProcessor restored;
            restored.setStateInformation(encoded.getData(), static_cast<int>(encoded.getSize()));

            const auto* restoredMetrics
                = restored.getParameters().getParameter("performanceMetrics");
            expect(restoredMetrics != nullptr);
            if (restoredMetrics != nullptr)
                expectWithinAbsoluteError(restoredMetrics->getValue(), 1.0F, 0.0001F);

            juce::MemoryBlock roundTripped;
            restored.getStateInformation(roundTripped);
            auto roundTrippedXml = juce::AudioProcessor::getXmlFromBinary(
                roundTripped.getData(), static_cast<int>(roundTripped.getSize()));
            expect(roundTrippedXml != nullptr);
            if (roundTrippedXml != nullptr) {
                const auto roundTrippedTree = juce::ValueTree::fromXml(*roundTrippedXml);
                auto currentMetricsStates = 0;
                auto legacyMetricsStates = 0;
                for (auto childIndex = 0; childIndex < roundTrippedTree.getNumChildren();
                    ++childIndex) {
                    const auto id = roundTrippedTree.getChild(childIndex)["id"].toString();
                    currentMetricsStates += id == "performanceMetrics" ? 1 : 0;
                    legacyMetricsStates += id == "metalPerformanceHud" ? 1 : 0;
                }

                expectEquals(currentMetricsStates, 1);
                expectEquals(legacyMetricsStates, 0);
            }
        });

        testCase("State without either diagnostics setting keeps the default", [this] {
            PluginProcessor source;
            auto stateTree = source.getParameters().copyState();
            const auto metricsState = stateTree.getChildWithProperty("id", "performanceMetrics");
            expect(metricsState.isValid());
            if (metricsState.isValid())
                stateTree.removeChild(metricsState, nullptr);

            juce::MemoryBlock encoded;
            if (auto xml = stateTree.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, encoded);

            PluginProcessor restored;
            auto* metricsBeforeRestore
                = restored.getParameters().getParameter("performanceMetrics");
            expect(metricsBeforeRestore != nullptr);
            if (metricsBeforeRestore != nullptr)
                metricsBeforeRestore->setValueNotifyingHost(1.0F);

            restored.setStateInformation(encoded.getData(), static_cast<int>(encoded.getSize()));
            const auto* restoredMetrics
                = restored.getParameters().getParameter("performanceMetrics");
            expect(restoredMetrics != nullptr);
            if (restoredMetrics != nullptr)
                expectWithinAbsoluteError(restoredMetrics->getValue(), 0.0F, 0.0001F);
        });

        testCase("The custom editor starts detached and inactive", [this] {
            PluginProcessor processor;
            auto* metrics = processor.getParameters().getParameter("performanceMetrics");
            expect(metrics != nullptr);

            if (metrics != nullptr)
                metrics->setValueNotifyingHost(1.0F);

            std::unique_ptr<juce::AudioProcessorEditor> editor { processor.createEditor() };

            expect(editor != nullptr);

            if (editor == nullptr)
                return;

            expect(editor->isResizable());
            expectEquals(editor->getWidth(), 1200);
            expectEquals(editor->getHeight(), 800);

            if (const auto* constrainer = editor->getConstrainer())
                expectEquals(constrainer->getMinimumWidth(), 720);
            else
                expect(false, "Resizable editor has no bounds constrainer");

            auto* metricsControl = editor->findChildWithID("performanceMetricsToggle");
            auto* metricsPanel = editor->findChildWithID("performanceMetricsPanel");
            expect(metricsControl != nullptr);
            expect(metricsPanel != nullptr);
            expect(metricsPanel != nullptr && metricsPanel->isVisible());

            if (auto* metricsButton = dynamic_cast<juce::Button*>(metricsControl)) {
                expect(metricsButton->getToggleState());

                if (metrics != nullptr) {
                    metrics->setValueNotifyingHost(0.0F);
                    expect(!metricsButton->getToggleState());
                    expect(metricsPanel != nullptr && !metricsPanel->isVisible());
                }
            } else {
                expect(false, "Performance metrics control is not a button");
            }

            const auto telemetry = processor.getAnalysisTelemetry();
            expectEquals(telemetry.capture.attemptedChunks, std::uint64_t { 0 });
            expectEquals(telemetry.scheduler.submitted, std::uint64_t { 0 });
        });
    }
};

static PluginProcessorTests pluginProcessorTests;
} // namespace audio_insight
