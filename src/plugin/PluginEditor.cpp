// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginEditor.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace audio_insight {
namespace {
constexpr auto spectrumFloorParameter = "spectrumFloor";
constexpr auto spectrumCeilingParameter = "spectrumCeiling";
constexpr auto spectrumSmoothingParameter = "spectrumSmoothing";
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

void layoutParameterControl(juce::Rectangle<int> area, juce::Label& label, juce::Slider& slider)
{
    constexpr int labelWidth = 52;

    label.setBounds(area.removeFromLeft(labelWidth));
    slider.setBounds(area);
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
          [this] { return visualization.isEffectivelyRendering(); }),
      floorAttachment(processorToUse.getParameters(), spectrumFloorParameter, floorSlider),
      ceilingAttachment(processorToUse.getParameters(), spectrumCeilingParameter, ceilingSlider),
      smoothingAttachment(
          processorToUse.getParameters(), spectrumSmoothingParameter, smoothingSlider),
      metricsAttachment(processorToUse.getParameters(), performanceMetricsParameter, metricsButton),
      aboutOverlay(std::make_unique<AboutOverlay>([this] { setAboutVisible(false); }))
{
    setName("Audio Insight editor");
    setOpaque(true);
    setResizable(true, false);
    setResizeLimits(720, 420, 2560, 1600);

    addAndMakeVisible(visualization);
    addChildComponent(metricsPanel);
    visualization.setEffectiveActivityCallback(
        [this](const bool isActive) { metricsPanel.setCollectionActivity(isActive); });

    configureParameterControl(
        floorLabel, floorSlider, "Floor", "Lower decibel limit of the spectrum display");
    configureParameterControl(
        ceilingLabel, ceilingSlider, "Ceiling", "Upper decibel limit of the spectrum display");
    configureParameterControl(
        smoothingLabel, smoothingSlider, "Smooth", "Temporal smoothing of the spectrum display");

    metricsButton.setComponentID("performanceMetricsToggle");
    metricsButton.setClickingTogglesState(true);
    metricsButton.setTooltip("Show every renderer and analysis-pipeline metric for this instance");
    metricsButton.setColour(juce::TextButton::buttonColourId, controlBackground.brighter(0.08F));
    metricsButton.setColour(juce::TextButton::buttonOnColourId, accent.darker(0.45F));
    metricsButton.setColour(juce::TextButton::textColourOffId, secondaryText);
    metricsButton.setColour(juce::TextButton::textColourOnId, primaryText);
    metricsButton.onStateChange = [this] { updateMetricsPanelVisibility(); };
    addAndMakeVisible(metricsButton);

    aboutButton.setTooltip("Show license, source, third-party, and sponsorship information");
    aboutButton.onClick = [this] { setAboutVisible(true); };
    addAndMakeVisible(aboutButton);

    addChildComponent(*aboutOverlay);

    floorSlider.addListener(this);
    ceilingSlider.addListener(this);
    smoothingSlider.addListener(this);

    updateSpectrumSettings();
    updateMetricsPanelVisibility();
    setSize(1200, 800);
    updateRenderingState();
}

PluginEditor::~PluginEditor()
{
    shuttingDown = true;
    visualization.setEffectiveActivityCallback({ });
    metricsPanel.setPollingActive(false);
    visualization.setRenderingActive(false);

    floorSlider.removeListener(this);
    ceilingSlider.removeListener(this);
    smoothingSlider.removeListener(this);

    metricsButton.onStateChange = nullptr;
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

    controls.removeFromLeft(116);
    controls.removeFromRight(12);

    constexpr int gap = 10;
    const auto widthPerControl = std::min(220, std::max(1, (controls.getWidth() - (2 * gap)) / 3));

    auto floorArea = controls.removeFromLeft(widthPerControl);
    controls.removeFromLeft(gap);
    auto ceilingArea = controls.removeFromLeft(widthPerControl);
    controls.removeFromLeft(gap);
    auto smoothingArea = controls.removeFromLeft(widthPerControl);

    layoutParameterControl(floorArea, floorLabel, floorSlider);
    layoutParameterControl(ceilingArea, ceilingLabel, ceilingSlider);
    layoutParameterControl(smoothingArea, smoothingLabel, smoothingSlider);

    if (metricsPanel.isVisible()) {
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
}

void PluginEditor::sliderValueChanged(juce::Slider*)
{
    updateSpectrumSettings();
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

void PluginEditor::configureParameterControl(juce::Label& label, juce::Slider& slider,
    const juce::String& labelText, const juce::String& accessibilityDescription)
{
    label.setText(labelText, juce::dontSendNotification);
    label.setFont(juce::Font { juce::FontOptions { 12.0F } });
    label.setColour(juce::Label::textColourId, secondaryText);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);

    slider.setName(labelText);
    slider.setTitle(labelText);
    slider.setDescription(accessibilityDescription);
    slider.setTooltip(accessibilityDescription);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 22);
    slider.setColour(juce::Slider::trackColourId, accent);
    slider.setColour(juce::Slider::thumbColourId, primaryText);
    slider.setColour(juce::Slider::textBoxTextColourId, primaryText);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, controlBackground.brighter(0.08F));
    slider.setColour(juce::Slider::textBoxOutlineColourId, panelOutline);
    addAndMakeVisible(slider);
}

void PluginEditor::updateSpectrumSettings() noexcept
{
    visualization.setSpectrumSettings(SpectrumRenderSettings {
        static_cast<float>(floorSlider.getValue()), static_cast<float>(ceilingSlider.getValue()),
        static_cast<float>(smoothingSlider.getValue()) });
}

void PluginEditor::updateRenderingState()
{
    editorIsShowing = isShowing();
    editorIsAttached = getPeer() != nullptr;

    const auto aboutIsVisible = aboutOverlay != nullptr && aboutOverlay->isVisible();
    const auto shouldRender
        = !shuttingDown && editorIsShowing && editorIsAttached && !aboutIsVisible;
    visualization.setRenderingActive(shouldRender);
    metricsPanel.setPollingActive(shouldRender && metricsPanel.isVisible());
}

void PluginEditor::updateMetricsPanelVisibility()
{
    const auto aboutIsVisible = aboutOverlay != nullptr && aboutOverlay->isVisible();
    metricsPanel.setVisible(metricsButton.getToggleState() && !aboutIsVisible);
    resized();
    updateRenderingState();
}

void PluginEditor::setMainControlsVisible(const bool shouldBeVisible)
{
    floorLabel.setVisible(shouldBeVisible);
    ceilingLabel.setVisible(shouldBeVisible);
    smoothingLabel.setVisible(shouldBeVisible);
    floorSlider.setVisible(shouldBeVisible);
    ceilingSlider.setVisible(shouldBeVisible);
    smoothingSlider.setVisible(shouldBeVisible);
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
        setMainControlsVisible(false);
        aboutOverlay->setVisible(true);
        aboutOverlay->toFront(false);
        aboutOverlay->focusInitialControl();
    } else {
        aboutOverlay->setVisible(false);
        visualization.setVisible(true);
        setMainControlsVisible(true);
        metricsPanel.setVisible(metricsButton.getToggleState());

        if (aboutButton.isShowing())
            aboutButton.grabKeyboardFocus();
    }

    resized();
    updateRenderingState();
}
} // namespace audio_insight
