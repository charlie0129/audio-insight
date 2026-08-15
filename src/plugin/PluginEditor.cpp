// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginEditor.h"
#include "../ui/SpectrumSmoothingMapping.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace audio_insight {
namespace {
constexpr auto performanceMetricsParameter = "performanceMetrics";

constexpr auto projectUrl = "https://github.com/charlie0129/audio-insight";
constexpr auto sponsorUrl = "https://github.com/sponsors/charlie0129";

constexpr int controlStripHeight = 52;

const auto editorBackground = juce::Colour { 0xff090d13 };
const auto controlBackground = juce::Colour { 0xff111822 };
const auto panelBackground = juce::Colour { 0xff151d28 };
const auto panelOutline = juce::Colour { 0xff304156 };
const auto primaryText = juce::Colour { 0xffedf4fc };
const auto secondaryText = juce::Colour { 0xffa9b8ca };
const auto accent = juce::Colour { 0xff55c7e8 };

bool readMetricsRequested(PluginProcessor& processor) noexcept
{
    const auto* value = processor.getParameters().getRawParameterValue(performanceMetricsParameter);
    return value != nullptr && value->load(std::memory_order_relaxed) >= 0.5F;
}

} // namespace

class PluginEditor::AboutOverlay final : public juce::Component {
public:
    explicit AboutOverlay(std::function<void()> closeActionToUse)
        : closeAction(std::move(closeActionToUse)),
          sourceLink("Source, license & notices", juce::URL { projectUrl }),
          sponsorLink("Sponsor development", juce::URL { sponsorUrl })
    {
        setName("About and legal information");
        setOpaque(true);
        setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);

        titleLabel.setText("Audio Insight - About & Legal", juce::dontSendNotification);
        titleLabel.setFont(juce::Font { juce::FontOptions { 23.0F, juce::Font::bold } });
        titleLabel.setColour(juce::Label::textColourId, primaryText);
        titleLabel.setJustificationType(juce::Justification::centredLeft);
        titleLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(titleLabel);

        legalLabel.setText("License: GNU Affero General Public License v3.0 or later "
                           "(AGPL-3.0-or-later). You may redistribute and modify the project-owned "
                           "code under those terms.\n\n"
                           "Audio Insight is distributed in the hope that it will be useful, but "
                           "WITHOUT ANY WARRANTY; without even the implied warranty of "
                           "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n"
                           "The canonical license text, corresponding source, and third-party "
                           "notices are available in the project repository. Built with JUCE 9.0.1 "
                           "under JUCE's upstream AGPLv3 terms; JUCE and bundled third-party "
                           "components retain their own notices and licenses.\n\n"
                           "Copyright holder: to be confirmed.",
            juce::dontSendNotification);
        legalLabel.setFont(juce::Font { juce::FontOptions { 14.0F } });
        legalLabel.setColour(juce::Label::textColourId, secondaryText);
        legalLabel.setJustificationType(juce::Justification::topLeft);
        legalLabel.setMinimumHorizontalScale(0.92F);
        legalLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(legalLabel);

        sponsorMessage.setText(
            "If Audio Insight is useful to you, consider sponsoring its development.",
            juce::dontSendNotification);
        sponsorMessage.setFont(juce::Font { juce::FontOptions { 13.0F } });
        sponsorMessage.setColour(juce::Label::textColourId, secondaryText);
        sponsorMessage.setJustificationType(juce::Justification::centredLeft);
        sponsorMessage.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(sponsorMessage);

        sourceLink.setFont(
            juce::Font { juce::FontOptions { 13.0F } }, false, juce::Justification::centredLeft);
        sourceLink.setColour(juce::HyperlinkButton::textColourId, accent);
        sourceLink.setTooltip("Open the Audio Insight source repository in your browser");
        sourceLink.setWantsKeyboardFocus(true);
        sourceLink.setExplicitFocusOrder(2);
        addAndMakeVisible(sourceLink);

