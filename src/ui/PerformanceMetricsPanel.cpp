// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PerformanceMetricsPanel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace audio_insight {
namespace {
constexpr int headerHeight = 48;
constexpr int contentPadding = 12;
constexpr int sectionTitleHeight = 27;
constexpr int metricRowHeight = 20;
constexpr int sectionGap = 10;
constexpr int graphHeight = 176;
constexpr int summaryCardHeight = 54;
constexpr int pollingIntervalMilliseconds = 250;
constexpr int narrowMetricRowHeight = 34;

const auto background = juce::Colour { 0xff0d131c };
const auto headerBackground = juce::Colour { 0xff111a25 };
const auto cardBackground = juce::Colour { 0xff151f2b };
const auto alternatingRow = juce::Colour { 0xff111923 };
const auto outline = juce::Colour { 0xff304156 };
const auto primaryText = juce::Colour { 0xffedf4fc };
const auto secondaryText = juce::Colour { 0xffa9b8ca };
const auto mutedText = juce::Colour { 0xff718399 };
const auto accent = juce::Colour { 0xff55c7e8 };
const auto healthy = juce::Colour { 0xff73d69b };
const auto warning = juce::Colour { 0xffffbf69 };

juce::String toJuceString(const std::string& value)
{
    return juce::String::fromUTF8(value.c_str(), static_cast<int>(value.size()));
}

int metricRowHeightForWidth(const int width) noexcept
{
    return width < 460 ? narrowMetricRowHeight : metricRowHeight;
}

juce::String formatMilliseconds(const std::uint64_t nanoseconds, const bool available)
{
    if (!available)
        return "--";

    return juce::String { static_cast<double>(nanoseconds) / 1'000'000.0, 3 } + " ms";
}

juce::String formatRate(const PerformanceMetricsDerived& derived, const std::string& suffix)
{
    const auto match
        = std::find_if(derived.rates.begin(), derived.rates.end(), [&](const auto& rate) {
              return rate.sourceFieldName == suffix || rate.sourceFieldName.ends_with("." + suffix);
          });

    if (match == derived.rates.end() || !match->available || !std::isfinite(match->value))
        return "--";

    return juce::String { match->value, 1 } + " " + toJuceString(match->unit);
}

juce::String formatIntervalStatistic(const double milliseconds, const bool available)
{
    if (!available || !std::isfinite(milliseconds) || milliseconds < 0.0)
        return "--";

    return juce::String { milliseconds, 3 } + " ms";
}

juce::String formatFrequency(double hertz)
{
    if (!std::isfinite(hertz) || hertz <= 0.0)
        return "--";

    return juce::String { hertz, 1 } + " Hz";
}

juce::Font monospacedFont(float height, bool bold = false)
{
    return juce::Font { juce::FontOptions { juce::Font::getDefaultMonospacedFontName(), height,
        bold ? juce::Font::bold : juce::Font::plain } };
}

void drawSummaryCard(juce::Graphics& graphics, juce::Rectangle<int> bounds,
    const juce::String& label, const juce::String& value, const juce::String& detail,
    juce::Colour valueColour = primaryText)
{
    const auto card = bounds.toFloat();
    graphics.setColour(cardBackground);
    graphics.fillRoundedRectangle(card, 5.0F);
    graphics.setColour(outline);
    graphics.drawRoundedRectangle(card.reduced(0.5F), 5.0F, 1.0F);

    auto content = bounds.reduced(8, 5);
    graphics.setColour(secondaryText);
    graphics.setFont(juce::Font { juce::FontOptions { 10.5F, juce::Font::bold } });
    graphics.drawText(label, content.removeFromTop(14), juce::Justification::centredLeft, false);

    auto valueRow = content.removeFromTop(19);
    graphics.setColour(valueColour);
    graphics.setFont(monospacedFont(15.0F, true));
    graphics.drawText(value, valueRow, juce::Justification::centredLeft, false);

    graphics.setColour(mutedText);
    graphics.setFont(monospacedFont(9.5F));
    graphics.drawText(detail, content, juce::Justification::centredLeft, false);
}
} // namespace

