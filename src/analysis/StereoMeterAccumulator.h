// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/VisualizationFrame.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight
{
struct StereoMeterReading
{
    std::array<float, 2> peakLinear{};
    std::array<float, 2> rmsLinear{};
    std::array<float, 2> peakDecibels{minimumDisplayDecibels, minimumDisplayDecibels};
    std::array<float, 2> rmsDecibels{minimumDisplayDecibels, minimumDisplayDecibels};

    std::uint64_t generation = 0;
    std::uint64_t firstSequence = 0;
    std::uint64_t lastSequence = 0;
    std::uint64_t capturedFrameEnd = 0;
    std::uint64_t representedBlocks = 0;
    std::uint64_t representedFrames = 0;
    double sampleRate = 0.0;
    bool followsDiscontinuity = false;
};

/**
    A dedicated bounded stereo sample-peak/RMS handoff.

    The audio producer calculates block statistics once, then publishes them to
    fixed slots. If all slots are ready, it coalesces into the newest ready slot:
    peaks use max and RMS energy/counts are accumulated. This is deliberately
    separate from raw sample capture, so reclaiming FFT input cannot erase a
    large recent meter peak.
*/
class StereoMeterAccumulator final
{
public:
    static constexpr std::size_t slotCount = 8;

    struct PublishResult
    {
        std::uint64_t sequence = 0;
        bool published = false;
        bool coalesced = false;
        bool dropped = false;
    };

    struct Telemetry
    {
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

    /** Audio-thread entry point. Null channels are treated as silence. */
    [[nodiscard]] PublishResult publishBlock(const float* left, const float* right,
                                             std::size_t frameCount, double sampleRate,
                                             std::uint64_t generation) noexcept;

    /**
        Drains and coalesces all complete publications available at the time of
        the bounded scan. Returns false without changing destination if empty.
    */
    [[nodiscard]] bool consumeLatest(StereoMeterReading& destination) noexcept;

    /** Convenience helper that updates only the meter fields of a public frame. */
    [[nodiscard]] bool consumeInto(VisualizationFrame& destination) noexcept;

    [[nodiscard]] Telemetry telemetry() const noexcept;

    /** Requires a quiescent producer and consumer. Sequence remains monotonic. */
    void discardPending() noexcept;

private:
    enum class SlotState : std::uint32_t
    {
        free,
        writing,
        ready,
        reading
    };

    static_assert(std::atomic<SlotState>::is_always_lock_free);
    static_assert(std::atomic<SlotState>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    struct Aggregate
    {
        std::array<float, 2> peak{};
        std::array<double, 2> sumSquares{};
        std::uint64_t frameCount = 0;
        std::uint64_t representedBlocks = 0;
        std::uint64_t generation = 0;
        std::uint64_t firstSequence = 0;
        std::uint64_t lastSequence = 0;
        std::uint64_t capturedFrameEnd = 0;
        double sampleRate = 0.0;
        bool containsSequenceGap = false;
    };

    struct Slot
    {
        std::atomic<SlotState> state{SlotState::free};
        Aggregate aggregate{};
    };

    [[nodiscard]] static Aggregate measure(const float* left, const float* right,
                                           std::size_t frameCount, double sampleRate,
                                           std::uint64_t generation, std::uint64_t sequence,
                                           std::uint64_t capturedFrameEnd) noexcept;
    static void merge(Aggregate& destination, const Aggregate& source) noexcept;
    [[nodiscard]] static float linearToDecibels(float value) noexcept;
    [[nodiscard]] bool publishToFreeSlot(const Aggregate& aggregate) noexcept;
    [[nodiscard]] bool coalesceIntoNewestReady(const Aggregate& aggregate) noexcept;
    void updateReadyHighWaterMark() noexcept;

    std::array<Slot, slotCount> slots_{};

    // Producer-owned counters.
    std::uint64_t nextSequence_ = 1;
    std::uint64_t capturedFrameCursor_ = 0;
    std::uint32_t producerReadyHighWaterMark_ = 0;

    // Consumer-owned continuity state.
    bool consumerHasPreviousSequence_ = false;
    std::uint64_t consumerPreviousGeneration_ = 0;
    std::uint64_t consumerPreviousSequence_ = 0;

    std::atomic<std::uint64_t> attemptedBlocks_{0};
    std::atomic<std::uint64_t> publishedBlocks_{0};
    std::atomic<std::uint64_t> coalescedBlocks_{0};
    std::atomic<std::uint64_t> droppedBlocks_{0};
    std::atomic<std::uint64_t> consumerDiscontinuities_{0};
    std::atomic<std::uint32_t> readyHighWaterMark_{0};
};
} // namespace audio_insight