        sponsorLink.setFont(
            juce::Font { juce::FontOptions { 13.0F } }, false, juce::Justification::centredRight);
        sponsorLink.setColour(juce::HyperlinkButton::textColourId, accent);
        sponsorLink.setTooltip("Open the Audio Insight sponsorship page in your browser");
        sponsorLink.setWantsKeyboardFocus(true);
        sponsorLink.setExplicitFocusOrder(3);
        addAndMakeVisible(sponsorLink);

        closeButton.setTooltip("Return to the visualization");
        closeButton.setWantsKeyboardFocus(true);
        closeButton.setExplicitFocusOrder(1);
        closeButton.onClick = [this] {
            if (closeAction)
                closeAction();
        };
        addAndMakeVisible(closeButton);
    }

    ~AboutOverlay() override
    {
        closeButton.onClick = nullptr;
    }

    void clearCloseAction()
    {
        closeAction = { };
    }

    void focusInitialControl()
    {
        if (closeButton.isShowing())
            closeButton.grabKeyboardFocus();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (!key.isKeyCode(juce::KeyPress::escapeKey))
            return false;

        if (closeAction)
            closeAction();

        return true;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(editorBackground);

        const auto panel = getPanelBounds().toFloat();
        graphics.setColour(panelBackground);
        graphics.fillRoundedRectangle(panel, 10.0F);
        graphics.setColour(panelOutline);
        graphics.drawRoundedRectangle(panel.reduced(0.5F), 10.0F, 1.0F);
    }

    void resized() override
    {
        auto content = getPanelBounds().reduced(28, 24);

        auto titleRow = content.removeFromTop(34);
        closeButton.setBounds(titleRow.removeFromRight(70));
        titleRow.removeFromRight(12);
        titleLabel.setBounds(titleRow);

        content.removeFromTop(14);

        auto linkRow = content.removeFromBottom(26);
        sourceLink.setBounds(linkRow.removeFromLeft(std::min(210, linkRow.getWidth() / 2)));
        sponsorLink.setBounds(linkRow);

        auto sponsorRow = content.removeFromBottom(42);
        sponsorMessage.setBounds(sponsorRow);

        content.removeFromBottom(6);
        legalLabel.setBounds(content);
    }

private:
    [[nodiscard]] juce::Rectangle<int> getPanelBounds() const
    {
        const auto available = getLocalBounds().reduced(18);
        const auto width = std::min(700, available.getWidth());
        const auto height = std::min(440, available.getHeight());
        return available.withSizeKeepingCentre(width, height);
    }

    std::function<void()> closeAction;
    juce::Label titleLabel;
    juce::Label legalLabel;
    juce::Label sponsorMessage;
    juce::HyperlinkButton sourceLink;
    juce::HyperlinkButton sponsorLink;
    juce::TextButton closeButton { "Close" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AboutOverlay)
};

