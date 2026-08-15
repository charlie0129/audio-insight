// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SpectrogramColumnQueue.h"

#include <limits>

namespace audio_insight {
SpectrogramColumnQueue::PublishResult SpectrogramColumnQueue::publish(
    const SpectrogramColumn& column) noexcept
{
    PublishResult result;
    attemptedColumns_.fetch_add(1, std::memory_order_relaxed);

    bool reclaimedReady = false;
    auto* const slot = claimSlot(reclaimedReady);
    if (slot == nullptr) {
        result.droppedIncoming = true;
        droppedIncomingColumns_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    slot->column = column;
    slot->sequence.store(column.sequence, std::memory_order_relaxed);
    slot->state.store(SlotState::ready, std::memory_order_release);
    publishedColumns_.fetch_add(1, std::memory_order_relaxed);
    if (reclaimedReady)
        reclaimedReadyColumns_.fetch_add(1, std::memory_order_relaxed);

    updateReadyHighWaterMark();
    result.published = true;
    result.reclaimedOldestReady = reclaimedReady;
    return result;
}

bool SpectrogramColumnQueue::copyNext(SpectrogramColumn& destination) const noexcept
{
    for (std::size_t attempt = 0; attempt < slots_.size(); ++attempt) {
        auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
        auto oldestIndex = slots_.size();

        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
                continue;
            const auto sequence = slots_[index].sequence.load(std::memory_order_relaxed);
            if (sequence < oldestSequence) {
                oldestSequence = sequence;
                oldestIndex = index;
            }
        }

        if (oldestIndex == slots_.size())
            return false;

        auto expected = SlotState::ready;
        auto& slot = slots_[oldestIndex];
        if (!slot.state.compare_exchange_strong(expected, SlotState::reading,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            continue;
        }

        // The slot can make a complete ready -> writing -> ready cycle between
        // the scan and this claim. Validate the sequence after claiming so
        // that ABA cannot make an older retained column follow a newer one.
        if (slot.sequence.load(std::memory_order_relaxed) != oldestSequence) {
            slot.state.store(SlotState::ready, std::memory_order_release);
            continue;
        }

        destination = slot.column;
        slot.state.store(SlotState::free, std::memory_order_release);
        consumedColumns_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    return false;
}

void SpectrogramColumnQueue::discardPending() noexcept
{
    for (auto& slot : slots_) {
        auto expected = SlotState::ready;
        if (slot.state.compare_exchange_strong(
                expected, SlotState::free, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            discardedReadyColumns_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void SpectrogramColumnQueue::discardPendingExceptCaptureBoundary(
    const std::uint64_t captureGeneration) noexcept
{
    for (auto& slot : slots_) {
        auto expected = SlotState::ready;
        if (!slot.state.compare_exchange_strong(expected, SlotState::reading,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            continue;
        }

        const auto preserve = captureGeneration != 0 && slot.column.captureBoundary
            && slot.column.resetMarker && slot.column.captureGeneration == captureGeneration;
        if (preserve) {
            slot.state.store(SlotState::ready, std::memory_order_release);
        } else {
            slot.state.store(SlotState::free, std::memory_order_release);
            discardedReadyColumns_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

SpectrogramColumnQueue::Telemetry SpectrogramColumnQueue::telemetry() const noexcept
{
    Telemetry result;
    result.attemptedColumns = attemptedColumns_.load(std::memory_order_relaxed);
    result.publishedColumns = publishedColumns_.load(std::memory_order_relaxed);
    result.reclaimedReadyColumns = reclaimedReadyColumns_.load(std::memory_order_relaxed);
    result.droppedIncomingColumns = droppedIncomingColumns_.load(std::memory_order_relaxed);
    result.consumedColumns = consumedColumns_.load(std::memory_order_relaxed);
    result.discardedReadyColumns = discardedReadyColumns_.load(std::memory_order_relaxed);
    result.readyHighWaterMark = readyHighWaterMark_.load(std::memory_order_relaxed);

    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == SlotState::ready)
            ++result.readyColumns;

    return result;
}

SpectrogramColumnQueue::Slot* SpectrogramColumnQueue::claimSlot(bool& reclaimedReady) noexcept
{
    reclaimedReady = false;
    for (auto& slot : slots_) {
        auto expected = SlotState::free;
        if (slot.state.compare_exchange_strong(expected, SlotState::writing,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return &slot;
        }
    }

    for (std::size_t attempt = 0; attempt < slots_.size(); ++attempt) {
        auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
        auto oldestIndex = slots_.size();

        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
                continue;
            if (slots_[index].column.captureBoundary)
                continue;

            const auto sequence = slots_[index].sequence.load(std::memory_order_relaxed);
            if (sequence < oldestSequence) {
                oldestSequence = sequence;
                oldestIndex = index;
            }
        }

        if (oldestIndex == slots_.size())
            return nullptr;

        auto expected = SlotState::ready;
        if (slots_[oldestIndex].state.compare_exchange_strong(expected, SlotState::writing,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            if (slots_[oldestIndex].sequence.load(std::memory_order_relaxed) != oldestSequence) {
                slots_[oldestIndex].state.store(SlotState::ready, std::memory_order_release);
                continue;
            }

            reclaimedReady = true;
            return &slots_[oldestIndex];
        }
    }

    return nullptr;
}

void SpectrogramColumnQueue::updateReadyHighWaterMark() noexcept
{
    std::uint32_t ready = 0;
    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == SlotState::ready)
            ++ready;

    if (ready > producerReadyHighWaterMark_) {
        producerReadyHighWaterMark_ = ready;
        readyHighWaterMark_.store(ready, std::memory_order_relaxed);
    }
}
} // namespace audio_insight
