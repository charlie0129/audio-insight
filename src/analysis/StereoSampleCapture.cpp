// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StereoSampleCapture.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

namespace audio_insight {
StereoSampleCapture::ReadHandle::ReadHandle(
    StereoSampleCapture& owner, const std::size_t slotIndex, CapturedStereoChunkView view) noexcept
    : owner_(&owner), slotIndex_(slotIndex), view_(view)
{
}

StereoSampleCapture::ReadHandle::ReadHandle(ReadHandle&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), slotIndex_(other.slotIndex_), view_(other.view_)
{
}

StereoSampleCapture::ReadHandle& StereoSampleCapture::ReadHandle::operator=(
    ReadHandle&& other) noexcept
{
    if (this != &other) {
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
    view_ = { };
}

StereoSampleCapture::StereoSampleCapture() noexcept = default;

StereoSampleCapture::PublishResult StereoSampleCapture::publishBlock(const float* const left,
    const float* const right, const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation, const std::uint32_t channelCount,
    const std::uint64_t captureLifecycleGeneration) noexcept
{
    assert(frameCount <= maximumFramesPerPublishCall);
    if (frameCount > maximumFramesPerPublishCall)
        return { };

    const auto lifecycleGeneration
        = captureLifecycleGeneration == 0 ? generation : captureLifecycleGeneration;
    if (producerCaptureLifecycleGeneration_ != lifecycleGeneration) {
        abandonPackedChunk();
        retireReadyChunksForLifecycleChange();
        producerCaptureLifecycleGeneration_ = lifecycleGeneration;
        captureDiscontinuityRevision_ = 0;
        publishedCaptureDiscontinuityRevision_.store(0, std::memory_order_relaxed);
        publishedCaptureDiscontinuityLifecycleGeneration_.store(
            lifecycleGeneration, std::memory_order_release);
    }

    PublishResult result;
    result.precedingCaptureDiscontinuityRevision = captureDiscontinuityRevision_;
    result.captureDiscontinuityRevision = captureDiscontinuityRevision_;
    std::size_t offset = 0;

    while (offset < frameCount) {
        if (packedSlotIndex_ == slotCount
            && !beginPackedChunk(
                sampleRate, generation, channelCount, lifecycleGeneration, offset, result)) {
            const auto droppedFrames = std::min(framesPerSlot, frameCount - offset);
            capturedFrameCursor_ += droppedFrames;
            capturedFrames_.store(capturedFrameCursor_, std::memory_order_relaxed);
            offset += droppedFrames;
            continue;
        }

        assert(packedSlotIndex_ < slotCount);
        auto& slot = slots_[packedSlotIndex_];
        const auto copiedFrames = std::min(framesPerSlot - packedFrameCount_, frameCount - offset);
        if (left != nullptr) {
            std::memcpy(slot.left.data() + static_cast<std::ptrdiff_t>(packedFrameCount_),
                left + static_cast<std::ptrdiff_t>(offset), copiedFrames * sizeof(float));
        } else {
            std::fill_n(slot.left.data() + static_cast<std::ptrdiff_t>(packedFrameCount_),
                copiedFrames, 0.0F);
        }

        if (right != nullptr) {
            std::memcpy(slot.right.data() + static_cast<std::ptrdiff_t>(packedFrameCount_),
                right + static_cast<std::ptrdiff_t>(offset), copiedFrames * sizeof(float));
        } else {
            std::fill_n(slot.right.data() + static_cast<std::ptrdiff_t>(packedFrameCount_),
                copiedFrames, 0.0F);
        }

        packedFrameCount_ += copiedFrames;
        capturedFrameCursor_ += copiedFrames;
        capturedFrames_.store(capturedFrameCursor_, std::memory_order_relaxed);
        const auto previousBufferedState
            = bufferedState_.fetch_add(copiedFrames, std::memory_order_relaxed);
        assert((previousBufferedState & partialFrameMask) == packedFrameCount_ - copiedFrames);
        static_cast<void>(previousBufferedState);
        offset += copiedFrames;

        if (packedFrameCount_ == framesPerSlot)
            finalizePackedChunk(result);
    }

    return result;
}

StereoSampleCapture::Slot* StereoSampleCapture::claimSlot(
    bool& reclaimedReady, std::size_t& slotIndex) noexcept
{
    reclaimedReady = false;

    // In the healthy ring the next producer index is already free, keeping the
    // audio-thread claim O(1).
    auto expected = SlotState::free;
    if (slots_[nextClaimIndex_].state.compare_exchange_strong(
            expected, SlotState::writing, std::memory_order_acquire, std::memory_order_relaxed)) {
        slotIndex = nextClaimIndex_;
        nextClaimIndex_ = (slotIndex + 1) % slotCount;
        return &slots_[slotIndex];
    }

    // Under contention perform one bounded pass: prefer any free storage, then
    // reclaim only the oldest ready payload. A consumer winning the final CAS
    // makes this publication drop safely instead of adding an unbounded retry.
    std::size_t oldestReadyIndex = slotCount;
    auto oldestReadySequence = std::numeric_limits<std::uint64_t>::max();
    if (expected == SlotState::ready) {
        oldestReadyIndex = nextClaimIndex_;
        oldestReadySequence
            = slots_[nextClaimIndex_].publishedSequence.load(std::memory_order_relaxed);
    }

    for (std::size_t offset = 1; offset < slotCount; ++offset) {
        const auto index = (nextClaimIndex_ + offset) % slotCount;
        expected = SlotState::free;
        if (slots_[index].state.compare_exchange_strong(expected, SlotState::writing,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            slotIndex = index;
            nextClaimIndex_ = (slotIndex + 1) % slotCount;
            return &slots_[slotIndex];
        }

        if (expected != SlotState::ready)
            continue;

        const auto sequence = slots_[index].publishedSequence.load(std::memory_order_relaxed);
        if (sequence < oldestReadySequence) {
            oldestReadySequence = sequence;
            oldestReadyIndex = index;
        }
    }

    if (oldestReadyIndex == slotCount)
        return nullptr;

    expected = SlotState::ready;
    auto& oldestReady = slots_[oldestReadyIndex];
    if (!oldestReady.state.compare_exchange_strong(
            expected, SlotState::writing, std::memory_order_acquire, std::memory_order_relaxed)) {
        return nullptr;
    }

    removeReadyFrames(oldestReady.frameCount);
    reclaimedReady = true;
    slotIndex = oldestReadyIndex;
    nextClaimIndex_ = (slotIndex + 1) % slotCount;
    return &oldestReady;
}

bool StereoSampleCapture::beginPackedChunk(const double sampleRate, const std::uint64_t generation,
    const std::uint32_t channelCount, const std::uint64_t captureLifecycleGeneration,
    const std::size_t inputFrameOffset, PublishResult& result) noexcept
{
    const auto sequence = nextSequence_++;
    ++result.attemptedChunks;
    attemptedChunks_.fetch_add(1, std::memory_order_relaxed);
    lastAttemptedSequence_.store(sequence, std::memory_order_relaxed);

    auto reclaimedReady = false;
    auto slotIndex = std::size_t { 0 };
    auto* const slot = claimSlot(reclaimedReady, slotIndex);
    if (slot == nullptr) {
        beginCaptureDiscontinuity(sequence, inputFrameOffset, result);
        ++result.droppedIncomingChunks;
        droppedIncomingChunks_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (reclaimedReady) {
        beginCaptureDiscontinuity(sequence, inputFrameOffset, result);
        ++result.reclaimedReadyChunks;
        reclaimedReadyChunks_.fetch_add(1, std::memory_order_relaxed);
    }

    slot->frameCount = 0;
    slot->generation = generation;
    slot->sequence = sequence;
    slot->capturedFrameEnd = capturedFrameCursor_;
    slot->sampleRate = sampleRate;
    slot->channelCount = channelCount;
    slot->captureDiscontinuityRevision = captureDiscontinuityRevision_;
    slot->captureLifecycleGeneration = captureLifecycleGeneration;
    packedSlotIndex_ = slotIndex;
    packedFrameCount_ = 0;
    assert((bufferedState_.load(std::memory_order_relaxed) & partialFrameMask) == 0);
    result.captureDiscontinuityRevision = captureDiscontinuityRevision_;
    return true;
}

void StereoSampleCapture::finalizePackedChunk(PublishResult& result) noexcept
{
    assert(packedSlotIndex_ < slotCount);
    assert(packedFrameCount_ > 0 && packedFrameCount_ <= framesPerSlot);
    auto& slot = slots_[packedSlotIndex_];
    assert(slot.state.load(std::memory_order_relaxed) == SlotState::writing);

    slot.frameCount = packedFrameCount_;
    slot.capturedFrameEnd = capturedFrameCursor_;
    slot.publishedSequence.store(slot.sequence, std::memory_order_relaxed);
    addReadyFrames(packedFrameCount_);
    slot.state.store(SlotState::ready, std::memory_order_release);

    ++result.publishedChunks;
    publishedChunks_.fetch_add(1, std::memory_order_relaxed);
    packedSlotIndex_ = slotCount;
    packedFrameCount_ = 0;
}

void StereoSampleCapture::abandonPackedChunk() noexcept
{
    if (packedSlotIndex_ == slotCount)
        return;

    assert(packedSlotIndex_ < slotCount);
    auto& slot = slots_[packedSlotIndex_];
    assert(slot.state.load(std::memory_order_relaxed) == SlotState::writing);
    const auto previousBufferedState
        = bufferedState_.fetch_sub(packedFrameCount_, std::memory_order_relaxed);
    assert((previousBufferedState & partialFrameMask) == packedFrameCount_);
    static_cast<void>(previousBufferedState);
    slot.state.store(SlotState::free, std::memory_order_release);
    packedSlotIndex_ = slotCount;
    packedFrameCount_ = 0;
}

void StereoSampleCapture::retireReadyChunksForLifecycleChange() noexcept
{
    for (auto& slot : slots_) {
        auto expected = SlotState::ready;
        if (!slot.state.compare_exchange_strong(
                expected, SlotState::free, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            continue;
        }

        removeReadyFrames(slot.frameCount);
    }

    nextClaimIndex_ = 0;
}

void StereoSampleCapture::beginCaptureDiscontinuity(const std::uint64_t sequence,
    const std::size_t inputFrameOffset, PublishResult& result) noexcept
{
    if (captureDiscontinuityRevision_ == 0
        || captureDiscontinuityRevision_
            == acknowledgedCaptureDiscontinuityRevision_.load(std::memory_order_acquire)) {
        captureDiscontinuityRevision_ = sequence;
        if (!result.beganCaptureDiscontinuity) {
            result.beganCaptureDiscontinuity = true;
            result.firstDiscontinuityFrameOffset = inputFrameOffset;
        }
        overflowEpisodes_.fetch_add(1, std::memory_order_relaxed);
    }

    result.captureDiscontinuityRevision = captureDiscontinuityRevision_;
    publishedCaptureDiscontinuityRevision_.store(
        captureDiscontinuityRevision_, std::memory_order_release);
}

bool StereoSampleCapture::tryAcquireOldest(ReadHandle& destination) noexcept
{
    destination.release();

    std::size_t oldestIndex = slots_.size();
    auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
            continue;

        const auto sequence = slots_[index].publishedSequence.load(std::memory_order_relaxed);
        if (sequence < oldestSequence) {
            oldestSequence = sequence;
            oldestIndex = index;
        }
    }

    if (oldestIndex == slots_.size())
        return false;

    auto expected = SlotState::ready;
    auto& slot = slots_[oldestIndex];
    if (!slot.state.compare_exchange_strong(
            expected, SlotState::reading, std::memory_order_acquire, std::memory_order_relaxed)) {
        return false;
    }
    removeReadyFrames(slot.frameCount);

    bool followsDiscontinuity = false;
    if (consumerHasPreviousSequence_ && slot.generation == consumerPreviousGeneration_) {
        followsDiscontinuity = slot.sequence != consumerPreviousSequence_ + 1
            || slot.channelCount != consumerPreviousChannelCount_;
    }

    if (followsDiscontinuity)
        consumerDiscontinuities_.fetch_add(1, std::memory_order_relaxed);

    consumerHasPreviousSequence_ = true;
    consumerPreviousGeneration_ = slot.generation;
    consumerPreviousSequence_ = slot.sequence;
    consumerPreviousChannelCount_ = slot.channelCount;
    // Preserve the revision stamped when this payload was written. The
    // coordinator observes the producer-wide revision separately after
    // acquisition so a surviving pre-gap slot cannot be reclassified as
    // post-gap input by a concurrent overflow.
    destination = ReadHandle(*this, oldestIndex,
        { slot.left.data(), slot.right.data(), slot.frameCount, slot.generation, slot.sequence,
            slot.capturedFrameEnd, slot.sampleRate, followsDiscontinuity, slot.channelCount,
            slot.captureDiscontinuityRevision, slot.captureLifecycleGeneration });
    return true;
}

StereoSampleCapture::Telemetry StereoSampleCapture::telemetry() const noexcept
{
    Telemetry result;
    result.attemptedChunks = attemptedChunks_.load(std::memory_order_relaxed);
    result.publishedChunks = publishedChunks_.load(std::memory_order_relaxed);
    result.reclaimedReadyChunks = reclaimedReadyChunks_.load(std::memory_order_relaxed);
    result.droppedIncomingChunks = droppedIncomingChunks_.load(std::memory_order_relaxed);
    result.overflowEpisodes = overflowEpisodes_.load(std::memory_order_relaxed);
    result.consumerDiscontinuities = consumerDiscontinuities_.load(std::memory_order_relaxed);
    result.lastAttemptedSequence = lastAttemptedSequence_.load(std::memory_order_relaxed);
    result.capturedFrames = capturedFrames_.load(std::memory_order_relaxed);
    const auto bufferedState = bufferedState_.load(std::memory_order_relaxed);
    result.partialFrames = static_cast<std::uint32_t>(bufferedState & partialFrameMask);
    result.readyFrames = (bufferedState >> 16U) & readyFrameMask;
    result.readySlots = static_cast<std::uint32_t>(bufferedState >> 48U);
    result.readyFrameHighWaterMark
        = std::max(readyFrameHighWaterMark_.load(std::memory_order_relaxed), result.readyFrames);
    result.readyHighWaterMark
        = std::max(readyHighWaterMark_.load(std::memory_order_relaxed), result.readySlots);

    return result;
}

std::uint64_t StereoSampleCapture::captureDiscontinuityRevision(
    const std::uint64_t captureLifecycleGeneration) const noexcept
{
    if (publishedCaptureDiscontinuityLifecycleGeneration_.load(std::memory_order_acquire)
        != captureLifecycleGeneration) {
        return 0;
    }

    return publishedCaptureDiscontinuityRevision_.load(std::memory_order_acquire);
}

void StereoSampleCapture::acknowledgeCaptureDiscontinuityRevision(
    const std::uint64_t revision) noexcept
{
    acknowledgedCaptureDiscontinuityRevision_.store(revision, std::memory_order_release);
}

void StereoSampleCapture::discardPending() noexcept
{
    abandonPackedChunk();
    for (auto& slot : slots_) {
        const auto state = slot.state.load(std::memory_order_acquire);
        assert(state != SlotState::writing && state != SlotState::reading);

        if (state == SlotState::ready) {
            slot.state.store(SlotState::free, std::memory_order_release);
            removeReadyFrames(slot.frameCount);
        }
    }

    assert(bufferedState_.load(std::memory_order_relaxed) == 0);
    nextClaimIndex_ = 0;

    consumerHasPreviousSequence_ = false;
    consumerPreviousGeneration_ = 0;
    consumerPreviousSequence_ = 0;
    consumerPreviousChannelCount_ = 0;
}

void StereoSampleCapture::releaseReadSlot(const std::size_t slotIndex) noexcept
{
    assert(slotIndex < slots_.size());
    assert(slots_[slotIndex].state.load(std::memory_order_relaxed) == SlotState::reading);
    slots_[slotIndex].state.store(SlotState::free, std::memory_order_release);
}

void StereoSampleCapture::addReadyFrames(const std::size_t frameCount) noexcept
{
    const auto transfer = readySlotUnit + frameCount * readyFrameUnit - frameCount;
    const auto bufferedState
        = bufferedState_.fetch_add(transfer, std::memory_order_relaxed) + transfer;
    const auto readyCount = static_cast<std::uint32_t>(bufferedState >> 48U);
    const auto readyFrameCount = (bufferedState >> 16U) & readyFrameMask;
    assert((bufferedState & partialFrameMask) == 0);

    if (readyCount > producerReadyHighWaterMark_) {
        producerReadyHighWaterMark_ = readyCount;
        readyHighWaterMark_.store(readyCount, std::memory_order_relaxed);
    }

    if (readyFrameCount > producerReadyFrameHighWaterMark_) {
        producerReadyFrameHighWaterMark_ = readyFrameCount;
        readyFrameHighWaterMark_.store(readyFrameCount, std::memory_order_relaxed);
    }
}

void StereoSampleCapture::removeReadyFrames(const std::size_t frameCount) noexcept
{
    const auto removal = readySlotUnit + frameCount * readyFrameUnit;
    const auto previousState = bufferedState_.fetch_sub(removal, std::memory_order_relaxed);
    const auto previousSlots = static_cast<std::uint32_t>(previousState >> 48U);
    const auto previousFrames = (previousState >> 16U) & readyFrameMask;
    assert(previousSlots > 0);
    assert(previousFrames >= frameCount);
    static_cast<void>(previousSlots);
    static_cast<void>(previousFrames);
}
} // namespace audio_insight
