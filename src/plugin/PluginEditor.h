// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../core/VisualizationDataSource.h"
#include "../ui/AnalyzerSettingsPanel.h"
#include "../ui/DashboardLayoutEdit.h"
#include "../ui/DashboardLayoutStore.h"
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
    PluginEditor(PluginProcessor& processor, VisualizationDataSource& dataSource,
        DashboardLayoutStore layoutStore);
    ~PluginEditor() override;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void visibilityChanged() override;

private:
    class AboutOverlay;

    void analyzerConfigurationChanged() noexcept override;
    void timerCallback() override;

    void componentMovedOrResized(bool wasMoved, bool wasResized) override;
    void componentPeerChanged() override;
    void componentVisibilityChanged() override;

    void updateAnalyzerRenderSettings(const AnalyzerConfiguration& configuration) noexcept;
    void updateRenderingState();
    void updateUtilityPresentation();
    void updateMainControlVisibility();
    void synchronizeMetricsRequestedFromParameter();
    void setMetricsParameterRequested(bool requested);
    void setSettingsVisible(bool shouldBeVisible);
    void setMainControlsVisible(bool shouldBeVisible);
    void setAboutVisible(bool shouldBeVisible);
    void beginDashboardLayoutEdit();
    void finishDashboardLayoutEdit();
    void cancelDashboardLayoutEdit();
    void resetDashboardLayoutEdit();

    PluginProcessor& processor_;
    DashboardLayoutStore dashboardLayoutStore;
    DashboardLayoutEdit dashboardLayoutEdit;
    MetalVisualization visualization;
    PerformanceMetricsPanel metricsPanel;
    EditorUtilityState utilityState;
    AnalyzerSettingsPanel settingsPanel;

    juce::TextButton editLayoutButton { "Edit layout" };
    juce::TextButton doneLayoutButton { "Done" };
    juce::TextButton cancelLayoutButton { "Cancel" };
    juce::TextButton resetLayoutButton { "Reset layout" };
    juce::TextButton settingsButton { "Settings" };
    juce::TextButton metricsButton { "Metrics" };
    juce::TextButton aboutButton { "About" };

    juce::RangedAudioParameter* metricsParameter = nullptr;
    std::atomic<float>* metricsParameterValue = nullptr;
    std::atomic<bool> analyzerConfigurationUpdatePending { false };

    std::unique_ptr<AboutOverlay> aboutOverlay;

    bool editorIsShowing = false;
    bool editorIsAttached = false;
    bool editorComponentIsVisible = false;
    bool mainControlsRequestedVisible = true;
    std::atomic<bool> shuttingDown { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};
} // namespace audio_insight
