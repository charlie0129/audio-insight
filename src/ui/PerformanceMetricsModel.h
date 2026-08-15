// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "MetalVisualization.h"
#include "analysis/AnalysisCoordinator.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace audio_insight {
/** One point-in-time UI-side read of this instance's renderer and analysis telemetry. */
struct PerformanceMetricsSnapshot {
    MetalRenderTelemetry metal;
    AnalysisTelemetry analysis;
};

enum class PerformanceMetricKind { raw, derivedRate, derivedStatistic };

/** A presentation-ready value with a stable machine-readable field name. */
struct PerformanceMetricRow {
    std::string fieldName;
    std::string label;
    std::string value;
    std::string unit;

    // Scalar raw rows retain the source value and unit so copied reports do not
    // lose precision when display values are converted (for example ns to ms).
    // Expensive aggregate serialization may be deferred until Copy is requested.
    std::string rawValue;
    std::string rawUnit;
    PerformanceMetricKind kind = PerformanceMetricKind::raw;
};

struct PerformanceMetricGroup {
    std::string name;
    std::vector<PerformanceMetricRow> rows;
};

/** A counter delta divided by monotonic elapsed time. */
struct PerformanceMetricRate {
    std::string sourceFieldName;
    std::string label;
    std::string unit;
    double value = 0.0;
    std::uint64_t counterDelta = 0;
    double sampleIntervalSeconds = 0.0;
    bool available = false;
};

/** Statistics over either a bounded UI sample history or the renderer's exact history. */
struct FrameIntervalStatistics {
    std::size_t sampleCount = 0;
    double latestMilliseconds = 0.0;
    double minimumMilliseconds = 0.0;
    double meanMilliseconds = 0.0;
    double percentile95Milliseconds = 0.0;
    double maximumMilliseconds = 0.0;
    double standardDeviationMilliseconds = 0.0;
    double equivalentHertz = 0.0;
    bool available = false;
};

struct FrameIntervalStatisticsSet {
    FrameIntervalStatistics displayCallbacks;
    FrameIntervalStatistics targetCallbacks;
    FrameIntervalStatistics targetPresentations;
    FrameIntervalStatistics presentedFrames;
};

struct PerformanceMetricsDerived {
    std::vector<PerformanceMetricRate> rates;
    FrameIntervalStatisticsSet frameIntervals;
    bool ratesRebased = false;
};

/** Complete immutable output for a metrics overlay repaint or text copy. */
struct PerformanceMetricsViewModel {
    std::vector<PerformanceMetricGroup> sections;
    PerformanceMetricsDerived derived;
    std::string report;
};

/**
    Pure UI-side state machine for Audio Insight's per-instance metrics view.

    update() must be called off the audio thread. monotonicSeconds is supplied by
    the UI-side sampler and must use one monotonic timebase. Counter rates are
    emitted only after a positive monotonic interval in the same Metal telemetry
    epoch. A counter rollback invalidates only that counter's current rate; it is
    never interpreted as unsigned wraparound.
*/
class PerformanceMetricsModel final {
public:
    explicit PerformanceMetricsModel(std::size_t intervalHistoryCapacity = 300);

    /**
        Builds display rows and derived values. Set includeCopyReport to false
        for frequent UI samples, then call buildCopyReport() with the same
        snapshot on demand.
    */
    [[nodiscard]] PerformanceMetricsViewModel update(const PerformanceMetricsSnapshot& snapshot,
        double monotonicSeconds, bool includeCopyReport = true);
    [[nodiscard]] static std::string buildCopyReport(
        const PerformanceMetricsViewModel& view, const PerformanceMetricsSnapshot& snapshot);
    void reset() noexcept;

private:
    struct IntervalHistory {
        std::deque<std::uint64_t> samples;
    };

    std::size_t historyCapacity_ = 1;
    std::optional<PerformanceMetricsSnapshot> previousSnapshot_;
    std::optional<double> previousSampleTime_;
    IntervalHistory displayCallbackIntervals_;
    IntervalHistory targetCallbackIntervals_;
    IntervalHistory targetPresentationIntervals_;
};
} // namespace audio_insight