PluginEditor::PluginEditor(PluginProcessor& processorToUse, VisualizationDataSource& dataSource)
    : AudioProcessorEditor(processorToUse), ComponentMovementWatcher(this),
      processor_(processorToUse), visualization(dataSource),
      metricsPanel(
          [this] {
              return PerformanceMetricsSnapshot { visualization.getRenderTelemetry(),
                  processor_.getAnalysisTelemetry() };
          },
          [this] { visualization.resetRenderTelemetry(); },
          [this] { return visualization.isEffectivelyRendering(); },
          [this] { return visualization.getRenderTelemetry(); }),
      utilityState(readMetricsRequested(processorToUse)),
      settingsPanel(
          processorToUse.getAnalyzerConfiguration(),
          [this](const AnalyzerConfiguration& configuration) {
              processor_.setAnalyzerConfiguration(configuration);
              updateSpectrumSettings(configuration);
          },
          [this] { setSettingsVisible(false); }),
      aboutOverlay(std::make_unique<AboutOverlay>([this] { setAboutVisible(false); }))
{
    setName("Audio Insight editor");
    setOpaque(true);
    setResizable(true, false);
    setResizeLimits(720, 420, 2560, 1600);

    visualization.setComponentID("metalVisualization");
    addAndMakeVisible(visualization);
    addChildComponent(metricsPanel);
    visualization.setEffectiveActivityCallback(
        [this](const bool isActive) { metricsPanel.setCollectionActivity(isActive); });

    settingsButton.setComponentID("analyzerSettingsToggle");
    settingsButton.setTooltip("Open per-instance analyzer settings");
    settingsButton.setColour(juce::TextButton::buttonColourId, controlBackground.brighter(0.08F));
    settingsButton.setColour(juce::TextButton::buttonOnColourId, accent.darker(0.45F));
    settingsButton.setColour(juce::TextButton::textColourOffId, secondaryText);
    settingsButton.setColour(juce::TextButton::textColourOnId, primaryText);
    settingsButton.onClick = [this] { setSettingsVisible(!utilityState.isSettingsOpen()); };
    addAndMakeVisible(settingsButton);

    metricsButton.setComponentID("performanceMetricsToggle");
    metricsButton.setTooltip("Show every renderer and analysis-pipeline metric for this instance");
    metricsButton.setColour(juce::TextButton::buttonColourId, controlBackground.brighter(0.08F));
    metricsButton.setColour(juce::TextButton::buttonOnColourId, accent.darker(0.45F));
    metricsButton.setColour(juce::TextButton::textColourOffId, secondaryText);
    metricsButton.setColour(juce::TextButton::textColourOnId, primaryText);
    metricsButton.onClick = [this] {
        if (const auto requested = utilityState.pressMetrics())
            setMetricsParameterRequested(*requested);
        updateUtilityPresentation();
    };
    addAndMakeVisible(metricsButton);

    aboutButton.setTooltip("Show license, source, third-party, and sponsorship information");
    aboutButton.onClick = [this] { setAboutVisible(true); };
    addAndMakeVisible(aboutButton);

    addChildComponent(settingsPanel);
    addChildComponent(*aboutOverlay);

    metricsParameter = processor_.getParameters().getParameter(performanceMetricsParameter);
    metricsParameterValue
        = processor_.getParameters().getRawParameterValue(performanceMetricsParameter);
    processor_.addAnalyzerConfigurationListener(this);

    updateSpectrumSettings(processor_.getAnalyzerConfiguration());
    synchronizeMetricsRequestedFromParameter();
    updateUtilityPresentation();
    setSize(1200, 800);
    updateRenderingState();
}

PluginEditor::~PluginEditor()
{
    shuttingDown.store(true, std::memory_order_release);
    stopTimer();
    processor_.removeAnalyzerConfigurationListener(this);
    visualization.setEffectiveActivityCallback({ });
    metricsPanel.setPollingActive(false);
    visualization.setRenderingActive(false);

    settingsButton.onClick = nullptr;
    metricsButton.onClick = nullptr;
    aboutButton.onClick = nullptr;
    aboutOverlay->clearCloseAction();
}

void PluginEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(editorBackground);

    graphics.setColour(controlBackground);
    graphics.fillRect(getLocalBounds().removeFromTop(controlStripHeight));

    graphics.setColour(primaryText);
    graphics.setFont(juce::Font { juce::FontOptions { 15.0F, juce::Font::bold } });
    graphics.drawText(
        "AUDIO INSIGHT", 14, 0, 112, controlStripHeight, juce::Justification::centredLeft, false);

    graphics.setColour(panelOutline);
    graphics.drawHorizontalLine(controlStripHeight - 1, 0.0F, static_cast<float>(getWidth()));
}

