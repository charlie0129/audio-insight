// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PeakRmsBallistics.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/** One complete producer-time Peak/RMS endpoint plus capture metadata. */
struct StereoMeterReading {
    std::array<float, 2> peakLinear { };
    std::array<float, 2> rmsLinear { };
    std::array<float, 2> heldPeakLinear { };
    std::array<float, 2> peakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> rmsDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<float, 2> heldPeakDecibels { minimumDisplayDecibels, minimumDisplayDecibels };
    std::array<bool, 2> over { false, false };

    std::uint64_t generation = 0;
    std::uint64_t firstSequence = 0;
    std::uint64_t lastSequence = 0;
    std::uint64_t capturedFrameEnd = 0;
    std::uint64_t representedBlocks = 0;
    std::uint64_t representedFrames = 0;
    std::uint64_t appliedUserResetEpoch = 0;
    std::uint64_t appliedLiveClearEpoch = 0;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    bool followsDiscontinuity = false;
    bool valid = false;
};

/**
    A dedicated bounded sample-peak/RMS handoff with producer-owned ballistics.

    The audio producer advances PeakRmsBallistics from every raw sample and each
    slot publishes a complete endpoint snapshot. A consumer can therefore discard
    or coalesce older snapshots without changing temporal math or losing a peak,
    hold, or OVER event already represented by the newer endpoint.
*/
class StereoMeterAccumulator final {
public:
    static constexpr std::size_t slotCount = 8;

    struct PublishResult {
        std::uint64_t sequence = 0;
        bool published = false;
        bool coalesced = false;
        bool dropped = false;
    };

    struct Telemetry {
        std::uint64_t attemptedBlocks = 0;
        std::uint64_t publishedBlocks = 0;
        std::uint64_t coalescedBlocks = 0;
        std::uint64_t droppedBlocks = 0;
        std::uint64_t consumerDiscontinuities = 0;
        std::uint32_t readyHighWaterMark = 0;
        std::uint32_t readySlots = 0;
    };

    StereoMeterAccumulator() noexcept;

    StereoMeterAccumulator(const StereoMeterAccumulator&) = delete;
    StereoMeterAccumulator& operator=(const StereoMeterAccumulator&) = delete;

    /**
        Audio-thread entry point. Null channels are silence. Unsupported metadata
        publishes an invalid endpoint. The discontinuity flag resets all temporal
        meter state before this block is processed.
    */
    [[nodiscard]] PublishResult publishBlock(const float* left, const float* right,
        std::size_t frameCount, double sampleRate, std::uint64_t generation,
        std::uint32_t channelCount = 2, bool followsDiscontinuity = false) noexcept;

    /**
        Drains a bounded set of complete publications and returns the newest
        endpoint with metadata for the newest contiguous represented segment.
    */
    [[nodiscard]] bool consumeLatest(StereoMeterReading& destination) noexcept;

    /** Convenience helper that updates the meter fields of a public frame. */
    [[nodiscard]] bool consumeInto(VisualizationFrame& destination) noexcept;

    /**
        Non-real-time requests. The producer applies every accumulated request at
        the next audio-block boundary and stamps the latest applied epoch.
    */
    [[nodiscard]] std::uint64_t requestUserReset() noexcept;
    [[nodiscard]] std::uint64_t requestLiveClear() noexcept;
    [[nodiscard]] std::uint64_t requestedUserResetEpoch() const noexcept;
    [[nodiscard]] std::uint64_t requestedLiveClearEpoch() const noexcept;

    [[nodiscard]] Telemetry telemetry() const noexcept;

    /** Requires a quiescent producer and consumer. Epochs and sequences remain monotonic. */
    void discardPending() noexcept;

private:
    enum class SlotState : std::uint32_t { free, writing, ready, reading };

    static_assert(std::atomic<SlotState>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    struct Slot {
        std::atomic<SlotState> state { SlotState::free };
        StereoMeterReading reading;
    };

    [[nodiscard]] StereoMeterReading measureEndpoint(const float* left, const float* right,
        std::size_t frameCount, double sampleRate, std::uint64_t generation, std::uint64_t sequence,
        std::uint64_t capturedFrameEnd, std::uint32_t channelCount,
        bool followsDiscontinuity) noexcept;
    static void prependRepresentedMetadata(
        StereoMeterReading& newer, const StereoMeterReading& older) noexcept;
    [[nodiscard]] static bool formatsMatch(
        const StereoMeterReading& left, const StereoMeterReading& right) noexcept;
    [[nodiscard]] bool publishToFreeSlot(const StereoMeterReading& reading) noexcept;
    [[nodiscard]] bool coalesceIntoNewestReady(const StereoMeterReading& reading) noexcept;
    void updateReadyHighWaterMark() noexcept;

    std::array<Slot, slotCount> slots_ { };
    PeakRmsBallistics ballistics_;

    // Producer-owned state.
    std::uint64_t nextSequence_ = 1;
    std::uint64_t capturedFrameCursor_ = 0;
    std::uint64_t appliedUserResetEpoch_ = 0;
    std::uint64_t appliedLiveClearEpoch_ = 0;
    std::uint32_t producerReadyHighWaterMark_ = 0;

    // Consumer-owned continuity state.
    bool consumerHasPreviousSequence_ = false;
    bool consumerPreviousValid_ = false;
    std::uint64_t consumerPreviousGeneration_ = 0;
    std::uint64_t consumerPreviousSequence_ = 0;
    std::uint32_t consumerPreviousChannelCount_ = 0;
    double consumerPreviousSampleRate_ = 0.0;

    std::atomic<std::uint64_t> requestedUserResetEpoch_ { 0 };
    std::atomic<std::uint64_t> requestedLiveClearEpoch_ { 0 };
    std::atomic<std::uint64_t> attemptedBlocks_ { 0 };
    std::atomic<std::uint64_t> publishedBlocks_ { 0 };
    std::atomic<std::uint64_t> coalescedBlocks_ { 0 };
    std::atomic<std::uint64_t> droppedBlocks_ { 0 };
    std::atomic<std::uint64_t> consumerDiscontinuities_ { 0 };
    std::atomic<std::uint32_t> readyHighWaterMark_ { 0 };
};
} // namespace audio_insight
