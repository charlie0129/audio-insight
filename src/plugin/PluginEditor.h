// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PluginProcessor.h"
#include "../core/VisualizationDataSource.h"
#include "../ui/MetalVisualization.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace audio_insight
{
/**
    Resizable first-release editor containing spectrum controls, the native
    Metal visualization, and an in-editor About/Legal view.

    The data source is owned by the processor and must outlive this editor.
*/
class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::ComponentMovementWatcher,
                           private juce::Slider::Listener
{
public:
    PluginEditor(PluginProcessor& processor, VisualizationDataSource& dataSource);
    ~PluginEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class AboutOverlay;

    void sliderValueChanged(juce::Slider* slider) override;

    void componentMovedOrResized(bool wasMoved, bool wasResized) override;
    void componentPeerChanged() override;
    void componentVisibilityChanged() override;

    void configureParameterControl(juce::Label& label,
                                   juce::Slider& slider,
                                   const juce::String& labelText,
                                   const juce::String& accessibilityDescription);
    void updateSpectrumSettings() noexcept;
    void updateRenderingState();
    void setMainControlsVisible(bool shouldBeVisible);
    void setAboutVisible(bool shouldBeVisible);

    MetalVisualization visualization;

    juce::Label floorLabel;
    juce::Label ceilingLabel;
    juce::Label smoothingLabel;
    juce::Slider floorSlider;
    juce::Slider ceilingSlider;
    juce::Slider smoothingSlider;

    juce::AudioProcessorValueTreeState::SliderAttachment floorAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment ceilingAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment smoothingAttachment;

    juce::TextButton aboutButton { "About" };
    std::unique_ptr<AboutOverlay> aboutOverlay;

    bool editorIsShowing = false;
    bool editorIsAttached = false;
    bool shuttingDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};
} // namespace audio_insight