class PerformanceMetricsPanel::MetricsContent final : public juce::Component {
public:
    MetricsContent()
    {
        setName("All performance metrics");
        setComponentID("performanceMetricsContent");
        setInterceptsMouseClicks(false, true);

        graphAccessibilityLabel_.setComponentID("presentedFramePacingGraphAccessibility");
        graphAccessibilityLabel_.setDescription(
            "Exact chronological history of actual drawable presentation intervals");
        addAndMakeVisible(graphAccessibilityLabel_);
    }

    void setData(PerformanceMetricsSnapshot snapshot, PerformanceMetricsViewModel viewModel)
    {
        snapshot_ = std::move(snapshot);
        viewModel_ = std::move(viewModel);
        updateAccessibilityText();
        resized();
        repaint();
    }

    [[nodiscard]] int getRequiredHeight(int width) const noexcept
    {
        const auto effectiveWidth = std::max(1, width - (2 * contentPadding));
        const auto columns = effectiveWidth >= 560 ? 3 : 2;
        const auto summaryRows = (6 + columns - 1) / columns;
        auto height = contentPadding + 22 + (summaryRows * summaryCardHeight)
            + ((summaryRows - 1) * 6) + 14 + graphHeight + 16;
        const auto rowHeight = metricRowHeightForWidth(effectiveWidth);

        for (const auto& section : viewModel_.sections)
            height += sectionTitleHeight + (static_cast<int>(section.rows.size()) * rowHeight)
                + sectionGap;

        return height + 32;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(background);
        auto content = getLocalBounds().reduced(contentPadding, 0);
        content.removeFromTop(contentPadding);

        drawSectionTitle(graphics, content.removeFromTop(22), "LIVE SUMMARY");
        drawSummary(graphics, content);
        content.removeFromTop(14);
        drawFrameIntervalGraph(graphics, content.removeFromTop(graphHeight));
        content.removeFromTop(16);
        const auto rowHeight = metricRowHeightForWidth(content.getWidth());

        for (const auto& section : viewModel_.sections) {
            drawSectionTitle(
                graphics, content.removeFromTop(sectionTitleHeight), toJuceString(section.name));

            for (std::size_t rowIndex = 0; rowIndex < section.rows.size(); ++rowIndex) {
                auto rowBounds = content.removeFromTop(rowHeight);
                if ((rowIndex % 2) != 0) {
                    graphics.setColour(alternatingRow);
                    graphics.fillRect(rowBounds);
                }

                drawMetricRow(graphics, rowBounds, section.rows[rowIndex]);
            }

            content.removeFromTop(sectionGap);
        }

        graphics.setColour(mutedText);
        graphics.setFont(juce::Font { juce::FontOptions { 10.0F } });
        graphics.drawText("Copy exports stable field names, raw values, units, and derived rates.",
            content.removeFromTop(20), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(contentPadding, 0);
        content.removeFromTop(contentPadding + 22);

        const auto columns = content.getWidth() >= 560 ? 3 : 2;
        const auto summaryRows = (6 + columns - 1) / columns;
        content.removeFromTop((summaryRows * summaryCardHeight) + ((summaryRows - 1) * 6));
        content.removeFromTop(14);
        graphAccessibilityLabel_.setBounds(content.removeFromTop(graphHeight));
        content.removeFromTop(16);
        const auto rowHeight = metricRowHeightForWidth(content.getWidth());

        auto accessibleRowIndex = std::size_t { 0 };
        for (const auto& section : viewModel_.sections) {
            content.removeFromTop(sectionTitleHeight);

            for ([[maybe_unused]] const auto& row : section.rows) {
                if (accessibleRowIndex < metricAccessibilityLabels_.size()) {
                    metricAccessibilityLabels_[accessibleRowIndex]->setBounds(
                        content.removeFromTop(rowHeight));
                } else {
                    content.removeFromTop(rowHeight);
                }

                ++accessibleRowIndex;
            }

            content.removeFromTop(sectionGap);
        }
    }

private:
    class AccessibilityOnlyLabel final : public juce::Label {
    public:
        AccessibilityOnlyLabel()
        {
            setInterceptsMouseClicks(false, false);
            setAccessible(true);
        }

        void paint(juce::Graphics&) override
        {
        }
    };

    void updateAccessibilityText()
    {
        auto rowCount = std::size_t { 0 };
        for (const auto& section : viewModel_.sections)
            rowCount += section.rows.size();

        if (metricAccessibilityLabels_.size() != rowCount) {
            metricAccessibilityLabels_.clear();
            metricAccessibilityLabels_.reserve(rowCount);

            for (std::size_t index = 0; index < rowCount; ++index) {
                auto label = std::make_unique<AccessibilityOnlyLabel>();
                label->setComponentID(juce::String { "performanceMetricAccessibility" }
                    + juce::String { static_cast<int>(index) });
                addAndMakeVisible(*label);
                metricAccessibilityLabels_.emplace_back(std::move(label));
            }
        }

        auto accessibleRowIndex = std::size_t { 0 };
        for (const auto& section : viewModel_.sections) {
            for (const auto& row : section.rows) {
                auto value = toJuceString(row.value);
                if (!row.unit.empty())
                    value << " " << toJuceString(row.unit);

                auto& label = *metricAccessibilityLabels_[accessibleRowIndex++];
                label.setText(toJuceString(row.label) + ": " + value, juce::dontSendNotification);
                label.setDescription(toJuceString(row.fieldName));
            }
        }

        const auto& statistics = viewModel_.derived.frameIntervals.presentedFrames;
        auto graphDescription = juce::String { "Presented frame pacing graph. " };

        if (statistics.available) {
            graphDescription
                << juce::String { static_cast<int>(statistics.sampleCount) } << " exact intervals. "
                << "Latest "
                << formatIntervalStatistic(statistics.latestMilliseconds, statistics.available)
                << ", p95 "
                << formatIntervalStatistic(
                       statistics.percentile95Milliseconds, statistics.available)
                << ", mean "
                << formatIntervalStatistic(statistics.meanMilliseconds, statistics.available)
                << ", minimum "
                << formatIntervalStatistic(statistics.minimumMilliseconds, statistics.available)
                << ", maximum "
                << formatIntervalStatistic(statistics.maximumMilliseconds, statistics.available)
                << ", standard deviation "
                << formatIntervalStatistic(
                       statistics.standardDeviationMilliseconds, statistics.available)
                << ", equivalent rate " << formatFrequency(statistics.equivalentHertz) << ".";
        } else {
            graphDescription << "Waiting for presented frames.";
        }

        graphAccessibilityLabel_.setText(graphDescription, juce::dontSendNotification);
    }

    void drawSummary(juce::Graphics& graphics, juce::Rectangle<int>& content) const
    {
        const auto columns = content.getWidth() >= 560 ? 3 : 2;
        const auto rows = (6 + columns - 1) / columns;
        const auto gap = 6;
        const auto cardWidth = std::max(1, (content.getWidth() - ((columns - 1) * gap)) / columns);
        const auto summaryHeight = (rows * summaryCardHeight) + ((rows - 1) * gap);
        const auto summary = content.removeFromTop(summaryHeight);

        const auto& metal = snapshot_.metal;
        const auto& presented = viewModel_.derived.frameIntervals.presentedFrames;
        const auto targetMilliseconds
            = static_cast<double>(metal.lastTargetPresentationIntervalNanoseconds) / 1'000'000.0;
        const auto totalDrops = metal.skippedPresentations;
        const auto cpuTimingAvailable = metal.submittedFrames != 0;
        const auto gpuTimingAvailable = metal.gpuTimingSamples != 0;
        const auto presentationTimingAvailable = metal.presentationLatenessSamples != 0;

        struct Card {
            juce::String label;
            juce::String value;
            juce::String detail;
            juce::Colour colour;
        };

        const std::array<Card, 6> cards {
            Card { "PRESENTED", formatRate(viewModel_.derived, "presentedFrames"),
                "target "
                    + formatFrequency(targetMilliseconds > 0.0 ? 1000.0 / targetMilliseconds : 0.0),
                healthy },
            Card { "FRAME INTERVAL",
                formatIntervalStatistic(presented.latestMilliseconds, presented.available),
                "p95 "
                    + formatIntervalStatistic(
                        presented.percentile95Milliseconds, presented.available),
                accent },
            Card { "CPU ENCODE",
                formatMilliseconds(metal.lastCpuEncodeNanoseconds, cpuTimingAvailable),
                "max " + formatMilliseconds(metal.maximumCpuEncodeNanoseconds, cpuTimingAvailable),
                primaryText },
            Card { "GPU EXECUTE",
                formatMilliseconds(metal.lastGpuExecutionNanoseconds, gpuTimingAvailable),
                "max "
                    + formatMilliseconds(metal.maximumGpuExecutionNanoseconds, gpuTimingAvailable),
                primaryText },
            Card { "AFTER TARGET", juce::String { metal.presentationsAfterTarget },
                "last "
                    + formatMilliseconds(
                        metal.lastPresentationLatenessNanoseconds, presentationTimingAvailable),
                metal.presentationsAfterTarget == 0 ? healthy : warning },
            Card { "SKIPPED", juce::String { totalDrops },
                "GPU " + juce::String { metal.gpuBackpressureDrops } + "  drawable "
                    + juce::String { metal.drawableUnavailableDrops },
                totalDrops == 0 ? healthy : warning }
        };

        for (std::size_t index = 0; index < cards.size(); ++index) {
            const auto column = static_cast<int>(index) % columns;
            const auto row = static_cast<int>(index) / columns;
            const auto x = summary.getX() + (column * (cardWidth + gap));
            const auto y = summary.getY() + (row * (summaryCardHeight + gap));
            drawSummaryCard(graphics, { x, y, cardWidth, summaryCardHeight }, cards[index].label,
                cards[index].value, cards[index].detail, cards[index].colour);
        }
    }

    void drawFrameIntervalGraph(juce::Graphics& graphics, juce::Rectangle<int> bounds) const
    {
        const auto panel = bounds.toFloat();
        graphics.setColour(cardBackground);
        graphics.fillRoundedRectangle(panel, 5.0F);
        graphics.setColour(outline);
        graphics.drawRoundedRectangle(panel.reduced(0.5F), 5.0F, 1.0F);

        auto content = bounds.reduced(9, 7);
        auto title = content.removeFromTop(18);
        graphics.setColour(secondaryText);
        graphics.setFont(juce::Font { juce::FontOptions { 10.5F, juce::Font::bold } });
        graphics.drawText(
            "PRESENTED FRAME PACING - EXACT 240", title, juce::Justification::centredLeft, false);

        const auto& metal = snapshot_.metal;
        const auto count = std::min(
            metal.presentedFrameIntervalHistoryCount, metal.presentedFrameIntervalHistory.size());
        auto graph = content.reduced(0, 5).toFloat();

        if (count < 2) {
            graphics.setColour(mutedText);
            graphics.setFont(monospacedFont(11.0F));
            graphics.drawText("Waiting for presented frames...", graph.toNearestInt(),
                juce::Justification::centred, false);
            return;
        }

        auto maximumMilliseconds = 20.0;
        for (std::size_t index = 0; index < count; ++index) {
            maximumMilliseconds = std::max(maximumMilliseconds,
                static_cast<double>(metal.presentedFrameIntervalHistory[index].nanoseconds)
                    / 1'000'000.0);
        }

        const auto targetMilliseconds
            = static_cast<double>(metal.lastTargetPresentationIntervalNanoseconds) / 1'000'000.0;
        maximumMilliseconds
            = std::max(maximumMilliseconds * 1.08, std::max(20.0, targetMilliseconds * 2.1));

        graphics.setColour(outline.withAlpha(0.65F));
        for (auto line = 1; line < 4; ++line) {
            const auto y = graph.getY() + (graph.getHeight() * static_cast<float>(line) / 4.0F);
            graphics.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        }

        if (targetMilliseconds > 0.0) {
            const auto targetY = graph.getBottom()
                - (static_cast<float>(targetMilliseconds / maximumMilliseconds)
                    * graph.getHeight());
            graphics.setColour(accent.withAlpha(0.65F));
            graphics.drawLine(graph.getX(), targetY, graph.getRight(), targetY, 1.0F);
        }

        juce::Path path;
        auto previousSequence = std::uint64_t { 0 };
        for (std::size_t index = 0; index < count; ++index) {
            const auto& sample = metal.presentedFrameIntervalHistory[index];
            const auto x = graph.getX()
                + (static_cast<float>(index) / static_cast<float>(count - 1) * graph.getWidth());
            const auto milliseconds = static_cast<double>(sample.nanoseconds) / 1'000'000.0;
            const auto y = graph.getBottom()
                - (static_cast<float>(milliseconds / maximumMilliseconds) * graph.getHeight());

            if (previousSequence == 0 || sample.sequence != previousSequence + 1)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);

            previousSequence = sample.sequence;
        }

        graphics.setColour(healthy);
        graphics.strokePath(path, juce::PathStrokeType { 1.35F });

        graphics.setFont(monospacedFont(9.5F));
        graphics.setColour(mutedText);
        graphics.drawText(juce::String { maximumMilliseconds, 1 } + " ms",
            juce::roundToInt(graph.getX()) + 3, juce::roundToInt(graph.getY()), 72, 14,
            juce::Justification::centredLeft, false);
        graphics.drawText("0 ms", juce::roundToInt(graph.getX()) + 3,
            juce::roundToInt(graph.getBottom()) - 14, 60, 14, juce::Justification::centredLeft,
            false);
    }

