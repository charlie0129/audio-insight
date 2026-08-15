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
constexpr int pacingGraphHeight = 190;
constexpr int latencyGraphHeight = 190;
constexpr int graphGap = 8;
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
const auto critical = juce::Colour { 0xffff6b7a };
const auto submitQueue = juce::Colour { 0xffa78bfa };
const auto gpuExecution = juce::Colour { 0xff55d6be };
const auto compositorWait = juce::Colour { 0xffffbf69 };

struct LivePacingStatistics {
    std::size_t sampleCount = 0;
    double latestMilliseconds = 0.0;
    double meanMilliseconds = 0.0;
    double percentile95Milliseconds = 0.0;
    double effectiveHertz = 0.0;
    double recentHertz = 0.0;
    bool available = false;
    bool recentRateAvailable = false;
};

std::size_t boundedPresentedHistoryCount(const MetalRenderTelemetry& telemetry) noexcept
{
    return std::min(telemetry.presentedFrameIntervalHistoryCount,
        telemetry.presentedFrameIntervalHistory.size());
}

LivePacingStatistics calculateLivePacingStatistics(const MetalRenderTelemetry& telemetry) noexcept
{
    LivePacingStatistics result;
    std::array<std::uint64_t, presentedFrameIntervalHistoryCapacity> sortedNanoseconds { };
    const auto count = boundedPresentedHistoryCount(telemetry);
    auto validCount = std::size_t { 0 };
    long double sumNanoseconds = 0.0L;

    for (std::size_t index = 0; index < count; ++index) {
        const auto& sample = telemetry.presentedFrameIntervalHistory[index];
        if (sample.sequence == 0 || sample.nanoseconds == 0)
            continue;

        sortedNanoseconds[validCount++] = sample.nanoseconds;
        sumNanoseconds += static_cast<long double>(sample.nanoseconds);
        result.latestMilliseconds = static_cast<double>(sample.nanoseconds) / 1'000'000.0;
    }

    if (validCount == 0)
        return result;

    std::sort(sortedNanoseconds.begin(), sortedNanoseconds.begin() + validCount);
    const auto percentileIndex = std::min(validCount - 1,
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(validCount))) - 1);
    result.sampleCount = validCount;
    result.meanMilliseconds
        = static_cast<double>(sumNanoseconds / static_cast<long double>(validCount)) / 1'000'000.0;
    result.percentile95Milliseconds
        = static_cast<double>(sortedNanoseconds[percentileIndex]) / 1'000'000.0;
    if (result.meanMilliseconds > 0.0)
        result.effectiveHertz = 1'000.0 / result.meanMilliseconds;

    constexpr std::uint64_t recentWindowNanoseconds = 1'000'000'000;
    auto recentNanoseconds = std::uint64_t { 0 };
    auto recentIntervals = std::size_t { 0 };
    for (auto index = count; index > 0; --index) {
        const auto& sample = telemetry.presentedFrameIntervalHistory[index - 1];
        if (sample.sequence == 0 || sample.nanoseconds == 0)
            continue;

        recentNanoseconds += sample.nanoseconds;
        ++recentIntervals;
        if (recentNanoseconds >= recentWindowNanoseconds)
            break;
    }

    if (recentNanoseconds != 0 && recentIntervals != 0) {
        result.recentHertz = static_cast<double>(recentIntervals) * 1'000'000'000.0
            / static_cast<double>(recentNanoseconds);
        result.recentRateAvailable
            = recentNanoseconds >= recentWindowNanoseconds / 2 && std::isfinite(result.recentHertz);
    }

    result.available = std::isfinite(result.latestMilliseconds)
        && std::isfinite(result.meanMilliseconds) && std::isfinite(result.percentile95Milliseconds)
        && std::isfinite(result.effectiveHertz);
    return result;
}

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

bool PerformanceMetricsSummaryCadence::consumeIfDue(const double monotonicSeconds) noexcept
{
    if (!std::isfinite(monotonicSeconds))
        return false;

    if (!hasRefreshed_) {
        lastRefreshSeconds_ = monotonicSeconds;
        hasRefreshed_ = true;
        return true;
    }

    if (monotonicSeconds < lastRefreshSeconds_
        || monotonicSeconds - lastRefreshSeconds_ < minimumIntervalSeconds) {
        return false;
    }

    lastRefreshSeconds_ = monotonicSeconds;
    return true;
}