void PluginEditor::resized()
{
    auto bounds = getLocalBounds();
    auto controls = bounds.removeFromTop(controlStripHeight).reduced(12, 8);

    auto aboutArea = controls.removeFromRight(72);
    aboutButton.setBounds(aboutArea);

    controls.removeFromRight(8);
    auto metricsArea = controls.removeFromRight(68);
    metricsButton.setBounds(metricsArea);

    controls.removeFromRight(8);
    auto settingsArea = controls.removeFromRight(76);
    settingsButton.setBounds(settingsArea);

    const auto aboutIsVisible = aboutOverlay != nullptr && aboutOverlay->isVisible();
    const auto settingsPresentation = utilityState.settingsPresentation(bounds.getWidth());
    const auto settingsIsVisible
        = !aboutIsVisible && settingsPresentation != SettingsPresentation::closed;
    const auto dashboardIsVisible
        = !aboutIsVisible && utilityState.shouldRenderDashboard(bounds.getWidth());
    const auto metricsAreVisible = !aboutIsVisible && utilityState.isMetricsVisible();

    settingsPanel.setVisible(settingsIsVisible);
    metricsPanel.setVisible(metricsAreVisible);
    visualization.setVisible(dashboardIsVisible);

    if (settingsPresentation == SettingsPresentation::sideInspector && settingsIsVisible) {
        settingsPanel.setBounds(bounds.removeFromRight(EditorUtilityState::settingsInspectorWidth));
    } else if (settingsPresentation == SettingsPresentation::fullContent && settingsIsVisible) {
        settingsPanel.setBounds(bounds);
    }

    if (metricsAreVisible) {
        constexpr int minimumVisualizationWidth = 320;
        constexpr int preferredMinimumMetricsWidth = 360;
        constexpr int maximumMetricsWidth = 720;

        const auto availableMetricsWidth
            = std::max(1, bounds.getWidth() - minimumVisualizationWidth);
        const auto minimumMetricsWidth
            = std::min(preferredMinimumMetricsWidth, availableMetricsWidth);
        const auto metricsWidth = std::clamp(juce::roundToInt(bounds.getWidth() * 0.43F),
            minimumMetricsWidth, std::min(maximumMetricsWidth, availableMetricsWidth));
        auto panelBounds = bounds.removeFromRight(metricsWidth);
        bounds.removeFromRight(1);
        metricsPanel.setBounds(panelBounds);
    }

    visualization.setBounds(bounds);
    aboutOverlay->setBounds(getLocalBounds());
    updateRenderingState();
}

void PluginEditor::analyzerConfigurationChanged() noexcept
{
    if (!shuttingDown.load(std::memory_order_acquire))
        analyzerConfigurationUpdatePending.store(true, std::memory_order_release);
}

void PluginEditor::timerCallback()
{
    // APVTS listeners may run on the audio thread. Polling its raw atomic here
    // keeps all utility-UI mutation on the message thread without posting work
    // from a real-time callback.
    synchronizeMetricsRequestedFromParameter();

    if (analyzerConfigurationUpdatePending.exchange(false, std::memory_order_acq_rel)) {
        const auto configuration = processor_.getAnalyzerConfiguration();
        settingsPanel.setConfiguration(configuration);
        updateSpectrumSettings(configuration);
    }
}

void PluginEditor::componentMovedOrResized(bool, bool)
{
    updateRenderingState();
}

void PluginEditor::componentPeerChanged()
{
    updateRenderingState();
}

void PluginEditor::componentVisibilityChanged()
{
    updateRenderingState();
}

void PluginEditor::updateSpectrumSettings(const AnalyzerConfiguration& configuration) noexcept
{
    const auto sanitized = AnalyzerConfigurationCodec::sanitize(configuration);
    visualization.setSpectrumSettings(SpectrumRenderSettings {
        static_cast<float>(sanitized.spectrum.floorDb),
        static_cast<float>(sanitized.spectrum.ceilingDb),
        static_cast<float>(
            SpectrumSmoothingMapping::toNormalized(sanitized.spectrum.temporalAveraging)),
        static_cast<float>(sanitized.sharedAnalysis.frequencySpacing),
    });
}

