// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AnalyzerSettingsPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace audio_insight {
namespace {
const auto panelBackground = juce::Colour { 0xff111822 };
const auto sectionBackground = juce::Colour { 0xff151d28 };
const auto panelOutline = juce::Colour { 0xff304156 };
const auto primaryText = juce::Colour { 0xffedf4fc };
const auto secondaryText = juce::Colour { 0xffa9b8ca };
const auto disabledText = juce::Colour { 0xff758396 };
const auto accent = juce::Colour { 0xff55c7e8 };

constexpr int headerHeight = 52;
constexpr int contentHeight = 1210;
constexpr int sectionGap = 10;
constexpr int sectionHeaderHeight = 34;
constexpr int rowHeight = 34;
constexpr double temporalAveragingOffTrackProportion = 0.04;
constexpr std::array spectrogramHistoryDurations { 2, 5, 10, 20, 30, 60 };

enum SectionIndex : std::size_t {
    sharedSection,
    spectrumSection,
    peakRmsSection,
    spectrogramSection,
    stereoSection,
    loudnessSection,
    sectionCount,
};

constexpr std::array sectionTitles {
    "Shared analysis",
    "Spectrum",
    "Peak / RMS",
    "Spectrogram",
    "Stereo",
    "Loudness",
};

void configureLabel(juce::Label& label, const juce::String& text, const bool enabled = true)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::Font { juce::FontOptions { 12.0F } });
    label.setColour(juce::Label::textColourId, enabled ? secondaryText : disabledText);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setInterceptsMouseClicks(false, false);
    label.setEnabled(enabled);
}

void configureSlider(juce::Slider& slider, const juce::String& name,
    const juce::String& description, const juce::String& suffix)
{
    slider.setName(name);
    slider.setTitle(name);
    slider.setDescription(description);
    slider.setTooltip(description);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
    slider.setTextValueSuffix(suffix);
    slider.setScrollWheelEnabled(false);
    slider.setWantsKeyboardFocus(true);
    slider.setColour(juce::Slider::trackColourId, accent);
    slider.setColour(juce::Slider::thumbColourId, primaryText);
    slider.setColour(juce::Slider::textBoxTextColourId, primaryText);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, panelBackground.brighter(0.08F));
    slider.setColour(juce::Slider::textBoxOutlineColourId, panelOutline);
}

juce::NormalisableRange<double> makeTemporalAveragingRange()
{
    return { 0.0, TemporalAveragingSettings::maximumMilliseconds,
        [](const double, const double, const double proportion) {
            if (proportion <= temporalAveragingOffTrackProportion) {
                return TemporalAveragingSettings::minimumMilliseconds * proportion
                    / temporalAveragingOffTrackProportion;
            }

            const auto enabledProportion = (proportion - temporalAveragingOffTrackProportion)
                / (1.0 - temporalAveragingOffTrackProportion);
            const auto enabledRatio = TemporalAveragingSettings::maximumMilliseconds
                / TemporalAveragingSettings::minimumMilliseconds;
            return TemporalAveragingSettings::minimumMilliseconds
                * std::pow(enabledRatio, enabledProportion);
        },
        [](const double, const double, const double value) {
            if (value <= TemporalAveragingSettings::minimumMilliseconds) {
                return temporalAveragingOffTrackProportion * value
                    / TemporalAveragingSettings::minimumMilliseconds;
            }

            const auto enabledRatio = TemporalAveragingSettings::maximumMilliseconds
                / TemporalAveragingSettings::minimumMilliseconds;
            const auto enabledProportion
                = std::log(value / TemporalAveragingSettings::minimumMilliseconds)
                / std::log(enabledRatio);
            return temporalAveragingOffTrackProportion
                + ((1.0 - temporalAveragingOffTrackProportion) * enabledProportion);
        },
        [](const double, const double, const double value) {
            if (!std::isfinite(value))
                return TemporalAveragingSettings::defaultMilliseconds;
            if (value <= TemporalAveragingSettings::minimumMilliseconds * 0.5)
                return 0.0;

            return std::clamp(std::round(value), TemporalAveragingSettings::minimumMilliseconds,
                TemporalAveragingSettings::maximumMilliseconds);
        } };
}

juce::String formatSrgbColor(const SrgbColor color)
{
    return "#"
        + juce::String::toHexString(static_cast<int>(color.packedRgb()))
              .paddedLeft('0', 6)
              .toUpperCase();
}

std::optional<SrgbColor> parseSrgbColor(const juce::String& untrimmedText)
{
    const auto text = untrimmedText.trim();
    if (text.length() != 7 || text[0] != '#')
        return std::nullopt;

    auto packed = std::uint32_t { 0 };
    for (auto index = 1; index < text.length(); ++index) {
        const auto digit = juce::CharacterFunctions::getHexDigitValue(text[index]);
        if (digit < 0)
            return std::nullopt;
        packed = (packed << 4U) | static_cast<std::uint32_t>(digit);
    }

    return SrgbColor::fromPackedRgb(packed);
}

template <typename Control>
void configureUnavailableControl(Control& control, const juce::String& tooltip)
{
    control.setEnabled(false);
    control.setAlpha(0.58F);
    control.setTooltip(tooltip);
}
} // namespace

