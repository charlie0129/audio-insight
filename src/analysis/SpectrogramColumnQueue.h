// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/SpectrogramColumn.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/** Fixed-capacity, single-logical-producer/single-logical-consumer column handoff. */
class SpectrogramColumnQueue final {
public:
    static constexpr std::size_t capacity = 32;

    struct PublishResult final {
        bool published = false;
        bool reclaimedOldestReady = false;
        bool droppedIncoming = false;
    };

    struct Telemetry final {
        std::uint64_t attemptedColumns = 0;
        std::uint64_t publishedColumns = 0;
        std::uint64_t reclaimedReadyColumns = 0;
        std::uint64_t droppedIncomingColumns = 0;
        std::uint64_t consumedColumns = 0;
        std::uint64_t discardedReadyColumns = 0;
        std::uint32_t readyHighWaterMark = 0;
        std::uint32_t readyColumns = 0;
    };

    SpectrogramColumnQueue() noexcept = default;

    [[nodiscard]] PublishResult publish(const SpectrogramColumn& column) noexcept;
    [[nodiscard]] bool copyNext(SpectrogramColumn& destination) const noexcept;

    /** Retires every currently ready column without touching a writing/reading slot. */
    void discardPending() noexcept;

    /** Retires ready columns except one active tagged capture-boundary marker. */
    void discardPendingExceptCaptureBoundary(std::uint64_t captureGeneration) noexcept;

    [[nodiscard]] Telemetry telemetry() const noexcept;

private:
    enum class SlotState : std::uint32_t { free, writing, ready, reading };

    static_assert(std::atomic<SlotState>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    struct Slot final {
        std::atomic<SlotState> state { SlotState::free };
        std::atomic<std::uint64_t> sequence { 0 };
        SpectrogramColumn column;
    };

    [[nodiscard]] Slot* claimSlot(bool& reclaimedReady) noexcept;
    void updateReadyHighWaterMark() noexcept;

    mutable std::array<Slot, capacity> slots_ { };
    std::uint32_t producerReadyHighWaterMark_ = 0;
    std::atomic<std::uint64_t> attemptedColumns_ { 0 };
    std::atomic<std::uint64_t> publishedColumns_ { 0 };
    std::atomic<std::uint64_t> reclaimedReadyColumns_ { 0 };
    std::atomic<std::uint64_t> droppedIncomingColumns_ { 0 };
    mutable std::atomic<std::uint64_t> consumedColumns_ { 0 };
    std::atomic<std::uint64_t> discardedReadyColumns_ { 0 };
    std::atomic<std::uint32_t> readyHighWaterMark_ { 0 };
};
} // namespace audio_insight
