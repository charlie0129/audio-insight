// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace audio_insight {
namespace {
constexpr auto stateTreeName = "AudioInsightState";
constexpr auto spectrumFloorParameter = "spectrumFloor";
constexpr auto spectrumCeilingParameter = "spectrumCeiling";
constexpr auto spectrumSmoothingParameter = "spectrumSmoothing";
constexpr auto performanceMetricsParameter = "performanceMetrics";
constexpr auto legacyMetalPerformanceHudParameter = "metalPerformanceHud";

std::optional<double> readLegacyParameterValue(
    const juce::ValueTree& state, const char* parameterId)
{
    const auto parameterState = state.getChildWithProperty("id", parameterId);
    if (!parameterState.isValid() || !parameterState.hasProperty("value"))
        return std::nullopt;

    const auto value = parameterState.getProperty("value");
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

    return std::isfinite(number) ? std::optional<double> { number } : std::nullopt;
}

int countAnalyzerConfigurationChildren(const juce::ValueTree& state)
{
    auto count = 0;
    for (const auto& child : state)
        count += child.hasType(AnalyzerConfigurationCodec::treeType()) ? 1 : 0;
    return count;
}

void removeAnalyzerConfigurationChildren(juce::ValueTree& state)
{
    for (auto childIndex = state.getNumChildren() - 1; childIndex >= 0; --childIndex) {
        if (state.getChild(childIndex).hasType(AnalyzerConfigurationCodec::treeType()))
            state.removeChild(childIndex, nullptr);
    }
}

SpectrumAnalysisConfiguration spectrumAnalysisConfiguration(
    const AnalyzerConfiguration& configuration) noexcept
{
    return { static_cast<std::size_t>(configuration.sharedAnalysis.fftSize),
        configuration.sharedAnalysis.window, configuration.sharedAnalysis.requestedFftSliceRateHz };
}

SpectrumTemporalConfiguration spectrumTemporalConfiguration(
    const AnalyzerConfiguration& configuration) noexcept
{
    return { configuration.spectrum.temporalAveraging.enabled,
        configuration.spectrum.temporalAveraging.milliseconds, configuration.spectrum.peakHoldMode,
        configuration.spectrum.finitePeakHoldSeconds };
}
} // namespace

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, stateTreeName, createParameterLayout())
{
}

void PluginProcessor::prepareToPlay(const double sampleRate, int)
{
    analysisCoordinator.setCaptureFormat(
        sampleRate, static_cast<std::uint32_t>(getTotalNumInputChannels()));
    currentSampleRate = sampleRate;
}

void PluginProcessor::releaseResources()
{
    currentSampleRate = 0.0;
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals disableDenormals;

    const auto inputChannels = getTotalNumInputChannels();
    const auto outputChannels = getTotalNumOutputChannels();

    for (auto channel = inputChannels; channel < outputChannels; ++channel)
        audio.clear(channel, 0, audio.getNumSamples());

    if (inputChannels > 0 && audio.getNumSamples() > 0 && currentSampleRate > 0.0) {
        const auto* const left = audio.getReadPointer(0);
        const auto* const right = inputChannels > 1 ? audio.getReadPointer(1) : nullptr;
        analysisCoordinator.captureAudioBlock(left, right,
            static_cast<std::size_t>(audio.getNumSamples()), currentSampleRate,
            static_cast<std::uint32_t>(inputChannels));
    }

    // The processor is intentionally transparent. JUCE supplies the input and
    // output in the same buffer for this effect, so no sample copy is needed.
}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();

    if (output != juce::AudioChannelSet::mono() && output != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == output;
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new PluginEditor(*this, *this);
}

bool PluginProcessor::hasEditor() const
{
    return true;
}

const juce::String PluginProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PluginProcessor::acceptsMidi() const
{
    return false;
}

bool PluginProcessor::producesMidi() const
{
    return false;
}

bool PluginProcessor::isMidiEffect() const
{
    return false;
}

double PluginProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PluginProcessor::getNumPrograms()
{
    return 1;
}

int PluginProcessor::getCurrentProgram()
{
    return 0;
}

void PluginProcessor::setCurrentProgram(int)
{
}

const juce::String PluginProcessor::getProgramName(int)
{
    return { };
}

