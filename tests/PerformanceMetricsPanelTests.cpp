// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PerformanceMetricsPanel.h"

#include <cstdint>
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
        beginTest("Inactive rendering skips repeated snapshot and model work");
        {
            auto activityPermitsCollection = false;
            auto snapshotCalls = 0;
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
                [&] { return activityPermitsCollection; });

            panel.setBounds(0, 0, 420, 300);
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

            activityPermitsCollection = true;
            panel.pollNow();
            expectEquals(snapshotCalls, 2);
            expect(panel.isCollectingMetrics());
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
            expect(viewport != nullptr);
            expect(content != nullptr);
            expect(content != nullptr && viewport != nullptr
                && content->getHeight() > viewport->getHeight());
            expect(firstMetric != nullptr);
            expect(secondMetric != nullptr);
            expect(graph != nullptr);
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

            panel.setPollingActive(false);
        }
    }
};

static PerformanceMetricsPanelTests performanceMetricsPanelTests;
} // namespace
} // namespace audio_insight
