// SPDX-License-Identifier: AGPL-3.0-or-later

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "ui/MetalVisualization.h"

#include <juce_core/juce_core.h>

#include <memory>

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

class EditorLayoutTemporaryDirectory final {
public:
    EditorLayoutTemporaryDirectory()
        : directory(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("audio-insight-plugin-editor-layout-tests", { }, false))
    {
        created = directory.createDirectory().wasOk();
    }

    ~EditorLayoutTemporaryDirectory()
    {
        if (created)
            directory.deleteRecursively(false);
    }

    [[nodiscard]] bool wasCreated() const noexcept
    {
        return created;
    }

    [[nodiscard]] juce::File child(const juce::String& name) const
    {
        return directory.getChildFile(name);
    }

private:
    juce::File directory;
    bool created = false;
};
} // namespace

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
            const auto telemetry = processor.getAnalysisTelemetry();
            expect(telemetry.capture.attemptedChunks == 0);
            expect(telemetry.audioCallback.callbackCount == 1);
            expect(telemetry.audioCallback.processedFrames == 128);
            expect(telemetry.audioCallback.trackedBlocks[1].callbackCount == 1);
            expect(telemetry.audioCallback.trackedBlocks[1].budgetNanoseconds == 25'000);

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

        testCase("prepareToPlay restarts active analysis only for a changed host format", [this] {
            PluginProcessor processor;
            processor.prepareToPlay(48'000.0, 128);
            processor.setVisualizationActive(true);

            VisualizationFrame initial;
            expect(processor.copyLatestVisualizationFrame(initial));
            expect(initial.generation != 0);

            processor.prepareToPlay(96'000.0, 128);
            VisualizationFrame changed;
            expect(processor.copyLatestVisualizationFrame(changed));
            expect(changed.generation > initial.generation);
            expect(!changed.spectrumValid && !changed.meterValid);

            processor.prepareToPlay(96'000.0, 128);
            expect(!processor.copyLatestVisualizationFrame(changed));
            processor.setVisualizationActive(false);
        });

        testCase("Parameter state round-trips", [this] {
            PluginProcessor source;
            auto* floor = source.getParameters().getParameter("spectrumFloor");
            auto* smoothing = source.getParameters().getParameter("spectrumSmoothing");
            auto* metrics = source.getParameters().getParameter("performanceMetrics");
            expect(floor != nullptr);
            expect(smoothing != nullptr);
            expect(metrics != nullptr);

            if (floor == nullptr || smoothing == nullptr || metrics == nullptr)
                return;

            expectWithinAbsoluteError(smoothing->getValue(), 0.40F, 0.0001F);
            expectWithinAbsoluteError(metrics->getValue(), 0.0F, 0.0001F);
            expect(!floor->isAutomatable());
            expect(!smoothing->isAutomatable());
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

        testCase("Analyzer configuration round-trips beside compatibility parameters", [this] {
            PluginProcessor source;
            auto configuration = source.getAnalyzerConfiguration();
            configuration.sharedAnalysis.fftSize = 8192;
            configuration.sharedAnalysis.frequencySpacing = 0.35;
            configuration.spectrum.floorDb = -132.0;
            configuration.spectrum.ceilingDb = 6.0;
            configuration.spectrum.temporalAveraging.enabled = false;
            configuration.spectrum.temporalAveraging.milliseconds = 250.0;
            configuration.spectrogram.historyDurationSeconds = 30;
            configuration.loudness.referenceLufs = -14.5;
            source.setAnalyzerConfiguration(configuration);

            juce::MemoryBlock state;
            source.getStateInformation(state);
            auto xml = juce::AudioProcessor::getXmlFromBinary(
                state.getData(), static_cast<int>(state.getSize()));
            expect(xml != nullptr);

            if (xml != nullptr) {
                const auto tree = juce::ValueTree::fromXml(*xml);
                auto analyzerChildren = 0;
                for (const auto& child : tree) {
                    analyzerChildren
                        += child.hasType(AnalyzerConfigurationCodec::treeType()) ? 1 : 0;
                }
                expectEquals(analyzerChildren, 1);
            }

            PluginProcessor restored;
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            const auto actual = restored.getAnalyzerConfiguration();
            expectEquals(actual.sharedAnalysis.fftSize, 8192);
            expectWithinAbsoluteError(actual.sharedAnalysis.frequencySpacing, 0.35, 1.0e-12);
            expectWithinAbsoluteError(actual.spectrum.floorDb, -132.0, 1.0e-12);
            expectWithinAbsoluteError(actual.spectrum.ceilingDb, 6.0, 1.0e-12);
            expect(!actual.spectrum.temporalAveraging.enabled);
            expectWithinAbsoluteError(
                actual.spectrum.temporalAveraging.milliseconds, 250.0, 1.0e-12);
            expectEquals(actual.spectrogram.historyDurationSeconds, 30);
            expectWithinAbsoluteError(actual.loudness.referenceLufs, -14.5, 1.0e-12);
        });

        testCase("Restored FFT settings reach analysis without creating an editor", [this] {
            PluginProcessor source;
            auto configuration = source.getAnalyzerConfiguration();
            configuration.sharedAnalysis.fftSize = 16'384;
            configuration.sharedAnalysis.window = FftWindow::fiveTermFlatTop;
            configuration.sharedAnalysis.requestedFftSliceRateHz = 120;
            source.setAnalyzerConfiguration(configuration);

            juce::MemoryBlock state;
            source.getStateInformation(state);

            PluginProcessor restored;
            const auto beforeRestore = restored.getAnalysisTelemetry();
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            const auto afterRestore = restored.getAnalysisTelemetry();

            expect(afterRestore.configuredFftSize == 16'384);
            expect(afterRestore.configuredFftWindow
                == static_cast<std::uint32_t>(FftWindow::fiveTermFlatTop));
            expect(afterRestore.requestedFftSliceRateHz == 120);
            expect(afterRestore.fftGeneration > beforeRestore.fftGeneration);
            expect(
                afterRestore.fftConfigurationChanges == beforeRestore.fftConfigurationChanges + 1);
            expectEquals(afterRestore.capture.attemptedChunks, std::uint64_t { 0 });
            expectEquals(afterRestore.scheduler.submitted, std::uint64_t { 0 });
        });

        testCase("Presentation-only analyzer edits do not reconfigure the FFT", [this] {
            PluginProcessor processor;
            const auto beforeEdit = processor.getAnalysisTelemetry();
            auto configuration = processor.getAnalyzerConfiguration();
            configuration.sharedAnalysis.frequencySpacing = 0.25;
            configuration.spectrum.floorDb = -120.0;
            configuration.spectrum.ceilingDb = 6.0;
            configuration.spectrum.slope = SpectrumSlope::db4Point5PerOctave;
            configuration.spectrum.fillOpacity = 0.42;
            configuration.spectrum.traceColor = SrgbColor::fromPackedRgb(0x123456U);
            processor.setAnalyzerConfiguration(configuration);
            const auto afterEdit = processor.getAnalysisTelemetry();

            expectEquals(afterEdit.fftGeneration, beforeEdit.fftGeneration);
            expectEquals(afterEdit.fftConfigurationChanges, beforeEdit.fftConfigurationChanges);
            expect(afterEdit.configuredFftSize == beforeEdit.configuredFftSize);
            expect(afterEdit.configuredFftWindow == beforeEdit.configuredFftWindow);
            expect(afterEdit.requestedFftSliceRateHz == beforeEdit.requestedFftSliceRateHz);
            expect(afterEdit.spectrumTemporalConfigurationChanges
                == beforeEdit.spectrumTemporalConfigurationChanges);
            expect(
                afterEdit.spectrogramMappingGeneration > beforeEdit.spectrogramMappingGeneration);
            expect(afterEdit.spectrogramMappingChanges == beforeEdit.spectrogramMappingChanges + 1);
        });

        testCase("Spectrogram colour edits do not rebuild its frequency mapping", [this] {
            PluginProcessor processor;
            const auto beforeEdit = processor.getAnalysisTelemetry();
            auto configuration = processor.getAnalyzerConfiguration();
            configuration.spectrogram.palette = SpectrogramPalette::inferno;
            configuration.spectrogram.colorResponse = -1.25;
            configuration.spectrogram.colorFloorDb = -150.0;
            configuration.spectrogram.colorCeilingDb = 6.0;
            configuration.spectrogram.historyDurationSeconds = 30;
            configuration.spectrogram.historyMode = SpectrogramHistoryMode::overwrite;
            processor.setAnalyzerConfiguration(configuration);
            const auto afterEdit = processor.getAnalysisTelemetry();

            expectEquals(afterEdit.fftGeneration, beforeEdit.fftGeneration);
            expectEquals(afterEdit.fftConfigurationChanges, beforeEdit.fftConfigurationChanges);
            expectEquals(
                afterEdit.spectrogramMappingGeneration, beforeEdit.spectrogramMappingGeneration);
            expectEquals(afterEdit.spectrogramMappingChanges, beforeEdit.spectrogramMappingChanges);
        });

        testCase("Temporal analyzer edits remain scoped away from the FFT generation", [this] {
            PluginProcessor processor;
            const auto beforeEdit = processor.getAnalysisTelemetry();
            auto configuration = processor.getAnalyzerConfiguration();
            configuration.spectrum.temporalAveraging.milliseconds = 125.0;
            configuration.spectrum.peakHoldMode = SpectrumPeakHoldMode::finite;
            processor.setAnalyzerConfiguration(configuration);
            const auto afterEdit = processor.getAnalysisTelemetry();

            expectEquals(afterEdit.fftGeneration, beforeEdit.fftGeneration);
            expectEquals(afterEdit.fftConfigurationChanges, beforeEdit.fftConfigurationChanges);
            expect(afterEdit.spectrumTemporalConfigurationChanges
                == beforeEdit.spectrumTemporalConfigurationChanges + 1);
        });

        testCase("Fresh state uses responsive time-based averaging defaults", [this] {
            PluginProcessor processor;
            const auto configuration = processor.getAnalyzerConfiguration();
            expect(configuration.spectrum.temporalAveraging.enabled);
            expectWithinAbsoluteError(
                configuration.spectrum.temporalAveraging.milliseconds, 75.0, 1.0e-12);
        });

        testCase("Analyzer configuration listeners observe changes until removed", [this] {
            class Listener final : public PluginProcessor::AnalyzerConfigurationListener {
            public:
                void analyzerConfigurationChanged() noexcept override
                {
                    ++notifications;
                }

                int notifications = 0;
            };

            class HostListener final : public juce::AudioProcessorListener {
            public:
                void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override
                {
                }

                void audioProcessorChanged(
                    juce::AudioProcessor*, const ChangeDetails& details) override
                {
                    ++notifications;
                    observedNonParameterStateChange
                        = observedNonParameterStateChange || details.nonParameterStateChanged;
                }

                int notifications = 0;
                bool observedNonParameterStateChange = false;
            };

            PluginProcessor processor;
            Listener listener;
            HostListener hostListener;
            processor.addAnalyzerConfigurationListener(&listener);
            processor.addListener(&hostListener);

            auto configuration = processor.getAnalyzerConfiguration();
            configuration.spectrum.floorDb = -108.0;
            processor.setAnalyzerConfiguration(configuration);
            expectEquals(listener.notifications, 1);
            expectEquals(hostListener.notifications, 1);
            expect(hostListener.observedNonParameterStateChange);

            processor.removeAnalyzerConfigurationListener(&listener);
            configuration.spectrum.floorDb = -120.0;
            processor.setAnalyzerConfiguration(configuration);
            expectEquals(listener.notifications, 1);
            expectEquals(hostListener.notifications, 2);
            processor.removeListener(&hostListener);

            juce::MemoryBlock encoded;
            processor.getStateInformation(encoded);
            PluginProcessor restored;
            HostListener restoreHostListener;
            restored.addListener(&restoreHostListener);
            restored.setStateInformation(encoded.getData(), static_cast<int>(encoded.getSize()));
            expectEquals(restoreHostListener.notifications, 0);
            restored.removeListener(&restoreHostListener);
        });

        testCase("Legacy Spectrum parameters migrate only when configuration is absent", [this] {
            PluginProcessor source;
            auto* floor = source.getParameters().getParameter("spectrumFloor");
            auto* ceiling = source.getParameters().getParameter("spectrumCeiling");
            auto* smoothing = source.getParameters().getParameter("spectrumSmoothing");
            expect(floor != nullptr);
            expect(ceiling != nullptr);
            expect(smoothing != nullptr);
            if (floor == nullptr || ceiling == nullptr || smoothing == nullptr)
                return;

            floor->setValueNotifyingHost(floor->convertTo0to1(-100.0F));
            ceiling->setValueNotifyingHost(ceiling->convertTo0to1(6.0F));
            smoothing->setValueNotifyingHost(0.40F);

            auto legacyTree = source.getParameters().copyState();
            expect(!legacyTree.getChildWithName(AnalyzerConfigurationCodec::treeType()).isValid());
            juce::MemoryBlock legacyState;
            if (auto xml = legacyTree.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, legacyState);

            PluginProcessor restored;
            restored.setStateInformation(
                legacyState.getData(), static_cast<int>(legacyState.getSize()));
            const auto migrated = restored.getAnalyzerConfiguration();
            expectWithinAbsoluteError(migrated.spectrum.floorDb, -100.0, 1.0e-12);
            expectWithinAbsoluteError(migrated.spectrum.ceilingDb, 6.0, 1.0e-12);
            expectWithinAbsoluteError(
                migrated.spectrum.temporalAveraging.milliseconds, 84.6, 1.0e-5);

            juce::MemoryBlock currentState;
            restored.getStateInformation(currentState);
            auto currentXml = juce::AudioProcessor::getXmlFromBinary(
                currentState.getData(), static_cast<int>(currentState.getSize()));
            expect(currentXml != nullptr);
            if (currentXml != nullptr) {
                const auto currentTree = juce::ValueTree::fromXml(*currentXml);
                expect(
                    currentTree.getChildWithName(AnalyzerConfigurationCodec::treeType()).isValid());
            }
        });

        testCase("Current analyzer configuration wins over compatibility shims", [this] {
            PluginProcessor source;
            auto stateTree = source.getParameters().copyState();
            auto configuration = AnalyzerConfigurationCodec::defaults();
            configuration.spectrum.floorDb = -144.0;
            configuration.spectrum.temporalAveraging.milliseconds = 125.0;
            stateTree.addChild(AnalyzerConfigurationCodec::encode(configuration), -1, nullptr);

            auto floorState = stateTree.getChildWithProperty("id", "spectrumFloor");
            auto smoothingState = stateTree.getChildWithProperty("id", "spectrumSmoothing");
            floorState.setProperty("value", "-72", nullptr);
            smoothingState.setProperty("value", "1", nullptr);

            juce::MemoryBlock encoded;
            if (auto xml = stateTree.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, encoded);

            PluginProcessor restored;
            restored.setStateInformation(encoded.getData(), static_cast<int>(encoded.getSize()));
            const auto actual = restored.getAnalyzerConfiguration();
            expectWithinAbsoluteError(actual.spectrum.floorDb, -144.0, 1.0e-12);
            expectWithinAbsoluteError(
                actual.spectrum.temporalAveraging.milliseconds, 125.0, 1.0e-12);
        });

        testCase("Malformed existing analyzer state defaults instead of legacy migration", [this] {
            PluginProcessor source;
            auto stateTree = source.getParameters().copyState();
            auto floorState = stateTree.getChildWithProperty("id", "spectrumFloor");
            auto smoothingState = stateTree.getChildWithProperty("id", "spectrumSmoothing");
            floorState.setProperty("value", "-120", nullptr);
            smoothingState.setProperty("value", "1", nullptr);

            auto malformed = AnalyzerConfigurationCodec::encode({ });
            malformed.setProperty("version", "999", nullptr);
            stateTree.addChild(malformed, -1, nullptr);

            juce::MemoryBlock encoded;
            if (auto xml = stateTree.createXml())
                juce::AudioProcessor::copyXmlToBinary(*xml, encoded);

            PluginProcessor restored;
            restored.setStateInformation(encoded.getData(), static_cast<int>(encoded.getSize()));
            const auto actual = restored.getAnalyzerConfiguration();
            expectWithinAbsoluteError(actual.spectrum.floorDb, -90.0, 1.0e-12);
            expectWithinAbsoluteError(
                actual.spectrum.temporalAveraging.milliseconds, 75.0, 1.0e-12);
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
            EditorLayoutTemporaryDirectory temporary;
            expect(temporary.wasCreated());
            if (!temporary.wasCreated())
                return;

            const auto layoutFile = temporary.child("dashboard-layout.json");
            const DashboardLayoutStore layoutStore(layoutFile);
            constexpr DashboardLayoutSplits initialLayout { 20, 32, 24, 38 };
            expect(layoutStore.commit(initialLayout));
            auto* metrics = processor.getParameters().getParameter("performanceMetrics");
            expect(metrics != nullptr);

            if (metrics != nullptr)
                metrics->setValueNotifyingHost(1.0F);

            std::unique_ptr<juce::AudioProcessorEditor> editor { new PluginEditor(
                processor, processor, layoutStore) };

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
            auto* settingsControl = editor->findChildWithID("analyzerSettingsToggle");
            auto* settingsPanel = editor->findChildWithID("analyzerSettingsPanel");
            auto* aboutControl = editor->findChildWithID("aboutToggle");
            auto* aboutPanel = editor->findChildWithID("aboutPanel");
            auto* aboutCloseControl
                = aboutPanel != nullptr ? findDescendantWithId(*aboutPanel, "aboutClose") : nullptr;
            auto* aboutViewportComponent = aboutPanel != nullptr
                ? findDescendantWithId(*aboutPanel, "aboutViewport")
                : nullptr;
            auto* aboutScrollableContent = aboutPanel != nullptr
                ? findDescendantWithId(*aboutPanel, "aboutScrollableContent")
                : nullptr;
            auto* aboutSponsorLink = aboutPanel != nullptr
                ? findDescendantWithId(*aboutPanel, "aboutSponsorLink")
                : nullptr;
            auto* editLayoutControl = editor->findChildWithID("dashboardLayoutEditToggle");
            auto* doneLayoutControl = editor->findChildWithID("dashboardLayoutDone");
            auto* cancelLayoutControl = editor->findChildWithID("dashboardLayoutCancel");
            auto* resetLayoutControl = editor->findChildWithID("dashboardLayoutReset");
            auto* visualizationComponent = editor->findChildWithID("metalVisualization");
            expect(metricsControl != nullptr);
            expect(metricsPanel != nullptr);
            expect(settingsControl != nullptr);
            expect(settingsPanel != nullptr);
            expect(aboutControl != nullptr);
            expect(aboutPanel != nullptr);
            expect(aboutCloseControl != nullptr);
            expect(aboutViewportComponent != nullptr);
            expect(aboutScrollableContent != nullptr);
            expect(aboutSponsorLink != nullptr);
            expect(editLayoutControl != nullptr);
            expect(doneLayoutControl != nullptr);
            expect(cancelLayoutControl != nullptr);
            expect(resetLayoutControl != nullptr);
            expect(visualizationComponent != nullptr);
            expect(metricsPanel != nullptr && metricsPanel->isVisible());
            expect(settingsPanel != nullptr && !settingsPanel->isVisible());
            expect(aboutPanel != nullptr && !aboutPanel->isVisible());

            auto* metricsButton = dynamic_cast<juce::Button*>(metricsControl);
            auto* settingsButton = dynamic_cast<juce::Button*>(settingsControl);
            auto* aboutButton = dynamic_cast<juce::Button*>(aboutControl);
            auto* aboutCloseButton = dynamic_cast<juce::Button*>(aboutCloseControl);
            auto* aboutViewport = dynamic_cast<juce::Viewport*>(aboutViewportComponent);
            auto* editLayoutButton = dynamic_cast<juce::Button*>(editLayoutControl);
            auto* doneLayoutButton = dynamic_cast<juce::Button*>(doneLayoutControl);
            auto* cancelLayoutButton = dynamic_cast<juce::Button*>(cancelLayoutControl);
            auto* resetLayoutButton = dynamic_cast<juce::Button*>(resetLayoutControl);
            auto* visualization = dynamic_cast<MetalVisualization*>(visualizationComponent);
            expect(metricsButton != nullptr);
            expect(settingsButton != nullptr);
            expect(aboutButton != nullptr);
            expect(aboutCloseButton != nullptr);
            expect(aboutViewport != nullptr);
            expect(editLayoutButton != nullptr);
            expect(doneLayoutButton != nullptr);
            expect(cancelLayoutButton != nullptr);
            expect(resetLayoutButton != nullptr);
            expect(visualization != nullptr);

            if (editLayoutButton != nullptr) {
                expect(editLayoutButton->isVisible());
                expect(!editLayoutButton->isEnabled());
            }

            if (doneLayoutButton != nullptr && cancelLayoutButton != nullptr
                && resetLayoutButton != nullptr) {
                expect(!doneLayoutButton->isVisible());
                expect(!cancelLayoutButton->isVisible());
                expect(!resetLayoutButton->isVisible());
            }

            if (visualization != nullptr) {
                expect(visualization->getDashboardLayoutSplits() == initialLayout);
                const auto renderSettings = visualization->getSpectrumSettings();
                expectWithinAbsoluteError(renderSettings.slopeDecibelsPerOctave, 0.0F, 0.0001F);
                expectWithinAbsoluteError(renderSettings.frequencySpacing, 1.0F, 0.0001F);
                expectWithinAbsoluteError(renderSettings.fillOpacity, 0.18F, 0.0001F);
                expect(renderSettings.traceColourRgb == 0x55c7e8U);
            }

            if (settingsButton != nullptr && settingsPanel != nullptr && metricsPanel != nullptr
                && metricsButton != nullptr && aboutButton != nullptr && aboutPanel != nullptr
                && aboutCloseButton != nullptr && aboutViewport != nullptr
                && aboutScrollableContent != nullptr && aboutSponsorLink != nullptr
                && visualizationComponent != nullptr) {
                settingsButton->onClick();
                expect(settingsButton->getToggleState());
                expect(settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(metricsButton->getToggleState());
                expect(metricsButton->getDescription().containsIgnoreCase("temporarily hidden"));
                if (metrics != nullptr)
                    expectWithinAbsoluteError(metrics->getValue(), 1.0F, 0.0001F);

                aboutButton->onClick();
                expect(aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                expect(!aboutButton->isVisible());
                expect(metricsButton->getToggleState());
                expect(aboutPanel->getBounds() == juce::Rectangle<int>(600, 52, 600, 748));
                expect(
                    visualizationComponent->getBounds() == juce::Rectangle<int>(0, 52, 599, 748));

                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(settingsButton->getToggleState());
                expect(settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                expect(metricsButton->getToggleState());

                aboutButton->onClick();
                expect(aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());

                editor->setSize(720, 420);
                expect(aboutPanel->getBounds() == juce::Rectangle<int>(360, 52, 360, 368),
                    "Unexpected minimum-size About bounds: " + aboutPanel->getBounds().toString());
                expect(visualizationComponent->getBounds() == juce::Rectangle<int>(0, 52, 359, 368),
                    "Unexpected minimum-size Metal bounds: "
                        + visualizationComponent->getBounds().toString());
                expect(aboutCloseButton->isVisible()
                        && aboutPanel->getLocalBounds().contains(aboutCloseButton->getBounds()),
                    "About Close is not reachable at minimum size");
                expect(aboutScrollableContent->getHeight() > aboutViewport->getViewHeight(),
                    "About content does not overflow its minimum-size viewport: content="
                        + juce::String(aboutScrollableContent->getHeight())
                        + ", view=" + juce::String(aboutViewport->getViewHeight()));
                expect(aboutSponsorLink->getBottom() <= aboutScrollableContent->getHeight(),
                    "The final About link lies outside the scrollable content");
                expectEquals(aboutViewport->getViewPositionY(), 0,
                    "The newly opened About viewport did not begin at the top");

                const auto eventTime = juce::Time::getCurrentTime();
                const auto targetPoint = juce::Point<float> { 20.0F, 20.0F };
                const juce::MouseEvent wheelEvent(juce::Desktop::getInstance().getMainMouseSource(),
                    targetPoint, juce::ModifierKeys { }, juce::MouseInputSource::defaultPressure,
                    juce::MouseInputSource::defaultOrientation,
                    juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                    juce::MouseInputSource::defaultTiltY, aboutScrollableContent,
                    aboutScrollableContent, eventTime, targetPoint, eventTime, 0, false);
                constexpr juce::MouseWheelDetails wheel {
                    0.0F,
                    -1.0F,
                    false,
                    false,
                    false,
                };
                aboutScrollableContent->mouseWheelMove(wheelEvent, wheel);
                expect(aboutViewport->getViewPositionY() > 0,
                    "A downward wheel event did not scroll the minimum-size About viewport");

                editor->setSize(2000, 800);
                expect(aboutPanel->getBounds() == juce::Rectangle<int>(1300, 52, 700, 748));
                expect(
                    visualizationComponent->getBounds() == juce::Rectangle<int>(0, 52, 1299, 748));

                editor->setSize(1200, 800);
                settingsButton->onClick();
                expect(!settingsButton->getToggleState());
                expect(!settingsPanel->isVisible());
                expect(metricsButton->getToggleState());
                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(!settingsButton->getToggleState());
                expect(!settingsPanel->isVisible());
                expect(metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                expect(aboutButton->isVisible());
                expect(metricsButton->getToggleState());
                if (metrics != nullptr)
                    expectWithinAbsoluteError(metrics->getValue(), 1.0F, 0.0001F);

                aboutButton->onClick();
                expect(aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                expect(metricsButton->getToggleState());

                aboutButton->onClick();
                metricsButton->onClick();
                expect(!metricsButton->getToggleState());
                if (metrics != nullptr)
                    expectWithinAbsoluteError(metrics->getValue(), 0.0F, 0.0001F);
                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());

                aboutButton->onClick();
                expect(aboutPanel->isVisible());
                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());

                aboutButton->onClick();
                settingsButton->onClick();
                expect(settingsButton->getToggleState());
                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                expect(!metricsButton->getToggleState());

                editor->setSize(1079, 800);
                expect(settingsPanel->isVisible());
                expect(settingsPanel->getBounds() == juce::Rectangle<int>(0, 52, 1079, 748));
                expect(visualizationComponent != nullptr && !visualizationComponent->isVisible());

                editor->setSize(1080, 800);
                expect(settingsPanel->getBounds() == juce::Rectangle<int>(720, 52, 360, 748));
                expect(visualizationComponent != nullptr && visualizationComponent->isVisible());
                if (visualizationComponent != nullptr)
                    expectEquals(visualizationComponent->getWidth(), 720);

                if (auto* floorControl
                    = findDescendantWithId(*settingsPanel, "settingsSpectrumFloor")) {
                    if (auto* floorSlider = dynamic_cast<juce::Slider*>(floorControl)) {
                        floorSlider->setValue(-120.0, juce::sendNotificationSync);
                        expectWithinAbsoluteError(
                            processor.getAnalyzerConfiguration().spectrum.floorDb, -120.0, 1.0e-12);
                        if (visualization != nullptr) {
                            expectWithinAbsoluteError(
                                visualization->getSpectrumSettings().floorDecibels, -120.0F,
                                0.0001F);
                        }
                    } else {
                        expect(false, "Spectrum floor Settings control is not a slider");
                    }
                } else {
                    expect(false, "Spectrum floor Settings control was not found");
                }

                metricsButton->onClick();
                expect(!settingsButton->getToggleState());
                expect(!settingsPanel->isVisible());
                expect(metricsPanel->isVisible());
                expect(metricsButton->getToggleState());
                if (metrics != nullptr)
                    expectWithinAbsoluteError(metrics->getValue(), 1.0F, 0.0001F);

                metricsButton->onClick();
                expect(!metricsButton->getToggleState());
                expect(!metricsPanel->isVisible());
                if (metrics != nullptr)
                    expectWithinAbsoluteError(metrics->getValue(), 0.0F, 0.0001F);

                editor->setSize(720, 420);
                settingsButton->onClick();
                expect(settingsButton->getToggleState());
                expect(settingsPanel->isVisible());
                expect(settingsPanel->getBounds() == juce::Rectangle<int>(0, 52, 720, 368));
                expect(!visualizationComponent->isVisible());
                aboutButton->onClick();
                expect(aboutPanel->isVisible());
                expect(!settingsPanel->isVisible());
                expect(!metricsPanel->isVisible());
                expect(!visualizationComponent->isVisible());
                expect(aboutPanel->getBounds() == juce::Rectangle<int>(0, 52, 720, 368));
                aboutCloseButton->onClick();
                expect(!aboutPanel->isVisible());
                expect(settingsPanel->isVisible());
                expect(settingsPanel->getBounds() == juce::Rectangle<int>(0, 52, 720, 368));
                expect(!metricsPanel->isVisible());
                expect(!visualizationComponent->isVisible());

                settingsButton->onClick();
                expect(!settingsPanel->isVisible());
                expect(visualizationComponent->isVisible());
                editor->setSize(1200, 800);

                if (editLayoutButton != nullptr && doneLayoutButton != nullptr
                    && cancelLayoutButton != nullptr && resetLayoutButton != nullptr
                    && visualization != nullptr) {
                    expect(editLayoutButton->isEnabled());
                    const auto originalSplits = visualization->getDashboardLayoutSplits();
                    editLayoutButton->onClick();
                    expect(visualization->isDashboardLayoutEditing());
                    expect(!editLayoutButton->isVisible());
                    expect(doneLayoutButton->isVisible());
                    expect(cancelLayoutButton->isVisible());
                    expect(resetLayoutButton->isVisible());

                    visualization->setDashboardLayoutSplits({ 14, 24, 16, 36 });
                    resetLayoutButton->onClick();
                    expect(visualization->getDashboardLayoutSplits()
                        == DashboardLayout::defaultSplits);

                    visualization->setDashboardLayoutSplits({ 14, 24, 16, 36 });
                    cancelLayoutButton->onClick();
                    expect(!visualization->isDashboardLayoutEditing());
                    expect(visualization->getDashboardLayoutSplits() == originalSplits);
                    expect(editLayoutButton->isVisible());
                    expect(!doneLayoutButton->isVisible());

                    editor->setVisible(true);
                    editLayoutButton->onClick();
                    visualization->setDashboardLayoutSplits({ 26, 40, 34, 42 });
                    editor->setVisible(false);
                    expect(!visualization->isDashboardLayoutEditing());
                    expect(visualization->getDashboardLayoutSplits() == originalSplits);
                    editor->setVisible(true);

                    editLayoutButton->onClick();
                    constexpr DashboardLayoutSplits savedLayout { 26, 40, 34, 42 };
                    visualization->setDashboardLayoutSplits(savedLayout);
                    doneLayoutButton->onClick();
                    expect(!visualization->isDashboardLayoutEditing());
                    expect(visualization->getDashboardLayoutSplits() == savedLayout);
                    expect(layoutStore.load() == savedLayout);
                    expectEquals(doneLayoutButton->getButtonText(), juce::String("Done"));
                }
            }

            const auto telemetry = processor.getAnalysisTelemetry();
            expectEquals(telemetry.capture.attemptedChunks, std::uint64_t { 0 });
            expectEquals(telemetry.scheduler.submitted, std::uint64_t { 0 });
        });
    }
};

static PluginProcessorTests pluginProcessorTests;
} // namespace audio_insight
