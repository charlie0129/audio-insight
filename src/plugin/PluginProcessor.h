// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "analysis/AnalysisCoordinator.h"
#include "state/AnalyzerConfiguration.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <mutex>

namespace audio_insight {
class PluginProcessor final : public juce::AudioProcessor, public VisualizationDataSource {
public:
    class AnalyzerConfigurationListener {
    public:
        virtual ~AnalyzerConfigurationListener() = default;

        /**
            Called synchronously on the non-audio thread that changed the
            configuration. Implementations must marshal UI work to the message
            thread and must not call this API from the audio callback.
        */
        virtual void analyzerConfigurationChanged() noexcept = 0;
    };

    PluginProcessor();
    ~PluginProcessor() override = default;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

    [[nodiscard]] bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    [[nodiscard]] juce::AudioProcessorEditor* createEditor() override;
    [[nodiscard]] bool hasEditor() const override;

    [[nodiscard]] const juce::String getName() const override;
    [[nodiscard]] bool acceptsMidi() const override;
    [[nodiscard]] bool producesMidi() const override;
    [[nodiscard]] bool isMidiEffect() const override;
    [[nodiscard]] double getTailLengthSeconds() const override;

    [[nodiscard]] int getNumPrograms() override;
    [[nodiscard]] int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    [[nodiscard]] const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destinationData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    [[nodiscard]] juce::AudioProcessorValueTreeState& getParameters() noexcept;

    /** Non-audio-thread access to the versioned per-instance analyzer state. */
    [[nodiscard]] AnalyzerConfiguration getAnalyzerConfiguration() const;
    void setAnalyzerConfiguration(AnalyzerConfiguration configuration);
    void addAnalyzerConfigurationListener(AnalyzerConfigurationListener* listener);
    void removeAnalyzerConfigurationListener(AnalyzerConfigurationListener* listener);
    [[nodiscard]] AnalysisTelemetry getAnalysisTelemetry() const noexcept;

    void requestAnalysis() noexcept override;
    void setVisualizationActive(bool shouldBeActive) noexcept override;
    void resetSpectrum() noexcept override;
    void resetPeakRms() noexcept override;
    void resetLoudness() noexcept override;
    [[nodiscard]] bool copyLatestVisualizationFrame(
        VisualizationFrame& destination) const noexcept override;
    [[nodiscard]] bool copyNextSpectrogramColumn(
        SpectrogramColumn& destination) const noexcept override;
    void discardPendingSpectrogramColumns() noexcept override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void replaceAnalyzerConfiguration(
        AnalyzerConfiguration configuration, bool notifyHostOfStateChange);

    juce::AudioProcessorValueTreeState parameters;
    AnalysisCoordinator analysisCoordinator;
    mutable std::mutex analyzerConfigurationMutex;
    AnalyzerConfiguration analyzerConfiguration;
    juce::ThreadSafeListenerList<AnalyzerConfigurationListener> analyzerConfigurationListeners;
    double currentSampleRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};
} // namespace audio_insight
