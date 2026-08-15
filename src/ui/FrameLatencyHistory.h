// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
inline constexpr std::size_t frameLatencyHistoryCapacity = 240;

/** One display-link submission's callback-to-presentation timing decomposition. */
struct FrameLatencySample {
    std::uint64_t sequence = 0;
    std::uint64_t presentedHostTimestampNanoseconds = 0;
    std::uint64_t cpuEncodeNanoseconds = 0;
    std::uint64_t submitQueueWaitNanoseconds = 0;
    std::uint64_t gpuExecutionNanoseconds = 0;
    std::uint64_t compositorWaitNanoseconds = 0;
    std::uint64_t totalNanoseconds = 0;
    bool totalValid = false;
    bool componentsValid = false;
};

/**
    Classifies timestamp ordering and derives a latency sample without relying
    on Metal or callback ordering.
*/
[[nodiscard]] FrameLatencySample classifyFrameLatency(std::uint64_t sequence,
    std::uint64_t callbackNanoseconds, std::uint64_t cpuReadyNanoseconds,
    std::uint64_t gpuStartNanoseconds, std::uint64_t gpuEndNanoseconds,
    std::uint64_t presentedNanoseconds) noexcept;

enum class PresentationOutcomeTransition : std::uint8_t {
    none,
    newlySkipped,
    newlyPresented,
    upgradedSkippedToPresented,
};

/**
    Lifetime-safe, per-submission correlation state shared by Metal's command
    completion and drawable presentation handlers.

    Each callback has a single writer for its timestamp fields. Atomic event
    publication makes whichever callback arrives second derive the sample
    exactly once, without retaining the renderer itself.
*/
class FrameLatencySubmission final {
public:
    FrameLatencySubmission(std::uint64_t sequence, std::uint64_t callbackNanoseconds) noexcept;

    void setCpuReadyTimestamp(std::uint64_t cpuReadyNanoseconds) noexcept;

    [[nodiscard]] bool recordGpuCompletion(std::uint64_t gpuStartNanoseconds,
        std::uint64_t gpuEndNanoseconds, FrameLatencySample& sample) noexcept;

    [[nodiscard]] bool recordPresentation(
        std::uint64_t presentedNanoseconds, FrameLatencySample& sample) noexcept;

    /** Marks a skipped outcome only while no actual presentation is known. */
    [[nodiscard]] PresentationOutcomeTransition classifySkipped() noexcept;

    /**
        Marks an actual presentation. A presentation arriving after an earlier
        failure classification upgrades that provisional skipped outcome.
    */
    [[nodiscard]] PresentationOutcomeTransition classifyPresented() noexcept;

private:
    [[nodiscard]] bool publishTimingEvent(std::uint8_t event, FrameLatencySample& sample) noexcept;

    const std::uint64_t sequence_;
    const std::uint64_t callbackNanoseconds_;
    std::atomic<std::uint64_t> cpuReadyNanoseconds_ { 0 };
    std::atomic<std::uint64_t> gpuStartNanoseconds_ { 0 };
    std::atomic<std::uint64_t> gpuEndNanoseconds_ { 0 };
    std::atomic<std::uint64_t> presentedNanoseconds_ { 0 };
    std::atomic<std::uint8_t> timingState_ { 0 };
    std::atomic<std::uint8_t> presentationOutcome_ { 0 };
};

/**
    Fixed-capacity history ordered by actual drawable presentation timestamp.
    The display-link callback sequence remains attached as the correlation key.

    This type does not synchronize its own access. The Metal backend protects
    it with its non-audio presentation telemetry lock because completion and
    presentation callbacks may finish concurrently.
*/
class FrameLatencyHistory final {
public:
    [[nodiscard]] bool record(FrameLatencySample sample) noexcept;

    [[nodiscard]] std::size_t snapshot(
        std::array<FrameLatencySample, frameLatencyHistoryCapacity>& destination) const noexcept;

private:
    std::array<FrameLatencySample, frameLatencyHistoryCapacity> samples_ { };
    std::size_t count_ = 0;
};
} // namespace audio_insight