void PerformanceMetricsSummaryCadence::reset() noexcept
{
    lastRefreshSeconds_ = 0.0;
    hasRefreshed_ = false;
}

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

        latencyGraphAccessibilityLabel_.setComponentID("frameLatencyCompositionGraphAccessibility");
        latencyGraphAccessibilityLabel_.setDescription(
            "Per-frame callback-to-presentation latency split into four general stages");
        addAndMakeVisible(latencyGraphAccessibilityLabel_);
    }

    void setRawData(PerformanceMetricsViewModel viewModel)
    {
        viewModel_ = std::move(viewModel);
        updateRawAccessibilityText();
        resized();
    }

    void setGraphData(MetalRenderTelemetry telemetry, const bool updateSummary)
    {
        graphTelemetry_ = std::move(telemetry);
        if (updateSummary) {
            summaryTelemetry_ = graphTelemetry_;
            livePacingStatistics_ = calculateLivePacingStatistics(graphTelemetry_);
            fallbackPresentedRate_ = formatRate(viewModel_.derived, "presentedFrames");
            updateLiveAccessibilityText();
        }

        repaint(getLiveGraphRegionBounds());
        if (updateSummary)
            repaint(getSummaryRegionBounds());
    }

    void repaintRawData()
    {
        repaint(getRawTableRegionBounds());
    }

    [[nodiscard]] juce::Rectangle<int> getLiveGraphRegionBounds() const noexcept
    {
        auto content = getLocalBounds().reduced(contentPadding, 0);
        content.removeFromTop(contentPadding + 22 + getSummaryHeight(content.getWidth()) + 14);
        return content.removeFromTop(pacingGraphHeight + graphGap + latencyGraphHeight);
    }

    [[nodiscard]] juce::Rectangle<int> getSummaryRegionBounds() const noexcept
    {
        auto content = getLocalBounds().reduced(contentPadding, 0);
        content.removeFromTop(contentPadding);
        return content.removeFromTop(22 + getSummaryHeight(content.getWidth()));
    }

    [[nodiscard]] juce::Rectangle<int> getRawTableRegionBounds() const noexcept
    {
        auto content = getLocalBounds().reduced(contentPadding, 0);
        content.removeFromTop(contentPadding + 22 + getSummaryHeight(content.getWidth()) + 14
            + pacingGraphHeight + graphGap + latencyGraphHeight + 16);
        return getLocalBounds().withTop(content.getY());
    }

    [[nodiscard]] int getRequiredHeight(int width) const noexcept
    {
        const auto effectiveWidth = std::max(1, width - (2 * contentPadding));
        const auto columns = effectiveWidth >= 560 ? 3 : 2;
        const auto summaryRows = (6 + columns - 1) / columns;
        auto height = contentPadding + 22 + (summaryRows * summaryCardHeight)
            + ((summaryRows - 1) * 6) + 14 + pacingGraphHeight + graphGap + latencyGraphHeight + 16;
        const auto rowHeight = metricRowHeightForWidth(effectiveWidth);

        for (const auto& section : viewModel_.sections)
            height += sectionTitleHeight + (static_cast<int>(section.rows.size()) * rowHeight)
                + sectionGap;

        return height + 32;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(background);
        const auto clip = graphics.getClipBounds();
        auto content = getLocalBounds().reduced(contentPadding, 0);
        content.removeFromTop(contentPadding);

        const auto summaryTitleBounds = content.removeFromTop(22);
        const auto summaryBounds = content.removeFromTop(getSummaryHeight(content.getWidth()));
        content.removeFromTop(14);
        const auto pacingGraphBounds = content.removeFromTop(pacingGraphHeight);
        content.removeFromTop(graphGap);
        const auto latencyGraphBounds = content.removeFromTop(latencyGraphHeight);
        content.removeFromTop(16);

        if (clip.intersects(summaryTitleBounds))
            drawSectionTitle(graphics, summaryTitleBounds, "LIVE SUMMARY");
        if (clip.intersects(summaryBounds))
            drawSummary(graphics, summaryBounds);
        if (clip.intersects(pacingGraphBounds))
            drawFrameIntervalGraph(graphics, pacingGraphBounds);
        if (clip.intersects(latencyGraphBounds))
            drawLatencyCompositionGraph(graphics, latencyGraphBounds);

        // Vblank repaints are restricted to the live graphs (and, at most,
        // the lightweight summary), so they never walk or paint the raw table.
        if (clip.getBottom() <= latencyGraphBounds.getBottom())
            return;

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

        content.removeFromTop(getSummaryHeight(content.getWidth()));
        content.removeFromTop(14);
        graphAccessibilityLabel_.setBounds(content.removeFromTop(pacingGraphHeight));
        content.removeFromTop(graphGap);
        latencyGraphAccessibilityLabel_.setBounds(content.removeFromTop(latencyGraphHeight));
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

    [[nodiscard]] static int getSummaryHeight(const int width) noexcept
    {
        const auto columns = width >= 560 ? 3 : 2;
        const auto rows = (6 + columns - 1) / columns;
        return (rows * summaryCardHeight) + ((rows - 1) * 6);
    }

    void updateRawAccessibilityText()
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
    }

    void updateLiveAccessibilityText()
    {
        const auto& statistics = livePacingStatistics_;
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
                << ", equivalent rate " << formatFrequency(statistics.effectiveHertz) << ".";
        } else {
            graphDescription << "Waiting for presented frames.";
        }

        graphAccessibilityLabel_.setText(graphDescription, juce::dontSendNotification);

        const auto latencyCount = std::min(summaryTelemetry_.frameLatencyHistoryCount,
            summaryTelemetry_.frameLatencyHistory.size());
        auto completeLatencySamples = std::size_t { 0 };
        auto unclassifiableLatencySamples = std::size_t { 0 };
        for (std::size_t index = 0; index < latencyCount; ++index) {
            const auto& sample = summaryTelemetry_.frameLatencyHistory[index];
            if (sample.totalValid && sample.componentsValid)
                ++completeLatencySamples;
            else
                ++unclassifiableLatencySamples;
        }

        auto latencyDescription = juce::String { "Frame latency composition graph. " }
            + juce::String { static_cast<int>(completeLatencySamples) }
            + " complete samples split into CPU encode, submit plus queue, GPU execution, and "
              "compositor plus display wait. "
            + juce::String { static_cast<int>(unclassifiableLatencySamples) }
            + " samples have missing or unclassifiable timing.";
        latencyGraphAccessibilityLabel_.setText(latencyDescription, juce::dontSendNotification);
    }

    void drawSummary(juce::Graphics& graphics, juce::Rectangle<int> summary) const
    {
        const auto columns = summary.getWidth() >= 560 ? 3 : 2;
        const auto gap = 6;
        const auto cardWidth = std::max(1, (summary.getWidth() - ((columns - 1) * gap)) / columns);

        const auto& metal = summaryTelemetry_;
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

        const auto liveRate = livePacingStatistics_.recentRateAvailable
            ? juce::String { livePacingStatistics_.recentHertz, 1 } + " frames/s"
            : fallbackPresentedRate_;
        const auto exactRate = livePacingStatistics_.available
            ? formatFrequency(livePacingStatistics_.effectiveHertz)
            : juce::String { "--" };
        const auto exactWindow = livePacingStatistics_.available
            ? "exact " + juce::String { static_cast<int>(livePacingStatistics_.sampleCount) } + " "
                + exactRate
            : juce::String { "exact window --" };

        const std::array<Card, 6> cards {
            Card { "PRESENTED (~1 S)", liveRate,
                exactWindow + "  target "
                    + formatFrequency(targetMilliseconds > 0.0 ? 1000.0 / targetMilliseconds : 0.0),
                healthy },
            Card { "PACING P95",
                formatIntervalStatistic(livePacingStatistics_.percentile95Milliseconds,
                    livePacingStatistics_.available),
                "mean "
                    + formatIntervalStatistic(
                        livePacingStatistics_.meanMilliseconds, livePacingStatistics_.available)
                    + "  latest "
                    + formatIntervalStatistic(
                        livePacingStatistics_.latestMilliseconds, livePacingStatistics_.available),
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
            Card { "AFTER TARGET (>0 NS)", juce::String { metal.presentationsAfterTarget },
                "last "
                    + formatMilliseconds(
                        metal.lastPresentationLatenessNanoseconds, presentationTimingAvailable),
                primaryText },
            Card { "SKIPPED", juce::String { totalDrops },
                "admission " + juce::String { metal.gpuBackpressureDrops } + "  drawable "
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

    static void drawLegendEntry(juce::Graphics& graphics, juce::Rectangle<int> bounds,
        const juce::Colour colour, const juce::String& label)
    {
        auto swatch = bounds.removeFromLeft(9).withSizeKeepingCentre(6, 6);
        graphics.setColour(colour);
        graphics.fillRect(swatch);
        bounds.removeFromLeft(2);
        graphics.setColour(mutedText);
        graphics.setFont(monospacedFont(8.5F));
        graphics.drawFittedText(label, bounds, juce::Justification::centredLeft, 1, 0.65F);
    }

    static void drawUnavailableMarker(
        juce::Graphics& graphics, const float x, const float y, const float radius = 2.5F)
    {
        graphics.setColour(critical);
        graphics.drawLine(x - radius, y - radius, x + radius, y + radius, 1.0F);
        graphics.drawLine(x - radius, y + radius, x + radius, y - radius, 1.0F);
    }

    static void drawValidZeroMarker(
        juce::Graphics& graphics, const float x, const float baseline, const juce::Colour colour)
    {
        graphics.setColour(colour);
        graphics.drawLine(x - 3.0F, baseline - 1.0F, x + 3.0F, baseline - 1.0F, 1.5F);
    }

    void drawFrameIntervalGraph(juce::Graphics& graphics, juce::Rectangle<int> bounds) const
    {
        const auto panel = bounds.toFloat();
        graphics.setColour(cardBackground);
        graphics.fillRoundedRectangle(panel, 5.0F);
        graphics.setColour(outline);
        graphics.drawRoundedRectangle(panel.reduced(0.5F), 5.0F, 1.0F);

        auto content = bounds.reduced(9, 7);
        const auto title = content.removeFromTop(17);
        graphics.setColour(secondaryText);
        graphics.setFont(juce::Font { juce::FontOptions { 10.5F, juce::Font::bold } });
        graphics.drawText("PRESENTED FRAME PACING - EVERY EXACT INTERVAL", title,
            juce::Justification::centredLeft, false);

        const auto statistics = content.removeFromTop(16);
        graphics.setColour(accent);
        graphics.setFont(monospacedFont(9.5F, true));
        const auto statisticsText = livePacingStatistics_.available
            ? "MEAN "
                + formatIntervalStatistic(
                    livePacingStatistics_.meanMilliseconds, livePacingStatistics_.available)
                + "   EFFECTIVE " + formatFrequency(livePacingStatistics_.effectiveHertz)
                + "   P95 "
                + formatIntervalStatistic(
                    livePacingStatistics_.percentile95Milliseconds, livePacingStatistics_.available)
            : juce::String { "WAITING FOR PRESENTED FRAMES" };
        graphics.drawFittedText(
            statisticsText, statistics, juce::Justification::centredLeft, 1, 0.72F);

        auto legend = content.removeFromTop(15);
        const auto legendWidth = std::max(1, legend.getWidth() / 3);
        drawLegendEntry(graphics, legend.removeFromLeft(legendWidth), healthy, "INTERVAL");
        drawLegendEntry(graphics, legend.removeFromLeft(legendWidth), warning, ">1.25x TARGET");
        drawLegendEntry(graphics, legend, critical, "SEQUENCE GAP");

        const auto& metal = graphTelemetry_;
        const auto count = boundedPresentedHistoryCount(metal);
        auto graphArea = content.reduced(0, 3);
        auto axis = graphArea.removeFromLeft(48);
        const auto graph = graphArea.toFloat();

        if (count == 0) {
            graphics.setColour(mutedText);
            graphics.setFont(monospacedFont(11.0F));
            graphics.drawText("Waiting for presented frames...", graph.toNearestInt(),
                juce::Justification::centred, false);
            return;
        }

        auto observedMaximumMilliseconds = 0.0;
        for (std::size_t index = 0; index < count; ++index) {
            observedMaximumMilliseconds = std::max(observedMaximumMilliseconds,
                static_cast<double>(metal.presentedFrameIntervalHistory[index].nanoseconds)
                    / 1'000'000.0);
        }

        const auto targetMilliseconds
            = static_cast<double>(metal.lastTargetPresentationIntervalNanoseconds) / 1'000'000.0;
        auto maximumMilliseconds = std::max(1.0, observedMaximumMilliseconds * 1.08);
        auto targetMultipleCount = 0;
        if (targetMilliseconds > 0.0) {
            constexpr auto maximumTargetMultiple = 1'000'000;
            const auto unboundedTargetMultiple
                = std::ceil(maximumMilliseconds / targetMilliseconds);
            targetMultipleCount = std::clamp(juce::roundToInt(std::min(unboundedTargetMultiple,
                                                 static_cast<double>(maximumTargetMultiple))),
                2, maximumTargetMultiple);
            maximumMilliseconds = static_cast<double>(targetMultipleCount) * targetMilliseconds;
        }

        graphics.setFont(monospacedFont(8.5F));
        if (targetMultipleCount > 0) {
            const auto labelStep = std::max(1, (targetMultipleCount + 4) / 5);
            auto previousMultiple = 0;
            for (auto labelIndex = 0; labelIndex <= 5; ++labelIndex) {
                const auto multiple
                    = labelIndex == 0 ? 1 : std::min(targetMultipleCount, labelIndex * labelStep);
                if (multiple == previousMultiple)
                    continue;
                previousMultiple = multiple;

                const auto multipleMilliseconds
                    = static_cast<double>(multiple) * targetMilliseconds;
                const auto y = graph.getBottom()
                    - (static_cast<float>(multipleMilliseconds / maximumMilliseconds)
                        * graph.getHeight());
                graphics.setColour((multiple == 1 ? accent : outline).withAlpha(0.72F));
                graphics.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
                graphics.setColour(multiple == 1 ? accent : mutedText);
                graphics.drawFittedText(
                    juce::String { multiple } + "x " + juce::String { multipleMilliseconds, 1 },
                    axis.withY(std::clamp(juce::roundToInt(y) - 7, graphArea.getY(),
                                   graphArea.getBottom() - 14))
                        .withHeight(14),
                    juce::Justification::centredRight, 1, 0.75F);

                if (multiple == targetMultipleCount)
                    break;
            }
        } else {
            graphics.setColour(outline.withAlpha(0.65F));
            for (auto line = 1; line < 4; ++line) {
                const auto y = graph.getY() + (graph.getHeight() * static_cast<float>(line) / 4.0F);
                graphics.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
            }
            graphics.setColour(mutedText);
            graphics.drawFittedText(juce::String { maximumMilliseconds, 1 } + " ms",
                axis.withY(juce::roundToInt(graph.getY())).withHeight(14),
                juce::Justification::centredRight, 1, 0.75F);
        }

        const auto sampleSpacing = graph.getWidth() / static_cast<float>(count);
        const auto strokeWidth = std::clamp(sampleSpacing * 0.62F, 1.0F, 3.0F);
        auto previousSequence = std::uint64_t { 0 };
        constexpr std::array<float, 2> gapPattern { 2.0F, 2.0F };
        for (std::size_t index = 0; index < count; ++index) {
            const auto& sample = metal.presentedFrameIntervalHistory[index];
            const auto x = graph.getX() + ((static_cast<float>(index) + 0.5F) * sampleSpacing);
            const auto milliseconds = static_cast<double>(sample.nanoseconds) / 1'000'000.0;
            const auto y = graph.getBottom()
                - (static_cast<float>(milliseconds / maximumMilliseconds) * graph.getHeight());
            const auto missedTarget
                = targetMilliseconds > 0.0 && milliseconds > targetMilliseconds * 1.25;

            graphics.setColour(missedTarget ? warning : healthy);
            graphics.drawLine(x, graph.getBottom(), x, std::max(graph.getY(), y), strokeWidth);

            if (previousSequence != 0 && sample.sequence != previousSequence + 1) {
                graphics.setColour(critical.withAlpha(0.8F));
                graphics.drawDashedLine({ x, graph.getY(), x, graph.getBottom() },
                    gapPattern.data(), static_cast<int>(gapPattern.size()), 0.85F);

                juce::Path marker;
                marker.addTriangle(
                    x, graph.getY(), x - 3.0F, graph.getY() + 5.0F, x + 3.0F, graph.getY() + 5.0F);
                graphics.fillPath(marker);
            }

            previousSequence = sample.sequence;
        }

        graphics.setColour(mutedText);
        graphics.setFont(monospacedFont(8.5F));
        graphics.drawText("0 ms",
            axis.withY(juce::roundToInt(graph.getBottom()) - 13).withHeight(13),
            juce::Justification::centredRight, false);
    }

    [[nodiscard]] const FrameLatencySample* findLatencySample(
        const std::uint64_t sequence) const noexcept
    {
        const auto latencyCount = std::min(
            graphTelemetry_.frameLatencyHistoryCount, graphTelemetry_.frameLatencyHistory.size());
        for (std::size_t index = 0; index < latencyCount; ++index) {
            if (graphTelemetry_.frameLatencyHistory[index].sequence == sequence)
                return &graphTelemetry_.frameLatencyHistory[index];
        }

        return nullptr;
    }

    void drawLatencyCompositionGraph(juce::Graphics& graphics, juce::Rectangle<int> bounds) const
    {
        const auto panel = bounds.toFloat();
        graphics.setColour(cardBackground);
        graphics.fillRoundedRectangle(panel, 5.0F);
        graphics.setColour(outline);
        graphics.drawRoundedRectangle(panel.reduced(0.5F), 5.0F, 1.0F);

        auto content = bounds.reduced(9, 7);
        const auto title = content.removeFromTop(17);
        graphics.setColour(secondaryText);
        graphics.setFont(juce::Font { juce::FontOptions { 10.5F, juce::Font::bold } });
        graphics.drawText("FRAME LATENCY COMPOSITION - SAME PRESENTED SEQUENCES", title,
            juce::Justification::centredLeft, false);

        const auto explanation = content.removeFromTop(16);
        graphics.setColour(mutedText);
        graphics.setFont(monospacedFont(9.0F));
        graphics.drawFittedText("STACK TOTAL = DISPLAY-LINK CALLBACK TO ACTUAL PRESENTATION",
            explanation, juce::Justification::centredLeft, 1, 0.72F);

        auto legend = content.removeFromTop(15);
        const auto legendWidth = std::max(1, legend.getWidth() / 5);
        drawLegendEntry(graphics, legend.removeFromLeft(legendWidth), accent, "CPU");
        drawLegendEntry(
            graphics, legend.removeFromLeft(legendWidth), submitQueue, "SUBMIT + QUEUE");
        drawLegendEntry(graphics, legend.removeFromLeft(legendWidth), gpuExecution, "GPU");
        drawLegendEntry(graphics, legend.removeFromLeft(legendWidth), compositorWait, "DISPLAY");
        drawLegendEntry(graphics, legend, critical, "X N/A");

        const auto pacingCount = boundedPresentedHistoryCount(graphTelemetry_);
        const auto latencyCount = std::min(
            graphTelemetry_.frameLatencyHistoryCount, graphTelemetry_.frameLatencyHistory.size());
        auto graphArea = content.reduced(0, 3);
        auto axis = graphArea.removeFromLeft(48);
        const auto graph = graphArea.toFloat();

        if (pacingCount == 0 || latencyCount == 0) {
            graphics.setColour(mutedText);
            graphics.setFont(monospacedFont(11.0F));
            graphics.drawText("Waiting for correlated latency samples...", graph.toNearestInt(),
                juce::Justification::centred, false);
            return;
        }

        std::array<const FrameLatencySample*, presentedFrameIntervalHistoryCapacity>
            matchedLatencySamples { };
        auto maximumNanoseconds = std::uint64_t { 0 };
        auto hasValidTotal = false;
        for (std::size_t index = 0; index < pacingCount; ++index) {
            const auto sequence = graphTelemetry_.presentedFrameIntervalHistory[index].sequence;
            const auto* sample = findLatencySample(sequence);
            matchedLatencySamples[index] = sample;
            if (sample != nullptr && sample->totalValid) {
                hasValidTotal = true;
                maximumNanoseconds = std::max(maximumNanoseconds, sample->totalNanoseconds);
            }
        }

        if (!hasValidTotal) {
            graphics.setColour(mutedText);
            graphics.setFont(monospacedFont(11.0F));
            graphics.drawText("Latency endpoints are unavailable...", graph.toNearestInt(),
                juce::Justification::centred, false);
            return;
        }

        const auto maximumMilliseconds
            = std::max(1.0, static_cast<double>(maximumNanoseconds) / 1'000'000.0 * 1.08);
        graphics.setColour(outline.withAlpha(0.65F));
        for (auto line = 1; line < 4; ++line) {
            const auto y = graph.getY() + (graph.getHeight() * static_cast<float>(line) / 4.0F);
            graphics.drawHorizontalLine(juce::roundToInt(y), graph.getX(), graph.getRight());
        }

        graphics.setColour(mutedText);
        graphics.setFont(monospacedFont(8.5F));
        graphics.drawFittedText(juce::String { maximumMilliseconds, 1 } + " ms",
            axis.withY(juce::roundToInt(graph.getY())).withHeight(14),
            juce::Justification::centredRight, 1, 0.75F);
        graphics.drawText("0 ms",
            axis.withY(juce::roundToInt(graph.getBottom()) - 13).withHeight(13),
            juce::Justification::centredRight, false);

        const auto sampleSpacing = graph.getWidth() / static_cast<float>(pacingCount);
        const auto barWidth = std::clamp(sampleSpacing * 0.68F, 1.0F, 4.0F);
        for (std::size_t index = 0; index < pacingCount; ++index) {
            const auto* sample = matchedLatencySamples[index];
            const auto x = graph.getX() + ((static_cast<float>(index) + 0.5F) * sampleSpacing);

            if (sample == nullptr || !sample->totalValid) {
                drawUnavailableMarker(graphics, x, graph.getBottom() - 4.0F);
                continue;
            }

            if (sample->totalNanoseconds == 0) {
                drawValidZeroMarker(
                    graphics, x, graph.getBottom(), sample->componentsValid ? healthy : mutedText);
                if (!sample->componentsValid)
                    drawUnavailableMarker(graphics, x, graph.getBottom() - 7.0F);
                continue;
            }

            const auto totalY = graph.getBottom()
                - (static_cast<float>(static_cast<double>(sample->totalNanoseconds) / 1'000'000.0
                       / maximumMilliseconds)
                    * graph.getHeight());
            if (!sample->componentsValid) {
                graphics.setColour(mutedText);
                graphics.drawLine(x, graph.getBottom(), x, totalY, barWidth);
                drawUnavailableMarker(graphics, x, std::max(graph.getY() + 3.0F, totalY));
                continue;
            }

            const std::array<std::pair<std::uint64_t, juce::Colour>, 4> components {
                std::pair { sample->cpuEncodeNanoseconds, accent },
                std::pair { sample->submitQueueWaitNanoseconds, submitQueue },
                std::pair { sample->gpuExecutionNanoseconds, gpuExecution },
                std::pair { sample->compositorWaitNanoseconds, compositorWait },
            };

            auto stackBottom = graph.getBottom();
            for (const auto& [nanoseconds, colour] : components) {
                if (nanoseconds == 0)
                    continue;

                const auto componentHeight = static_cast<float>(static_cast<double>(nanoseconds)
                                                 / 1'000'000.0 / maximumMilliseconds)
                    * graph.getHeight();
                const auto stackTop = std::max(graph.getY(), stackBottom - componentHeight);
                graphics.setColour(colour);
                graphics.fillRect(juce::Rectangle<float> { x - (barWidth * 0.5F), stackTop,
                    barWidth, std::max(0.75F, stackBottom - stackTop) });
                stackBottom = stackTop;
            }
        }
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

    MetalRenderTelemetry graphTelemetry_;
    MetalRenderTelemetry summaryTelemetry_;
    PerformanceMetricsViewModel viewModel_;
    LivePacingStatistics livePacingStatistics_;
    juce::String fallbackPresentedRate_ { "--" };
    AccessibilityOnlyLabel graphAccessibilityLabel_;
    AccessibilityOnlyLabel latencyGraphAccessibilityLabel_;
    std::vector<std::unique_ptr<AccessibilityOnlyLabel>> metricAccessibilityLabels_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MetricsContent)
};

PerformanceMetricsPanel::PerformanceMetricsPanel(SnapshotProvider snapshotProvider,
    ResetRenderTelemetryAction resetRenderTelemetry, ActivityProvider activityProvider,
    GraphSnapshotProvider graphSnapshotProvider, TimeProvider timeProvider)
    : snapshotProvider_(std::move(snapshotProvider)),
      resetRenderTelemetry_(std::move(resetRenderTelemetry)),
      activityProvider_(std::move(activityProvider)),
      graphSnapshotProvider_(std::move(graphSnapshotProvider)),
      timeProvider_(std::move(timeProvider)), content_(std::make_unique<MetricsContent>())
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

    vblankAttachment_ = juce::VBlankAttachment(this,
        [this](const double displayTimestampSeconds) { handleVBlank(displayTimestampSeconds); });
}

PerformanceMetricsPanel::~PerformanceMetricsPanel()
{
    vblankAttachment_ = { };
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
    summaryCadence_.reset();
    hasLiveTelemetry_ = false;
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
        summaryCadence_.reset();
        setCollectionState(false);
        return;
    }

    model_.reset();
    summaryCadence_.reset();
    hasLiveTelemetry_ = false;
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
        const auto sampleTime = currentTimeSeconds();
        auto viewModel = model_.update(snapshot, sampleTime, false);
        latestSnapshot_ = std::move(snapshot);
        latestViewModel_ = std::move(viewModel);
        content_->setRawData(latestViewModel_);
        resizeContent();
        content_->repaintRawData();

        if (!hasLiveTelemetry_ || !graphSnapshotProvider_) {
            content_->setGraphData(latestSnapshot_.metal, summaryCadence_.consumeIfDue(sampleTime));
            hasLiveTelemetry_ = true;
        }
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

void PerformanceMetricsPanel::refreshGraphNow([[maybe_unused]] const double displayTimestampSeconds)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (!isGraphRefreshVisible() || !graphSnapshotProvider_)
        return;

    try {
        auto telemetry = graphSnapshotProvider_();
        const auto summaryDue = summaryCadence_.consumeIfDue(currentTimeSeconds());
        content_->setGraphData(std::move(telemetry), summaryDue);
        hasLiveTelemetry_ = true;
    } catch (...) {
        // Diagnostics must never destabilize the host if telemetry copying fails.
    }
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
    hasLiveTelemetry_ = false;
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

void PerformanceMetricsPanel::handleVBlank(const double displayTimestampSeconds)
{
    refreshGraphNow(displayTimestampSeconds);
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

bool PerformanceMetricsPanel::isGraphRefreshVisible() const noexcept
{
    if (!pollingActive_ || !collectingMetrics_ || !isVisible() || content_ == nullptr)
        return false;

    return viewport_.getViewArea().intersects(content_->getLiveGraphRegionBounds());
}

double PerformanceMetricsPanel::currentTimeSeconds() const
{
    return timeProvider_ ? timeProvider_() : monotonicSeconds();
}

double PerformanceMetricsPanel::monotonicSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace audio_insight
