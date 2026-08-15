// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PerformanceMetricsModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace audio_insight {
/**
    Scrollable, per-instance observability dashboard displayed beside the Metal view.

    Snapshot collection and formatting run only on the message thread at a low cadence.
    The panel never overlaps the native NSViewComponent, so AppKit heavyweight-view
    ordering cannot hide it.
*/
class PerformanceMetricsPanel final : public juce::Component, private juce::Timer {
public:
    using SnapshotProvider = std::function<PerformanceMetricsSnapshot()>;
    using ResetRenderTelemetryAction = std::function<void()>;
    using ActivityProvider = std::function<bool()>;

    PerformanceMetricsPanel(SnapshotProvider snapshotProvider,
        ResetRenderTelemetryAction resetRenderTelemetry, ActivityProvider activityProvider = { });
    ~PerformanceMetricsPanel() override;

    void setPollingActive(bool shouldPoll);
    /** Starts or stops sampling when native renderer activity changes. */
    void setCollectionActivity(bool isActive);
    [[nodiscard]] bool isPollingActive() const noexcept;
    [[nodiscard]] bool isCollectingMetrics() const noexcept;

    /** Immediately samples and rebuilds the view model on the message thread. */
    void refreshNow();

    /** Performs one activity-gated polling cycle, matching the timer callback. */
    void pollNow();

    /** Resets renderer telemetry and immediately refreshes the retained diagnostics view. */
    void resetRenderTelemetryNow();

    [[nodiscard]] const PerformanceMetricsViewModel& getViewModel() const noexcept;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class MetricsContent;

    void timerCallback() override;
    void resizeContent();
    void setCollectionState(bool isCollecting);
    static double monotonicSeconds() noexcept;

    SnapshotProvider snapshotProvider_;
    ResetRenderTelemetryAction resetRenderTelemetry_;
    ActivityProvider activityProvider_;
    PerformanceMetricsModel model_;
    PerformanceMetricsSnapshot latestSnapshot_;
    PerformanceMetricsViewModel latestViewModel_;

    std::unique_ptr<MetricsContent> content_;
    juce::Viewport viewport_;
    juce::TextButton resetButton_ { "Reset render" };
    juce::TextButton copyButton_ { "Copy" };
    juce::Label collectionStatusLabel_;
    bool pollingActive_ = false;
    bool collectingMetrics_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformanceMetricsPanel)
};
} // namespace audio_insight