class AnalyzerSettingsPanel::Content final : public juce::Component,
                                             private juce::Slider::Listener {
public:
    Content(AnalyzerConfiguration initialConfiguration, ConfigurationChanged configurationChanged,
        CloseRequested closeRequested)
        : configurationChanged_(std::move(configurationChanged)),
          closeRequested_(std::move(closeRequested))
    {
        setName("Analyzer settings controls");
        setFocusContainerType(juce::Component::FocusContainerType::focusContainer);

        for (auto index = std::size_t { 0 }; index < resetButtons_.size(); ++index) {
            auto& heading = sectionHeadings_[index];
            heading.setText(sectionTitles[index], juce::dontSendNotification);
            heading.setFont(juce::Font { juce::FontOptions { 13.0F, juce::Font::bold } });
            heading.setColour(juce::Label::textColourId, primaryText);
            heading.setJustificationType(juce::Justification::centredLeft);
            heading.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(heading);

            auto& reset = resetButtons_[index];
            reset.setButtonText("Reset");
            reset.setName(juce::String("Reset ") + sectionTitles[index]);
            reset.setTitle(juce::String("Reset ") + sectionTitles[index] + " settings");
            reset.setDescription(
                juce::String("Restore the ") + sectionTitles[index] + " section defaults");
            reset.setTooltip(juce::String("Restore the ") + sectionTitles[index]
                + " section to its compiled defaults");
            reset.onClick = [this, index] { resetSection(index); };
            addAndMakeVisible(reset);
        }

        configureLabel(
            sharedStatus_, "FFT changes restart Spectrum overlap; Peak / RMS keeps running.");
        sharedStatus_.setMinimumHorizontalScale(0.8F);
        addAndMakeVisible(sharedStatus_);

        fftSize_.addItemList({ "1024", "2048", "4096", "8192", "16384" }, 1);
        fftSize_.setSelectedId(3, juce::dontSendNotification);
        fftSize_.setName("FFT size");
        fftSize_.setTitle("FFT size");
        fftSize_.setDescription("Number of samples in each Fourier transform");
        fftSize_.setTooltip("Smaller FFTs respond sooner; larger FFTs resolve frequency better");
        fftSize_.setComponentID("settingsFftSize");
        fftSize_.onChange = [this] {
            if (synchronizing_)
                return;

            constexpr std::array fftSizes { 1024, 2048, 4096, 8192, 16384 };
            const auto index = static_cast<std::size_t>(std::max(1, fftSize_.getSelectedId()) - 1);
            configuration_.sharedAnalysis.fftSize = fftSizes[std::min(index, fftSizes.size() - 1)];
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(fftSize_);

        window_.addItemList({ "Rectangular", "Periodic Hann", "Blackman-Harris", "Flat-top" }, 1);
        window_.setSelectedId(2, juce::dontSendNotification);
        window_.setName("FFT window");
        window_.setTitle("FFT window");
        window_.setDescription("Window function applied before each Fourier transform");
        window_.setTooltip("Select the FFT window and its frequency-leakage tradeoff");
        window_.setComponentID("settingsFftWindow");
        window_.onChange = [this] {
            if (synchronizing_)
                return;

            configuration_.sharedAnalysis.window
                = static_cast<FftWindow>(std::max(1, window_.getSelectedId()) - 1);
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(window_);

        fftRate_.addItemList({ "15 Hz", "30 Hz", "60 Hz", "120 Hz" }, 1);
        fftRate_.setSelectedId(3, juce::dontSendNotification);
        fftRate_.setName("FFT slice rate");
        fftRate_.setTitle("FFT slice rate");
        fftRate_.setDescription("Requested number of new FFT snapshots per second");
        fftRate_.setTooltip(
            "FFT analysis rate; Metal rendering remains paced by the display refresh rate");
        fftRate_.setComponentID("settingsFftSliceRate");
        fftRate_.onChange = [this] {
            if (synchronizing_)
                return;

            constexpr std::array fftRates { 15, 30, 60, 120 };
            const auto index = static_cast<std::size_t>(std::max(1, fftRate_.getSelectedId()) - 1);
            configuration_.sharedAnalysis.requestedFftSliceRateHz
                = fftRates[std::min(index, fftRates.size() - 1)];
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(fftRate_);

        configureSlider(frequencySpacing_, "Frequency spacing",
            "Shared linear-to-logarithmic frequency spacing for Spectrum and Spectrogram", "");
        frequencySpacing_.setComponentID("settingsFrequencySpacing");
        frequencySpacing_.setRange(0.0, 1.0, 0.0);
        frequencySpacing_.textFromValueFunction = [](const double value) {
            if (value <= 0.0)
                return juce::String("Linear");
            if (value >= 1.0)
                return juce::String("Logarithmic");
            return juce::String(juce::roundToInt(value * 100.0)) + "% log";
        };
        frequencySpacing_.valueFromTextFunction = [](juce::String text) {
            text = text.trim().toLowerCase();
            if (text == "linear")
                return 0.0;
            if (text == "log" || text == "logarithmic")
                return 1.0;
            if (text.containsChar('%'))
                return text.getDoubleValue() / 100.0;
            return text.getDoubleValue();
        };
        frequencySpacing_.addListener(this);
        addAndMakeVisible(frequencySpacing_);

        configureLabel(floorLabel_, "Floor");
        configureLabel(ceilingLabel_, "Ceiling");
        configureLabel(averagingLabel_, "Temporal averaging");
        addAndMakeVisible(floorLabel_);
        addAndMakeVisible(ceilingLabel_);
        addAndMakeVisible(averagingLabel_);

        configureSlider(
            floor_, "Spectrum floor", "Lower decibel limit of the Spectrum display", " dB");
        floor_.setComponentID("settingsSpectrumFloor");
        floor_.setRange(SpectrumSettings::minimumFloorDb, SpectrumSettings::maximumFloorDb, 1.0);
        floor_.addListener(this);
        addAndMakeVisible(floor_);

        configureSlider(
            ceiling_, "Spectrum ceiling", "Upper decibel limit of the Spectrum display", " dB");
        ceiling_.setComponentID("settingsSpectrumCeiling");
        ceiling_.setRange(
            SpectrumSettings::minimumCeilingDb, SpectrumSettings::maximumCeilingDb, 1.0);
        ceiling_.addListener(this);
        addAndMakeVisible(ceiling_);

        configureSlider(averaging_, "Spectrum temporal averaging",
            "Average Spectrum power over time; Off uses each unsmoothed FFT snapshot", "");
        averaging_.setComponentID("settingsSpectrumTemporalAveraging");
        averaging_.setNormalisableRange(makeTemporalAveragingRange());
        averaging_.setNumDecimalPlacesToDisplay(0);
        averaging_.textFromValueFunction = [](const double value) {
            if (value <= 0.0)
                return juce::String("Off");
            return juce::String(juce::roundToInt(value)) + " ms";
        };
        averaging_.valueFromTextFunction = [](juce::String text) {
            text = text.trim().toLowerCase();
            if (text == "off" || text == "none" || text == "immediate")
                return 0.0;
            return text.getDoubleValue();
        };
        averaging_.addListener(this);
        addAndMakeVisible(averaging_);

        slope_.addItemList({ "0 dB/oct", "+3 dB/oct", "+4.5 dB/oct", "+6 dB/oct" }, 1);
        slope_.setName("Spectrum slope compensation");
        slope_.setTitle("Spectrum slope compensation");
        slope_.setDescription("Presentation slope compensation referenced at 1 kHz");
        slope_.setTooltip("Raise higher-frequency presentation by a fixed dB-per-octave slope");
        slope_.setComponentID("settingsSpectrumSlope");
        slope_.setWantsKeyboardFocus(true);
        slope_.onChange = [this] {
            if (synchronizing_)
                return;

            configuration_.spectrum.slope
                = static_cast<SpectrumSlope>(std::max(1, slope_.getSelectedId()) - 1);
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(slope_);

        peakHoldMode_.addItemList({ "Off", "Finite", "Infinite" }, 1);
        peakHoldMode_.setName("Spectrum peak hold mode");
        peakHoldMode_.setTitle("Spectrum peak hold mode");
        peakHoldMode_.setDescription("Whether Spectrum peak hold is off, finite, or infinite");
        peakHoldMode_.setTooltip("Select Off, a timed hold followed by decay, or Infinite hold");
        peakHoldMode_.setComponentID("settingsSpectrumPeakHoldMode");
        peakHoldMode_.setWantsKeyboardFocus(true);
        peakHoldMode_.onChange = [this] {
            if (synchronizing_)
                return;

            configuration_.spectrum.peakHoldMode
                = static_cast<SpectrumPeakHoldMode>(std::max(1, peakHoldMode_.getSelectedId()) - 1);
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(peakHoldMode_);

        configureSlider(peakHoldDuration_, "Spectrum finite peak-hold duration",
            "Duration of finite Spectrum peak hold before decay", " s");
        peakHoldDuration_.setComponentID("settingsSpectrumPeakHoldDuration");
        peakHoldDuration_.setRange(SpectrumSettings::minimumPeakHoldSeconds,
            SpectrumSettings::maximumPeakHoldSeconds, 0.01);
        peakHoldDuration_.addListener(this);
        addAndMakeVisible(peakHoldDuration_);

        traceColor_.setName("Spectrum trace colour");
        traceColor_.setTitle("Spectrum trace colour");
        traceColor_.setDescription("sRGB colour used for the Spectrum trace, as #RRGGBB");
        traceColor_.setTooltip(
            "Enter an sRGB colour as #RRGGBB, then press Return or leave the field");
        traceColor_.setComponentID("settingsSpectrumTraceColor");
        traceColor_.setReadOnly(false);
        traceColor_.setMultiLine(false);
        traceColor_.setInputRestrictions(7, "#0123456789abcdefABCDEF");
        traceColor_.setSelectAllWhenFocused(true);
        traceColor_.setWantsKeyboardFocus(true);
        traceColor_.setJustification(juce::Justification::centredRight);
        traceColor_.setColour(juce::TextEditor::textColourId, primaryText);
        traceColor_.setColour(
            juce::TextEditor::backgroundColourId, panelBackground.brighter(0.08F));
        traceColor_.setColour(juce::TextEditor::outlineColourId, panelOutline);
        traceColor_.onReturnKey = [this] { commitTraceColor(); };
        traceColor_.onFocusLost = [this] { commitTraceColor(); };
        traceColor_.onEscapeKey = [this] {
            traceColor_.setText(formatSrgbColor(configuration_.spectrum.traceColor), false);
            if (closeRequested_)
                closeRequested_();
        };
        addAndMakeVisible(traceColor_);

        configureSlider(fillOpacity_, "Spectrum fill opacity",
            "Opacity of the area beneath the Spectrum trace", "%");
        fillOpacity_.setComponentID("settingsSpectrumFillOpacity");
        fillOpacity_.setRange(0.0, 50.0, 0.1);
        fillOpacity_.addListener(this);
        addAndMakeVisible(fillOpacity_);

        configureLabel(peakRmsStatus_,
            "Live sample peak and RMS currently use fixed calibrated ballistics.", false);
        peakRmsStatus_.setMinimumHorizontalScale(0.8F);
        addAndMakeVisible(peakRmsStatus_);

        configureLabel(
            spectrogramStatus_, "Controls apply immediately and are saved with this instance.");
        spectrogramStatus_.setComponentID("settingsSpectrogramStatus");
        spectrogramStatus_.setMinimumHorizontalScale(0.8F);
        addAndMakeVisible(spectrogramStatus_);

        palette_.addItemList({ "Blue Fire", "Inferno", "Viridis", "Grayscale" }, 1);
        palette_.setSelectedId(1, juce::dontSendNotification);
        palette_.setName("Spectrogram palette");
        palette_.setTitle("Spectrogram palette");
        palette_.setDescription("Colour palette for Spectrogram energy");
        palette_.setTooltip("Select the colour palette used to render Spectrogram energy");
        palette_.setComponentID("settingsSpectrogramPalette");
        palette_.setWantsKeyboardFocus(true);
        palette_.onChange = [this] {
            if (synchronizing_)
                return;

            configuration_.spectrogram.palette
                = static_cast<SpectrogramPalette>(std::max(1, palette_.getSelectedId()) - 1);
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(palette_);

        historyDuration_.addItemList({ "2 s", "5 s", "10 s", "20 s", "30 s", "60 s" }, 1);
        historyDuration_.setSelectedId(3, juce::dontSendNotification);
        historyDuration_.setName("Spectrogram history duration");
        historyDuration_.setTitle("Spectrogram history duration");
        historyDuration_.setDescription("Visible Spectrogram history duration");
        historyDuration_.setTooltip("Select how many seconds of Spectrogram history are visible");
        historyDuration_.setComponentID("settingsSpectrogramHistory");
        historyDuration_.setWantsKeyboardFocus(true);
        historyDuration_.onChange = [this] {
            if (synchronizing_)
                return;

            const auto index
                = static_cast<std::size_t>(std::max(1, historyDuration_.getSelectedId()) - 1);
            configuration_.spectrogram.historyDurationSeconds
                = spectrogramHistoryDurations[std::min(
                    index, spectrogramHistoryDurations.size() - 1)];
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(historyDuration_);

        configureSlider(colorResponse_, "Spectrogram colour response",
            "Response curve applied when mapping calibrated dB to the Spectrogram palette", "");
        colorResponse_.setComponentID("settingsSpectrogramColorResponse");
        colorResponse_.setRange(SpectrogramSettings::minimumColorResponse,
            SpectrogramSettings::maximumColorResponse, 0.01);
        colorResponse_.addListener(this);
        addAndMakeVisible(colorResponse_);

        configureSlider(colorFloor_, "Spectrogram colour floor",
            "Lower decibel limit mapped by the Spectrogram palette", " dB");
        colorFloor_.setComponentID("settingsSpectrogramColorFloor");
        colorFloor_.setRange(SpectrogramSettings::minimumColorFloorDb,
            SpectrogramSettings::maximumColorFloorDb, 1.0);
        colorFloor_.addListener(this);
        addAndMakeVisible(colorFloor_);

        configureSlider(colorCeiling_, "Spectrogram colour ceiling",
            "Upper decibel limit mapped by the Spectrogram palette", " dB");
        colorCeiling_.setComponentID("settingsSpectrogramColorCeiling");
        colorCeiling_.setRange(SpectrogramSettings::minimumColorCeilingDb,
            SpectrogramSettings::maximumColorCeilingDb, 1.0);
        colorCeiling_.addListener(this);
        addAndMakeVisible(colorCeiling_);

        historyMode_.addItemList({ "Scroll", "Overwrite" }, 1);
        historyMode_.setName("Spectrogram history mode");
        historyMode_.setTitle("Spectrogram history mode");
        historyMode_.setDescription("Whether Spectrogram history scrolls or overwrites in place");
        historyMode_.setTooltip("Select scrolling history or a wrapping overwrite head");
        historyMode_.setComponentID("settingsSpectrogramHistoryMode");
        historyMode_.setWantsKeyboardFocus(true);
        historyMode_.onChange = [this] {
            if (synchronizing_)
                return;

            configuration_.spectrogram.historyMode = static_cast<SpectrogramHistoryMode>(
                std::max(1, historyMode_.getSelectedId()) - 1);
            publishSanitizedConfiguration();
        };
        addAndMakeVisible(historyMode_);

        configureLabel(stereoStatus_, "Live fixed design; no adjustable settings.");
        stereoStatus_.setComponentID("settingsStereoStatus");
        stereoStatus_.setMinimumHorizontalScale(0.8F);
        addAndMakeVisible(stereoStatus_);

        configureLabel(loudnessStatus_, "Not yet implemented", false);
        loudnessStatus_.setComponentID("settingsLoudnessUnavailable");
        addAndMakeVisible(loudnessStatus_);

        configureSlider(loudnessReference_, "Loudness reference",
            "Presentation reference for the future loudness meter", " LUFS");
        loudnessReference_.setComponentID("settingsLoudnessReference");
        loudnessReference_.setRange(
            LoudnessSettings::minimumReferenceLufs, LoudnessSettings::maximumReferenceLufs, 0.5);
        loudnessReference_.setValue(
            LoudnessSettings::defaultReferenceLufs, juce::dontSendNotification);
        addAndMakeVisible(loudnessReference_);
        configureUnavailableControl(loudnessReference_, "Loudness is not yet implemented");

        const std::array<juce::Component*, 7> rowControls { &fftSize_, &window_, &fftRate_,
            &frequencySpacing_, &palette_, &historyDuration_, &loudnessReference_ };
        const std::array<const char*, 7> rowLabelText { "FFT size", "Window", "Slice rate",
            "Frequency scale", "Palette", "History", "Reference" };
        for (auto index = std::size_t { 0 }; index < rowLabels_.size(); ++index) {
            configureLabel(rowLabels_[index], rowLabelText[index], rowControls[index]->isEnabled());
            addAndMakeVisible(rowLabels_[index]);
        }

        const std::array<const char*, 5> spectrumLabelText { "Slope", "Peak hold", "Hold duration",
            "Trace colour", "Fill opacity" };
        for (auto index = std::size_t { 0 }; index < spectrumLabels_.size(); ++index) {
            configureLabel(spectrumLabels_[index], spectrumLabelText[index]);
            addAndMakeVisible(spectrumLabels_[index]);
        }

        const std::array<const char*, 4> spectrogramLabelText { "Colour response", "Colour floor",
            "Colour ceiling", "History mode" };
        for (auto index = std::size_t { 0 }; index < spectrogramLabels_.size(); ++index) {
            configureLabel(spectrogramLabels_[index], spectrogramLabelText[index]);
            addAndMakeVisible(spectrogramLabels_[index]);
        }

        resetButtons_[spectrumSection].setComponentID("settingsSpectrumReset");
        resetButtons_[sharedSection].setComponentID("settingsSharedReset");
        resetButtons_[spectrogramSection].setComponentID("settingsSpectrogramReset");
        resetButtons_[sharedSection].setExplicitFocusOrder(2);
        fftSize_.setExplicitFocusOrder(3);
        window_.setExplicitFocusOrder(4);
        fftRate_.setExplicitFocusOrder(5);
        frequencySpacing_.setExplicitFocusOrder(6);
        resetButtons_[spectrumSection].setExplicitFocusOrder(7);
        floor_.setExplicitFocusOrder(8);
        ceiling_.setExplicitFocusOrder(9);
        averaging_.setExplicitFocusOrder(10);
        slope_.setExplicitFocusOrder(11);
        peakHoldMode_.setExplicitFocusOrder(12);
        peakHoldDuration_.setExplicitFocusOrder(13);
        traceColor_.setExplicitFocusOrder(14);
        fillOpacity_.setExplicitFocusOrder(15);
        resetButtons_[spectrogramSection].setExplicitFocusOrder(16);
        palette_.setExplicitFocusOrder(17);
        colorResponse_.setExplicitFocusOrder(18);
        colorFloor_.setExplicitFocusOrder(19);
        colorCeiling_.setExplicitFocusOrder(20);
        historyDuration_.setExplicitFocusOrder(21);
        historyMode_.setExplicitFocusOrder(22);
        resetButtons_[peakRmsSection].setEnabled(false);
        resetButtons_[stereoSection].setComponentID("settingsStereoReset");
        configureUnavailableControl(resetButtons_[stereoSection],
            "Stereo uses a fixed vectorscope and correlation design with no settings to reset");
        resetButtons_[stereoSection].setDescription(
            "Stereo is live with a fixed design and has no adjustable settings to reset");
        resetButtons_[loudnessSection].setEnabled(false);

        setConfiguration(initialConfiguration);
    }

    ~Content() override
    {
        fftSize_.onChange = nullptr;
        window_.onChange = nullptr;
        fftRate_.onChange = nullptr;
        slope_.onChange = nullptr;
        peakHoldMode_.onChange = nullptr;
        palette_.onChange = nullptr;
        historyDuration_.onChange = nullptr;
        historyMode_.onChange = nullptr;
        traceColor_.onReturnKey = nullptr;
        traceColor_.onFocusLost = nullptr;
        traceColor_.onEscapeKey = nullptr;
        floor_.removeListener(this);
        ceiling_.removeListener(this);
        averaging_.removeListener(this);
        frequencySpacing_.removeListener(this);
        peakHoldDuration_.removeListener(this);
        fillOpacity_.removeListener(this);
        colorResponse_.removeListener(this);
        colorFloor_.removeListener(this);
        colorCeiling_.removeListener(this);
        for (auto& reset : resetButtons_)
            reset.onClick = nullptr;
    }

    void setConfiguration(AnalyzerConfiguration configuration)
    {
        configuration_ = AnalyzerConfigurationCodec::sanitize(configuration);
        synchronizeControls();
    }

    [[nodiscard]] AnalyzerConfiguration getConfiguration() const noexcept
    {
        return configuration_;
    }

    void paint(juce::Graphics& graphics) override
    {
        for (auto index = std::size_t { 0 }; index < sectionAreas_.size(); ++index) {
            const auto area = sectionAreas_[index].toFloat();
            graphics.setColour(sectionBackground);
            graphics.fillRoundedRectangle(area, 7.0F);
            graphics.setColour(panelOutline);
            graphics.drawRoundedRectangle(area.reduced(0.5F), 7.0F, 1.0F);
        }
    }

    void resized() override
    {
        auto available = getLocalBounds().reduced(10, 8);
        const auto sectionWidth = std::min(680, available.getWidth());
        auto y = available.getY();

        const std::array heights { 224, 322, 92, 280, 80, 124 };
        for (auto index = std::size_t { 0 }; index < sectionAreas_.size(); ++index) {
            sectionAreas_[index] = { available.getX(), y, sectionWidth, heights[index] };
            auto header = sectionAreas_[index].reduced(10, 0).removeFromTop(sectionHeaderHeight);
            resetButtons_[index].setBounds(header.removeFromRight(56).reduced(0, 5));
            header.removeFromRight(8);
            sectionHeadings_[index].setBounds(header);
            y += heights[index] + sectionGap;
        }

        auto shared = sectionAreas_[sharedSection].reduced(12, 8);
        shared.removeFromTop(sectionHeaderHeight - 8);
        sharedStatus_.setBounds(shared.removeFromTop(30));
        layoutLabeledRow(shared.removeFromTop(rowHeight), rowLabels_[0], fftSize_);
        layoutLabeledRow(shared.removeFromTop(rowHeight), rowLabels_[1], window_);
        layoutLabeledRow(shared.removeFromTop(rowHeight), rowLabels_[2], fftRate_);
        layoutLabeledRow(shared.removeFromTop(rowHeight), rowLabels_[3], frequencySpacing_);

        auto spectrum = sectionAreas_[spectrumSection].reduced(12, 8);
        spectrum.removeFromTop(sectionHeaderHeight - 8);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), floorLabel_, floor_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), ceilingLabel_, ceiling_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), averagingLabel_, averaging_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), spectrumLabels_[0], slope_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), spectrumLabels_[1], peakHoldMode_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), spectrumLabels_[2], peakHoldDuration_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), spectrumLabels_[3], traceColor_);
        layoutLabeledRow(spectrum.removeFromTop(rowHeight), spectrumLabels_[4], fillOpacity_);

        auto peak = sectionAreas_[peakRmsSection].reduced(12, 8);
        peak.removeFromTop(sectionHeaderHeight - 8);
        peakRmsStatus_.setBounds(peak.removeFromTop(36));

        auto spectrogram = sectionAreas_[spectrogramSection].reduced(12, 8);
        spectrogram.removeFromTop(sectionHeaderHeight - 8);
        spectrogramStatus_.setBounds(spectrogram.removeFromTop(26));
        layoutLabeledRow(spectrogram.removeFromTop(rowHeight), rowLabels_[4], palette_);
        layoutLabeledRow(
            spectrogram.removeFromTop(rowHeight), spectrogramLabels_[0], colorResponse_);
        layoutLabeledRow(spectrogram.removeFromTop(rowHeight), spectrogramLabels_[1], colorFloor_);
        layoutLabeledRow(
            spectrogram.removeFromTop(rowHeight), spectrogramLabels_[2], colorCeiling_);
        layoutLabeledRow(spectrogram.removeFromTop(rowHeight), rowLabels_[5], historyDuration_);
        layoutLabeledRow(spectrogram.removeFromTop(rowHeight), spectrogramLabels_[3], historyMode_);

        auto stereo = sectionAreas_[stereoSection].reduced(12, 8);
        stereo.removeFromTop(sectionHeaderHeight - 8);
        stereoStatus_.setBounds(stereo.removeFromTop(28));

        auto loudness = sectionAreas_[loudnessSection].reduced(12, 8);
        loudness.removeFromTop(sectionHeaderHeight - 8);
        loudnessStatus_.setBounds(loudness.removeFromTop(26));
        layoutLabeledRow(loudness.removeFromTop(rowHeight), rowLabels_[6], loudnessReference_);
    }

