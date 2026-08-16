// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/** A non-owning view whose lifetime is tied to a StereoSampleCapture::ReadHandle. */
struct CapturedStereoChunkView {
    const float* left = nullptr;
    const float* right = nullptr;
    std::size_t frameCount = 0;

    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t capturedFrameEnd = 0;
    double sampleRate = 0.0;
    bool followsDiscontinuity = false;
    std::uint32_t channelCount = 2;
    std::uint64_t captureDiscontinuityRevision = 0;
    std::uint64_t captureLifecycleGeneration = 0;
};

/**
    A fixed-capacity single-producer sample handoff.

    publishBlock() is intended for the audio callback. It performs no allocation,
    locking, waiting, wakeup, or unbounded retry. When the queue is full, the
    producer reclaims the oldest ready (but not reading) slot. If every slot is
    owned by a reader, the incoming chunk is dropped. Both cases remain visible
    through Telemetry and monotonically increasing chunk sequences.

    There may be only one logical consumer, although that consumer may keep more
    than one ReadHandle alive. discardPending() requires the producer to be
    quiescent and all read handles to have been released.
*/
class StereoSampleCapture final {
public:
    // Keep the original 32,768-frame storage budget, but divide it into small
    // packed chunks. A 64-frame host callback therefore no longer consumes one
    // whole 2,048-frame slot. The resulting capacity is about 683 ms at 48 kHz,
    // 341 ms at 96 kHz, and 171 ms at 192 kHz, independent of host block size.
    static constexpr std::size_t slotCount = 128;
    static constexpr std::size_t framesPerSlot = 256;
    static constexpr std::size_t bufferedFrameCapacity = slotCount * framesPerSlot;
    static constexpr std::size_t maximumFramesPerPublishCall = framesPerSlot;
    static constexpr std::size_t noDiscontinuityFrameOffset = static_cast<std::size_t>(-1);

    struct PublishResult {
        std::uint32_t attemptedChunks = 0;
        std::uint32_t publishedChunks = 0;
        std::uint32_t reclaimedReadyChunks = 0;
        std::uint32_t droppedIncomingChunks = 0;
        std::size_t firstDiscontinuityFrameOffset = noDiscontinuityFrameOffset;
        std::uint64_t precedingCaptureDiscontinuityRevision = 0;
        std::uint64_t captureDiscontinuityRevision = 0;
        bool beganCaptureDiscontinuity = false;
    };

    struct Telemetry {
        std::uint64_t attemptedChunks = 0;
        std::uint64_t publishedChunks = 0;
        std::uint64_t reclaimedReadyChunks = 0;
        std::uint64_t droppedIncomingChunks = 0;
        std::uint64_t overflowEpisodes = 0;
        std::uint64_t consumerDiscontinuities = 0;
        std::uint64_t lastAttemptedSequence = 0;
        std::uint64_t capturedFrames = 0;
        std::uint64_t readyFrames = 0;
        std::uint64_t readyFrameHighWaterMark = 0;
        std::uint64_t bufferedFrameCapacity = StereoSampleCapture::bufferedFrameCapacity;
        std::uint32_t readyHighWaterMark = 0;
        std::uint32_t readySlots = 0;
        std::uint32_t partialFrames = 0;

        [[nodiscard]] std::uint64_t lostChunks() const noexcept
        {
            return reclaimedReadyChunks + droppedIncomingChunks;
        }
    };

    class ReadHandle final {
    public:
        ReadHandle() noexcept = default;
        ReadHandle(ReadHandle&& other) noexcept;
        ReadHandle& operator=(ReadHandle&& other) noexcept;
        ~ReadHandle();

        ReadHandle(const ReadHandle&) = delete;
        ReadHandle& operator=(const ReadHandle&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return owner_ != nullptr;
        }
        [[nodiscard]] const CapturedStereoChunkView& view() const noexcept
        {
            return view_;
        }
        void release() noexcept;

    private:
        friend class StereoSampleCapture;

        ReadHandle(StereoSampleCapture& owner, std::size_t slotIndex,
            CapturedStereoChunkView view) noexcept;

        StereoSampleCapture* owner_ = nullptr;
        std::size_t slotIndex_ = 0;
        CapturedStereoChunkView view_ { };
    };

    StereoSampleCapture() noexcept;

    StereoSampleCapture(const StereoSampleCapture&) = delete;
    StereoSampleCapture& operator=(const StereoSampleCapture&) = delete;

    /**
        Packs frames into fixed 256-frame chunks. frameCount must not exceed
        maximumFramesPerPublishCall; Debug builds assert this precondition and
        release builds reject oversized calls without mutating capture state.

        A null channel pointer is treated as silence. Passing the same pointer for
        both channels is supported. generation is the public capture generation;
        captureLifecycleGeneration is the stable producer-admission token. Neither
        is derived from host transport state.
    */
    [[nodiscard]] PublishResult publishBlock(const float* left, const float* right,
        std::size_t frameCount, double sampleRate, std::uint64_t generation,
        std::uint32_t channelCount = 2, std::uint64_t captureLifecycleGeneration = 0) noexcept;

    /** Acquires the oldest currently ready chunk without waiting. */
    [[nodiscard]] bool tryAcquireOldest(ReadHandle& destination) noexcept;

