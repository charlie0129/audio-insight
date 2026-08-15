// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AnalysisCoordinator.h"

#include "SpectrogramColumnMapper.h"
#include "SpectrogramColumnQueue.h"
#include "SpectrumAnalyzer.h"
#include "StereoFieldAnalyzer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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

[[nodiscard]] bool hasIdenticalBits(const double left, const double right) noexcept
{
    return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
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

        // A consumer can retire every ready slot while the producer moves from
        // its free-slot scan to its reclaim scan. Retry a bounded number of
        // times so those newly freed slots are observed instead of dropping a
        // publication even though capacity is available.
        for (std::size_t attempt = 0; attempt <= slots_.size() && selected == slots_.size();
            ++attempt) {
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                auto expected = SlotState::free;
                if (slots_[index].state.compare_exchange_strong(expected, SlotState::writing,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    selected = index;
                    break;
                }
            }

            if (selected != slots_.size())
                break;

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

struct AnalysisCoordinator::State final : SharedAnalysisScheduler::JobClient,
                                          SpectrumTransformSink {
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

    void beginGeneration(
        const std::uint64_t captureGeneration, const std::uint64_t jobGeneration) noexcept
    {
        workingFrame = { };
        workingFrame.spectrumDecibels.fill(minimumSpectrumDecibels);
        workingFrame.generation = captureGeneration;
        newestCapturedFrameEnd = 0;
        nextCoalescedInputSequence = 1;
        spectrum.reset(&workingFrame);
        observeSpectrumReset(captureGeneration);
        workingFrame.spectrumSequence = nextSpectrumSequence++;
        stereoField.reset(&workingFrame);
        workingFrame.stereoCorrelation = 0.0F;
        workingFrame.stereoCorrelationValid = false;
        workingFrame.stereoSequence = nextStereoSequence++;
        mirrorStereoFrameState();
        spectrumCapturedFrameEnd.store(0, std::memory_order_relaxed);
        meterCapturedFrameEnd.store(0, std::memory_order_relaxed);
        hasPublishedAudioFrame.store(false, std::memory_order_relaxed);
        staleClearPending.store(false, std::memory_order_relaxed);
        spectrumClearPending.store(false, std::memory_order_relaxed);
        peakRmsResetPendingEpoch.store(0, std::memory_order_relaxed);
        requiredUserResetEpoch = 0;
        requiredLiveClearEpoch = 0;
        lastAnalyzedCaptureRevision.store(
            captureRevision.load(std::memory_order_acquire), std::memory_order_release);

        static_cast<void>(snapshots.publish(workingFrame));

        // Publish generations only after all worker-owned state and the initial
        // snapshot are complete. Defensive checks in execute() can therefore
        // never expose partially initialised state.
        currentCaptureGeneration.store(captureGeneration, std::memory_order_release);
        currentJobGeneration.store(jobGeneration, std::memory_order_release);
    }

    [[nodiscard]] bool reconfigureSpectrum(const SpectrumAnalysisConfiguration& configuration,
        const std::uint64_t fftGeneration, const std::uint64_t jobGeneration) noexcept
    {
        if (!spectrum.reconfigure(configuration, fftGeneration, &workingFrame))
            return false;

        observeSpectrumReset();

        workingFrame.generation = currentCaptureGeneration.load(std::memory_order_acquire);
        workingFrame.spectrumSequence = nextSpectrumSequence++;
        spectrumCapturedFrameEnd.store(0, std::memory_order_relaxed);
        configuredFftSize.store(
            static_cast<std::uint32_t>(configuration.fftSize), std::memory_order_relaxed);
        configuredFftWindow.store(
            static_cast<std::uint32_t>(configuration.window), std::memory_order_relaxed);
        requestedFftSliceRateHz.store(
            static_cast<std::uint32_t>(configuration.requestedSliceRateHz),
            std::memory_order_relaxed);
        currentFftGeneration.store(fftGeneration, std::memory_order_relaxed);
        fftConfigurationChanges.fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(snapshots.publish(workingFrame));

        currentJobGeneration.store(jobGeneration, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool reconfigureSpectrumTemporal(
        const SpectrumTemporalConfiguration& configuration,
        const std::uint64_t jobGeneration) noexcept
    {
        if (!spectrum.reconfigureTemporal(configuration, &workingFrame))
            return false;

        workingFrame.generation = currentCaptureGeneration.load(std::memory_order_acquire);
        workingFrame.spectrumSequence = nextSpectrumSequence++;
        spectrumCapturedFrameEnd.store(0, std::memory_order_relaxed);
        spectrumTemporalConfigurationChanges.fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(snapshots.publish(workingFrame));
        currentJobGeneration.store(jobGeneration, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool reconfigureSpectrogramMapping(const double frequencySpacing,
        const std::uint64_t mappingGeneration, const std::uint64_t jobGeneration) noexcept
    {
        if (!spectrogramMapper.setFrequencySpacing(frequencySpacing, mappingGeneration))
            return false;

        spectrogramColumns.discardPending();
        spectrogramCapturedFrameEnd.store(0, std::memory_order_relaxed);
        spectrogramRowCount.store(0, std::memory_order_relaxed);
        spectrogramMappingGeneration.store(mappingGeneration, std::memory_order_relaxed);
        spectrogramMappingChanges.fetch_add(1, std::memory_order_relaxed);

        mappingSeedWasPublished = false;
        mappingSeedPending = true;
        const auto emittedSeed = spectrum.emitLatestRawTransform(*this);
        mappingSeedPending = false;
        if (!emittedSeed || !mappingSeedWasPublished) {
            static_cast<void>(publishSpectrogramResetMarker(
                currentCaptureGeneration.load(std::memory_order_acquire)));
        }

        currentJobGeneration.store(jobGeneration, std::memory_order_release);
        return true;
    }

    void discardPendingCapture() noexcept
    {
        samples.discardPending();
        meters.discardPending();
    }

    void endGeneration() noexcept
    {
        currentCaptureGeneration.store(0, std::memory_order_release);
        spectrum.reset(nullptr);
        stereoField.reset(nullptr);
        stereoFieldValid.store(false, std::memory_order_relaxed);
        stereoCorrelationValid.store(false, std::memory_order_relaxed);
        stereoMono.store(false, std::memory_order_relaxed);
        observeSpectrumReset(0, false);
        spectrogramCapturedFrameEnd.store(0, std::memory_order_relaxed);
        spectrogramRowCount.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool copyNextSpectrogramColumn(SpectrogramColumn& destination) const noexcept
    {
        return spectrogramColumns.copyNext(destination);
    }

    void consumeSpectrumTransform(const SpectrumTransformView& transform) noexcept override
    {
        spectrogramTransformsOffered.fetch_add(1, std::memory_order_relaxed);

        auto observedResetFromTransform = false;
        if (transform.resetEpoch != observedSpectrumResetEpoch) {
            spectrogramColumns.discardPending();
            observedSpectrumResetEpoch = transform.resetEpoch;
            spectrogramCapturedFrameEnd.store(0, std::memory_order_relaxed);
            spectrogramRowCount.store(0, std::memory_order_relaxed);
            observedResetFromTransform = true;
        }

        auto columnSequence = nextSpectrogramColumnSequence++;
        if (columnSequence == 0)
            columnSequence = nextSpectrogramColumnSequence++;

        if (!spectrogramMapper.map(
                transform, columnSequence, mappingSeedPending, spectrogramColumnScratch)) {
            spectrogramMappingFailures.fetch_add(1, std::memory_order_relaxed);
            if (observedResetFromTransform)
                static_cast<void>(publishSpectrogramResetMarker(transform.captureGeneration));
            return;
        }

        spectrogramColumnsMapped.fetch_add(1, std::memory_order_relaxed);
        const auto publication = spectrogramColumns.publish(spectrogramColumnScratch);
        if (publication.published) {
            spectrogramCapturedFrameEnd.store(
                transform.capturedFrameEnd, std::memory_order_relaxed);
            spectrogramRowCount.store(spectrogramColumnScratch.rowCount, std::memory_order_relaxed);
            if (mappingSeedPending)
                mappingSeedWasPublished = true;
        } else if (observedResetFromTransform) {
            static_cast<void>(publishSpectrogramResetMarker(transform.captureGeneration));
        }
    }

    [[nodiscard]] bool publishSpectrogramResetMarker(const std::uint64_t captureGeneration) noexcept
    {
        if (captureGeneration == 0)
            return false;

        SpectrogramColumn marker;
        auto sequence = nextSpectrogramColumnSequence++;
        if (sequence == 0)
            sequence = nextSpectrogramColumnSequence++;

        marker.sequence = sequence;
        marker.captureGeneration = captureGeneration;
        marker.fftGeneration = spectrum.fftGeneration();
        marker.mappingGeneration = spectrogramMapper.mappingGeneration();
        marker.resetEpoch = spectrum.resetEpoch();
        marker.sampleRate = spectrum.sampleRate();
        marker.fftSize = static_cast<std::uint32_t>(spectrum.configuredFftSize());
        marker.binCount = static_cast<std::uint32_t>(spectrum.configuredBinCount());
        marker.hopSizeFrames = static_cast<std::uint32_t>(spectrum.hopSize());
        marker.requestedSliceRateHz
            = static_cast<std::uint32_t>(spectrum.configuration().requestedSliceRateHz);
        marker.resetMarker = true;
        return spectrogramColumns.publish(marker).published;
    }

    void observeSpectrumReset(
        const std::uint64_t captureGeneration, const bool publishMarker = true) noexcept
    {
        const auto epoch = spectrum.resetEpoch();
        if (epoch == observedSpectrumResetEpoch)
            return;

        spectrogramColumns.discardPending();
        observedSpectrumResetEpoch = epoch;
        spectrogramCapturedFrameEnd.store(0, std::memory_order_relaxed);
        spectrogramRowCount.store(0, std::memory_order_relaxed);
        if (publishMarker)
            static_cast<void>(publishSpectrogramResetMarker(captureGeneration));
    }

    void observeSpectrumReset() noexcept
    {
        observeSpectrumReset(currentCaptureGeneration.load(std::memory_order_acquire));
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

    void requestSpectrumClear() noexcept
    {
        spectrumClearPending.store(true, std::memory_order_release);
        spectrumUserClears.fetch_add(1, std::memory_order_relaxed);
    }

    void applyPeakRmsReading(const StereoMeterReading& reading) noexcept
    {
        if (workingFrame.spectrumValid
            && (workingFrame.channelCount != reading.channelCount
                || sampleRatesDiffer(workingFrame.sampleRate, reading.sampleRate))) {
            spectrum.reset(&workingFrame);
            observeSpectrumReset();
            workingFrame.spectrumSequence = nextSpectrumSequence++;
        }

        workingFrame.peakDecibels = reading.peakDecibels;
        workingFrame.rmsDecibels = reading.rmsDecibels;
        workingFrame.heldPeakDecibels = reading.heldPeakDecibels;
        workingFrame.over = reading.over;
        workingFrame.channelCount = reading.channelCount;
        workingFrame.meterValid = reading.valid;
        workingFrame.stereoCorrelation = reading.correlation;
        workingFrame.stereoCorrelationValid
            = reading.valid && reading.channelCount == 2 && reading.correlationValid;
        workingFrame.stereoMono = reading.valid && reading.channelCount == 1;

        if (reading.appliedLiveClearEpoch < requiredLiveClearEpoch) {
            workingFrame.peakDecibels.fill(minimumDisplayDecibels);
            workingFrame.rmsDecibels.fill(minimumDisplayDecibels);
            workingFrame.stereoCorrelation = 0.0F;
            workingFrame.stereoCorrelationValid = false;
        }

        if (reading.appliedUserResetEpoch < requiredUserResetEpoch) {
            workingFrame.heldPeakDecibels.fill(minimumDisplayDecibels);
            workingFrame.over.fill(false);
        }

        workingFrame.meterSequence = nextMeterSequence++;
    }

    void mirrorStereoFrameState() noexcept
    {
        stereoSequence.store(workingFrame.stereoSequence, std::memory_order_relaxed);
        stereoFieldValid.store(workingFrame.stereoFieldValid, std::memory_order_relaxed);
        stereoCorrelationValid.store(
            workingFrame.stereoCorrelationValid, std::memory_order_relaxed);
        stereoMono.store(workingFrame.stereoMono, std::memory_order_relaxed);
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

        const auto jobGeneration = context.generation();
        const auto captureGeneration = currentCaptureGeneration.load(std::memory_order_acquire);
        if (context.stopRequested() || captureGeneration == 0
            || jobGeneration != currentJobGeneration.load(std::memory_order_acquire)) {
            finish(true);
            return;
        }

        bool frameChanged = false;
        bool stereoStateChanged = false;
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
        const auto activeFftSize = spectrum.configuredFftSize();
        auto retentionCapacity = std::max(activeFftSize, StereoSampleCapture::framesPerSlot);
        auto retainedChunkCount = std::size_t { 0 };

        for (std::size_t consumed = 0; consumed < StereoSampleCapture::slotCount; ++consumed) {
            if (context.stopRequested()) {
                finish(true);
                return;
            }

            if (!samples.tryAcquireOldest(handle))
                break;

            const auto& chunk = handle.view();
            if (chunk.generation != captureGeneration) {
                ignoredGenerationChunks.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            static_cast<void>(stereoField.process(chunk));
            stereoStateChanged = true;

            ++retainedChunkCount;
            if (retainedChunkCount == 2) {
                // One ordinary capture slot must remain intact even for a 1024-
                // point FFT so hop cadence is not mistaken for backlog. Once a
                // second slot is pending, latest-wins analysis keeps only the
                // newest complete FFT window.
                retentionCapacity = activeFftSize;
                if (retainedFrames > retentionCapacity) {
                    const auto overflow = retainedFrames - retentionCapacity;
                    std::memmove(spectrumLeftScratch.data(),
                        spectrumLeftScratch.data() + static_cast<std::ptrdiff_t>(overflow),
                        retentionCapacity * sizeof(float));
                    std::memmove(spectrumRightScratch.data(),
                        spectrumRightScratch.data() + static_cast<std::ptrdiff_t>(overflow),
                        retentionCapacity * sizeof(float));
                    retainedFrames = retentionCapacity;
                    discardedFrames += overflow;
                    inputFollowsDiscontinuity = true;
                }
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
            if (chunkFrames > retentionCapacity) {
                discardedFrames += retainedFrames + (chunkFrames - retentionCapacity);
                chunkOffset = chunkFrames - retentionCapacity;
                chunkFrames = retentionCapacity;
                retainedFrames = 0;
                inputFollowsDiscontinuity = true;
            } else if (retainedFrames + chunkFrames > retentionCapacity) {
                const auto overflow = retainedFrames + chunkFrames - retentionCapacity;
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

        if (stereoStateChanged) {
            static_cast<void>(stereoField.writeFrame(workingFrame));
            newestCapturedFrameEnd
                = std::max(newestCapturedFrameEnd, workingFrame.stereoCapturedFrameEnd);
            frameChanged = true;
        }

        if (context.stopRequested()) {
            finish(true);
            return;
        }

        if (retainedFrames > 0) {
            const CapturedStereoChunkView coalescedInput {
                spectrumLeftScratch.data(),
                spectrumRightScratch.data(),
                retainedFrames,
                captureGeneration,
                nextCoalescedInputSequence++,
                retainedFrameEnd,
                retainedSampleRate,
                inputFollowsDiscontinuity,
                retainedChannelCount,
            };
            const auto spectrumWasValid = workingFrame.spectrumValid;
            const auto producedSpectrum = spectrum.process(coalescedInput, workingFrame, this);
            observeSpectrumReset();
            if (producedSpectrum) {
                workingFrame.spectrumSequence = nextSpectrumSequence++;
                newestCapturedFrameEnd
                    = std::max(newestCapturedFrameEnd, workingFrame.capturedFrameEnd);
                spectrumCapturedFrameEnd.store(
                    workingFrame.spectrumCapturedFrameEnd, std::memory_order_relaxed);
            } else if (spectrumWasValid && !workingFrame.spectrumValid) {
                workingFrame.spectrumSequence = nextSpectrumSequence++;
            }
            frameChanged = frameChanged || producedSpectrum
                || spectrumWasValid != workingFrame.spectrumValid;
        }

        StereoMeterReading meterReading;
        if (meters.consumeLatest(meterReading) && meterReading.generation == captureGeneration) {
            const auto meterGapIsNewerThanField = meterReading.rawCaptureDiscontinuity
                && meterReading.capturedFrameEnd > workingFrame.stereoCapturedFrameEnd;
            if (meterGapIsNewerThanField)
                stereoField.reset(&workingFrame);

            applyPeakRmsReading(meterReading);
            newestCapturedFrameEnd
                = std::max(newestCapturedFrameEnd, meterReading.capturedFrameEnd);
            workingFrame.sampleRate = meterReading.sampleRate;
            meterCapturedFrameEnd.store(meterReading.capturedFrameEnd, std::memory_order_relaxed);
            frameChanged = true;
            stereoStateChanged = true;
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

        if (spectrumClearPending.exchange(false, std::memory_order_acq_rel)) {
            spectrum.clearTemporalState(&workingFrame);
            workingFrame.spectrumSequence = nextSpectrumSequence++;
            spectrumCapturedFrameEnd.store(0, std::memory_order_relaxed);
            frameChanged = true;
        }

        if (context.stopRequested()
            || jobGeneration != currentJobGeneration.load(std::memory_order_acquire)
            || captureGeneration != currentCaptureGeneration.load(std::memory_order_acquire)) {
            finish(true);
            return;
        }

        const auto staleWasPending = staleClearPending.exchange(false, std::memory_order_acq_rel);
        const auto shouldClearStaleFrame = staleWasPending
            && staleClearRevision.load(std::memory_order_relaxed)
                == captureRevision.load(std::memory_order_acquire);

        if (shouldClearStaleFrame) {
            spectrum.reset(&workingFrame);
            observeSpectrumReset();
            stereoField.reset(&workingFrame);
            requiredLiveClearEpoch = std::max(requiredLiveClearEpoch, meters.requestLiveClear());
            workingFrame.peakDecibels.fill(minimumDisplayDecibels);
            workingFrame.rmsDecibels.fill(minimumDisplayDecibels);
            workingFrame.stereoCorrelation = 0.0F;
            workingFrame.stereoCorrelationValid = false;
            workingFrame.meterSequence = nextMeterSequence++;
            workingFrame.spectrumSequence = nextSpectrumSequence++;
            stereoStateChanged = true;
            hasPublishedAudioFrame.store(false, std::memory_order_release);
            staleFramesPublished.fetch_add(1, std::memory_order_relaxed);
            frameChanged = true;
        } else if (consumedValidAudio) {
            hasPublishedAudioFrame.store(true, std::memory_order_release);
        }

        if (frameChanged) {
            if (stereoStateChanged) {
                workingFrame.stereoSequence = nextStereoSequence++;
                mirrorStereoFrameState();
            }
            workingFrame.generation = captureGeneration;
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
        const auto stereoStatistics = stereoField.statistics();
        const auto correlationStatistics = meters.correlationTelemetry();
        result.stereoFieldProcessedChunks = stereoStatistics.processedChunks;
        result.stereoFieldProcessedFrames = stereoStatistics.processedFrames;
        result.stereoFieldSelectedPoints = stereoStatistics.selectedPoints;
        result.stereoFieldHistoryResets = stereoStatistics.historyResets;
        result.stereoFieldInvalidChunks = stereoStatistics.invalidChunks;
        result.stereoCorrelationProcessedSamples = correlationStatistics.processedSamples;
        result.stereoCorrelationPublishedEndpoints = correlationStatistics.publishedEndpoints;
        result.stereoCorrelationConsumedEndpoints = correlationStatistics.consumedEndpoints;
        result.stereoCorrelationStateResets = correlationStatistics.stateResets;
        result.stereoCapturedFrameEnd = stereoStatistics.capturedFrameEnd;
        result.stereoSequence = stereoSequence.load(std::memory_order_relaxed);
        result.latestCaptureRevision = captureRevision.load(std::memory_order_relaxed);
        result.lastAnalyzedCaptureRevision
            = lastAnalyzedCaptureRevision.load(std::memory_order_relaxed);
        result.emptyAnalysisRequestsAvoided
            = emptyAnalysisRequestsAvoided.load(std::memory_order_relaxed);
        result.staleFramesPublished = staleFramesPublished.load(std::memory_order_relaxed);
        result.peakRmsUserResets = peakRmsUserResets.load(std::memory_order_relaxed);
        result.spectrumUserClears = spectrumUserClears.load(std::memory_order_relaxed);
        result.fftConfigurationChanges = fftConfigurationChanges.load(std::memory_order_relaxed);
        result.spectrumTemporalConfigurationChanges
            = spectrumTemporalConfigurationChanges.load(std::memory_order_relaxed);
        const auto spectrogramQueueTelemetry = spectrogramColumns.telemetry();
        result.spectrogramTransformsOffered
            = spectrogramTransformsOffered.load(std::memory_order_relaxed);
        result.spectrogramColumnsMapped = spectrogramColumnsMapped.load(std::memory_order_relaxed);
        result.spectrogramMappingFailures
            = spectrogramMappingFailures.load(std::memory_order_relaxed);
        result.spectrogramColumnsPublished = spectrogramQueueTelemetry.publishedColumns;
        result.spectrogramColumnsReclaimed = spectrogramQueueTelemetry.reclaimedReadyColumns;
        result.spectrogramColumnsDropped = spectrogramQueueTelemetry.droppedIncomingColumns;
        result.spectrogramColumnsConsumed = spectrogramQueueTelemetry.consumedColumns;
        result.spectrogramColumnsDiscarded = spectrogramQueueTelemetry.discardedReadyColumns;
        result.spectrogramMappingChanges
            = spectrogramMappingChanges.load(std::memory_order_relaxed);
        result.spectrogramCapturedFrameEnd
            = spectrogramCapturedFrameEnd.load(std::memory_order_relaxed);
        result.spectrogramMappingGeneration
            = spectrogramMappingGeneration.load(std::memory_order_relaxed);
        result.fftGeneration = currentFftGeneration.load(std::memory_order_relaxed);
        result.configuredFftSize = configuredFftSize.load(std::memory_order_relaxed);
        result.configuredFftWindow = configuredFftWindow.load(std::memory_order_relaxed);
        result.requestedFftSliceRateHz = requestedFftSliceRateHz.load(std::memory_order_relaxed);
        result.spectrogramRowCount = spectrogramRowCount.load(std::memory_order_relaxed);
        result.spectrogramQueueReadyHighWaterMark = spectrogramQueueTelemetry.readyHighWaterMark;
        result.spectrogramQueueReadyColumns = spectrogramQueueTelemetry.readyColumns;
        result.stereoFieldPointCount = stereoStatistics.pointCount;
        result.stereoPointStrideFrames = stereoStatistics.pointStrideFrames;
        result.stereoFieldValid = stereoFieldValid.load(std::memory_order_relaxed);
        result.stereoCorrelationValid = stereoCorrelationValid.load(std::memory_order_relaxed);
        result.stereoMono = stereoMono.load(std::memory_order_relaxed);
        return result;
    }

    StereoSampleCapture samples;
    StereoMeterAccumulator meters;
    StereoFieldAnalyzer stereoField;
    SpectrumAnalyzer spectrum;
    SpectrogramColumnMapper spectrogramMapper;
    mutable SpectrogramColumnQueue spectrogramColumns;
    VisualizationSnapshotExchange snapshots;
    std::array<float, maximumFftSize> spectrumLeftScratch { };
    std::array<float, maximumFftSize> spectrumRightScratch { };
    VisualizationFrame workingFrame;
    SpectrogramColumn spectrogramColumnScratch;
    std::uint64_t newestCapturedFrameEnd = 0;
    std::uint64_t nextCoalescedInputSequence = 1;
    std::uint64_t nextSpectrumSequence = 1;
    std::uint64_t nextMeterSequence = 1;
    std::uint64_t nextStereoSequence = 1;
    std::uint64_t nextSpectrogramColumnSequence = 1;
    std::uint64_t observedSpectrumResetEpoch = 0;
    std::uint64_t requiredUserResetEpoch = 0;
    std::uint64_t requiredLiveClearEpoch = 0;
    bool mappingSeedPending = false;
    bool mappingSeedWasPublished = false;
    std::atomic<std::uint64_t> currentJobGeneration { 0 };
    std::atomic<std::uint64_t> currentCaptureGeneration { 0 };
    std::atomic<std::uint64_t> captureRevision { 0 };
    std::atomic<std::uint64_t> lastAnalyzedCaptureRevision { 0 };
    std::atomic<bool> hasPublishedAudioFrame { false };
    std::atomic<bool> staleClearPending { false };
    std::atomic<std::uint64_t> peakRmsResetPendingEpoch { 0 };
    std::atomic<bool> spectrumClearPending { false };
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
    std::atomic<std::uint64_t> stereoSequence { 0 };
    std::atomic<std::uint64_t> emptyAnalysisRequestsAvoided { 0 };
    std::atomic<std::uint64_t> staleFramesPublished { 0 };
    std::atomic<std::uint64_t> peakRmsUserResets { 0 };
    std::atomic<std::uint64_t> spectrumUserClears { 0 };
    std::atomic<std::uint64_t> fftConfigurationChanges { 0 };
    std::atomic<std::uint64_t> spectrumTemporalConfigurationChanges { 0 };
    std::atomic<std::uint64_t> spectrogramTransformsOffered { 0 };
    std::atomic<std::uint64_t> spectrogramColumnsMapped { 0 };
    std::atomic<std::uint64_t> spectrogramMappingFailures { 0 };
    std::atomic<std::uint64_t> spectrogramMappingChanges { 0 };
    std::atomic<std::uint64_t> spectrogramCapturedFrameEnd { 0 };
    std::atomic<std::uint64_t> spectrogramMappingGeneration { 1 };
    std::atomic<std::uint64_t> currentFftGeneration { 1 };
    std::atomic<std::uint32_t> configuredFftSize { static_cast<std::uint32_t>(fftSize) };
    std::atomic<std::uint32_t> configuredFftWindow { static_cast<std::uint32_t>(
        FftWindow::periodicHann) };
    std::atomic<std::uint32_t> requestedFftSliceRateHz { 60 };
    std::atomic<std::uint32_t> spectrogramRowCount { 0 };
    std::atomic<bool> stereoFieldValid { false };
    std::atomic<bool> stereoCorrelationValid { false };
    std::atomic<bool> stereoMono { false };
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

void AnalysisCoordinator::setSpectrumAnalysisConfiguration(
    SpectrumAnalysisConfiguration configuration) noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        if (client_ == nullptr || !SpectrumAnalyzer::isSupportedConfiguration(configuration)
            || configuration == spectrumConfiguration_) {
            return;
        }

        auto jobGeneration = client_->generation();
        const auto isActive = captureGeneration_.load(std::memory_order_acquire) != 0;
        if (isActive) {
            jobGeneration = client_->cancelAndAdvanceGeneration();
            if (!client_->waitUntilIdle())
                return;
        }

        const auto fftGeneration = nextNonzeroGeneration(fftGenerationCounter_);
        if (!state_->reconfigureSpectrum(configuration, fftGeneration, jobGeneration))
            return;

        spectrumConfiguration_ = configuration;
        const auto serviceRate = std::max(60, configuration.requestedSliceRateHz);
        analysisRequestPeriodNanoseconds_
            = std::max<std::int64_t>(1, std::llround(1'000'000'000.0 / serviceRate));
        nextAnalysisRequestNanoseconds_.store(0, std::memory_order_relaxed);

        // Peak/RMS is producer-owned and keeps advancing throughout this
        // worker-only FFT boundary. One immediate job catches up its coalesced
        // endpoint and begins the new FFT overlap without waiting for a later
        // display callback.
        if (isActive)
            static_cast<void>(client_->request());
    } catch (...) {
        // A presentation-analysis setting must never affect transparent audio.
    }
}

void AnalysisCoordinator::setSpectrumTemporalConfiguration(
    SpectrumTemporalConfiguration configuration) noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        if (client_ == nullptr || !SpectrumAnalyzer::isSupportedTemporalConfiguration(configuration)
            || configuration == spectrumTemporalConfiguration_) {
            return;
        }

        auto jobGeneration = client_->generation();
        const auto isActive = captureGeneration_.load(std::memory_order_acquire) != 0;
        if (isActive) {
            jobGeneration = client_->cancelAndAdvanceGeneration();
            if (!client_->waitUntilIdle())
                return;
        }

        if (!state_->reconfigureSpectrumTemporal(configuration, jobGeneration))
            return;

        spectrumTemporalConfiguration_ = configuration;
        nextAnalysisRequestNanoseconds_.store(0, std::memory_order_relaxed);

        if (isActive)
            static_cast<void>(client_->request());
    } catch (...) {
        // Analyzer presentation state must never affect transparent audio.
    }
}

void AnalysisCoordinator::setSpectrogramFrequencySpacing(const double spacing) noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        if (client_ == nullptr || !std::isfinite(spacing) || spacing < 0.0 || spacing > 1.0
            || hasIdenticalBits(spacing, spectrogramFrequencySpacing_)) {
            return;
        }

        auto jobGeneration = client_->generation();
        const auto isActive = captureGeneration_.load(std::memory_order_acquire) != 0;
        if (isActive) {
            jobGeneration = client_->cancelAndAdvanceGeneration();
            if (!client_->waitUntilIdle())
                return;
        }

        const auto mappingGeneration = nextNonzeroGeneration(spectrogramMappingGenerationCounter_);
        if (!state_->reconfigureSpectrogramMapping(spacing, mappingGeneration, jobGeneration))
            return;

        spectrogramFrequencySpacing_ = spacing;
        nextAnalysisRequestNanoseconds_.store(0, std::memory_order_relaxed);

        if (isActive)
            static_cast<void>(client_->request());
    } catch (...) {
        // A presentation mapping must never affect transparent audio.
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

        const auto period = analysisRequestPeriodNanoseconds_;
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
            state_->endGeneration();
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

    const auto jobGeneration = client_->cancelAndAdvanceGeneration();
    if (!client_->waitUntilIdle())
        return false;

    if (discardPendingCapture)
        state_->discardPendingCapture();

    const auto captureGeneration = nextNonzeroGeneration(captureGenerationCounter_);
    state_->beginGeneration(captureGeneration, jobGeneration);
    const auto now
        = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
              .count();
    const auto captureRevision = state_->latestCaptureRevision();
    lastRequestedCaptureRevision_ = captureRevision;
    lastObservedCaptureRevision_ = captureRevision;
    lastObservedCaptureNanoseconds_ = now;
    staleClearRequested_ = false;
    nextAnalysisRequestNanoseconds_.store(0, std::memory_order_relaxed);
    captureGeneration_.store(captureGeneration, std::memory_order_release);
    return true;
}

std::uint64_t AnalysisCoordinator::nextNonzeroGeneration(std::uint64_t& counter) noexcept
{
    ++counter;
    if (counter == 0)
        ++counter;
    return counter;
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

void AnalysisCoordinator::resetSpectrum() noexcept
{
    try {
        const std::lock_guard lifecycleLock(lifecycleMutex_);
        if (captureGeneration_.load(std::memory_order_acquire) == 0 || client_ == nullptr)
            return;

        state_->requestSpectrumClear();
        static_cast<void>(client_->request());
    } catch (...) {
        // A reset is transient presentation state and must never affect audio.
    }
}

bool AnalysisCoordinator::copyLatestVisualizationFrame(
    VisualizationFrame& destination) const noexcept
{
    return state_->snapshots.copyLatest(destination);
}

bool AnalysisCoordinator::copyNextSpectrogramColumn(SpectrogramColumn& destination) const noexcept
{
    return state_->copyNextSpectrogramColumn(destination);
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

void AnalysisCoordinator::skipNextMeterEndpointSequenceForTesting() noexcept
{
    state_->meters.skipNextEndpointSequenceForTesting();
}
#endif
} // namespace audio_insight
