// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginProcessor.h"

#include <memory>

namespace audio_insight
{
namespace
{
constexpr auto stateTreeName = "AudioInsightState";
constexpr auto spectrumFloorParameter = "spectrumFloor";
constexpr auto spectrumCeilingParameter = "spectrumCeiling";
constexpr auto spectrumSmoothingParameter = "spectrumSmoothing";
} // namespace

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, stateTreeName, createParameterLayout())
{
}

void PluginProcessor::prepareToPlay(double, int)
{
}

void PluginProcessor::releaseResources()
{
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals disableDenormals;

    const auto inputChannels = getTotalNumInputChannels();
    const auto outputChannels = getTotalNumOutputChannels();

    for (auto channel = inputChannels; channel < outputChannels; ++channel)
        audio.clear(channel, 0, audio.getNumSamples());

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
    return new juce::GenericAudioProcessorEditor(*this);
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
    return {};
}

void PluginProcessor::changeProgramName(int, const juce::String&)
{
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destinationData)
{
    if (const auto state = parameters.copyState(); auto xml = state.createXml())
        copyXmlToBinary(*xml, destinationData);
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes); xml != nullptr)
    {
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }
}

juce::AudioProcessorValueTreeState& PluginProcessor::getParameters() noexcept
{
    return parameters;
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { spectrumFloorParameter, 1 },
        "Spectrum floor",
        juce::NormalisableRange<float> { -120.0F, -40.0F, 1.0F },
        -90.0F,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { spectrumCeilingParameter, 1 },
        "Spectrum ceiling",
        juce::NormalisableRange<float> { -24.0F, 12.0F, 1.0F },
        0.0F,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { spectrumSmoothingParameter, 1 },
        "Spectrum smoothing",
        juce::NormalisableRange<float> { 0.0F, 1.0F, 0.01F },
        0.65F));

    return layout;
}
} // namespace audio_insight

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new audio_insight::PluginProcessor();
}