private:
    void sliderValueChanged(juce::Slider* slider) override
    {
        if (synchronizing_)
            return;

        if (slider == &floor_)
            configuration_.spectrum.floorDb = floor_.getValue();
        else if (slider == &ceiling_)
            configuration_.spectrum.ceilingDb = ceiling_.getValue();
        else if (slider == &averaging_) {
            const auto milliseconds = averaging_.getValue();
            configuration_.spectrum.temporalAveraging.enabled = milliseconds > 0.0;
            if (milliseconds > 0.0)
                configuration_.spectrum.temporalAveraging.milliseconds = milliseconds;
        } else if (slider == &frequencySpacing_)
            configuration_.sharedAnalysis.frequencySpacing = frequencySpacing_.getValue();
        else if (slider == &peakHoldDuration_)
            configuration_.spectrum.finitePeakHoldSeconds = peakHoldDuration_.getValue();
        else if (slider == &fillOpacity_)
            configuration_.spectrum.fillOpacity = fillOpacity_.getValue() / 100.0;
        else if (slider == &colorResponse_)
            configuration_.spectrogram.colorResponse = colorResponse_.getValue();
        else if (slider == &colorFloor_)
            configuration_.spectrogram.colorFloorDb = colorFloor_.getValue();
        else if (slider == &colorCeiling_)
            configuration_.spectrogram.colorCeilingDb = colorCeiling_.getValue();
        else
            return;

        publishSanitizedConfiguration();
    }

    void resetSection(const std::size_t section)
    {
        if (synchronizing_)
            return;

        if (section == sharedSection) {
            configuration_.sharedAnalysis = AnalyzerConfigurationCodec::defaults().sharedAnalysis;
            publishSanitizedConfiguration();
        } else if (section == spectrumSection) {
            configuration_.spectrum = AnalyzerConfigurationCodec::defaults().spectrum;
            publishSanitizedConfiguration();
        } else if (section == spectrogramSection) {
            configuration_.spectrogram = AnalyzerConfigurationCodec::defaults().spectrogram;
            publishSanitizedConfiguration();
        }
    }

    void publishSanitizedConfiguration()
    {
        configuration_ = AnalyzerConfigurationCodec::sanitize(configuration_);
        synchronizeControls();
        if (configurationChanged_)
            configurationChanged_(configuration_);
    }

    void commitTraceColor()
    {
        if (synchronizing_)
            return;

        const auto parsed = parseSrgbColor(traceColor_.getText());
        if (!parsed.has_value()) {
            traceColor_.setText(formatSrgbColor(configuration_.spectrum.traceColor), false);
            return;
        }

        if (*parsed == configuration_.spectrum.traceColor) {
            traceColor_.setText(formatSrgbColor(*parsed), false);
            return;
        }

        configuration_.spectrum.traceColor = *parsed;
        publishSanitizedConfiguration();
    }

    void updatePeakHoldDurationAvailability()
    {
        const auto finite = configuration_.spectrum.peakHoldMode == SpectrumPeakHoldMode::finite;
        peakHoldDuration_.setEnabled(finite);
        peakHoldDuration_.setWantsKeyboardFocus(finite);
        peakHoldDuration_.setAlpha(finite ? 1.0F : 0.58F);
        peakHoldDuration_.setTooltip(finite
                ? "Duration of finite Spectrum peak hold before fixed decay begins"
                : "Available when Spectrum peak hold is set to Finite");
        spectrumLabels_[2].setEnabled(finite);
        spectrumLabels_[2].setAlpha(finite ? 1.0F : 0.58F);
    }

    void synchronizeControls()
    {
        const juce::ScopedValueSetter synchronizing(synchronizing_, true);
        floor_.setValue(configuration_.spectrum.floorDb, juce::dontSendNotification);
        ceiling_.setValue(configuration_.spectrum.ceilingDb, juce::dontSendNotification);
        averaging_.setValue(configuration_.spectrum.temporalAveraging.enabled
                ? configuration_.spectrum.temporalAveraging.milliseconds
                : 0.0,
            juce::dontSendNotification);
        frequencySpacing_.setValue(
            configuration_.sharedAnalysis.frequencySpacing, juce::dontSendNotification);

        slope_.setSelectedId(
            static_cast<int>(configuration_.spectrum.slope) + 1, juce::dontSendNotification);
        peakHoldMode_.setSelectedId(
            static_cast<int>(configuration_.spectrum.peakHoldMode) + 1, juce::dontSendNotification);
        peakHoldDuration_.setValue(
            configuration_.spectrum.finitePeakHoldSeconds, juce::dontSendNotification);
        updatePeakHoldDurationAvailability();
        traceColor_.setText(formatSrgbColor(configuration_.spectrum.traceColor), false);
        fillOpacity_.setValue(
            configuration_.spectrum.fillOpacity * 100.0, juce::dontSendNotification);

        constexpr std::array fftSizes { 1024, 2048, 4096, 8192, 16384 };
        const auto fftSizeChoice
            = std::find(fftSizes.begin(), fftSizes.end(), configuration_.sharedAnalysis.fftSize);
        fftSize_.setSelectedId(static_cast<int>(std::distance(fftSizes.begin(), fftSizeChoice)) + 1,
            juce::dontSendNotification);
        window_.setSelectedId(
            static_cast<int>(configuration_.sharedAnalysis.window) + 1, juce::dontSendNotification);

        constexpr std::array fftRates { 15, 30, 60, 120 };
        const auto fftRate = std::find(fftRates.begin(), fftRates.end(),
            configuration_.sharedAnalysis.requestedFftSliceRateHz);
        fftRate_.setSelectedId(static_cast<int>(std::distance(fftRates.begin(), fftRate)) + 1,
            juce::dontSendNotification);
        palette_.setSelectedId(
            static_cast<int>(configuration_.spectrogram.palette) + 1, juce::dontSendNotification);
        colorResponse_.setValue(
            configuration_.spectrogram.colorResponse, juce::dontSendNotification);
        colorFloor_.setValue(configuration_.spectrogram.colorFloorDb, juce::dontSendNotification);
        colorCeiling_.setValue(
            configuration_.spectrogram.colorCeilingDb, juce::dontSendNotification);
        const auto historyDuration = std::find(spectrogramHistoryDurations.begin(),
            spectrogramHistoryDurations.end(), configuration_.spectrogram.historyDurationSeconds);
        historyDuration_.setSelectedId(
            static_cast<int>(std::distance(spectrogramHistoryDurations.begin(), historyDuration))
                + 1,
            juce::dontSendNotification);
        historyMode_.setSelectedId(static_cast<int>(configuration_.spectrogram.historyMode) + 1,
            juce::dontSendNotification);
        loudnessReference_.setValue(
            configuration_.loudness.referenceLufs, juce::dontSendNotification);
    }

    static void layoutLabeledRow(
        juce::Rectangle<int> row, juce::Label& label, juce::Component& control)
    {
        const auto labelWidth = std::clamp(row.getWidth() * 2 / 5, 106, 148);
        label.setBounds(row.removeFromLeft(labelWidth));
        row.removeFromLeft(8);
        control.setBounds(row);
    }

    ConfigurationChanged configurationChanged_;
    CloseRequested closeRequested_;
    AnalyzerConfiguration configuration_;
    bool synchronizing_ = false;

    std::array<juce::Rectangle<int>, sectionCount> sectionAreas_;
    std::array<juce::Label, sectionCount> sectionHeadings_;
    std::array<juce::TextButton, sectionCount> resetButtons_;
    std::array<juce::Label, 7> rowLabels_;
    std::array<juce::Label, 5> spectrumLabels_;
    std::array<juce::Label, 4> spectrogramLabels_;

    juce::Label sharedStatus_;
    juce::ComboBox fftSize_;
    juce::ComboBox window_;
    juce::ComboBox fftRate_;
    juce::Slider frequencySpacing_;

    juce::Label floorLabel_;
    juce::Label ceilingLabel_;
    juce::Label averagingLabel_;
    juce::Slider floor_;
    juce::Slider ceiling_;
    juce::Slider averaging_;
    juce::ComboBox slope_;
    juce::ComboBox peakHoldMode_;
    juce::Slider peakHoldDuration_;
    juce::TextEditor traceColor_;
    juce::Slider fillOpacity_;

    juce::Label peakRmsStatus_;

    juce::Label spectrogramStatus_;
    juce::ComboBox palette_;
    juce::Slider colorResponse_;
    juce::Slider colorFloor_;
    juce::Slider colorCeiling_;
    juce::ComboBox historyDuration_;
    juce::ComboBox historyMode_;

    juce::Label stereoStatus_;

    juce::Label loudnessStatus_;
    juce::Slider loudnessReference_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Content)
};