    [[nodiscard]] Telemetry telemetry() const noexcept;

    /** Latest raw capture boundary for one lifecycle, including unconsumed drops. */
    [[nodiscard]] std::uint64_t captureDiscontinuityRevision(
        std::uint64_t captureLifecycleGeneration) const noexcept;

    /**
        Marks a revision as handled after the consumer has published post-gap
        state. Safe from the single non-audio consumer; acquisition alone does
        not acknowledge a revision.
    */
    void acknowledgeCaptureDiscontinuityRevision(std::uint64_t revision) noexcept;

    /**
        Drops all queued chunks and resets consumer continuity tracking.
        Call only after the producer is quiescent and all ReadHandles are gone.
        Producer-owned sequence and frame counters deliberately remain monotonic.
    */
    void discardPending() noexcept;

private:
    enum class SlotState : std::uint32_t { free, writing, ready, reading };

    static_assert(std::atomic<SlotState>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    static constexpr std::uint64_t partialFrameMask = (std::uint64_t { 1 } << 16U) - 1U;
    static constexpr std::uint64_t readyFrameMask = (std::uint64_t { 1 } << 32U) - 1U;
    static constexpr std::uint64_t readyFrameUnit = std::uint64_t { 1 } << 16U;
    static constexpr std::uint64_t readySlotUnit = std::uint64_t { 1 } << 48U;

    struct Slot {
        std::atomic<SlotState> state { SlotState::free };
        // Safe selection key: readers must not inspect non-atomic payload fields
        // until they have changed state from ready to reading.
        std::atomic<std::uint64_t> publishedSequence { 0 };
        std::array<float, framesPerSlot> left { };
        std::array<float, framesPerSlot> right { };
        std::size_t frameCount = 0;
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        std::uint64_t capturedFrameEnd = 0;
        double sampleRate = 0.0;
        std::uint32_t channelCount = 2;
        std::uint64_t captureDiscontinuityRevision = 0;
        std::uint64_t captureLifecycleGeneration = 0;
    };

    [[nodiscard]] Slot* claimSlot(bool& reclaimedReady, std::size_t& slotIndex) noexcept;
    [[nodiscard]] bool beginPackedChunk(double sampleRate, std::uint64_t generation,
        std::uint32_t channelCount, std::uint64_t captureLifecycleGeneration,
        std::size_t inputFrameOffset, PublishResult& result) noexcept;
    void finalizePackedChunk(PublishResult& result) noexcept;
    void abandonPackedChunk() noexcept;
    void retireReadyChunksForLifecycleChange() noexcept;
    void beginCaptureDiscontinuity(
        std::uint64_t sequence, std::size_t inputFrameOffset, PublishResult& result) noexcept;
    void releaseReadSlot(std::size_t slotIndex) noexcept;
    void addReadyFrames(std::size_t frameCount) noexcept;
    void removeReadyFrames(std::size_t frameCount) noexcept;

    std::array<Slot, slotCount> slots_ { };

    // Written only by the audio producer.
    std::uint64_t nextSequence_ = 1;
    std::uint64_t capturedFrameCursor_ = 0;
    std::uint64_t producerCaptureLifecycleGeneration_ = 0;
    std::uint64_t captureDiscontinuityRevision_ = 0;
    std::size_t nextClaimIndex_ = 0;
    std::size_t packedSlotIndex_ = slotCount;
    std::size_t packedFrameCount_ = 0;
    std::uint32_t producerReadyHighWaterMark_ = 0;
    std::uint64_t producerReadyFrameHighWaterMark_ = 0;

    // Written only by the logical consumer.
    bool consumerHasPreviousSequence_ = false;
    std::uint64_t consumerPreviousGeneration_ = 0;
    std::uint64_t consumerPreviousSequence_ = 0;
    std::uint32_t consumerPreviousChannelCount_ = 0;

    std::atomic<std::uint64_t> attemptedChunks_ { 0 };
    std::atomic<std::uint64_t> publishedChunks_ { 0 };
    std::atomic<std::uint64_t> reclaimedReadyChunks_ { 0 };
    std::atomic<std::uint64_t> droppedIncomingChunks_ { 0 };
    std::atomic<std::uint64_t> overflowEpisodes_ { 0 };
    std::atomic<std::uint64_t> consumerDiscontinuities_ { 0 };
    std::atomic<std::uint64_t> lastAttemptedSequence_ { 0 };
    std::atomic<std::uint64_t> capturedFrames_ { 0 };
    std::atomic<std::uint64_t> publishedCaptureDiscontinuityRevision_ { 0 };
    std::atomic<std::uint64_t> publishedCaptureDiscontinuityLifecycleGeneration_ { 0 };
    std::atomic<std::uint64_t> acknowledgedCaptureDiscontinuityRevision_ { 0 };
    std::atomic<std::uint32_t> readyHighWaterMark_ { 0 };
    // One lock-free word keeps ready-slot, ready-frame, and producer-partial
    // counts coherent for concurrent telemetry snapshots.
    std::atomic<std::uint64_t> bufferedState_ { 0 };
    std::atomic<std::uint64_t> readyFrameHighWaterMark_ { 0 };
};
} // namespace audio_insight