    static void drawSectionTitle(
        juce::Graphics& graphics, juce::Rectangle<int> bounds, const juce::String& title)
    {
        graphics.setColour(accent);
        graphics.setFont(juce::Font { juce::FontOptions { 11.5F, juce::Font::bold } });
        graphics.drawText(title, bounds, juce::Justification::centredLeft, false);
        graphics.setColour(outline);
        graphics.drawHorizontalLine(bounds.getBottom() - 1, static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
    }

    static void drawMetricRow(
        juce::Graphics& graphics, juce::Rectangle<int> bounds, const PerformanceMetricRow& row)
    {
        auto content = bounds.reduced(5, 0);
        auto labelArea = content;
        auto valueArea = content;

        if (bounds.getHeight() >= narrowMetricRowHeight) {
            labelArea = content.removeFromTop(16);
            valueArea = content;
        } else {
            valueArea = content.removeFromRight(std::max(112, content.getWidth() * 45 / 100));
            labelArea = content;
            labelArea.removeFromRight(7);
        }

        graphics.setColour(secondaryText);
        graphics.setFont(juce::Font { juce::FontOptions { 10.5F } });
        graphics.drawFittedText(
            toJuceString(row.label), labelArea, juce::Justification::centredLeft, 1, 0.65F);

        auto value = toJuceString(row.value);
        if (!row.unit.empty())
            value << " " << toJuceString(row.unit);

        graphics.setColour(primaryText);
        graphics.setFont(monospacedFont(10.5F));
        graphics.drawFittedText(value, valueArea, juce::Justification::centredRight, 1, 0.6F);
    }

    PerformanceMetricsSnapshot snapshot_;
    PerformanceMetricsViewModel viewModel_;
    AccessibilityOnlyLabel graphAccessibilityLabel_;
    std::vector<std::unique_ptr<AccessibilityOnlyLabel>> metricAccessibilityLabels_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetricsContent)
};

