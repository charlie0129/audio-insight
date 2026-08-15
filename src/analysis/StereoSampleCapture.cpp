// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StereoSampleCapture.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

namespace audio_insight
{
StereoSampleCapture::ReadHandle::ReadHandle(StereoSampleCapture& owner, const std::size_t slotIndex,
                                            CapturedStereoChunkView view) noexcept
    : owner_(&owner), slotIndex_(slotIndex), view_(view)
{
}

StereoSampleCapture::ReadHandle::ReadHandle(ReadHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), slotIndex_(other.slotIndex_), view_(other.view_)
{
}

StereoSampleCapture::ReadHandle&
StereoSampleCapture::ReadHandle::operator=(ReadHandle&& other) noexcept
{
    if (this != &other)
    {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        slotIndex_ = other.slotIndex_;
        view_ = other.view_;
    }

    return *this;
}

StereoSampleCapture::ReadHandle::~ReadHandle()
{
    release();
}

void StereoSampleCapture::ReadHandle::release() noexcept
{
    if (owner_ == nullptr)
        return;

    owner_->releaseReadSlot(slotIndex_);
    owner_ = nullptr;
    view_ = {};
}

StereoSampleCapture::StereoSampleCapture() noexcept = default;

StereoSampleCapture::PublishResult
StereoSampleCapture::publishBlock(const float* const left, const float* const right,
                                  const std::size_t frameCount, const double sampleRate,
                                  const std::uint64_t generation) noexcept
{
    PublishResult result;
    std::size_t offset = 0;

    while (offset < frameCount)
    {
        const auto chunkFrames = std::min(framesPerSlot, frameCount - offset);
        publishChunk(left != nullptr ? left + offset : nullptr,
                     right != nullptr ? right + offset : nullptr, chunkFrames, sampleRate,
                     generation, result);
        offset += chunkFrames;
    }

    return result;
}

StereoSampleCapture::Slot* StereoSampleCapture::claimSlot(bool& reclaimedReady,
                                                          std::size_t& slotIndex) noexcept
{
    reclaimedReady = false;

    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        auto expected = SlotState::free;
        if (slots_[index].state.compare_exchange_strong(
                expected, SlotState::writing, std::memory_order_acquire, std::memory_order_relaxed))
        {
            slotIndex = index;
            return &slots_[index];
        }
    }

    // The consumer can win one of these claims while this bounded scan runs. A
    // failed claim is excluded on the next iteration because it is then reading.
    for (std::size_t attempt = 0; attempt < slots_.size(); ++attempt)
    {
        std::size_t oldestIndex = slots_.size();
        auto oldestSequence = std::numeric_limits<std::uint64_t>::max();

        for (std::size_t index = 0; index < slots_.size(); ++index)
        {
            if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
                continue;

            const auto sequence = slots_[index].publishedSequence.load(std::memory_order_relaxed);
            if (sequence < oldestSequence)
            {
                oldestSequence = sequence;
                oldestIndex = index;
            }
        }

        if (oldestIndex == slots_.size())
            return nullptr;

        auto expected = SlotState::ready;
        if (slots_[oldestIndex].state.compare_exchange_strong(
                expected, SlotState::writing, std::memory_order_acquire, std::memory_order_relaxed))
        {
            reclaimedReady = true;
            slotIndex = oldestIndex;
            return &slots_[oldestIndex];
        }
    }

    return nullptr;
}

