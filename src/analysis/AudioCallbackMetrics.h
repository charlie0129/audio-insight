// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
inline constexpr std::array<std::uint32_t, 5> trackedAudioCallbackBlockSizes {
    64,
    128,
    256,
    512,
    1024,
};

inline constexpr std::uint64_t audioCallbackDurationHistogramBucketNanoseconds = 1'000;
inline constexpr std::size_t audioCallbackDurationHistogramRegularBuckets = 1'024;
inline constexpr std::size_t audioCallbackDurationHistogramBucketCount
    = audioCallbackDurationHistogramRegularBuckets + 1;
inline constexpr std::size_t audioCallbackDurationHistogramOverflowBucket
    = audioCallbackDurationHistogramBucketCount - 1;

// Regular bucket n covers [n, n + 1) microseconds. The final bucket is an
// explicit overflow count for durations >= 1,024 microseconds.

enum AudioCallbackDetectorCoverage : std::uint32_t {
    concurrentCallbackDetection = 1U << 0U,
    monotonicClockAnomalyDetection = 1U << 1U,
};

/** Cumulative callback-duration telemetry for one exact host block size. */
struct AudioCallbackBlockTelemetry final {
    std::uint64_t callbackCount = 0;
    std::uint64_t timingSamples = 0;
    std::uint64_t budgetExceeded = 0;
    std::uint64_t budgetNanoseconds = 0;
    std::array<std::uint64_t, audioCallbackDurationHistogramBucketCount> durationHistogram { };
};

/**
    Cumulative, per-instance telemetry recorded around AudioProcessor::processBlock().

    The bounded detector covers concurrent entry into one processor instance and
    invalid/regressing monotonic-clock samples. Allocation and lock/wait detection
    are explicitly reported as unavailable instead of claiming coverage that the
    plugin cannot safely install inside an arbitrary host process.
*/
struct AudioCallbackTelemetry final {
    std::uint64_t callbackCount = 0;
    std::uint64_t processedFrames = 0;
    std::uint64_t timingSamples = 0;
    std::uint64_t timingUnavailable = 0;
    std::uint64_t budgetExceeded = 0;
    std::uint64_t untrackedBlockSizeCallbacks = 0;
    std::uint64_t concurrentCallbackViolations = 0;
    std::uint64_t clockAnomalyViolations = 0;
    std::uint64_t rtSafetyViolationCount = 0;
    std::uint32_t detectorCoverageFlags
        = concurrentCallbackDetection | monotonicClockAnomalyDetection;
    bool detectorActive = true;
    bool clockAvailable = false;
    bool allocationDetectorActive = false;
    bool lockWaitDetectorActive = false;
    std::array<AudioCallbackBlockTelemetry, trackedAudioCallbackBlockSizes.size()>
        trackedBlocks { };
};

/** Immutable conversion parameters initialized away from the audio callback. */
struct MachContinuousTimebase final {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
    bool valid = false;

    [[nodiscard]] static MachContinuousTimebase system() noexcept;
    [[nodiscard]] bool durationNanoseconds(std::uint64_t startTicks, std::uint64_t endTicks,
        std::uint64_t& destination) const noexcept;
};

/** The macOS continuous monotonic clock read used at processBlock boundaries. */
[[nodiscard]] std::uint64_t readMachContinuousTime() noexcept;

/**
    Fixed-storage collector whose callback entry points use only always-lock-free
    atomics and bounded arithmetic. The caller supplies clock ticks, which keeps
    timing and anomaly behavior deterministic in unit tests.
*/
class AudioCallbackMetrics final {
public:
    struct CallbackToken final {
        std::uint64_t startTicks = 0;
        std::uint32_t frameCount = 0;
    };

    explicit AudioCallbackMetrics(MachContinuousTimebase timebase) noexcept;

    AudioCallbackMetrics(const AudioCallbackMetrics&) = delete;
    AudioCallbackMetrics& operator=(const AudioCallbackMetrics&) = delete;

    /** Non-real-time format boundary used to precompute the accepted callback budgets. */
    void configureSampleRate(double sampleRate) noexcept;

    [[nodiscard]] CallbackToken beginCallback(
        std::uint32_t frameCount, std::uint64_t startTicks) noexcept;
    void finishCallback(CallbackToken token, std::uint64_t endTicks) noexcept;

    /** Non-real-time snapshot for Metrics. */
    [[nodiscard]] AudioCallbackTelemetry telemetry() const noexcept;

private:
    struct BlockCounters final {
        std::atomic<std::uint64_t> callbackCount { 0 };
        std::atomic<std::uint64_t> timingSamples { 0 };
        std::atomic<std::uint64_t> budgetExceeded { 0 };
        std::atomic<std::uint64_t> budgetNanoseconds { 0 };
        std::array<std::atomic<std::uint64_t>, audioCallbackDurationHistogramBucketCount>
            durationHistogram { };
    };

    [[nodiscard]] static std::size_t blockIndex(std::uint32_t frameCount) noexcept;
    [[nodiscard]] static std::size_t durationBucket(std::uint64_t nanoseconds) noexcept;

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    const MachContinuousTimebase timebase_;
    std::array<BlockCounters, trackedAudioCallbackBlockSizes.size()> blocks_ { };
    std::atomic<std::uint64_t> callbackCount_ { 0 };
    std::atomic<std::uint64_t> processedFrames_ { 0 };
    std::atomic<std::uint64_t> timingSamples_ { 0 };
    std::atomic<std::uint64_t> timingUnavailable_ { 0 };
    std::atomic<std::uint64_t> budgetExceeded_ { 0 };
    std::atomic<std::uint64_t> untrackedBlockSizeCallbacks_ { 0 };
    std::atomic<std::uint64_t> concurrentCallbackViolations_ { 0 };
    std::atomic<std::uint64_t> clockAnomalyViolations_ { 0 };
    std::atomic<std::uint32_t> activeCallbacks_ { 0 };
};
} // namespace audio_insight