PerformanceMetricsPanel::PerformanceMetricsPanel(SnapshotProvider snapshotProvider,
    ResetRenderTelemetryAction resetRenderTelemetry, ActivityProvider activityProvider)
    : snapshotProvider_(std::move(snapshotProvider)),
      resetRenderTelemetry_(std::move(resetRenderTelemetry)),
      activityProvider_(std::move(activityProvider)), content_(std::make_unique<MetricsContent>())
{
    setName("Performance metrics");
    setTitle("Performance metrics");
    setDescription("Live renderer and audio-analysis observability dashboard");
    setComponentID("performanceMetricsPanel");
    setOpaque(true);
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);

    viewport_.setViewedComponent(content_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setScrollBarThickness(10);
    viewport_.setWantsKeyboardFocus(true);
    viewport_.setComponentID("performanceMetricsViewport");
    addAndMakeVisible(viewport_);

    resetButton_.setTooltip(
        "Reset renderer counters, maxima, and exact frame-interval history for this instance");
    resetButton_.setComponentID("resetRenderTelemetryButton");
    resetButton_.setWantsKeyboardFocus(true);
    resetButton_.setColour(juce::TextButton::buttonColourId, cardBackground);
    resetButton_.setColour(juce::TextButton::textColourOffId, secondaryText);
    resetButton_.onClick = [this] { resetRenderTelemetryNow(); };
    addAndMakeVisible(resetButton_);

    copyButton_.setTooltip("Copy every raw and derived metric to the clipboard");
    copyButton_.setComponentID("copyPerformanceMetricsButton");
    copyButton_.setWantsKeyboardFocus(true);
    copyButton_.setColour(juce::TextButton::buttonColourId, cardBackground);
    copyButton_.setColour(juce::TextButton::textColourOffId, secondaryText);
    copyButton_.onClick = [this] {
        try {
            if (latestViewModel_.sections.empty())
                refreshNow();

            if (latestViewModel_.report.empty())
                latestViewModel_.report
                    = PerformanceMetricsModel::buildCopyReport(latestViewModel_, latestSnapshot_);

            juce::SystemClipboard::copyTextToClipboard(toJuceString(latestViewModel_.report));
        } catch (...) {
            // Diagnostics must never destabilize the host if export allocation fails.
        }
    };
    addAndMakeVisible(copyButton_);

    collectionStatusLabel_.setName("Metrics collection status");
    collectionStatusLabel_.setComponentID("performanceMetricsCollectionStatus");
    collectionStatusLabel_.setJustificationType(juce::Justification::centredRight);
    collectionStatusLabel_.setFont(monospacedFont(9.5F, true));
    collectionStatusLabel_.setInterceptsMouseClicks(true, false);
    collectionStatusLabel_.setAccessible(true);
    addAndMakeVisible(collectionStatusLabel_);
    setCollectionState(false);
}

