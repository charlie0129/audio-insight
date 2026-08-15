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
            auto* metalHud = source.getParameters().getParameter("metalPerformanceHud");
            expect(floor != nullptr);
            expect(metalHud != nullptr);

            if (floor == nullptr || metalHud == nullptr)
                return;

            expectWithinAbsoluteError(metalHud->getValue(), 0.0F, 0.0001F);
            expect(!metalHud->isMetaParameter());
            expect(!metalHud->isAutomatable());

            floor->setValueNotifyingHost(0.375F);
            metalHud->setValueNotifyingHost(1.0F);

            juce::MemoryBlock state;
            source.getStateInformation(state);
            expect(state.getSize() > 0);

            PluginProcessor restored;
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

            const auto* restoredFloor = restored.getParameters().getParameter("spectrumFloor");
            const auto* restoredMetalHud
                = restored.getParameters().getParameter("metalPerformanceHud");
            expect(restoredFloor != nullptr);
            expect(restoredMetalHud != nullptr);

            if (restoredFloor != nullptr)
                expectWithinAbsoluteError(restoredFloor->getValue(), 0.375F, 0.0001F);

            if (restoredMetalHud != nullptr)
                expectWithinAbsoluteError(restoredMetalHud->getValue(), 1.0F, 0.0001F);
        });

        testCase("The custom editor starts detached and inactive", [this] {
            PluginProcessor processor;
            auto* metalHud = processor.getParameters().getParameter("metalPerformanceHud");
            expect(metalHud != nullptr);

            if (metalHud != nullptr)
                metalHud->setValueNotifyingHost(1.0F);

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

            auto* metalHudControl = editor->findChildWithID("metalPerformanceHudToggle");
            expect(metalHudControl != nullptr);

            if (auto* metalHudButton = dynamic_cast<juce::Button*>(metalHudControl)) {
                expect(metalHudButton->getToggleState());

                if (metalHud != nullptr) {
                    metalHud->setValueNotifyingHost(0.0F);
                    expect(!metalHudButton->getToggleState());
                }
            } else {
                expect(false, "Metal HUD control is not a button");
            }

            const auto telemetry = processor.getAnalysisTelemetry();
            expectEquals(telemetry.capture.attemptedChunks, std::uint64_t { 0 });
            expectEquals(telemetry.scheduler.submitted, std::uint64_t { 0 });
        });
    }
};

static PluginProcessorTests pluginProcessorTests;
} // namespace audio_insight