void StereoSampleCapture::publishChunk(const float* const left, const float* const right,
                                       const std::size_t frameCount, const double sampleRate,
                                       const std::uint64_t generation,
                                       PublishResult& result) noexcept
{
    const auto sequence = nextSequence_++;
    capturedFrameCursor_ += frameCount;
    ++result.attemptedChunks;

    attemptedChunks_.fetch_add(1, std::memory_order_relaxed);
    lastAttemptedSequence_.store(sequence, std::memory_order_relaxed);
    capturedFrames_.store(capturedFrameCursor_, std::memory_order_relaxed);

    bool reclaimedReady = false;
    std::size_t slotIndex = 0;
    auto* const slot = claimSlot(reclaimedReady, slotIndex);

    if (slot == nullptr)
    {
        ++result.droppedIncomingChunks;
        droppedIncomingChunks_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (reclaimedReady)
    {
        ++result.reclaimedReadyChunks;
        reclaimedReadyChunks_.fetch_add(1, std::memory_order_relaxed);
    }

    if (left != nullptr)
        std::memcpy(slot->left.data(), left, frameCount * sizeof(float));
    else
        std::fill_n(slot->left.data(), frameCount, 0.0F);

    if (right != nullptr)
        std::memcpy(slot->right.data(), right, frameCount * sizeof(float));
    else
        std::fill_n(slot->right.data(), frameCount, 0.0F);

    slot->frameCount = frameCount;
    slot->generation = generation;
    slot->sequence = sequence;
    slot->capturedFrameEnd = capturedFrameCursor_;
    slot->sampleRate = sampleRate;
    slot->publishedSequence.store(sequence, std::memory_order_relaxed);
    slot->state.store(SlotState::ready, std::memory_order_release);

    ++result.publishedChunks;
    publishedChunks_.fetch_add(1, std::memory_order_relaxed);
    updateReadyHighWaterMark();
}

bool StereoSampleCapture::tryAcquireOldest(ReadHandle& destination) noexcept
{
    destination.release();

    for (std::size_t attempt = 0; attempt < slots_.size(); ++attempt)
    {
        std::size_t oldestIndex = slots_.size();
        auto oldestSequence = std::numeric_limits<std::uint64_t>::max();

        for (std::size_t index = 0; index < slots_.size(); ++index)
        {
            if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
                continue;

            const auto sequence = slots_[index].publishedSequence.load(std::memory_order_relaxed);
            if (sequence < oldestSequence)
            {
                oldestSequence = sequence;
                oldestIndex = index;
            }
        }

        if (oldestIndex == slots_.size())
            return false;

        auto expected = SlotState::ready;
        auto& slot = slots_[oldestIndex];
        if (!slot.state.compare_exchange_strong(
                expected, SlotState::reading, std::memory_order_acquire, std::memory_order_relaxed))
        {
            continue;
        }

        bool followsDiscontinuity = false;
        if (consumerHasPreviousSequence_ && slot.generation == consumerPreviousGeneration_)
            followsDiscontinuity = slot.sequence != consumerPreviousSequence_ + 1;

        if (followsDiscontinuity)
            consumerDiscontinuities_.fetch_add(1, std::memory_order_relaxed);

        consumerHasPreviousSequence_ = true;
        consumerPreviousGeneration_ = slot.generation;
        consumerPreviousSequence_ = slot.sequence;

        destination = ReadHandle(*this, oldestIndex,
                                 {slot.left.data(), slot.right.data(), slot.frameCount,
                                  slot.generation, slot.sequence, slot.capturedFrameEnd,
                                  slot.sampleRate, followsDiscontinuity});
        return true;
    }

    return false;
}

StereoSampleCapture::Telemetry StereoSampleCapture::telemetry() const noexcept
{
    Telemetry result;
    result.attemptedChunks = attemptedChunks_.load(std::memory_order_relaxed);
    result.publishedChunks = publishedChunks_.load(std::memory_order_relaxed);
    result.reclaimedReadyChunks = reclaimedReadyChunks_.load(std::memory_order_relaxed);
    result.droppedIncomingChunks = droppedIncomingChunks_.load(std::memory_order_relaxed);
    result.consumerDiscontinuities = consumerDiscontinuities_.load(std::memory_order_relaxed);
    result.lastAttemptedSequence = lastAttemptedSequence_.load(std::memory_order_relaxed);
    result.capturedFrames = capturedFrames_.load(std::memory_order_relaxed);
    result.readyHighWaterMark = readyHighWaterMark_.load(std::memory_order_relaxed);

    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == SlotState::ready)
            ++result.readySlots;

    return result;
}

void StereoSampleCapture::discardPending() noexcept
{
    for (auto& slot : slots_)
    {
        const auto state = slot.state.load(std::memory_order_acquire);
        assert(state != SlotState::writing && state != SlotState::reading);

        if (state == SlotState::ready)
            slot.state.store(SlotState::free, std::memory_order_release);
    }

    consumerHasPreviousSequence_ = false;
    consumerPreviousGeneration_ = 0;
    consumerPreviousSequence_ = 0;
}

void StereoSampleCapture::releaseReadSlot(const std::size_t slotIndex) noexcept
{
    assert(slotIndex < slots_.size());
    assert(slots_[slotIndex].state.load(std::memory_order_relaxed) == SlotState::reading);
    slots_[slotIndex].state.store(SlotState::free, std::memory_order_release);
}

void StereoSampleCapture::updateReadyHighWaterMark() noexcept
{
    std::uint32_t readyCount = 0;
    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == SlotState::ready)
            ++readyCount;

    if (readyCount > producerReadyHighWaterMark_)
    {
        producerReadyHighWaterMark_ = readyCount;
        readyHighWaterMark_.store(readyCount, std::memory_order_relaxed);
    }
}
} // namespace audio_insight