PerformanceMetricsPanel::~PerformanceMetricsPanel()
{
    stopTimer();
    resetButton_.onClick = nullptr;
    copyButton_.onClick = nullptr;
    viewport_.setViewedComponent(nullptr, false);
}

void PerformanceMetricsPanel::setPollingActive(const bool shouldPoll)
{
    if (pollingActive_ == shouldPoll)
        return;

    pollingActive_ = shouldPoll;
    model_.reset();
    setCollectionState(false);

    if (pollingActive_) {
        const auto activityPermitsCollection = !activityProvider_ || activityProvider_();
        setCollectionState(activityPermitsCollection);
        refreshNow();

        if (!activityPermitsCollection)
            model_.reset();

        if (activityPermitsCollection)
            startTimer(pollingIntervalMilliseconds);
    } else {
        stopTimer();
    }
}

void PerformanceMetricsPanel::setCollectionActivity(const bool isActive)
{
    if (!pollingActive_ || collectingMetrics_ == isActive)
        return;

    if (!isActive) {
        stopTimer();
        model_.reset();
        setCollectionState(false);
        return;
    }

    model_.reset();
    setCollectionState(true);
    refreshNow();
    startTimer(pollingIntervalMilliseconds);
}

bool PerformanceMetricsPanel::isPollingActive() const noexcept
{
    return pollingActive_;
}