void PluginEditor::updateRenderingState()
{
    editorIsShowing = isShowing();
    editorIsAttached = getPeer() != nullptr;

    const auto aboutIsVisible = aboutOverlay != nullptr && aboutOverlay->isVisible();
    const auto contentWidth = getWidth();
    const auto dashboardShouldRender = utilityState.shouldRenderDashboard(contentWidth);
    const auto editorIsActive
        = !shuttingDown.load(std::memory_order_acquire) && editorIsShowing && editorIsAttached;
    if (editorIsActive && !isTimerRunning())
        startTimerHz(30);
    else if (!editorIsActive && isTimerRunning())
        stopTimer();

    const auto shouldRender = editorIsActive && !aboutIsVisible && dashboardShouldRender;
    visualization.setRenderingActive(shouldRender);
    metricsPanel.setPollingActive(shouldRender && metricsPanel.isVisible());
}

void PluginEditor::updateUtilityPresentation()
{
    settingsButton.setToggleState(utilityState.isSettingsOpen(), juce::dontSendNotification);
    metricsButton.setToggleState(utilityState.isMetricsRequested(), juce::dontSendNotification);

    if (utilityState.isMetricsTemporarilyHidden()) {
        metricsButton.setDescription(
            "Performance metrics are requested and temporarily hidden while Settings is open");
        metricsButton.setTooltip("Switch from Settings to the requested performance metrics panel");
    } else {
        metricsButton.setDescription(utilityState.isMetricsRequested()
                ? "Performance metrics are requested and visible"
                : "Performance metrics are off");
        metricsButton.setTooltip(
            "Show every renderer and analysis-pipeline metric for this instance");
    }

    resized();
}

void PluginEditor::synchronizeMetricsRequestedFromParameter()
{
    if (shuttingDown.load(std::memory_order_acquire))
        return;

    const auto requested = metricsParameterValue != nullptr
        && metricsParameterValue->load(std::memory_order_relaxed) >= 0.5F;
    if (requested == utilityState.isMetricsRequested())
        return;

    utilityState.setMetricsRequested(requested);
    updateUtilityPresentation();
}

void PluginEditor::setMetricsParameterRequested(const bool requested)
{
    utilityState.setMetricsRequested(requested);

    if (metricsParameter == nullptr)
        return;

    metricsParameter->beginChangeGesture();
    metricsParameter->setValueNotifyingHost(requested ? 1.0F : 0.0F);
    metricsParameter->endChangeGesture();
}

void PluginEditor::setSettingsVisible(const bool shouldBeVisible)
{
    if (shouldBeVisible == utilityState.isSettingsOpen())
        return;

    if (shouldBeVisible) {
        settingsPanel.setConfiguration(processor_.getAnalyzerConfiguration());
        utilityState.openSettings();
    } else {
        utilityState.closeSettings();
    }

    updateUtilityPresentation();

    if (shouldBeVisible)
        settingsPanel.focusInitialControl();
    else if (settingsButton.isShowing())
        settingsButton.grabKeyboardFocus();
}

void PluginEditor::setMainControlsVisible(const bool shouldBeVisible)
{
    settingsButton.setVisible(shouldBeVisible);
    metricsButton.setVisible(shouldBeVisible);
    aboutButton.setVisible(shouldBeVisible);
}

void PluginEditor::setAboutVisible(bool shouldBeVisible)
{
    if (aboutOverlay->isVisible() == shouldBeVisible)
        return;

    if (shouldBeVisible) {
        visualization.setVisible(false);
        metricsPanel.setVisible(false);
        settingsPanel.setVisible(false);
        setMainControlsVisible(false);
        aboutOverlay->setVisible(true);
        aboutOverlay->toFront(false);
        aboutOverlay->focusInitialControl();
    } else {
        aboutOverlay->setVisible(false);
        setMainControlsVisible(true);

        if (aboutButton.isShowing())
            aboutButton.grabKeyboardFocus();
    }

    resized();
    updateRenderingState();
}
} // namespace audio_insight
