// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PerformanceMetricsModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace audio_insight {
/** Deterministic shared budget for all live-summary recompute/repaint sources. */
class PerformanceMetricsSummaryCadence final {
public:
    static constexpr double minimumIntervalSeconds = 0.1;

    /** Consumes a refresh opportunity only when at least 100 ms has elapsed. */
    [[nodiscard]] bool consumeIfDue(double monotonicSeconds) noexcept;
    void reset() noexcept;

private:
    double lastRefreshSeconds_ = 0.0;
    bool hasRefreshed_ = false;
};

/**
    Scrollable, per-instance observability dashboard displayed beside the Metal view.

    Raw snapshot collection and formatting run only on the message thread at a low cadence.
    Renderer graph histories use a separate display-vblank snapshot path that does not rebuild
    raw rows or their accessibility hierarchy. The panel never overlaps the native
    NSViewComponent, so AppKit heavyweight-view ordering cannot hide it.
*/
class PerformanceMetricsPanel final : public juce::Component, private juce::Timer {
public:
    using SnapshotProvider = std::function<PerformanceMetricsSnapshot()>;
    using GraphSnapshotProvider = std::function<MetalRenderTelemetry()>;
    using ResetRenderTelemetryAction = std::function<void()>;
    using ActivityProvider = std::function<bool()>;
    using TimeProvider = std::function<double()>;

    PerformanceMetricsPanel(SnapshotProvider snapshotProvider,
        ResetRenderTelemetryAction resetRenderTelemetry, ActivityProvider activityProvider = { },
        GraphSnapshotProvider graphSnapshotProvider = { }, TimeProvider timeProvider = { });
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

    /**
        Performs one activity-gated graph refresh, matching a display-vblank callback.

        This copies only renderer graph telemetry and never rebuilds the raw metrics model,
        report, metric rows, or accessibility hierarchy.
    */
    void refreshGraphNow(double displayTimestampSeconds);

    /** Resets renderer telemetry and immediately refreshes the retained diagnostics view. */
    void resetRenderTelemetryNow();

    [[nodiscard]] const PerformanceMetricsViewModel& getViewModel() const noexcept;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    class MetricsContent;

    void timerCallback() override;
    void handleVBlank(double displayTimestampSeconds);
    void resizeContent();
    void setCollectionState(bool isCollecting);
    [[nodiscard]] bool isGraphRefreshVisible() const noexcept;
    [[nodiscard]] double currentTimeSeconds() const;
    static double monotonicSeconds() noexcept;

    SnapshotProvider snapshotProvider_;
    ResetRenderTelemetryAction resetRenderTelemetry_;
    ActivityProvider activityProvider_;
    GraphSnapshotProvider graphSnapshotProvider_;
    TimeProvider timeProvider_;
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
    bool hasLiveTelemetry_ = false;
    PerformanceMetricsSummaryCadence summaryCadence_;
    juce::VBlankAttachment vblankAttachment_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PerformanceMetricsPanel)
};
} // namespace audio_insight