bool PerformanceMetricsPanel::isCollectingMetrics() const noexcept
{
    return collectingMetrics_;
}

void PerformanceMetricsPanel::refreshNow()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (!snapshotProvider_)
        return;

    try {
        auto snapshot = snapshotProvider_();
        auto viewModel = model_.update(snapshot, monotonicSeconds(), false);
        latestSnapshot_ = std::move(snapshot);
        latestViewModel_ = std::move(viewModel);
        content_->setData(latestSnapshot_, latestViewModel_);
        resizeContent();
        repaint();
    } catch (...) {
        // Diagnostics must never destabilize the host if formatting allocation fails.
    }
}

const PerformanceMetricsViewModel& PerformanceMetricsPanel::getViewModel() const noexcept
{
    return latestViewModel_;
}

void PerformanceMetricsPanel::pollNow()
{
    const auto activityPermitsCollection = !activityProvider_ || activityProvider_();

    if (!activityPermitsCollection) {
        setCollectionActivity(false);
        return;
    }

    if (!collectingMetrics_) {
        setCollectionActivity(true);
        return;
    }

    refreshNow();
}

void PerformanceMetricsPanel::resetRenderTelemetryNow()
{
    try {
        if (resetRenderTelemetry_)
            resetRenderTelemetry_();
    } catch (...) {
        refreshNow();
        return;
    }

    model_.reset();
    refreshNow();
}

void PerformanceMetricsPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(background);
    auto header = getLocalBounds().removeFromTop(headerHeight);
    graphics.setColour(headerBackground);
    graphics.fillRect(header);
    graphics.setColour(outline);
    graphics.drawHorizontalLine(header.getBottom() - 1, static_cast<float>(header.getX()),
        static_cast<float>(header.getRight()));

    auto titleArea = header.reduced(11, 0);
    titleArea.setRight(std::max(titleArea.getX(), collectionStatusLabel_.getX() - 6));
    graphics.setColour(primaryText);
    graphics.setFont(juce::Font { juce::FontOptions { 12.5F, juce::Font::bold } });
    graphics.drawText("METRICS", titleArea, juce::Justification::centredLeft, false);
}

void PerformanceMetricsPanel::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop(headerHeight).reduced(8, 7);

    copyButton_.setBounds(header.removeFromRight(54));
    header.removeFromRight(6);
    resetButton_.setBounds(header.removeFromRight(88));
    header.removeFromRight(8);
    collectionStatusLabel_.setBounds(header.removeFromRight(48));

    viewport_.setBounds(bounds);
    resizeContent();
}

void PerformanceMetricsPanel::timerCallback()
{
    pollNow();
}

void PerformanceMetricsPanel::resizeContent()
{
    if (content_ == nullptr)
        return;

    const auto contentWidth = std::max(1, viewport_.getWidth() - viewport_.getScrollBarThickness());
    const auto contentHeight
        = std::max(viewport_.getHeight(), content_->getRequiredHeight(contentWidth));
    content_->setSize(contentWidth, contentHeight);
}

void PerformanceMetricsPanel::setCollectionState(const bool isCollecting)
{
    const auto hadStatus = collectionStatusLabel_.getText().isNotEmpty();
    const auto stateChanged = collectingMetrics_ != isCollecting || !hadStatus;
    collectingMetrics_ = isCollecting;

    if (!stateChanged)
        return;

    const auto status = isCollecting ? juce::String { "LIVE" } : juce::String { "PAUSED" };
    const auto detail = isCollecting
        ? juce::String { "Performance metrics are being collected live for this plugin instance" }
        : juce::String { "Performance metrics collection is paused; retained values may be stale" };

    collectionStatusLabel_.setText(status, juce::dontSendNotification);
    collectionStatusLabel_.setDescription(detail);
    collectionStatusLabel_.setTooltip(detail);
    collectionStatusLabel_.setColour(juce::Label::textColourId, isCollecting ? healthy : mutedText);
    setDescription(detail);
    collectionStatusLabel_.repaint();
    repaint(0, 0, getWidth(), headerHeight);

    if (!hadStatus)
        return;

    if (auto* handler = collectionStatusLabel_.getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);

    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::structureChanged);
}

double PerformanceMetricsPanel::monotonicSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace audio_insight
