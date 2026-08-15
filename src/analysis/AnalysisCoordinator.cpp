// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AnalysisCoordinator.h"

#include "HannSpectrumAnalyzer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace audio_insight {
namespace {
using Clock = std::chrono::steady_clock;

constexpr auto analysisRequestPeriod = std::chrono::nanoseconds { 16'666'667 };
constexpr auto requestDeadlineTolerance = std::chrono::milliseconds { 1 };
constexpr auto requestDeadlineToleranceNanoseconds
    = std::chrono::duration_cast<std::chrono::nanoseconds>(requestDeadlineTolerance).count();
constexpr auto staleInputTimeout = std::chrono::milliseconds { 250 };
constexpr auto staleInputTimeoutNanoseconds
    = std::chrono::duration_cast<std::chrono::nanoseconds>(staleInputTimeout).count();

[[nodiscard]] bool sampleRatesDiffer(const double left, const double right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return true;

    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}

template <typename Integer>
void updateMaximum(std::atomic<Integer>& destination, const Integer candidate) noexcept
{
    auto previous = destination.load(std::memory_order_relaxed);
    while (candidate > previous
        && !destination.compare_exchange_weak(
            previous, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) { }
}

class VisualizationSnapshotExchange final {
public:
    [[nodiscard]] bool publish(const VisualizationFrame& frame) noexcept
    {
        std::size_t selected = slots_.size();

        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto expected = SlotState::free;
            if (slots_[index].state.compare_exchange_strong(expected, SlotState::writing,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                selected = index;
                break;
            }
        }

        if (selected == slots_.size()) {
            auto oldestSequence = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
                    continue;

                const auto sequence
                    = slots_[index].publicationSequence.load(std::memory_order_relaxed);
                if (sequence < oldestSequence) {
                    oldestSequence = sequence;
                    selected = index;
                }
            }

            if (selected != slots_.size()) {
                auto expected = SlotState::ready;
                if (!slots_[selected].state.compare_exchange_strong(expected, SlotState::writing,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    selected = slots_.size();
                }
            }
        }

        if (selected == slots_.size()) {
            droppedPublications_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto sequence = nextPublicationSequence_.fetch_add(1, std::memory_order_relaxed);
        auto& slot = slots_[selected];
        slot.frame = frame;
        slot.publicationSequence.store(sequence, std::memory_order_relaxed);
        slot.state.store(SlotState::ready, std::memory_order_release);
        publishedFrames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool copyLatest(VisualizationFrame& destination) const noexcept
    {
        VisualizationFrame newestFrame;
        std::uint64_t newestSequence = 0;

        // Two fixed passes also catch a publication into an index already
        // visited during the first pass. Older ready frames are retired here so
        // a later render can never regress to them.
        for (std::size_t pass = 0; pass < 2; ++pass) {
            for (auto& slot : slots_) {
                auto expected = SlotState::ready;
                if (!slot.state.compare_exchange_strong(expected, SlotState::reading,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    continue;
                }

                const auto sequence = slot.publicationSequence.load(std::memory_order_relaxed);
                if (sequence > newestSequence) {
                    newestFrame = slot.frame;
                    newestSequence = sequence;
                }

                slot.state.store(SlotState::free, std::memory_order_release);
            }
        }

        if (newestSequence == 0)
            return false;

        auto previous = lastCopiedSequence_.load(std::memory_order_relaxed);
        if (newestSequence <= previous
            || !lastCopiedSequence_.compare_exchange_strong(
                previous, newestSequence, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return false;
        }

        destination = newestFrame;
        return true;
    }

    [[nodiscard]] std::uint64_t publishedFrames() const noexcept
    {
        return publishedFrames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t droppedPublications() const noexcept
    {
        return droppedPublications_.load(std::memory_order_relaxed);
    }

private:
    enum class SlotState : std::uint32_t { free, writing, ready, reading };

    static_assert(std::atomic<SlotState>::is_always_lock_free);

    struct Slot {
        std::atomic<SlotState> state { SlotState::free };
        std::atomic<std::uint64_t> publicationSequence { 0 };
        VisualizationFrame frame;
    };

    static constexpr std::size_t slotCount = 3;
    mutable std::array<Slot, slotCount> slots_;
    std::atomic<std::uint64_t> nextPublicationSequence_ { 1 };
    mutable std::atomic<std::uint64_t> lastCopiedSequence_ { 0 };
    std::atomic<std::uint64_t> publishedFrames_ { 0 };
    std::atomic<std::uint64_t> droppedPublications_ { 0 };
};
} // namespace

struct AnalysisCoordinator::State final : SharedAnalysisScheduler::JobClient {
    void captureAudioBlock(const float* const left, const float* const right,
        const std::size_t frameCount, const double sampleRate, const std::uint64_t generation,
        const std::uint32_t channelCount) noexcept
    {
        std::size_t offset = 0;
        while (offset < frameCount) {
            const auto chunkFrames
                = std::min(StereoSampleCapture::framesPerSlot, frameCount - offset);
            const auto* const chunkLeft = left != nullptr ? left + offset : nullptr;
            const auto* const chunkRight = right != nullptr ? right + offset : nullptr;
            const auto samplePublication = samples.publishBlock(
                chunkLeft, chunkRight, chunkFrames, sampleRate, generation, channelCount);
            const auto followsCaptureDiscontinuity = samplePublication.reclaimedReadyChunks != 0
                || samplePublication.droppedIncomingChunks != 0;
            static_cast<void>(meters.publishBlock(chunkLeft, chunkRight, chunkFrames, sampleRate,
                generation, channelCount, followsCaptureDiscontinuity));
            offset += chunkFrames;
        }

        captureRevision.fetch_add(1, std::memory_order_release);
    }

    void beginGeneration(const std::uint64_t generation) noexcept
    {
        workingFrame = { };
        workingFrame.spectrumDecibels.fill(minimumDisplayDecibels);
        workingFrame.generation = generation;
        newestCapturedFrameEnd = 0;
        nextCoalescedInputSequence = 1;
        spectrum.reset(&workingFrame);
        spectrumCapturedFrameEnd.store(0, std::memory_order_relaxed);
        meterCapturedFrameEnd.store(0, std::memory_order_relaxed);
        hasPublishedAudioFrame.store(false, std::memory_order_relaxed);
        staleClearPending.store(false, std::memory_order_relaxed);
        peakRmsResetPendingEpoch.store(0, std::memory_order_relaxed);
        requiredUserResetEpoch = 0;
        requiredLiveClearEpoch = 0;
        lastAnalyzedCaptureRevision.store(
            captureRevision.load(std::memory_order_acquire), std::memory_order_release);

        static_cast<void>(snapshots.publish(workingFrame));

        // Publish the generation only after all worker-owned state and the
        // initial snapshot are complete. A defensive generation check in
        // execute() can therefore never expose a partially initialised state.
        currentGeneration.store(generation, std::memory_order_release);
    }

    void discardPendingCapture() noexcept
    {
        samples.discardPending();
        meters.discardPending();
    }

    [[nodiscard]] std::uint64_t latestCaptureRevision() const noexcept
    {
        return captureRevision.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t analyzedCaptureRevision() const noexcept
    {
        return lastAnalyzedCaptureRevision.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool hasAudioFrame() const noexcept
    {
        return hasPublishedAudioFrame.load(std::memory_order_acquire);
    }

    void noteEmptyRequestAvoided() noexcept
    {
        emptyAnalysisRequestsAvoided.fetch_add(1, std::memory_order_relaxed);
    }

    void armStaleClear(const std::uint64_t revision) noexcept
    {
        staleClearRevision.store(revision, std::memory_order_relaxed);
        staleClearPending.store(true, std::memory_order_release);
    }

    void cancelStaleClear() noexcept
    {
        staleClearPending.store(false, std::memory_order_release);
    }

    void requestPeakRmsReset() noexcept
    {
        const auto epoch = meters.requestUserReset();
        peakRmsResetPendingEpoch.store(epoch, std::memory_order_release);
        peakRmsUserResets.fetch_add(1, std::memory_order_relaxed);
    }

    void applyPeakRmsReading(const StereoMeterReading& reading) noexcept
    {
        if (workingFrame.spectrumValid
            && (workingFrame.channelCount != reading.channelCount
                || sampleRatesDiffer(workingFrame.sampleRate, reading.sampleRate))) {
            spectrum.reset(&workingFrame);
        }

        workingFrame.peakDecibels = reading.peakDecibels;
        workingFrame.rmsDecibels = reading.rmsDecibels;
        workingFrame.heldPeakDecibels = reading.heldPeakDecibels;
        workingFrame.over = reading.over;
        workingFrame.channelCount = reading.channelCount;
        workingFrame.meterValid = reading.valid;

        if (reading.appliedLiveClearEpoch < requiredLiveClearEpoch) {
            workingFrame.peakDecibels.fill(minimumDisplayDecibels);
            workingFrame.rmsDecibels.fill(minimumDisplayDecibels);
        }

        if (reading.appliedUserResetEpoch < requiredUserResetEpoch) {
            workingFrame.heldPeakDecibels.fill(minimumDisplayDecibels);
            workingFrame.over.fill(false);
        }

        workingFrame.meterSequence = nextMeterSequence++;
    }

    void execute(const SharedAnalysisScheduler::JobContext& context) override
    {
        const auto startedAt = Clock::now();
        const auto transformsAtStart = spectrum.statistics().transforms;
        const auto revisionAtStart = captureRevision.load(std::memory_order_acquire);
        jobsStarted.fetch_add(1, std::memory_order_relaxed);

        const auto finish = [this, startedAt, transformsAtStart](const bool stopped) {
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - startedAt)
                    .count());
            const auto transformsNow = spectrum.statistics().transforms;
            const auto jobTransforms
                = transformsNow >= transformsAtStart ? transformsNow - transformsAtStart : 0;
            lastJobNanoseconds.store(elapsed, std::memory_order_relaxed);
            updateMaximum(maximumJobNanoseconds, elapsed);
            spectrumTransforms.fetch_add(jobTransforms, std::memory_order_relaxed);
            lastJobSpectrumTransforms.store(jobTransforms, std::memory_order_relaxed);
            updateMaximum(maximumJobSpectrumTransforms, jobTransforms);

            if (stopped)
                jobsStopped.fetch_add(1, std::memory_order_relaxed);
            else
                jobsCompleted.fetch_add(1, std::memory_order_relaxed);
        };

        const auto generation = context.generation();
        if (context.stopRequested()
            || generation != currentGeneration.load(std::memory_order_acquire)) {
            finish(true);
            return;
        }

        bool frameChanged = false;
        bool consumedValidAudio = false;
        StereoSampleCapture::ReadHandle handle;
        std::size_t retainedFrames = 0;
        std::uint64_t retainedFrameEnd = 0;
        std::uint64_t previousChunkSequence = 0;
        std::uint64_t previousChunkFrameEnd = 0;
        double retainedSampleRate = 0.0;
        std::uint32_t retainedChannelCount = 0;
        bool hasPreviousChunk = false;
        bool inputFollowsDiscontinuity = false;
        std::uint64_t discardedFrames = 0;

        for (std::size_t consumed = 0; consumed < StereoSampleCapture::slotCount; ++consumed) {
            if (context.stopRequested()) {
                finish(true);
                return;
            }

            if (!samples.tryAcquireOldest(handle))
                break;

            const auto& chunk = handle.view();
            if (chunk.generation != generation) {
                ignoredGenerationChunks.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const auto hasValidRange = chunk.capturedFrameEnd >= chunk.frameCount;
            const auto chunkFrameStart
                = hasValidRange ? chunk.capturedFrameEnd - chunk.frameCount : 0;
            const auto isContiguous = hasPreviousChunk
                && previousChunkSequence != std::numeric_limits<std::uint64_t>::max()
                && chunk.sequence == previousChunkSequence + 1 && hasValidRange
                && chunkFrameStart == previousChunkFrameEnd
                && !sampleRatesDiffer(retainedSampleRate, chunk.sampleRate)
                && retainedChannelCount == chunk.channelCount && !chunk.followsDiscontinuity;

            if (hasPreviousChunk && !isContiguous) {
                retainedFrames = 0;
                inputFollowsDiscontinuity = true;
            } else if (chunk.followsDiscontinuity) {
                inputFollowsDiscontinuity = true;
            }

            auto chunkOffset = std::size_t { 0 };
            auto chunkFrames = chunk.frameCount;
            if (chunkFrames >= fftSize) {
                discardedFrames += retainedFrames + (chunkFrames - fftSize);
                chunkOffset = chunkFrames - fftSize;
                chunkFrames = fftSize;
                retainedFrames = 0;
                inputFollowsDiscontinuity = true;
            } else if (retainedFrames + chunkFrames > fftSize) {
                const auto overflow = retainedFrames + chunkFrames - fftSize;
                const auto remaining = retainedFrames - overflow;
                std::memmove(spectrumLeftScratch.data(),
                    spectrumLeftScratch.data() + static_cast<std::ptrdiff_t>(overflow),
                    remaining * sizeof(float));
                std::memmove(spectrumRightScratch.data(),
                    spectrumRightScratch.data() + static_cast<std::ptrdiff_t>(overflow),
                    remaining * sizeof(float));
                retainedFrames = remaining;
                discardedFrames += overflow;
                inputFollowsDiscontinuity = true;
            }

            if (chunkFrames > 0) {
                std::memcpy(
                    spectrumLeftScratch.data() + static_cast<std::ptrdiff_t>(retainedFrames),
                    chunk.left + static_cast<std::ptrdiff_t>(chunkOffset),
                    chunkFrames * sizeof(float));
                std::memcpy(
                    spectrumRightScratch.data() + static_cast<std::ptrdiff_t>(retainedFrames),
                    chunk.right + static_cast<std::ptrdiff_t>(chunkOffset),
                    chunkFrames * sizeof(float));
                retainedFrames += chunkFrames;
            }

            retainedFrameEnd = chunk.capturedFrameEnd;
            retainedSampleRate = chunk.sampleRate;
            retainedChannelCount = chunk.channelCount;
            previousChunkSequence = chunk.sequence;
            previousChunkFrameEnd = chunk.capturedFrameEnd;
            hasPreviousChunk = true;
        }

        if (discardedFrames > 0)
            backlogDiscardedFrames.fetch_add(discardedFrames, std::memory_order_relaxed);

        if (context.stopRequested()) {
            finish(true);
            return;
        }

        if (retainedFrames > 0) {
            const CapturedStereoChunkView coalescedInput {
                spectrumLeftScratch.data(),
                spectrumRightScratch.data(),
                retainedFrames,
                generation,
                nextCoalescedInputSequence++,
                retainedFrameEnd,
                retainedSampleRate,
                inputFollowsDiscontinuity,
                retainedChannelCount,
            };
            const auto spectrumWasValid = workingFrame.spectrumValid;
            const auto producedSpectrum = spectrum.process(coalescedInput, workingFrame);
            if (producedSpectrum) {
                newestCapturedFrameEnd
                    = std::max(newestCapturedFrameEnd, workingFrame.capturedFrameEnd);
                spectrumCapturedFrameEnd.store(
                    workingFrame.capturedFrameEnd, std::memory_order_relaxed);
            }
            frameChanged = frameChanged || producedSpectrum
                || spectrumWasValid != workingFrame.spectrumValid;
        }

        StereoMeterReading meterReading;
        if (meters.consumeLatest(meterReading) && meterReading.generation == generation) {
            applyPeakRmsReading(meterReading);
            newestCapturedFrameEnd
                = std::max(newestCapturedFrameEnd, meterReading.capturedFrameEnd);
            workingFrame.sampleRate = meterReading.sampleRate;
            meterCapturedFrameEnd.store(meterReading.capturedFrameEnd, std::memory_order_relaxed);
            frameChanged = true;
            consumedValidAudio = meterReading.valid;
        }

        const auto pendingResetEpoch
            = peakRmsResetPendingEpoch.exchange(0, std::memory_order_acq_rel);
        if (pendingResetEpoch != 0) {
            requiredUserResetEpoch = std::max(requiredUserResetEpoch, pendingResetEpoch);

            const auto resetWasAlreadyApplied = meterReading.valid
                && meterReading.appliedUserResetEpoch >= requiredUserResetEpoch;
            if (workingFrame.meterValid && !resetWasAlreadyApplied) {
                workingFrame.heldPeakDecibels.fill(minimumDisplayDecibels);
                workingFrame.over.fill(false);
                workingFrame.meterSequence = nextMeterSequence++;
                frameChanged = true;
            }
        }

        if (context.stopRequested()
            || generation != currentGeneration.load(std::memory_order_acquire)) {
            finish(true);
            return;
        }

        const auto staleWasPending = staleClearPending.exchange(false, std::memory_order_acq_rel);
        const auto shouldClearStaleFrame = staleWasPending
            && staleClearRevision.load(std::memory_order_relaxed)
                == captureRevision.load(std::memory_order_acquire);

        if (shouldClearStaleFrame) {
            spectrum.reset(&workingFrame);
            requiredLiveClearEpoch = std::max(requiredLiveClearEpoch, meters.requestLiveClear());
            workingFrame.peakDecibels.fill(minimumDisplayDecibels);
            workingFrame.rmsDecibels.fill(minimumDisplayDecibels);
            workingFrame.meterSequence = nextMeterSequence++;
            workingFrame.spectrumSequence
                = workingFrame.spectrumSequence == std::numeric_limits<std::uint64_t>::max()
                ? 1
                : workingFrame.spectrumSequence + 1;
            hasPublishedAudioFrame.store(false, std::memory_order_release);
            staleFramesPublished.fetch_add(1, std::memory_order_relaxed);
            frameChanged = true;
        } else if (consumedValidAudio) {
            hasPublishedAudioFrame.store(true, std::memory_order_release);
        }

        if (frameChanged) {
            workingFrame.generation = generation;
            workingFrame.capturedFrameEnd = newestCapturedFrameEnd;
            workingFrame.droppedChunks = samples.telemetry().lostChunks();
            static_cast<void>(snapshots.publish(workingFrame));
        }

        lastAnalyzedCaptureRevision.store(revisionAtStart, std::memory_order_release);
        finish(false);
    }

    [[nodiscard]] AnalysisTelemetry telemetry(
        const SharedAnalysisScheduler::Counters schedulerCounters) const noexcept
    {
        AnalysisTelemetry result;
        result.capture = samples.telemetry();
        result.meters = meters.telemetry();
        result.scheduler = schedulerCounters;
        result.jobsStarted = jobsStarted.load(std::memory_order_relaxed);
        result.jobsCompleted = jobsCompleted.load(std::memory_order_relaxed);
        result.jobsStopped = jobsStopped.load(std::memory_order_relaxed);
        result.ignoredGenerationChunks = ignoredGenerationChunks.load(std::memory_order_relaxed);
        result.publishedFrames = snapshots.publishedFrames();
        result.droppedFramePublications = snapshots.droppedPublications();
        result.lastJobNanoseconds = lastJobNanoseconds.load(std::memory_order_relaxed);
        result.maximumJobNanoseconds = maximumJobNanoseconds.load(std::memory_order_relaxed);
        result.spectrumTransforms = spectrumTransforms.load(std::memory_order_relaxed);
        result.lastJobSpectrumTransforms
            = lastJobSpectrumTransforms.load(std::memory_order_relaxed);
        result.maximumJobSpectrumTransforms
            = maximumJobSpectrumTransforms.load(std::memory_order_relaxed);
        result.backlogDiscardedFrames = backlogDiscardedFrames.load(std::memory_order_relaxed);
        result.spectrumCapturedFrameEnd = spectrumCapturedFrameEnd.load(std::memory_order_relaxed);
        result.meterCapturedFrameEnd = meterCapturedFrameEnd.load(std::memory_order_relaxed);
        result.latestCaptureRevision = captureRevision.load(std::memory_order_relaxed);
        result.lastAnalyzedCaptureRevision
            = lastAnalyzedCaptureRevision.load(std::memory_order_relaxed);
        result.emptyAnalysisRequestsAvoided
            = emptyAnalysisRequestsAvoided.load(std::memory_order_relaxed);
        result.staleFramesPublished = staleFramesPublished.load(std::memory_order_relaxed);
        result.peakRmsUserResets = peakRmsUserResets.load(std::memory_order_relaxed);
        return result;
    }

    StereoSampleCapture samples;
    StereoMeterAccumulator meters;
    HannSpectrumAnalyzer spectrum;
    VisualizationSnapshotExchange snapshots;
    std::array<float, fftSize> spectrumLeftScratch { };
    std::array<float, fftSize> spectrumRightScratch { };
    VisualizationFrame workingFrame;
    std::uint64_t newestCapturedFrameEnd = 0;
    std::uint64_t nextCoalescedInputSequence = 1;
    std::uint64_t nextMeterSequence = 1;
    std::uint64_t requiredUserResetEpoch = 0;
    std::uint64_t requiredLiveClearEpoch = 0;
    std::atomic<std::uint64_t> currentGeneration { 0 };
    std::atomic<std::uint64_t> captureRevision { 0 };
    std::atomic<std::uint64_t> lastAnalyzedCaptureRevision { 0 };
    std::atomic<bool> hasPublishedAudioFrame { false };
    std::atomic<bool> staleClearPending { false };
    std::atomic<std::uint64_t> peakRmsResetPendingEpoch { 0 };
    std::atomic<std::uint64_t> staleClearRevision { 0 };
    std::atomic<std::uint64_t> jobsStarted { 0 };
    std::atomic<std::uint64_t> jobsCompleted { 0 };
    std::atomic<std::uint64_t> jobsStopped { 0 };
    std::atomic<std::uint64_t> ignoredGenerationChunks { 0 };
    std::atomic<std::uint64_t> lastJobNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumJobNanoseconds { 0 };
    std::atomic<std::uint64_t> spectrumTransforms { 0 };
    std::atomic<std::uint64_t> lastJobSpectrumTransforms { 0 };
    std::atomic<std::uint64_t> maximumJobSpectrumTransforms { 0 };
    std::atomic<std::uint64_t> backlogDiscardedFrames { 0 };
    std::atomic<std::uint64_t> spectrumCapturedFrameEnd { 0 };
    std::atomic<std::uint64_t> meterCapturedFrameEnd { 0 };
    std::atomic<std::uint64_t> emptyAnalysisRequestsAvoided { 0 };
    std::atomic<std::uint64_t> staleFramesPublished { 0 };
    std::atomic<std::uint64_t> peakRmsUserResets { 0 };
};

AnalysisCoordinator::AnalysisCoordinator()
    : scheduler_(SharedAnalysisScheduler::acquire()), state_(std::make_shared<State>()),
      client_(scheduler_->createClient(state_))
{
}

AnalysisCoordinator::~AnalysisCoordinator()
{
    const std::lock_guard lifecycleLock(lifecycleMutex_);
    captureGeneration_.store(0, std::memory_order_release);

    if (client_ != nullptr)
        static_cast<void>(client_->cancelAndWait());

    client_.reset();
    state_.reset();
    scheduler_.reset();
}

void AnalysisCoordinator::captureAudioBlock(const float* const left, const float* const right,
    const std::size_t frameCount, const double sampleRate,
    const std::uint32_t channelCount) noexcept
{
    const auto generation = captureGeneration_.load(std::memory_order_acquire);
    if (generation == 0 || frameCount == 0)
        return;

    state_->captureAudioBlock(left, right, frameCount, sampleRate, generation, channelCount);
}

void AnalysisCoordinator::setCaptureFormat(
    const double sampleRate, const std::uint32_t channelCount) noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0
            || (channelCount != 1 && channelCount != 2)) {
            return;
        }

        const auto formatChanged = !hasConfiguredFormat_
            || sampleRatesDiffer(configuredSampleRate_, sampleRate)
            || configuredChannelCount_ != channelCount;
        if (!formatChanged)
            return;

        configuredSampleRate_ = sampleRate;
        configuredChannelCount_ = channelCount;
        hasConfiguredFormat_ = true;

        if (captureGeneration_.load(std::memory_order_acquire) == 0 || client_ == nullptr)
            return;

        static_cast<void>(restartActiveGenerationLocked(true));
    } catch (...) {
        captureGeneration_.store(0, std::memory_order_release);
    }
}

void AnalysisCoordinator::requestAnalysis() noexcept
{
    auto staleArmed = false;
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
        if (lifecycleTestHook_ != nullptr)
            lifecycleTestHook_(lifecycleTestHookContext_, LifecycleTestOperation::request);
#endif
        if (captureGeneration_.load(std::memory_order_acquire) == 0 || client_ == nullptr)
            return;

        const auto now
            = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
                  .count();
        const auto captureRevision = state_->latestCaptureRevision();
        if (captureRevision != lastObservedCaptureRevision_) {
            lastObservedCaptureRevision_ = captureRevision;
            lastObservedCaptureNanoseconds_ = now;
            staleClearRequested_ = false;
            state_->cancelStaleClear();
        } else if (lastObservedCaptureNanoseconds_ == 0) {
            lastObservedCaptureNanoseconds_ = now;
        }

        const auto coveredRevision
            = std::max(lastRequestedCaptureRevision_, state_->analyzedCaptureRevision());
        const auto hasNewCapture = captureRevision > coveredRevision;
        const auto staleClearIsDue = !staleClearRequested_ && state_->hasAudioFrame()
            && now - lastObservedCaptureNanoseconds_ >= staleInputTimeoutNanoseconds;

        if (!hasNewCapture && !staleClearIsDue) {
            state_->noteEmptyRequestAvoided();
            return;
        }

        auto due = nextAnalysisRequestNanoseconds_.load(std::memory_order_relaxed);

        if (due != 0 && now + requestDeadlineToleranceNanoseconds < due)
            return;

        const auto period = analysisRequestPeriod.count();
        const auto nextDue = due == 0 || now - due > period ? now + period : due + period;
        if (!nextAnalysisRequestNanoseconds_.compare_exchange_strong(
                due, nextDue, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }

        if (staleClearIsDue) {
            state_->armStaleClear(captureRevision);
            staleArmed = true;
        }

        if (!client_->request()) {
            if (staleArmed)
                state_->cancelStaleClear();
            return;
        }

        if (hasNewCapture)
            lastRequestedCaptureRevision_ = captureRevision;

        if (staleClearIsDue)
            staleClearRequested_ = true;
    } catch (...) {
        if (staleArmed && state_ != nullptr)
            state_->cancelStaleClear();

        // The renderer-facing contract is noexcept. Scheduler failures simply
        // leave the previous immutable frame on screen.
    }
}

void AnalysisCoordinator::setVisualizationActive(const bool shouldBeActive) noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
        if (lifecycleTestHook_ != nullptr) {
            lifecycleTestHook_(lifecycleTestHookContext_,
                shouldBeActive ? LifecycleTestOperation::activate
                               : LifecycleTestOperation::deactivate);
        }
#endif
        const auto isActive = captureGeneration_.load(std::memory_order_acquire) != 0;
        if (isActive == shouldBeActive || client_ == nullptr)
            return;

        if (!shouldBeActive) {
            captureGeneration_.store(0, std::memory_order_release);
            staleClearRequested_ = false;
            state_->cancelStaleClear();
            static_cast<void>(client_->cancelAndAdvanceGeneration());
            static_cast<void>(client_->waitUntilIdle());
            return;
        }

        static_cast<void>(restartActiveGenerationLocked(false));
    } catch (...) {
        captureGeneration_.store(0, std::memory_order_release);
    }
}

bool AnalysisCoordinator::restartActiveGenerationLocked(const bool discardPendingCapture)
{
    captureGeneration_.store(0, std::memory_order_release);
    staleClearRequested_ = false;
    state_->cancelStaleClear();

    const auto generation = client_->cancelAndAdvanceGeneration();
    if (!client_->waitUntilIdle())
        return false;

    if (discardPendingCapture)
        state_->discardPendingCapture();

    state_->beginGeneration(generation);
    const auto now
        = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
              .count();
    const auto captureRevision = state_->latestCaptureRevision();
    lastRequestedCaptureRevision_ = captureRevision;
    lastObservedCaptureRevision_ = captureRevision;
    lastObservedCaptureNanoseconds_ = now;
    staleClearRequested_ = false;
    nextAnalysisRequestNanoseconds_.store(0, std::memory_order_relaxed);
    captureGeneration_.store(generation, std::memory_order_release);
    return true;
}

void AnalysisCoordinator::resetPeakRms() noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        if (captureGeneration_.load(std::memory_order_acquire) == 0 || client_ == nullptr)
            return;

        state_->requestPeakRmsReset();
        static_cast<void>(client_->request());
    } catch (...) {
        // A reset is diagnostic/presentation state and must never affect audio.
    }
}

bool AnalysisCoordinator::copyLatestVisualizationFrame(
    VisualizationFrame& destination) const noexcept
{
    return state_->snapshots.copyLatest(destination);
}

bool AnalysisCoordinator::isVisualizationActive() const noexcept
{
    return captureGeneration_.load(std::memory_order_acquire) != 0;
}

AnalysisTelemetry AnalysisCoordinator::telemetry() const noexcept
{
    const auto schedulerCounters
        = client_ != nullptr ? client_->counters() : SharedAnalysisScheduler::Counters { };
    return state_->telemetry(schedulerCounters);
}

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
void AnalysisCoordinator::setLifecycleTestHook(
    void* const context, const LifecycleTestHook hook) noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        lifecycleTestHookContext_ = context;
        lifecycleTestHook_ = hook;
    } catch (...) {
    }
}
#endif
} // namespace audio_insight