AnalyzerSettingsPanel::AnalyzerSettingsPanel(AnalyzerConfiguration initialConfiguration,
    ConfigurationChanged configurationChanged, CloseRequested closeRequested)
    : closeRequested_(std::move(closeRequested)),
      content_(std::make_unique<Content>(
          initialConfiguration, std::move(configurationChanged), closeRequested_))
{
    setName("Analyzer settings");
    setComponentID("analyzerSettingsPanel");
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);

    titleLabel_.setText("SETTINGS", juce::dontSendNotification);
    titleLabel_.setFont(juce::Font { juce::FontOptions { 15.0F, juce::Font::bold } });
    titleLabel_.setColour(juce::Label::textColourId, primaryText);
    titleLabel_.setJustificationType(juce::Justification::centredLeft);
    titleLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(titleLabel_);

    closeButton_.setComponentID("settingsClose");
    closeButton_.setExplicitFocusOrder(1);
    closeButton_.setTooltip("Close Settings and return to the analyzer");
    closeButton_.setWantsKeyboardFocus(true);
    closeButton_.onClick = [this] {
        if (closeRequested_)
            closeRequested_();
    };
    addAndMakeVisible(closeButton_);

    viewport_.setName("Scrollable analyzer settings");
    viewport_.setComponentID("analyzerSettingsViewport");
    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(content_.get(), false);
    addAndMakeVisible(viewport_);

    juce::Desktop::getInstance().addFocusChangeListener(this);
}

