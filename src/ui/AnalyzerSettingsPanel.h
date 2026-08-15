// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "state/AnalyzerConfiguration.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace audio_insight {
/**
    Scrollable per-instance analyzer Settings inspector.

    Only controls backed by the currently live analyzers are enabled. Future
    controls remain visible and explicitly disabled so the panel never accepts
    a change that has no runtime effect.
*/
class AnalyzerSettingsPanel final : public juce::Component, private juce::FocusChangeListener {
public:
    using ConfigurationChanged = std::function<void(const AnalyzerConfiguration&)>;
    using CloseRequested = std::function<void()>;

    AnalyzerSettingsPanel(AnalyzerConfiguration initialConfiguration,
        ConfigurationChanged configurationChanged, CloseRequested closeRequested);
    ~AnalyzerSettingsPanel() override;

    void setConfiguration(AnalyzerConfiguration configuration);
    [[nodiscard]] AnalyzerConfiguration getConfiguration() const noexcept;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    void focusInitialControl();

private:
    class Content;

    void globalFocusChanged(juce::Component* focusedComponent) override;

    std::function<void()> closeRequested_;
    juce::Label titleLabel_;
    juce::TextButton closeButton_ { "Close" };
    juce::Viewport viewport_;
    std::unique_ptr<Content> content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyzerSettingsPanel)
};
} // namespace audio_insight