void PluginProcessor::changeProgramName(int, const juce::String&)
{
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    auto state = parameters.copyState();
    removeAnalyzerConfigurationChildren(state);
    state.addChild(AnalyzerConfigurationCodec::encode(getAnalyzerConfiguration()), -1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destinationData);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes); xml != nullptr) {
        if (xml->hasTagName(parameters.state.getType())) {
            auto restoredState = juce::ValueTree::fromXml(*xml);
            const auto analyzerConfigurationCount
                = countAnalyzerConfigurationChildren(restoredState);
            auto restoredAnalyzerConfiguration = AnalyzerConfigurationCodec::defaults();

            if (analyzerConfigurationCount == 1) {
                restoredAnalyzerConfiguration = AnalyzerConfigurationCodec::decodeOrDefault(
                    restoredState.getChildWithName(AnalyzerConfigurationCodec::treeType()));
            } else if (analyzerConfigurationCount == 0) {
                restoredAnalyzerConfiguration
                    = AnalyzerConfigurationCodec::migrateLegacy(LegacySpectrumSettings {
                        readLegacyParameterValue(restoredState, spectrumFloorParameter),
                        readLegacyParameterValue(restoredState, spectrumCeilingParameter),
                        readLegacyParameterValue(restoredState, spectrumSmoothingParameter),
                    });
            }

            removeAnalyzerConfigurationChildren(restoredState);
            const auto currentMetricsState
                = restoredState.getChildWithProperty("id", performanceMetricsParameter);
            auto legacyMetricsState
                = restoredState.getChildWithProperty("id", legacyMetalPerformanceHudParameter);

            if (!currentMetricsState.isValid() && legacyMetricsState.isValid()) {
                legacyMetricsState.setProperty("id", performanceMetricsParameter, nullptr);
                legacyMetricsState
                    = restoredState.getChildWithProperty("id", legacyMetalPerformanceHudParameter);
            } else if (!currentMetricsState.isValid()) {
                auto defaultMetricsState = parameters.copyState().getChildWithProperty(
                    "id", performanceMetricsParameter);
                if (defaultMetricsState.isValid()) {
                    defaultMetricsState = defaultMetricsState.createCopy();
                    defaultMetricsState.setProperty("value", 0.0F, nullptr);
                    restoredState.addChild(defaultMetricsState, -1, nullptr);
                }
            }

            while (legacyMetricsState.isValid()) {
                restoredState.removeChild(legacyMetricsState, nullptr);
                legacyMetricsState
                    = restoredState.getChildWithProperty("id", legacyMetalPerformanceHudParameter);
            }

            parameters.replaceState(restoredState);
            replaceAnalyzerConfiguration(std::move(restoredAnalyzerConfiguration), false);
        }
    }
}

juce::AudioProcessorValueTreeState& PluginProcessor::getParameters() noexcept
{
    return parameters;
}

AnalyzerConfiguration PluginProcessor::getAnalyzerConfiguration() const
{
    const std::scoped_lock lock(analyzerConfigurationMutex);
    return analyzerConfiguration;
}

void PluginProcessor::setAnalyzerConfiguration(AnalyzerConfiguration configuration)
{
    replaceAnalyzerConfiguration(std::move(configuration), true);
}

void PluginProcessor::replaceAnalyzerConfiguration(
    AnalyzerConfiguration configuration, const bool notifyHostOfStateChange)
{
    configuration = AnalyzerConfigurationCodec::sanitize(configuration);
    {
        const std::scoped_lock lock(analyzerConfigurationMutex);
        analyzerConfiguration = configuration;
    }

    analysisCoordinator.setSpectrumAnalysisConfiguration(
        spectrumAnalysisConfiguration(configuration));
    analysisCoordinator.setSpectrumTemporalConfiguration(
        spectrumTemporalConfiguration(configuration));
    analysisCoordinator.setSpectrogramFrequencySpacing(
        configuration.sharedAnalysis.frequencySpacing);

    analyzerConfigurationListeners.call(
        [](AnalyzerConfigurationListener& listener) { listener.analyzerConfigurationChanged(); });

    if (notifyHostOfStateChange) {
        updateHostDisplay(
            juce::AudioProcessorListener::ChangeDetails { }.withNonParameterStateChanged(true));
    }
}

void PluginProcessor::addAnalyzerConfigurationListener(AnalyzerConfigurationListener* listener)
{
    analyzerConfigurationListeners.add(listener);
}

void PluginProcessor::removeAnalyzerConfigurationListener(AnalyzerConfigurationListener* listener)
{
    analyzerConfigurationListeners.remove(listener);
}

AnalysisTelemetry PluginProcessor::getAnalysisTelemetry() const noexcept
{
    return analysisCoordinator.telemetry();
}

void PluginProcessor::requestAnalysis() noexcept
{
    analysisCoordinator.requestAnalysis();
}

void PluginProcessor::setVisualizationActive(const bool shouldBeActive) noexcept
{
    analysisCoordinator.setVisualizationActive(shouldBeActive);
}

void PluginProcessor::resetPeakRms() noexcept
{
    analysisCoordinator.resetPeakRms();
}

void PluginProcessor::resetSpectrum() noexcept
{
    analysisCoordinator.resetSpectrum();
}

void PluginProcessor::resetLoudness() noexcept
{
    analysisCoordinator.resetLoudness();
}

bool PluginProcessor::copyLatestVisualizationFrame(VisualizationFrame& destination) const noexcept
{
    return analysisCoordinator.copyLatestVisualizationFrame(destination);
}

bool PluginProcessor::copyNextSpectrogramColumn(SpectrogramColumn& destination) const noexcept
{
    return analysisCoordinator.copyNextSpectrogramColumn(destination);
}

void PluginProcessor::discardPendingSpectrogramColumns() noexcept
{
    analysisCoordinator.discardPendingSpectrogramColumns();
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(
        std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { spectrumFloorParameter, 1 },
            "Spectrum floor", juce::NormalisableRange<float> { -120.0F, -40.0F, 1.0F }, -90.0F,
            juce::AudioParameterFloatAttributes().withLabel("dB").withAutomatable(false)));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { spectrumCeilingParameter, 1 }, "Spectrum ceiling",
        juce::NormalisableRange<float> { -24.0F, 12.0F, 1.0F }, 0.0F,
        juce::AudioParameterFloatAttributes().withLabel("dB").withAutomatable(false)));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { spectrumSmoothingParameter, 1 }, "Spectrum smoothing",
        juce::NormalisableRange<float> { 0.0F, 1.0F, 0.01F }, 0.40F,
        juce::AudioParameterFloatAttributes().withAutomatable(false)));

    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { performanceMetricsParameter, 1 }, "Performance metrics", false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));

    return layout;
}
} // namespace audio_insight

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new audio_insight::PluginProcessor();
}