AnalyzerSettingsPanel::~AnalyzerSettingsPanel()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    closeButton_.onClick = nullptr;
    viewport_.setViewedComponent(nullptr, false);
}

void AnalyzerSettingsPanel::setConfiguration(AnalyzerConfiguration configuration)
{
    content_->setConfiguration(configuration);
}

AnalyzerConfiguration AnalyzerSettingsPanel::getConfiguration() const noexcept
{
    return content_->getConfiguration();
}

void AnalyzerSettingsPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(panelBackground);
    graphics.setColour(panelOutline);
    graphics.drawHorizontalLine(headerHeight - 1, 0.0F, static_cast<float>(getWidth()));
}

void AnalyzerSettingsPanel::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop(headerHeight).reduced(12, 8);
    closeButton_.setBounds(header.removeFromRight(62));
    header.removeFromRight(8);
    titleLabel_.setBounds(header);

    viewport_.setBounds(bounds);
    const auto contentWidth = std::max(320, viewport_.getMaximumVisibleWidth());
    content_->setSize(contentWidth, contentHeight);
}

bool AnalyzerSettingsPanel::keyPressed(const juce::KeyPress& key)
{
    if (!key.isKeyCode(juce::KeyPress::escapeKey))
        return false;

    if (closeRequested_)
        closeRequested_();
    return true;
}

void AnalyzerSettingsPanel::focusInitialControl()
{
    if (closeButton_.isShowing())
        closeButton_.grabKeyboardFocus();
}

void AnalyzerSettingsPanel::globalFocusChanged(juce::Component* focusedComponent)
{
    if (focusedComponent == nullptr || !content_->isParentOf(focusedComponent))
        return;

    constexpr int focusMargin = 6;
    const auto focusedBounds
        = content_->getLocalArea(focusedComponent, focusedComponent->getLocalBounds())
              .expanded(0, focusMargin);
    const auto visibleArea = viewport_.getViewArea();
    auto nextY = visibleArea.getY();

    if (focusedBounds.getY() < visibleArea.getY())
        nextY = focusedBounds.getY();
    else if (focusedBounds.getBottom() > visibleArea.getBottom())
        nextY = focusedBounds.getBottom() - visibleArea.getHeight();

    if (nextY != visibleArea.getY())
        viewport_.setViewPosition(visibleArea.getX(), std::max(0, nextY));
}
} // namespace audio_insight
