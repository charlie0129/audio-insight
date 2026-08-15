// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PerformanceMetricsPanel.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace audio_insight {
namespace {
class PerformanceMetricsPanelTests final : public juce::UnitTest {
public:
    PerformanceMetricsPanelTests() : UnitTest("Performance metrics panel", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Live summary cadence has one shared 100 ms budget");
        {
            PerformanceMetricsSummaryCadence cadence;
            expect(cadence.consumeIfDue(10.0));
            expect(!cadence.consumeIfDue(10.05));
            expect(!cadence.consumeIfDue(10.099));
            expect(cadence.consumeIfDue(10.101));
            expect(!cadence.consumeIfDue(10.15));
            expect(!cadence.consumeIfDue(std::numeric_limits<double>::quiet_NaN()));
            expect(!cadence.consumeIfDue(9.0));

            cadence.reset();
            expect(cadence.consumeIfDue(9.0));
        }

        beginTest("Raw polling cannot bypass the live summary cadence");
        {
            auto currentTime = 10.0;
            auto rawEpoch = std::uint64_t { 1 };
            auto graphIntervalNanoseconds = std::uint64_t { 16'666'667 };
            PerformanceMetricsPanel panel(
                [&] {
                    PerformanceMetricsSnapshot snapshot;
                    snapshot.metal.epoch = rawEpoch;
                    snapshot.metal.metalAvailable = true;
                    snapshot.metal.presentedFrameIntervalHistoryCount = 1;
                    snapshot.metal.presentedFrameIntervalHistory[0] = { 1, 8'333'333 };
                    return snapshot;
                },
                [] { }, [] { return true; },
                [&] {
                    MetalRenderTelemetry telemetry;
                    telemetry.epoch = rawEpoch;
                    telemetry.presentedFrameIntervalHistoryCount = 1;
                    telemetry.presentedFrameIntervalHistory[0] = { 1, graphIntervalNanoseconds };
                    return telemetry;
                },
                [&] { return currentTime; });

            panel.setBounds(0, 0, 490, 600);
            panel.setVisible(true);
            panel.setPollingActive(true);

            auto* viewport = dynamic_cast<juce::Viewport*>(
                panel.findChildWithID("performanceMetricsViewport"));
            auto* content = viewport != nullptr ? viewport->getViewedComponent() : nullptr;
            auto* graph = content != nullptr ? dynamic_cast<juce::Label*>(content->findChildWithID(
                                                   "presentedFramePacingGraphAccessibility"))
                                             : nullptr;
            auto* epoch = content != nullptr ? dynamic_cast<juce::Label*>(content->findChildWithID(
                                                   "performanceMetricAccessibility0"))
                                             : nullptr;
            expect(graph != nullptr);
            expect(epoch != nullptr);

            const auto initialGraphText = graph != nullptr ? graph->getText() : juce::String { };
            expect(initialGraphText.contains("8.333"));

            currentTime = 10.05;
            panel.refreshGraphNow(currentTime);
            expectEquals(graph != nullptr ? graph->getText() : juce::String { }, initialGraphText);

            rawEpoch = 2;
            currentTime = 10.06;
            panel.pollNow();
            expect(epoch != nullptr && epoch->getText().contains("2"));
            expectEquals(graph != nullptr ? graph->getText() : juce::String { }, initialGraphText);

            currentTime = 10.101;
            panel.refreshGraphNow(currentTime);
            const auto refreshedGraphText = graph != nullptr ? graph->getText() : juce::String { };
            expect(refreshedGraphText.contains("16.667"));
            expect(refreshedGraphText != initialGraphText);

            rawEpoch = 3;
            currentTime = 10.15;
            panel.pollNow();
            expect(epoch != nullptr && epoch->getText().contains("3"));
            expectEquals(
                graph != nullptr ? graph->getText() : juce::String { }, refreshedGraphText);

            panel.setPollingActive(false);
        }

        beginTest("Inactive rendering skips repeated snapshot and model work");
        {
            auto activityPermitsCollection = false;
            auto snapshotCalls = 0;
            auto graphSnapshotCalls = 0;
            auto resetCalls = 0;
            auto throwOnReset = false;
            PerformanceMetricsPanel panel(
                [&] {
                    ++snapshotCalls;
                    PerformanceMetricsSnapshot snapshot;
                    snapshot.metal.epoch = 1;
                    snapshot.metal.metalAvailable = true;
                    return snapshot;
                },
                [&] {
                    ++resetCalls;
                    if (throwOnReset)
                        throw std::runtime_error("simulated diagnostics allocation failure");
                },
                [&] { return activityPermitsCollection; },
                [&] {
                    ++graphSnapshotCalls;
                    MetalRenderTelemetry telemetry;
                    telemetry.epoch = 1;
                    return telemetry;
                });

            panel.setBounds(0, 0, 420, 300);
            panel.setVisible(true);
            panel.setPollingActive(true);
            expectEquals(snapshotCalls, 1);
            expect(panel.isPollingActive());
            expect(!panel.isCollectingMetrics());
            auto* status = dynamic_cast<juce::Label*>(
                panel.findChildWithID("performanceMetricsCollectionStatus"));
            expect(status != nullptr);
            if (status != nullptr) {
                auto handler = status->createAccessibilityHandler();
                expect(handler != nullptr);
                if (handler != nullptr)
                    expect(handler->getTitle().containsIgnoreCase("paused"));
            }
            expect(panel.getDescription().containsIgnoreCase("retained values may be stale"));

            panel.pollNow();
            expectEquals(snapshotCalls, 1);
            panel.refreshGraphNow(1.0);
            expectEquals(graphSnapshotCalls, 0);

            activityPermitsCollection = true;
            panel.pollNow();
            expectEquals(snapshotCalls, 2);
            expect(panel.isCollectingMetrics());
            panel.refreshGraphNow(1.0);
            panel.refreshGraphNow(1.01);
            expectEquals(graphSnapshotCalls, 2);
            expectEquals(snapshotCalls, 2);

            panel.setVisible(false);
            panel.refreshGraphNow(1.02);
            expectEquals(graphSnapshotCalls, 2);
            panel.setVisible(true);
            if (status != nullptr) {
                auto handler = status->createAccessibilityHandler();
                expect(handler != nullptr);
                if (handler != nullptr)
                    expect(handler->getTitle().containsIgnoreCase("live"));
            }
            expect(panel.getDescription().containsIgnoreCase("collected live"));

            activityPermitsCollection = false;
            const auto callsBeforePause = snapshotCalls;
            panel.pollNow();
            expectEquals(snapshotCalls, callsBeforePause);
            expect(!panel.isCollectingMetrics());
            expect(status != nullptr && status->getText().containsIgnoreCase("paused"));
            panel.refreshGraphNow(1.03);
            expectEquals(graphSnapshotCalls, 2);

            auto* reset
                = dynamic_cast<juce::Button*>(panel.findChildWithID("resetRenderTelemetryButton"));
            expect(reset != nullptr);
            if (reset != nullptr) {
                panel.resetRenderTelemetryNow();
                expectEquals(resetCalls, 1);
                expectEquals(snapshotCalls, callsBeforePause + 1);
                expect(panel.getViewModel().derived.ratesRebased);
            }

            throwOnReset = true;
            const auto callsBeforeFailedReset = snapshotCalls;
            panel.resetRenderTelemetryNow();
            expectEquals(resetCalls, 2);
            expectEquals(snapshotCalls, callsBeforeFailedReset + 1);

            panel.setPollingActive(false);
        }

        beginTest("A valid zero-total latency is rendered as measured zero");
        {
            PerformanceMetricsPanel panel(
                [] {
                    PerformanceMetricsSnapshot snapshot;
                    snapshot.metal.epoch = 4;
                    snapshot.metal.metalAvailable = true;
                    snapshot.metal.presentedFrameIntervalHistoryCount = 1;
                    snapshot.metal.presentedFrameIntervalHistory[0] = { 7, 8'333'333 };
                    snapshot.metal.frameLatencyHistoryCount = 1;
                    snapshot.metal.frameLatencyHistory[0]
                        = { 7, 1'000'000, 0, 0, 0, 0, 0, true, true };
                    return snapshot;
                },
                [] { });

            panel.setBounds(0, 0, 490, 700);
            panel.setVisible(true);
            panel.setPollingActive(true);

            auto* viewport = dynamic_cast<juce::Viewport*>(
                panel.findChildWithID("performanceMetricsViewport"));
            auto* content = viewport != nullptr ? viewport->getViewedComponent() : nullptr;
            auto* latencyGraph = content != nullptr
                ? content->findChildWithID("frameLatencyCompositionGraphAccessibility")
                : nullptr;
            expect(content != nullptr);
            expect(latencyGraph != nullptr);

            auto foundValidZeroMarker = false;
            if (content != nullptr && latencyGraph != nullptr) {
                juce::Image image(
                    juce::Image::ARGB, content->getWidth(), content->getHeight(), true);
                juce::Graphics graphics(image);
                content->paintEntireComponent(graphics, true);

                const auto search = latencyGraph->getBounds().removeFromBottom(28);
                for (auto y = search.getY(); y < search.getBottom() && !foundValidZeroMarker; ++y) {
                    for (auto x = search.getX(); x < search.getRight(); ++x) {
                        const auto pixel = image.getPixelAt(x, y);
                        if (pixel.getGreen() > 100 && pixel.getGreen() > pixel.getRed() + 30
                            && pixel.getGreen() > pixel.getBlue() + 15) {
                            foundValidZeroMarker = true;
                            break;
                        }
                    }
                }
            }
            expect(foundValidZeroMarker);

            panel.setPollingActive(false);
        }

        beginTest("Scrollable metric data and graph have accessibility nodes");
        {
            PerformanceMetricsPanel panel(
                [] {
                    PerformanceMetricsSnapshot snapshot;
                    snapshot.metal.epoch = 3;
                    snapshot.metal.metalAvailable = true;
                    snapshot.metal.presentedFrameIntervalHistoryCount = 2;
                    snapshot.metal.presentedFrameIntervalHistory[0] = { 1, 8'333'333 };
                    snapshot.metal.presentedFrameIntervalHistory[1] = { 2, 8'333'334 };
                    snapshot.metal.frameLatencyHistoryCount = 2;
                    snapshot.metal.frameLatencyHistory[0] = { 1, 9'000'000, 300'000, 100'000,
                        150'000, 8'450'000, 9'000'000, true, true };
                    snapshot.metal.frameLatencyHistory[1] = { 2, 17'333'334, 350'000, 120'000,
                        180'000, 7'683'334, 8'333'334, true, true };
                    return snapshot;
                },
                [] { });

            // Exercises the 460-point stacked-row breakpoint after content
            // padding, where required-height and paint calculations must agree.
            panel.setBounds(0, 0, 490, 300);
            panel.setPollingActive(true);

            auto* viewport = dynamic_cast<juce::Viewport*>(
                panel.findChildWithID("performanceMetricsViewport"));
            auto* content = viewport != nullptr ? viewport->getViewedComponent() : nullptr;
            auto* firstMetric = content != nullptr
                ? content->findChildWithID("performanceMetricAccessibility0")
                : nullptr;
            auto* secondMetric = content != nullptr
                ? content->findChildWithID("performanceMetricAccessibility1")
                : nullptr;
            auto* graph = content != nullptr
                ? content->findChildWithID("presentedFramePacingGraphAccessibility")
                : nullptr;
            auto* latencyGraph = content != nullptr
                ? content->findChildWithID("frameLatencyCompositionGraphAccessibility")
                : nullptr;
            expect(viewport != nullptr);
            expect(content != nullptr);
            expect(content != nullptr && viewport != nullptr
                && content->getHeight() > viewport->getHeight());
            expect(firstMetric != nullptr);
            expect(secondMetric != nullptr);
            expect(graph != nullptr);
            expect(latencyGraph != nullptr);
            if (firstMetric != nullptr && secondMetric != nullptr) {
                expect(firstMetric->getHeight() > 20);
                expectEquals(firstMetric->getBottom(), secondMetric->getY());
            }

            auto* lastMetric = firstMetric;
            if (content != nullptr) {
                for (auto index = 1;; ++index) {
                    auto* candidate = content->findChildWithID(
                        juce::String { "performanceMetricAccessibility" } + juce::String { index });
                    if (candidate == nullptr)
                        break;

                    lastMetric = candidate;
                }
            }
            expect(lastMetric != nullptr && content != nullptr
                && lastMetric->getBottom() <= content->getHeight());

            if (firstMetric != nullptr) {
                auto* label = dynamic_cast<juce::Label*>(firstMetric);
                expect(label != nullptr);
                auto handler = label != nullptr ? label->createAccessibilityHandler() : nullptr;
                expect(handler != nullptr);
                if (handler != nullptr)
                    expect(handler->getTitle().containsIgnoreCase("telemetry epoch"));
            }

            if (graph != nullptr) {
                auto* label = dynamic_cast<juce::Label*>(graph);
                expect(label != nullptr);
                auto handler = label != nullptr ? label->createAccessibilityHandler() : nullptr;
                expect(handler != nullptr);
                if (handler != nullptr) {
                    expect(handler->getTitle().containsIgnoreCase("exact intervals"));
                    expect(handler->getTitle().containsIgnoreCase("p95"));
                }
            }

            if (latencyGraph != nullptr) {
                auto* label = dynamic_cast<juce::Label*>(latencyGraph);
                expect(label != nullptr);
                auto handler = label != nullptr ? label->createAccessibilityHandler() : nullptr;
                expect(handler != nullptr);
                if (handler != nullptr) {
                    expect(handler->getTitle().containsIgnoreCase("latency composition"));
                    expect(handler->getTitle().containsIgnoreCase("submit plus queue"));
                }
            }

            panel.setPollingActive(false);
        }
    }
};

static PerformanceMetricsPanelTests performanceMetricsPanelTests;
} // namespace
} // namespace audio_insight
