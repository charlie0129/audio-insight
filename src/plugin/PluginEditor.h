// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../core/VisualizationDataSource.h"
#include "../ui/AnalyzerSettingsPanel.h"
#include "../ui/EditorUtilityState.h"
#include "../ui/MetalVisualization.h"
#include "../ui/PerformanceMetricsPanel.h"
#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>

namespace audio_insight {
/**
    Resizable analyzer editor containing the native Metal dashboard, utility
    panels, and an in-editor About/Legal view.

    The data source is owned by the processor and must outlive this editor.
*/
class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::ComponentMovementWatcher,
                           private PluginProcessor::AnalyzerConfigurationListener,
                           private juce::Timer {
public:
    PluginEditor(PluginProcessor& processor, VisualizationDataSource& dataSource);
    ~PluginEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class AboutOverlay;

    void analyzerConfigurationChanged() noexcept override;
    void timerCallback() override;

    void componentMovedOrResized(bool wasMoved, bool wasResized) override;
    void componentPeerChanged() override;
    void componentVisibilityChanged() override;

    void updateSpectrumSettings(const AnalyzerConfiguration& configuration) noexcept;
    void updateRenderingState();
    void updateUtilityPresentation();
    void synchronizeMetricsRequestedFromParameter();
    void setMetricsParameterRequested(bool requested);
    void setSettingsVisible(bool shouldBeVisible);
    void setMainControlsVisible(bool shouldBeVisible);
    void setAboutVisible(bool shouldBeVisible);

    PluginProcessor& processor_;
    MetalVisualization visualization;
    PerformanceMetricsPanel metricsPanel;
    EditorUtilityState utilityState;
    AnalyzerSettingsPanel settingsPanel;

    juce::TextButton settingsButton { "Settings" };
    juce::TextButton metricsButton { "Metrics" };
    juce::TextButton aboutButton { "About" };

    juce::RangedAudioParameter* metricsParameter = nullptr;
    std::atomic<float>* metricsParameterValue = nullptr;
    std::atomic<bool> analyzerConfigurationUpdatePending { false };

    std::unique_ptr<AboutOverlay> aboutOverlay;

    bool editorIsShowing = false;
    bool editorIsAttached = false;
    std::atomic<bool> shuttingDown { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};
} // namespace audio_insight
