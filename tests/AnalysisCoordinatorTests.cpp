// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/AnalysisCoordinator.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numbers>
#include <span>
#include <thread>

namespace audio_insight {
namespace {
using namespace std::chrono_literals;
constexpr auto ordinaryCaptureWindowFrames = std::size_t { 2048 };

void drainSpectrogramColumns(
    AnalysisCoordinator& coordinator, std::uint64_t& captureBoundaryGeneration)
{
    SpectrogramColumn column;
    while (coordinator.copyNextSpectrogramColumn(column)) {
        if (column.captureBoundary && column.resetMarker)
            captureBoundaryGeneration = column.captureGeneration;
    }
}

void drainRendererState(AnalysisCoordinator& coordinator)
{
    VisualizationFrame frame;
    while (coordinator.copyLatestVisualizationFrame(frame)) { }

    SpectrogramColumn column;
    while (coordinator.copyNextSpectrogramColumn(column)) { }
}

[[nodiscard]] bool isFullyInvalidCaptureBoundary(const VisualizationFrame& frame) noexcept
{
    return frame.captureBoundary && !frame.spectrumValid && !frame.meterValid
        && !frame.stereoFieldValid && !frame.stereoCorrelationValid && !frame.loudnessMomentaryValid
        && !frame.loudnessShortTermValid && !frame.loudnessIntegratedValid;
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }

    return predicate();
}

bool waitForCaptureBoundaryPublication(AnalysisCoordinator& coordinator,
    const std::uint64_t previousGeneration, const std::uint64_t previousPublishedFrames,
    std::uint64_t& boundaryGeneration)
{
    return waitUntil([&] {
        coordinator.requestAnalysis();
        const auto telemetry = coordinator.telemetry();
        if (telemetry.captureGeneration <= previousGeneration
            || telemetry.publishedFrames <= previousPublishedFrames) {
            return false;
        }

        boundaryGeneration = telemetry.captureGeneration;
        return true;
    });
}

bool copyCaptureBoundaryFrame(AnalysisCoordinator& coordinator,
    const std::uint64_t expectedGeneration, VisualizationFrame& destination)
{
    return waitUntil([&] {
        VisualizationFrame candidate;
        if (!coordinator.copyLatestVisualizationFrame(candidate))
            return false;
        if (!candidate.captureBoundary || candidate.generation != expectedGeneration)
            return false;
        destination = candidate;
        return true;
    });
}

bool copyCaptureBoundaryMarker(AnalysisCoordinator& coordinator,
    const std::uint64_t expectedGeneration, SpectrogramColumn& destination)
{
    return waitUntil([&] {
        SpectrogramColumn candidate;
        if (!coordinator.copyNextSpectrogramColumn(candidate))
            return false;
        if (!candidate.captureBoundary || !candidate.resetMarker
            || candidate.captureGeneration != expectedGeneration) {
            return false;
        }
        destination = candidate;
        return true;
    });
}

bool waitForCaptureBoundaryRecovery(AnalysisCoordinator& coordinator,
    const std::uint64_t expectedGeneration, VisualizationFrame& destination)
{
    return waitUntil([&] {
        coordinator.requestAnalysis();
        VisualizationFrame candidate;
        if (!coordinator.copyLatestVisualizationFrame(candidate))
            return false;
        if (candidate.captureBoundary || candidate.generation != expectedGeneration
            || (!candidate.meterValid && !candidate.stereoFieldValid)) {
            return false;
        }
        destination = candidate;
        return true;
    });
}

template <std::size_t FrameCount>
void captureRepeated(AnalysisCoordinator& coordinator, const std::array<float, FrameCount>& left,
    const std::array<float, FrameCount>& right, const std::size_t count,
    const double sampleRate = 48'000.0)
{
    for (std::size_t index = 0; index < count; ++index)
        coordinator.captureAudioBlock(left.data(), right.data(), left.size(), sampleRate, 2);
}

template <std::size_t FrameCount> [[nodiscard]] consteval std::size_t captureCallsToFillRawQueue()
{
    static_assert(StereoSampleCapture::bufferedFrameCapacity % FrameCount == 0);
    return StereoSampleCapture::bufferedFrameCapacity / FrameCount;
}

template <std::size_t FrameCount>
[[nodiscard]] consteval std::size_t captureCallsThroughFirstPackedOverflow()
{
    static_assert(StereoSampleCapture::framesPerSlot % FrameCount == 0);
    return captureCallsToFillRawQueue<FrameCount>()
        + StereoSampleCapture::framesPerSlot / FrameCount;
}

template <typename Predicate>
bool waitForFrame(AnalysisCoordinator& coordinator, VisualizationFrame& frame,
    Predicate&& predicate, const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto deliveredCaptureBoundaryGeneration = std::uint64_t { 0 };
    while (std::chrono::steady_clock::now() < deadline) {
        coordinator.requestAnalysis();
        drainSpectrogramColumns(coordinator, deliveredCaptureBoundaryGeneration);
        if (coordinator.copyLatestVisualizationFrame(frame)) {
            while (frame.captureBoundary && deliveredCaptureBoundaryGeneration != frame.generation
                && std::chrono::steady_clock::now() < deadline) {
                drainSpectrogramColumns(coordinator, deliveredCaptureBoundaryGeneration);
                if (deliveredCaptureBoundaryGeneration != frame.generation)
                    std::this_thread::sleep_for(1ms);
            }
            if (frame.captureBoundary && deliveredCaptureBoundaryGeneration != frame.generation)
                return false;
            if (predicate(frame))
                return true;
        }

        std::this_thread::sleep_for(2ms);
    }

    return false;
}

template <typename Predicate>
bool waitForFrameWithoutRequest(AnalysisCoordinator& coordinator, VisualizationFrame& frame,
    Predicate&& predicate, const std::chrono::milliseconds timeout = 500ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto deliveredCaptureBoundaryGeneration = std::uint64_t { 0 };
    while (std::chrono::steady_clock::now() < deadline) {
        drainSpectrogramColumns(coordinator, deliveredCaptureBoundaryGeneration);
        if (coordinator.copyLatestVisualizationFrame(frame)) {
            while (frame.captureBoundary && deliveredCaptureBoundaryGeneration != frame.generation
                && std::chrono::steady_clock::now() < deadline) {
                drainSpectrogramColumns(coordinator, deliveredCaptureBoundaryGeneration);
                if (deliveredCaptureBoundaryGeneration != frame.generation)
                    std::this_thread::sleep_for(1ms);
            }
            if (frame.captureBoundary && deliveredCaptureBoundaryGeneration != frame.generation)
                return false;
            if (predicate(frame))
                return true;
        }

        std::this_thread::sleep_for(1ms);
    }

    return false;
}

[[nodiscard]] bool isDisplayFloor(const float value) noexcept
{
    return std::abs(value - minimumDisplayDecibels) < 0.0001F;
}

constexpr double loudnessTestSampleRate = 48'000.0;
constexpr std::size_t loudnessMeasurementPeriodFrames = 4'800;
constexpr std::size_t loudnessPeriodsPerPackedBatch = 4;
static_assert((loudnessMeasurementPeriodFrames * loudnessPeriodsPerPackedBatch)
        % StereoSampleCapture::framesPerSlot
    == 0);

void fillLoudnessTone(
    std::span<float> signal, const double peakDecibels, std::uint64_t& toneFrameCursor)
{
    const auto amplitude = std::pow(10.0, peakDecibels / 20.0);
    for (std::size_t sample = 0; sample < signal.size(); ++sample) {
        const auto phase = 2.0 * std::numbers::pi * 1'000.0
            * static_cast<double>(toneFrameCursor + sample) / loudnessTestSampleRate;
        signal[sample] = static_cast<float>(amplitude * std::sin(phase));
    }
    toneFrameCursor += signal.size();
}

bool feedLoudnessTonePeriods(AnalysisCoordinator& coordinator, const std::size_t periodCount,
    const double peakDecibels, std::uint64_t& toneFrameCursor, VisualizationFrame& frame,
    const std::uint32_t channelCount = 2)
{
    if (periodCount == 0 || periodCount % loudnessPeriodsPerPackedBatch != 0)
        return false;

    std::array<float, loudnessMeasurementPeriodFrames> signal { };

    for (std::size_t period = 0; period < periodCount; ++period) {
        fillLoudnessTone(signal, peakDecibels, toneFrameCursor);
        coordinator.captureAudioBlock(signal.data(), channelCount == 2 ? signal.data() : nullptr,
            signal.size(), loudnessTestSampleRate, channelCount);

        if ((period + 1) % loudnessPeriodsPerPackedBatch != 0)
            continue;

        const auto expectedCapturedFrameEnd = coordinator.telemetry().capture.capturedFrames;
        if (!waitForFrame(coordinator, frame, [expectedCapturedFrameEnd](const auto& candidate) {
                return candidate.loudnessMeasurementCapturedFrameEnd >= expectedCapturedFrameEnd;
            })) {
            return false;
        }
    }

    return true;
}

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
struct LifecycleHookBarrier {
    explicit LifecycleHookBarrier(
        const AnalysisCoordinator::LifecycleTestOperation operationToBlock)
        : operation(operationToBlock)
    {
    }

    static void invoke(
        void* const context, const AnalysisCoordinator::LifecycleTestOperation operation) noexcept
    {
        auto& barrier = *static_cast<LifecycleHookBarrier*>(context);
        if (operation != barrier.operation)
            return;

        try {
            std::unique_lock lock(barrier.mutex);
            barrier.entered = true;
            barrier.condition.notify_all();
            barrier.condition.wait(lock, [&barrier] { return barrier.released; });
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [this] { return entered; });
    }

    void release()
    {
        const std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }

    AnalysisCoordinator::LifecycleTestOperation operation;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

struct WorkerHookBarrier {
    explicit WorkerHookBarrier(const AnalysisCoordinator::WorkerTestOperation operationToBlock)
        : operation(operationToBlock)
    {
    }

    static void invoke(
        void* const context, const AnalysisCoordinator::WorkerTestOperation operation) noexcept
    {
        auto& barrier = *static_cast<WorkerHookBarrier*>(context);
        if (operation != barrier.operation
            || barrier.wasEntered.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        try {
            std::unique_lock lock(barrier.mutex);
            barrier.entered = true;
            barrier.condition.notify_all();
            barrier.condition.wait(lock, [&barrier] { return barrier.released; });
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [this] { return entered; });
    }

    void release()
    {
        const std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }

    AnalysisCoordinator::WorkerTestOperation operation;
    std::atomic<bool> wasEntered { false };
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

struct WorkerPublicationBarrier {
    static void invoke(
        void* const context, const AnalysisCoordinator::WorkerTestOperation operation) noexcept
    {
        auto& barrier = *static_cast<WorkerPublicationBarrier*>(context);

        try {
            std::unique_lock lock(barrier.mutex);
            if (operation == AnalysisCoordinator::WorkerTestOperation::beforeFramePublication) {
                if (barrier.beforeEntered)
                    return;
                barrier.beforeEntered = true;
                barrier.condition.notify_all();
                barrier.condition.wait(lock, [&barrier] { return barrier.beforeReleased; });
                return;
            }

            if (operation == AnalysisCoordinator::WorkerTestOperation::afterFramePublication
                && barrier.beforeEntered && !barrier.afterEntered) {
                barrier.afterEntered = true;
                barrier.condition.notify_all();
                barrier.condition.wait(lock, [&barrier] { return barrier.afterReleased; });
            }
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilBeforeEntered(const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [this] { return beforeEntered; });
    }

    void releaseBefore()
    {
        const std::lock_guard lock(mutex);
        beforeReleased = true;
        condition.notify_all();
    }

    [[nodiscard]] bool waitUntilAfterEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return afterEntered; });
    }

    void releaseAfter()
    {
        const std::lock_guard lock(mutex);
        afterReleased = true;
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool beforeEntered = false;
    bool beforeReleased = false;
    bool afterEntered = false;
    bool afterReleased = false;
};
#endif

class AnalysisCoordinatorTests final : public juce::UnitTest {
public:
    AnalysisCoordinatorTests() : UnitTest("Analysis coordinator", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Inactive instances do not capture or schedule work");
        {
            AnalysisCoordinator coordinator;
            constexpr std::array<float, 16> samples { };

            coordinator.captureAudioBlock(samples.data(), samples.data(), samples.size(), 48'000.0);
            coordinator.requestAnalysis();

            const auto telemetry = coordinator.telemetry();
            expect(!coordinator.isVisualizationActive());
            expect(telemetry.capture.attemptedChunks == 0);
            expect(telemetry.meters.attemptedBlocks == 0);
            expect(telemetry.scheduler.submitted == 0);
            expectWithinAbsoluteError(telemetry.configuredSpectrumAttackMilliseconds, 0.0, 0.0);
            expectWithinAbsoluteError(telemetry.configuredSpectrumReleaseMilliseconds, 250.0, 0.0);
        }

        beginTest("Visible analysis publishes a spectrum and honest stereo meters");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);
            expect(coordinator.isVisualizationActive());

            std::array<float, fftSize> left { };
            std::array<float, fftSize> right { };
            constexpr std::size_t sineBin = 96;

            for (std::size_t frame = 0; frame < left.size(); ++frame) {
                left[frame] = 0.5F
                    * std::sin(
                        static_cast<float>(2.0 * std::numbers::pi * static_cast<double>(sineBin)
                            * static_cast<double>(frame) / static_cast<double>(fftSize)));
                right[frame] = left[frame] * 0.5F;
            }

            coordinator.captureAudioBlock(left.data(), right.data(), left.size(), 48'000.0);

            VisualizationFrame frame;
            const auto receivedSpectrum = waitForFrame(
                coordinator, frame, [](const auto& candidate) { return candidate.spectrumValid; });

            expect(receivedSpectrum, "The shared worker did not publish a spectrum");
            expect(frame.generation != 0);
            expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], -6.0206F, 0.15F);
            expectWithinAbsoluteError(frame.peakDecibels[0], -6.0206F, 0.02F);
            expectWithinAbsoluteError(frame.peakDecibels[1], -12.0412F, 0.02F);
            const auto representedSeconds = static_cast<double>(fftSize) / 48'000.0;
            const auto rmsIntegration = std::sqrt(1.0 - std::exp(-representedSeconds / 0.300));
            expectWithinAbsoluteError(frame.rmsDecibels[0],
                static_cast<float>(20.0 * std::log10((0.5 / std::sqrt(2.0)) * rmsIntegration)),
                0.02F);
            expectWithinAbsoluteError(frame.rmsDecibels[1],
                static_cast<float>(20.0 * std::log10((0.25 / std::sqrt(2.0)) * rmsIntegration)),
                0.02F);
            expect(frame.meterValid);
            expect(frame.meterSequence != 0);
            expectWithinAbsoluteError(frame.heldPeakDecibels[0], -6.0206F, 0.02F);
            expectWithinAbsoluteError(frame.heldPeakDecibels[1], -12.0412F, 0.02F);
            expect(!frame.over[0] && !frame.over[1]);
            expect(frame.stereoFieldValid);
            expect(!frame.stereoMono);
            expect(frame.stereoFieldPointCount > 0);
            expect(frame.stereoFieldPointCount <= maximumStereoFieldPointCount);
            expect(frame.stereoSequence != 0);
            expect(frame.stereoCorrelationValid);
            expectWithinAbsoluteError(frame.stereoCorrelation, 1.0F, 0.0001F);

            const auto activeTelemetry = coordinator.telemetry();
            expect(activeTelemetry.capture.attemptedChunks
                == fftSize / StereoSampleCapture::framesPerSlot);
            expect(activeTelemetry.meters.attemptedBlocks == 1);
            expect(activeTelemetry.jobsCompleted >= 1);
            expect(activeTelemetry.spectrumCapturedFrameEnd == fftSize);
            expect(activeTelemetry.meterCapturedFrameEnd == fftSize);
            expect(activeTelemetry.captureGeneration == frame.generation);
            expectWithinAbsoluteError(activeTelemetry.captureSampleRate, 48'000.0, 0.0);
            expect(activeTelemetry.spectrumFreshnessValid);
            expect(activeTelemetry.peakRmsFreshnessValid);
            expect(activeTelemetry.spectrumFreshnessFrames == 0);
            expect(activeTelemetry.peakRmsFreshnessFrames == 0);
            expect(activeTelemetry.spectrumFreshnessNanoseconds == 0);
            expect(activeTelemetry.peakRmsFreshnessNanoseconds == 0);
            expect(activeTelemetry.stereoCapturedFrameEnd == fftSize);
            expect(activeTelemetry.stereoFieldProcessedChunks
                == fftSize / StereoSampleCapture::framesPerSlot);
            expect(activeTelemetry.stereoFieldProcessedFrames == fftSize);
            expect(activeTelemetry.stereoFieldSelectedPoints == frame.stereoFieldPointCount);
            expect(activeTelemetry.stereoCorrelationProcessedSamples == fftSize);
            expect(activeTelemetry.stereoCorrelationPublishedEndpoints == 1);
            expect(activeTelemetry.stereoCorrelationConsumedEndpoints >= 1);
            expect(activeTelemetry.stereoSequence == frame.stereoSequence);
            expect(activeTelemetry.stereoFieldPointCount == frame.stereoFieldPointCount);
            expect(activeTelemetry.stereoFieldValid);
            expect(activeTelemetry.stereoCorrelationValid);
            expect(!activeTelemetry.stereoMono);
            expect(activeTelemetry.latestCaptureRevision == 1);
            expect(activeTelemetry.lastAnalyzedCaptureRevision == 1);

            const auto submittedBeforeEmptyRequests = activeTelemetry.scheduler.submitted;
            for (std::size_t request = 0; request < 32; ++request)
                coordinator.requestAnalysis();

            const auto afterEmptyRequests = coordinator.telemetry();
            expect(afterEmptyRequests.scheduler.submitted == submittedBeforeEmptyRequests);
            expect(afterEmptyRequests.emptyAnalysisRequestsAvoided
                >= activeTelemetry.emptyAnalysisRequestsAvoided + 32);

            constexpr std::array<float, 480> pendingSamples { };
            coordinator.captureAudioBlock(
                pendingSamples.data(), pendingSamples.data(), pendingSamples.size(), 48'000.0);
            const auto laggingTelemetry = coordinator.telemetry();
            expect(laggingTelemetry.spectrumFreshnessValid);
            expect(laggingTelemetry.peakRmsFreshnessValid);
            expect(laggingTelemetry.spectrumFreshnessFrames == pendingSamples.size());
            expect(laggingTelemetry.peakRmsFreshnessFrames == pendingSamples.size());
            expect(laggingTelemetry.spectrumFreshnessNanoseconds == 10'000'000);
            expect(laggingTelemetry.peakRmsFreshnessNanoseconds == 10'000'000);

            coordinator.setVisualizationActive(false);
            const auto beforeClosedCapture = coordinator.telemetry();
            expect(beforeClosedCapture.captureGeneration == 0);
            expect(beforeClosedCapture.captureSampleRate == 0.0);
            expect(!beforeClosedCapture.spectrumFreshnessValid);
            expect(!beforeClosedCapture.peakRmsFreshnessValid);
            coordinator.captureAudioBlock(left.data(), right.data(), left.size(), 48'000.0);
            coordinator.requestAnalysis();
            const auto afterClosedCapture = coordinator.telemetry();

            expect(!coordinator.isVisualizationActive());
            expect(afterClosedCapture.capture.attemptedChunks
                == beforeClosedCapture.capture.attemptedChunks);
            expect(afterClosedCapture.meters.attemptedBlocks
                == beforeClosedCapture.meters.attemptedBlocks);
            expect(
                afterClosedCapture.scheduler.submitted == beforeClosedCapture.scheduler.submitted);
        }

        const auto runLoudnessTests = [this] {
            beginTest("Coordinator publishes standards-based M, S, and I Loudness snapshots");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame frame;
                expect(feedLoudnessTonePeriods(coordinator, 32, -23.0, toneFrameCursor, frame));
                expect(frame.loudnessMomentaryValid);
                expect(frame.loudnessShortTermValid);
                expect(frame.loudnessIntegratedValid);
                expectWithinAbsoluteError(frame.loudnessMomentaryLufs, -23.0, 0.1);
                expectWithinAbsoluteError(frame.loudnessShortTermLufs, -23.0, 0.1);
                expectWithinAbsoluteError(frame.loudnessIntegratedLufs, -23.0, 0.1);
                expect(frame.loudnessMeasurementCapturedFrameEnd == toneFrameCursor);
                expect(frame.loudnessIntegratedCapturedFrameEnd == toneFrameCursor);
                expect(frame.loudnessSequence != 0);

                const auto telemetry = coordinator.telemetry();
                expect(telemetry.loudness.inputFrames == toneFrameCursor);
                expect(telemetry.loudness.measurementCompletions == 32);
                expect(telemetry.loudness.integrationBlockCompletions == 29);
                expect(telemetry.loudnessMeasurement.momentaryValid);
                expect(telemetry.loudnessMeasurement.shortTermValid);
                expect(telemetry.loudnessMeasurement.integratedValid);
            }

            beginTest("Loudness reset publishes without new audio and preserves M/S");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame beforeReset;
                expect(
                    feedLoudnessTonePeriods(coordinator, 32, -18.0, toneFrameCursor, beforeReset));
                expect(beforeReset.loudnessMomentaryValid && beforeReset.loudnessShortTermValid
                    && beforeReset.loudnessIntegratedValid);

                coordinator.resetLoudness();
                VisualizationFrame afterReset;
                expect(waitForFrameWithoutRequest(
                    coordinator, afterReset,
                    [sequence = beforeReset.loudnessSequence](const auto& candidate) {
                        return candidate.loudnessSequence > sequence
                            && !candidate.loudnessIntegratedValid;
                    },
                    2s));
                expect(afterReset.loudnessMomentaryValid);
                expect(afterReset.loudnessShortTermValid);
                expectWithinAbsoluteError(
                    afterReset.loudnessMomentaryLufs, beforeReset.loudnessMomentaryLufs, 0.0);
                expectWithinAbsoluteError(
                    afterReset.loudnessShortTermLufs, beforeReset.loudnessShortTermLufs, 0.0);
                expect(afterReset.loudnessMeasurementCapturedFrameEnd
                    == beforeReset.loudnessMeasurementCapturedFrameEnd);
                expect(afterReset.loudnessIntegratedCapturedFrameEnd == 0);
                expect(coordinator.telemetry().loudness.integrationResets == 1);
            }

            beginTest("Queued pre-reset samples cannot enter a new Integrated interval");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame frame;
                expect(feedLoudnessTonePeriods(coordinator, 32, -10.0, toneFrameCursor, frame));
                expect(frame.loudnessIntegratedValid);

                // End on a packed-capture boundary so every pre-reset sample is
                // publishable before the explicit Loudness boundary is taken.
                std::array<float, loudnessMeasurementPeriodFrames * 4> queuedBeforeReset { };
                fillLoudnessTone(queuedBeforeReset, -10.0, toneFrameCursor);
                coordinator.captureAudioBlock(queuedBeforeReset.data(), queuedBeforeReset.data(),
                    queuedBeforeReset.size(), loudnessTestSampleRate, 2);
                const auto resetBoundary = coordinator.telemetry().capture.capturedFrames;
                coordinator.resetLoudness();

                VisualizationFrame resetFrame;
                expect(waitForFrameWithoutRequest(
                    coordinator, resetFrame,
                    [resetBoundary](const auto& candidate) {
                        return !candidate.loudnessIntegratedValid
                            && candidate.loudnessMeasurementCapturedFrameEnd >= resetBoundary;
                    },
                    2s));

                std::array<float, loudnessMeasurementPeriodFrames * 4> afterReset { };
                fillLoudnessTone(afterReset, -30.0, toneFrameCursor);
                const auto expectedIntegratedEndpoint
                    = coordinator.telemetry().capture.capturedFrames + afterReset.size();
                coordinator.captureAudioBlock(afterReset.data(), afterReset.data(),
                    afterReset.size(), loudnessTestSampleRate, 2);

                VisualizationFrame integrated;
                expect(waitForFrame(
                    coordinator, integrated, [expectedIntegratedEndpoint](const auto& candidate) {
                        return candidate.loudnessIntegratedValid
                            && candidate.loudnessIntegratedCapturedFrameEnd
                            >= expectedIntegratedEndpoint;
                    }));
                expectWithinAbsoluteError(integrated.loudnessIntegratedLufs, -30.0, 0.15);
                expect(coordinator.telemetry().loudnessMeasurement.integrationBlockCount == 1);
            }

            beginTest("Loudness RESET is sample-exact inside a packed capture chunk");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame beforeReset;
                expect(
                    feedLoudnessTonePeriods(coordinator, 4, -18.0, toneFrameCursor, beforeReset));
                expect(beforeReset.loudnessIntegratedValid);

                const auto telemetryBeforeReset = coordinator.telemetry();
                constexpr auto partialFrameCount = StereoSampleCapture::framesPerSlot / 2;
                std::array<float, partialFrameCount> beforeBoundary { };
                fillLoudnessTone(beforeBoundary, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(beforeBoundary.data(), beforeBoundary.data(),
                    beforeBoundary.size(), loudnessTestSampleRate, 2);
                const auto resetBoundary = coordinator.telemetry().capture.capturedFrames;
                expect(coordinator.telemetry().capture.partialFrames == partialFrameCount);

                coordinator.resetLoudness();
                VisualizationFrame pendingReset;
                expect(waitForFrameWithoutRequest(
                    coordinator, pendingReset,
                    [sequence = beforeReset.loudnessSequence](const auto& candidate) {
                        return candidate.loudnessSequence > sequence
                            && !candidate.loudnessIntegratedValid;
                    },
                    2s));
                expect(coordinator.telemetry().loudness.capturedFrameEnd
                    == beforeReset.capturedFrameEnd);

                std::array<float, partialFrameCount> afterBoundary { };
                fillLoudnessTone(afterBoundary, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(afterBoundary.data(), afterBoundary.data(),
                    afterBoundary.size(), loudnessTestSampleRate, 2);

                std::array<float, loudnessMeasurementPeriodFrames * 4> continuedSignal { };
                fillLoudnessTone(continuedSignal, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(continuedSignal.data(), continuedSignal.data(),
                    continuedSignal.size(), loudnessTestSampleRate, 2);

                const auto expectedIntegratedEndpoint
                    = resetBoundary + loudnessMeasurementPeriodFrames * 4;
                VisualizationFrame integrated;
                expect(waitForFrame(
                    coordinator, integrated, [expectedIntegratedEndpoint](const auto& candidate) {
                        return candidate.loudnessIntegratedValid
                            && candidate.loudnessIntegratedCapturedFrameEnd
                            == expectedIntegratedEndpoint;
                    }));

                const auto telemetry = coordinator.telemetry();
                expect(telemetry.loudness.integrationResets
                    == telemetryBeforeReset.loudness.integrationResets + 1);
                expect(telemetry.loudness.inputChunks
                    == telemetryBeforeReset.loudness.inputChunks + 76);
                expect(telemetry.stereoFieldProcessedChunks
                    == telemetryBeforeReset.stereoFieldProcessedChunks + 76);
                expect(telemetry.loudnessMeasurement.integrationBlockCount == 1);
                expect(telemetry.loudnessMeasurement.integratedCapturedFrameEnd
                    == expectedIntegratedEndpoint);
            }

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
            beginTest("Racing Loudness RESET requests publish one coherent boundary");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame beforeReset;
                expect(
                    feedLoudnessTonePeriods(coordinator, 4, -18.0, toneFrameCursor, beforeReset));
                expect(beforeReset.loudnessIntegratedValid);
                const auto telemetryBeforeReset = coordinator.telemetry();

                WorkerHookBarrier rawAcquisitionBarrier(
                    AnalysisCoordinator::WorkerTestOperation::beforeRawAcquisition);
                coordinator.setWorkerTestHook(&rawAcquisitionBarrier, &WorkerHookBarrier::invoke);

                constexpr auto quarterSlot = StereoSampleCapture::framesPerSlot / 4;
                std::array<float, quarterSlot> firstPartial { };
                fillLoudnessTone(firstPartial, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(firstPartial.data(), firstPartial.data(),
                    firstPartial.size(), loudnessTestSampleRate, 2);
                coordinator.resetLoudness();
                expect(rawAcquisitionBarrier.waitUntilEntered(),
                    "The worker did not pause before raw acquisition");

                std::array<float, quarterSlot> secondPartial { };
                fillLoudnessTone(secondPartial, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(secondPartial.data(), secondPartial.data(),
                    secondPartial.size(), loudnessTestSampleRate, 2);
                const auto secondResetBoundary = coordinator.telemetry().capture.capturedFrames;

                WorkerHookBarrier resetCommitBarrier(
                    AnalysisCoordinator::WorkerTestOperation::beforeLoudnessResetCommit);
                coordinator.setWorkerTestHook(&resetCommitBarrier, &WorkerHookBarrier::invoke);
                std::thread secondResetThread([&coordinator] { coordinator.resetLoudness(); });
                const auto secondResetPaused = resetCommitBarrier.waitUntilEntered();
                expect(secondResetPaused, "The second RESET did not pause before publication");

                std::array<float, StereoSampleCapture::framesPerSlot / 2> completingPartial { };
                fillLoudnessTone(completingPartial, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(completingPartial.data(), completingPartial.data(),
                    completingPartial.size(), loudnessTestSampleRate, 2);

                const auto jobsCompletedBeforeRelease = coordinator.telemetry().jobsCompleted;
                rawAcquisitionBarrier.release();
                expect(waitUntil([&] {
                    const auto telemetry = coordinator.telemetry();
                    return telemetry.jobsCompleted > jobsCompletedBeforeRelease
                        && telemetry.loudness.capturedFrameEnd == beforeReset.capturedFrameEnd;
                }));

                resetCommitBarrier.release();
                secondResetThread.join();

                std::array<float, loudnessMeasurementPeriodFrames * 4> continuedSignal { };
                fillLoudnessTone(continuedSignal, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(continuedSignal.data(), continuedSignal.data(),
                    continuedSignal.size(), loudnessTestSampleRate, 2);

                const auto expectedIntegratedEndpoint
                    = secondResetBoundary + loudnessMeasurementPeriodFrames * 4;
                VisualizationFrame integrated;
                expect(waitForFrame(
                    coordinator, integrated, [expectedIntegratedEndpoint](const auto& candidate) {
                        return candidate.loudnessIntegratedValid
                            && candidate.loudnessIntegratedCapturedFrameEnd
                            == expectedIntegratedEndpoint;
                    }));

                const auto telemetry = coordinator.telemetry();
                expect(telemetry.loudness.integrationResets
                    == telemetryBeforeReset.loudness.integrationResets + 1);
                expect(telemetry.loudnessMeasurement.integratedCapturedFrameEnd
                    == expectedIntegratedEndpoint);
                coordinator.setWorkerTestHook(nullptr, nullptr);
            }
#endif

            beginTest("FFT and presentation mapping changes preserve Loudness state");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame beforeChanges;
                expect(feedLoudnessTonePeriods(
                    coordinator, 32, -20.0, toneFrameCursor, beforeChanges));
                const auto loudnessBefore = coordinator.telemetry().loudnessMeasurement;

                coordinator.setSpectrumAnalysisConfiguration(
                    { 2048, FftWindow::fourTermBlackmanHarris, 120 });
                coordinator.setSpectrumTemporalConfiguration(
                    { 25.0, 125.0, SpectrumPeakHoldMode::finite, 1.0 });
                coordinator.setSpectrogramFrequencySpacing(0.35);

                VisualizationFrame afterChanges;
                expect(waitForFrame(coordinator, afterChanges,
                    [generation = beforeChanges.generation](const auto& candidate) {
                        return candidate.generation == generation
                            && candidate.loudnessMomentaryValid && candidate.loudnessShortTermValid
                            && candidate.loudnessIntegratedValid;
                    }));
                const auto loudnessAfter = coordinator.telemetry().loudnessMeasurement;
                expect(loudnessAfter.stateSequence == loudnessBefore.stateSequence);
                expect(loudnessAfter.integrationBlockCount == loudnessBefore.integrationBlockCount);
                expectWithinAbsoluteError(
                    afterChanges.loudnessMomentaryLufs, beforeChanges.loudnessMomentaryLufs, 0.0);
                expectWithinAbsoluteError(
                    afterChanges.loudnessShortTermLufs, beforeChanges.loudnessShortTermLufs, 0.0);
                expectWithinAbsoluteError(
                    afterChanges.loudnessIntegratedLufs, beforeChanges.loudnessIntegratedLufs, 0.0);
            }

            beginTest("A reset racing FFT reconfiguration never republishes valid old I");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame ready;
                expect(feedLoudnessTonePeriods(coordinator, 4, -18.0, toneFrameCursor, ready));
                expect(ready.loudnessIntegratedValid);
                while (coordinator.copyLatestVisualizationFrame(ready)) { }

                coordinator.resetLoudness();
                coordinator.setSpectrumAnalysisConfiguration({ 2048, FftWindow::periodicHann, 60 });

                VisualizationFrame invalidated;
                expect(waitForFrameWithoutRequest(
                    coordinator, invalidated,
                    [](const auto& candidate) { return !candidate.loudnessIntegratedValid; }, 2s));
            }

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
            beginTest("A worker publication racing Loudness reset cannot expose valid old I");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame ready;
                expect(feedLoudnessTonePeriods(coordinator, 4, -18.0, toneFrameCursor, ready));
                expect(ready.loudnessIntegratedValid);
                while (coordinator.copyLatestVisualizationFrame(ready)) { }

                WorkerPublicationBarrier barrier;
                coordinator.setWorkerTestHook(&barrier, &WorkerPublicationBarrier::invoke);

                std::array<float, loudnessMeasurementPeriodFrames> nextPeriod { };
                fillLoudnessTone(nextPeriod, -18.0, toneFrameCursor);
                coordinator.captureAudioBlock(nextPeriod.data(), nextPeriod.data(),
                    nextPeriod.size(), loudnessTestSampleRate, 2);
                auto workerReachedPublication = false;
                const auto publicationDeadline = std::chrono::steady_clock::now() + 2s;
                while (!workerReachedPublication
                    && std::chrono::steady_clock::now() < publicationDeadline) {
                    coordinator.requestAnalysis();
                    workerReachedPublication = barrier.waitUntilBeforeEntered(2ms);
                }
                expect(workerReachedPublication,
                    "The worker did not pause before publishing its pre-reset frame");

                coordinator.resetLoudness();
                barrier.releaseBefore();
                expect(barrier.waitUntilAfterEntered(),
                    "The worker did not publish the frame that raced the reset");

                VisualizationFrame racedPublication;
                expect(coordinator.copyLatestVisualizationFrame(racedPublication));
                expect(!racedPublication.loudnessIntegratedValid,
                    "A frame published after reset exposed the old Integrated value");

                barrier.releaseAfter();
                VisualizationFrame appliedReset;
                expect(waitForFrameWithoutRequest(
                    coordinator, appliedReset,
                    [sequence = racedPublication.loudnessSequence](const auto& candidate) {
                        return candidate.loudnessSequence > sequence
                            && !candidate.loudnessIntegratedValid;
                    },
                    2s));
                coordinator.setWorkerTestHook(nullptr, nullptr);
            }

            beginTest("A producer-only gap cannot consume a pending Loudness reset boundary");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
                coordinator.setVisualizationActive(true);

                auto toneFrameCursor = std::uint64_t { 0 };
                VisualizationFrame beforeGap;
                expect(feedLoudnessTonePeriods(coordinator, 4, -18.0, toneFrameCursor, beforeGap));
                expect(beforeGap.loudnessIntegratedValid);

                WorkerHookBarrier barrier(
                    AnalysisCoordinator::WorkerTestOperation::beforeMeterConsumption);
                coordinator.setWorkerTestHook(&barrier, &WorkerHookBarrier::invoke);

                std::array<float, StereoSampleCapture::framesPerSlot> trigger { };
                trigger.fill(0.125F);
                coordinator.captureAudioBlock(
                    trigger.data(), trigger.data(), trigger.size(), loudnessTestSampleRate, 2);
                auto workerReachedMeter = false;
                const auto meterDeadline = std::chrono::steady_clock::now() + 2s;
                while (!workerReachedMeter && std::chrono::steady_clock::now() < meterDeadline) {
                    coordinator.requestAnalysis();
                    workerReachedMeter = barrier.waitUntilEntered(2ms);
                }
                expect(workerReachedMeter, "The worker did not reach the meter boundary seam");

                std::array<float, StereoSampleCapture::framesPerSlot> queuedBeforeReset { };
                queuedBeforeReset.fill(0.125F);
                for (std::size_t index = 0; index < StereoSampleCapture::slotCount * 2; ++index) {
                    coordinator.captureAudioBlock(queuedBeforeReset.data(),
                        queuedBeforeReset.data(), queuedBeforeReset.size(), loudnessTestSampleRate,
                        2);
                }

                const auto resetBoundary = coordinator.telemetry().capture.capturedFrames;
                coordinator.resetLoudness();
                barrier.release();

                VisualizationFrame afterGap;
                const auto receivedAfterGap = waitForFrame(
                    coordinator, afterGap,
                    [sequence = beforeGap.loudnessSequence, resetBoundary](const auto& candidate) {
                        return candidate.loudnessSequence > sequence
                            && candidate.capturedFrameEnd >= resetBoundary
                            && candidate.loudnessMomentaryValid;
                    },
                    2s);
                const auto afterGapTelemetry = coordinator.telemetry();
                expect(receivedAfterGap,
                    juce::String("No post-gap Loudness frame: frameEnd=")
                        + juce::String(static_cast<juce::int64>(afterGap.capturedFrameEnd))
                        + ", resetBoundary=" + juce::String(static_cast<juce::int64>(resetBoundary))
                        + ", M valid=" + juce::String(afterGap.loudnessMomentaryValid ? 1 : 0)
                        + ", ready=" + juce::String(afterGapTelemetry.capture.readySlots)
                        + ", reclaimed="
                        + juce::String(static_cast<juce::int64>(
                            afterGapTelemetry.capture.reclaimedReadyChunks))
                        + ", ignored="
                        + juce::String(
                            static_cast<juce::int64>(afterGapTelemetry.ignoredGenerationChunks))
                        + ", loudnessEnd="
                        + juce::String(
                            static_cast<juce::int64>(afterGapTelemetry.loudness.capturedFrameEnd))
                        + ", discontinuityResets="
                        + juce::String(static_cast<juce::int64>(
                            afterGapTelemetry.loudness.discontinuityResets)));
                expect(!afterGap.loudnessIntegratedValid,
                    "Pre-reset raw survivors entered the new Integrated interval");

                const auto telemetry = coordinator.telemetry();
                expect(telemetry.capture.reclaimedReadyChunks >= 4);
                expect(telemetry.loudness.discontinuityResets >= 1);
                expect(telemetry.loudness.integrationResets >= 1);
                expect(telemetry.loudness.capturedFrameEnd == resetBoundary);

                coordinator.setWorkerTestHook(nullptr, nullptr);
            }
#endif
        };
        runLoudnessTests();

        const auto runConfigurationTests = [this] {
            beginTest("FFT-only reconfiguration preserves capture and Peak/RMS state");
            {
                AnalysisCoordinator coordinator;
                coordinator.setVisualizationActive(true);

                std::array<float, fftSize> overSignal { };
                overSignal.fill(1.1F);
                coordinator.captureAudioBlock(
                    overSignal.data(), overSignal.data(), overSignal.size(), 48'000.0);

                VisualizationFrame beforeChange;
                expect(waitForFrame(coordinator, beforeChange, [](const auto& candidate) {
                    return candidate.spectrumValid && candidate.meterValid && candidate.over[0];
                }));

                const auto telemetryBefore = coordinator.telemetry();
                constexpr std::array replacements {
                    SpectrumAnalysisConfiguration { 1024, FftWindow::periodicHann, 60 },
                    SpectrumAnalysisConfiguration { 1024, FftWindow::fourTermBlackmanHarris, 60 },
                    SpectrumAnalysisConfiguration { 1024, FftWindow::fourTermBlackmanHarris, 120 },
                };
                auto precedingFrame = beforeChange;
                VisualizationFrame invalidated;
                for (const auto& replacement : replacements) {
                    coordinator.setSpectrumAnalysisConfiguration(replacement);

                    expect(coordinator.copyLatestVisualizationFrame(invalidated),
                        "FFT reconfiguration did not immediately publish its invalid snapshot");
                    expect(invalidated.generation == beforeChange.generation);
                    expect(invalidated.fftGeneration > precedingFrame.fftGeneration);
                    expect(!invalidated.spectrumValid);
                    expect(invalidated.spectrumSequence > precedingFrame.spectrumSequence);
                    expect(invalidated.spectrumFftSize == replacement.fftSize);
                    expect(invalidated.spectrumBinCount == (replacement.fftSize / 2) + 1);
                    expect(invalidated.meterValid);
                    expect(invalidated.meterSequence == beforeChange.meterSequence);
                    expect(invalidated.stereoSequence == beforeChange.stereoSequence);
                    expect(invalidated.stereoFieldValid == beforeChange.stereoFieldValid);
                    expect(invalidated.stereoFieldPointCount == beforeChange.stereoFieldPointCount);
                    expect(
                        invalidated.stereoCorrelationValid == beforeChange.stereoCorrelationValid);
                    expectWithinAbsoluteError(
                        invalidated.stereoCorrelation, beforeChange.stereoCorrelation, 0.0001F);
                    expect(invalidated.capturedFrameEnd == beforeChange.capturedFrameEnd);
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        expectWithinAbsoluteError(invalidated.peakDecibels[channel],
                            beforeChange.peakDecibels[channel], 0.0001F);
                        expectWithinAbsoluteError(invalidated.rmsDecibels[channel],
                            beforeChange.rmsDecibels[channel], 0.0001F);
                        expectWithinAbsoluteError(invalidated.heldPeakDecibels[channel],
                            beforeChange.heldPeakDecibels[channel], 0.0001F);
                        expect(invalidated.over[channel] == beforeChange.over[channel]);
                    }
                    precedingFrame = invalidated;
                }

                const auto telemetryAfterChange = coordinator.telemetry();
                expect(telemetryAfterChange.fftConfigurationChanges
                    == telemetryBefore.fftConfigurationChanges + replacements.size());
                expect(telemetryAfterChange.fftGeneration == invalidated.fftGeneration);
                expect(telemetryAfterChange.configuredFftSize == replacements.back().fftSize);
                expect(telemetryAfterChange.configuredFftWindow
                    == static_cast<std::uint32_t>(replacements.back().window));
                expect(telemetryAfterChange.requestedFftSliceRateHz
                    == replacements.back().requestedSliceRateHz);

                std::array<float, ordinaryCaptureWindowFrames> quieterSignal { };
                quieterSignal.fill(0.25F);
                coordinator.captureAudioBlock(
                    quieterSignal.data(), quieterSignal.data(), quieterSignal.size(), 48'000.0);

                VisualizationFrame warmedUp;
                expect(waitForFrame(coordinator, warmedUp,
                    [captureGeneration = beforeChange.generation,
                        fftGeneration = invalidated.fftGeneration](const auto& candidate) {
                        return candidate.generation == captureGeneration
                            && candidate.fftGeneration == fftGeneration && candidate.spectrumValid
                            && candidate.spectrumFftSize == 1024
                            && candidate.spectrumBinCount == 513;
                    }));
                expect(warmedUp.meterSequence > invalidated.meterSequence);
                expectWithinAbsoluteError(
                    warmedUp.heldPeakDecibels[0], beforeChange.heldPeakDecibels[0], 0.0001F);
                expect(warmedUp.over[0] && warmedUp.over[1]);
            }

            beginTest("Spectrum temporal configuration and Clear preserve unrelated state");
            {
                AnalysisCoordinator coordinator;
                coordinator.setVisualizationActive(true);

                std::array<float, fftSize> signal { };
                signal.fill(1.1F);
                coordinator.captureAudioBlock(
                    signal.data(), signal.data(), signal.size(), 48'000.0);

                VisualizationFrame beforeChange;
                expect(waitForFrame(coordinator, beforeChange, [](const auto& candidate) {
                    return candidate.spectrumValid && candidate.meterValid && candidate.over[0];
                }));
                const auto telemetryBefore = coordinator.telemetry();

                coordinator.setSpectrumTemporalConfiguration(
                    { 25.0, 250.0, SpectrumPeakHoldMode::infinite, 2.0 });

                VisualizationFrame invalidated;
                expect(coordinator.copyLatestVisualizationFrame(invalidated));
                expect(!invalidated.spectrumValid);
                expect(!invalidated.spectrumPeakHoldValid);
                expect(invalidated.generation == beforeChange.generation);
                expect(invalidated.fftGeneration == beforeChange.fftGeneration);
                expect(invalidated.meterValid);
                expect(invalidated.meterSequence == beforeChange.meterSequence);
                expect(invalidated.over == beforeChange.over);
                expectWithinAbsoluteError(
                    invalidated.heldPeakDecibels[0], beforeChange.heldPeakDecibels[0], 0.0001F);

                const auto telemetryAfterConfiguration = coordinator.telemetry();
                expect(telemetryAfterConfiguration.fftGeneration == telemetryBefore.fftGeneration);
                expect(telemetryAfterConfiguration.fftConfigurationChanges
                    == telemetryBefore.fftConfigurationChanges);
                expect(telemetryAfterConfiguration.spectrumTemporalConfigurationChanges
                    == telemetryBefore.spectrumTemporalConfigurationChanges + 1);
                expectWithinAbsoluteError(
                    telemetryAfterConfiguration.configuredSpectrumAttackMilliseconds, 25.0, 0.0);
                expectWithinAbsoluteError(
                    telemetryAfterConfiguration.configuredSpectrumReleaseMilliseconds, 250.0, 0.0);

                std::array<float, 1024> nextHop { };
                nextHop.fill(0.25F);
                coordinator.captureAudioBlock(
                    nextHop.data(), nextHop.data(), nextHop.size(), 48'000.0);

                VisualizationFrame warmed;
                expect(waitForFrame(coordinator, warmed,
                    [sequence = invalidated.spectrumSequence](const auto& candidate) {
                        return candidate.spectrumSequence > sequence && candidate.spectrumValid
                            && candidate.spectrumPeakHoldValid;
                    }));

                coordinator.resetSpectrum();
                coordinator.resetSpectrum();
                VisualizationFrame cleared;
                expect(waitForFrameWithoutRequest(coordinator, cleared,
                    [sequence = warmed.spectrumSequence](const auto& candidate) {
                        return candidate.spectrumSequence > sequence && !candidate.spectrumValid
                            && !candidate.spectrumPeakHoldValid;
                    }));
                expect(cleared.fftGeneration == warmed.fftGeneration);
                expect(cleared.meterValid);
                expect(cleared.meterSequence == warmed.meterSequence);
                expect(cleared.over == warmed.over);
                expect(coordinator.telemetry().spectrumUserClears == 2);
            }

            beginTest("One normal capture slot is not backlog for a 1024-point FFT");
            {
                AnalysisCoordinator coordinator;
                coordinator.setSpectrumAnalysisConfiguration({ 1024, FftWindow::periodicHann, 15 });
                coordinator.setVisualizationActive(true);

                std::array<float, ordinaryCaptureWindowFrames> samples { };
                samples.fill(0.25F);
                coordinator.captureAudioBlock(
                    samples.data(), samples.data(), samples.size(), 48'000.0);

                VisualizationFrame frame;
                expect(waitForFrame(coordinator, frame,
                    [](const auto& candidate) { return candidate.spectrumValid; }));
                expect(frame.spectrumFftSize == 1024);
                expect(frame.spectrumBinCount == 513);

                const auto telemetry = coordinator.telemetry();
                expect(telemetry.capture.reclaimedReadyChunks == 0);
                expect(telemetry.capture.droppedIncomingChunks == 0);
                expect(telemetry.backlogDiscardedFrames == 0,
                    "A single ordinary 2048-frame capture slot was truncated as backlog");
                expect(telemetry.spectrumTransforms == 1);
                expect(telemetry.spectrumCapturedFrameEnd == 1024);
                expect(telemetry.meterCapturedFrameEnd == ordinaryCaptureWindowFrames);
            }

            beginTest("A capture gap publishes a sequenced invalid Spectrum before warm-up");
            {
                AnalysisCoordinator coordinator;
                coordinator.setSpectrumAnalysisConfiguration(
                    { 16384, FftWindow::periodicHann, 60 });
                coordinator.setVisualizationActive(true);

                std::array<float, maximumFftSize> initialSignal { };
                initialSignal.fill(0.25F);
                coordinator.captureAudioBlock(
                    initialSignal.data(), initialSignal.data(), initialSignal.size(), 48'000.0);

                VisualizationFrame valid;
                expect(waitForFrame(coordinator, valid,
                    [](const auto& candidate) { return candidate.spectrumValid; }));

                constexpr std::array<float, 64> shortChunk { };
                for (std::size_t chunk = 0; chunk < captureCallsThroughFirstPackedOverflow<64>();
                    ++chunk) {
                    coordinator.captureAudioBlock(
                        shortChunk.data(), shortChunk.data(), shortChunk.size(), 48'000.0);
                }

                VisualizationFrame invalidated;
                expect(waitForFrame(coordinator, invalidated,
                    [capturedFrameEnd = valid.capturedFrameEnd](const auto& candidate) {
                        return candidate.capturedFrameEnd > capturedFrameEnd
                            && !candidate.spectrumValid;
                    }));
                expect(invalidated.generation > valid.generation);
                expect(invalidated.fftGeneration == valid.fftGeneration);
                expect(invalidated.spectrumSequence > valid.spectrumSequence,
                    "Capture-gap invalidation reused the preceding valid Spectrum sequence");
                expect(coordinator.telemetry().capture.reclaimedReadyChunks > 0);
            }

            beginTest("A meter-only endpoint gap preserves continuous Stereo field history");
            {
                AnalysisCoordinator coordinator;
                coordinator.setVisualizationActive(true);

                std::array<float, StereoSampleCapture::framesPerSlot> left { };
                std::array<float, StereoSampleCapture::framesPerSlot> right { };
                left.fill(0.5F);
                right.fill(-0.25F);
                coordinator.captureAudioBlock(left.data(), right.data(), left.size(), 48'000.0);

                VisualizationFrame first;
                expect(waitForFrame(
                    coordinator, first, [expectedEnd = left.size()](const auto& frame) {
                        return frame.stereoFieldValid && frame.meterValid
                            && frame.stereoCapturedFrameEnd == expectedEnd;
                    }));

                const auto telemetryBeforeGap = coordinator.telemetry();
                coordinator.skipNextMeterEndpointSequenceForTesting();
                coordinator.captureAudioBlock(left.data(), right.data(), left.size(), 48'000.0);

                VisualizationFrame second;
                expect(waitForFrame(coordinator, second,
                    [expectedEnd = left.size() * 2, firstSequence = first.stereoSequence](
                        const auto& frame) {
                        return frame.stereoFieldValid && frame.meterValid
                            && frame.stereoCapturedFrameEnd == expectedEnd
                            && frame.stereoSequence > firstSequence;
                    }));

                const auto telemetryAfterGap = coordinator.telemetry();
                expect(second.generation == first.generation);
                expect(telemetryAfterGap.captureGeneration == telemetryBeforeGap.captureGeneration);
                expect(second.stereoFieldPointCount > first.stereoFieldPointCount);
                expect(telemetryAfterGap.stereoFieldHistoryResets
                    == telemetryBeforeGap.stereoFieldHistoryResets);
                expect(telemetryAfterGap.capture.consumerDiscontinuities
                    == telemetryBeforeGap.capture.consumerDiscontinuities);
                expect(telemetryAfterGap.meters.consumerDiscontinuities
                    > telemetryBeforeGap.meters.consumerDiscontinuities);
            }

            beginTest("A 15 Hz FFT request rate still services meters at 60 Hz");
            {
                auto establishedTimingWindow = false;
                auto servicedSecondCapture = false;

                for (auto attempt = 0; attempt < 4 && !establishedTimingWindow; ++attempt) {
                    AnalysisCoordinator coordinator;
                    coordinator.setSpectrumAnalysisConfiguration(
                        { 1024, FftWindow::periodicHann, 15 });
                    coordinator.setVisualizationActive(true);

                    std::array<float, 64> samples { };
                    samples.fill(0.25F);
                    coordinator.captureAudioBlock(
                        samples.data(), samples.data(), samples.size(), 48'000.0);

                    const auto firstRequest = std::chrono::steady_clock::now();
                    coordinator.requestAnalysis();
                    while (coordinator.telemetry().jobsCompleted == 0
                        && std::chrono::steady_clock::now() - firstRequest < 55ms) {
                        std::this_thread::yield();
                    }

                    if (coordinator.telemetry().jobsCompleted == 0)
                        continue;

                    while (std::chrono::steady_clock::now() - firstRequest < 25ms)
                        std::this_thread::yield();

                    if (std::chrono::steady_clock::now() - firstRequest >= 60ms)
                        continue;

                    establishedTimingWindow = true;
                    const auto submittedBeforeSecond = coordinator.telemetry().scheduler.submitted;
                    coordinator.captureAudioBlock(
                        samples.data(), samples.data(), samples.size(), 48'000.0);
                    coordinator.requestAnalysis();
                    const auto secondRequestAccepted
                        = coordinator.telemetry().scheduler.submitted == submittedBeforeSecond + 1;

                    VisualizationFrame second;
                    servicedSecondCapture = secondRequestAccepted
                        && waitForFrameWithoutRequest(coordinator, second,
                            [expectedFrameEnd = samples.size() * 2](const auto& candidate) {
                                return candidate.meterValid
                                    && candidate.capturedFrameEnd == expectedFrameEnd;
                            });
                }

                expect(establishedTimingWindow,
                    "The test machine could not establish the 60-vs-15 Hz timing window");
                expect(servicedSecondCapture,
                    "The 15 Hz FFT cadence incorrectly throttled a meter-only update");
            }

            beginTest("A 120 Hz FFT request rate is not capped by a 60 Hz service gate");
            {
                auto establishedTimingWindow = false;
                auto servicedSecondCapture = false;

                for (auto attempt = 0; attempt < 4 && !establishedTimingWindow; ++attempt) {
                    AnalysisCoordinator coordinator;
                    coordinator.setSpectrumAnalysisConfiguration(
                        { 1024, FftWindow::periodicHann, 120 });
                    coordinator.setVisualizationActive(true);

                    std::array<float, 32> samples { };
                    samples.fill(0.25F);
                    coordinator.captureAudioBlock(
                        samples.data(), samples.data(), samples.size(), 48'000.0);

                    const auto firstRequest = std::chrono::steady_clock::now();
                    coordinator.requestAnalysis();
                    while (coordinator.telemetry().jobsCompleted == 0
                        && std::chrono::steady_clock::now() - firstRequest < 14ms) {
                        std::this_thread::yield();
                    }

                    if (coordinator.telemetry().jobsCompleted == 0)
                        continue;

                    while (std::chrono::steady_clock::now() - firstRequest < 10ms)
                        std::this_thread::yield();

                    if (std::chrono::steady_clock::now() - firstRequest >= 14ms)
                        continue;

                    establishedTimingWindow = true;
                    const auto submittedBeforeSecond = coordinator.telemetry().scheduler.submitted;
                    coordinator.captureAudioBlock(
                        samples.data(), samples.data(), samples.size(), 48'000.0);
                    coordinator.requestAnalysis();
                    const auto secondRequestAccepted
                        = coordinator.telemetry().scheduler.submitted == submittedBeforeSecond + 1;

                    VisualizationFrame second;
                    servicedSecondCapture = secondRequestAccepted
                        && waitForFrameWithoutRequest(coordinator, second,
                            [expectedFrameEnd = samples.size() * 2](const auto& candidate) {
                                return candidate.meterValid
                                    && candidate.capturedFrameEnd == expectedFrameEnd;
                            });
                    servicedSecondCapture = servicedSecondCapture
                        && coordinator.telemetry().meterCapturedFrameEnd == samples.size() * 2;
                }

                expect(establishedTimingWindow,
                    "The test machine could not establish the 120-vs-60 Hz timing window");
                expect(servicedSecondCapture,
                    "A second capture inside one 60 Hz period was not serviced at 120 Hz");
            }

            beginTest("Presentation-only changes do not reconfigure an unchanged FFT");
            {
                AnalysisCoordinator coordinator;
                coordinator.setVisualizationActive(true);

                VisualizationFrame initial;
                expect(coordinator.copyLatestVisualizationFrame(initial));
                const auto telemetryBefore = coordinator.telemetry();

                // Frequency spacing is intentionally absent from the worker-side
                // configuration. Its caller therefore republishes this same FFT
                // subset when only the presentation mapping changes.
                coordinator.setSpectrumAnalysisConfiguration({ });

                const auto telemetryAfter = coordinator.telemetry();
                expect(telemetryAfter.fftGeneration == telemetryBefore.fftGeneration);
                expect(telemetryAfter.fftConfigurationChanges
                    == telemetryBefore.fftConfigurationChanges);
                expect(telemetryAfter.scheduler.submitted == telemetryBefore.scheduler.submitted);
                expect(!coordinator.copyLatestVisualizationFrame(initial));
            }

            beginTest("A saturated capture queue fast-forwards to one newest FFT window");
            {
                AnalysisCoordinator coordinator;
                coordinator.setVisualizationActive(true);

                std::array<float, StereoSampleCapture::framesPerSlot> samples { };
                samples.fill(0.25F);
                constexpr auto publishedBlocks = StereoSampleCapture::slotCount * 2;
                for (std::size_t block = 0; block < publishedBlocks; ++block) {
                    coordinator.captureAudioBlock(
                        samples.data(), samples.data(), samples.size(), 48'000.0);
                }

                VisualizationFrame frame;
                const auto receivedSpectrum = waitForFrame(coordinator, frame,
                    [](const auto& candidate) { return candidate.spectrumValid; });
                expect(
                    receivedSpectrum, "The bounded backlog job did not publish its newest window");

                const auto backlogTelemetry = coordinator.telemetry();
                expect(backlogTelemetry.capture.reclaimedReadyChunks > 0);
                expect(backlogTelemetry.backlogDiscardedFrames >= fftSize);
                expect(backlogTelemetry.maximumJobSpectrumTransforms <= 1,
                    "A saturated queue performed more than one display-useful transform");
                expect(backlogTelemetry.spectrumCapturedFrameEnd == frame.capturedFrameEnd);
                expect(backlogTelemetry.meterCapturedFrameEnd == frame.capturedFrameEnd);

                const auto spectrumEndpoint = backlogTelemetry.spectrumCapturedFrameEnd;
                constexpr std::array<float, 128> shortBlock { };
                coordinator.captureAudioBlock(
                    shortBlock.data(), shortBlock.data(), shortBlock.size(), 48'000.0);

                const auto receivedMeterOnlyUpdate
                    = waitForFrame(coordinator, frame, [spectrumEndpoint](const auto& candidate) {
                          return candidate.capturedFrameEnd > spectrumEndpoint;
                      });
                expect(receivedMeterOnlyUpdate);

                const auto freshnessTelemetry = coordinator.telemetry();
                expect(freshnessTelemetry.spectrumCapturedFrameEnd == spectrumEndpoint);
                expect(freshnessTelemetry.meterCapturedFrameEnd
                    == spectrumEndpoint + shortBlock.size());
            }

            beginTest("Mono analysis publishes one honest meter channel");
            {
                AnalysisCoordinator coordinator;
                coordinator.setVisualizationActive(true);

                std::array<float, fftSize> mono { };
                mono.fill(0.5F);
                coordinator.captureAudioBlock(
                    mono.data(), nullptr, mono.size(), 48'000.0, std::uint32_t { 1 });

                VisualizationFrame frame;
                const auto received = waitForFrame(coordinator, frame,
                    [](const auto& candidate) { return candidate.channelCount == 1; });
                expect(received);
                expect(frame.channelCount == 1);
                expectWithinAbsoluteError(frame.peakDecibels[0], -6.0206F, 0.02F);
                expect(isDisplayFloor(frame.peakDecibels[1]));
                expect(isDisplayFloor(frame.rmsDecibels[1]));
                expect(frame.stereoFieldValid);
                expect(frame.stereoMono);
                expect(frame.stereoFieldPointCount > 0);
                expect(!frame.stereoCorrelationValid);
                for (std::size_t point = 0; point < frame.stereoFieldPointCount; ++point)
                    expectWithinAbsoluteError(
                        frame.stereoFieldPoints[point].horizontal, 0.0F, 1.0e-7F);
            }

            beginTest("Sample-rate changes start a clean capture generation");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(48'000.0, 2);
                coordinator.setVisualizationActive(true);

                std::array<float, fftSize> signal { };
                signal.fill(0.25F);
                coordinator.captureAudioBlock(
                    signal.data(), signal.data(), signal.size(), 48'000.0, 2);

                VisualizationFrame original;
                expect(waitForFrame(coordinator, original, [](const auto& candidate) {
                    return candidate.spectrumValid && candidate.meterValid;
                }));

                const auto resetsBeforeFormatChange = coordinator.telemetry().loudness;

                coordinator.setCaptureFormat(96'000.0, 2);
                VisualizationFrame restarted;
                expect(waitForFrame(coordinator, restarted,
                    [generation = original.generation](const auto& candidate) {
                        return candidate.generation > generation && !candidate.spectrumValid
                            && !candidate.meterValid && !candidate.stereoFieldValid
                            && !candidate.stereoCorrelationValid;
                    }));

                constexpr std::array<float, 128> shortSignal { };
                coordinator.captureAudioBlock(
                    shortSignal.data(), shortSignal.data(), shortSignal.size(), 96'000.0, 2);

                VisualizationFrame warming;
                expect(waitForFrame(coordinator, warming,
                    [generation = restarted.generation](const auto& candidate) {
                        return candidate.generation == generation && candidate.meterValid
                            && !candidate.spectrumValid;
                    }));
                expectWithinAbsoluteError(warming.sampleRate, 96'000.0, 0.001);

                const auto resetsAfterFormatChange = coordinator.telemetry().loudness;
                expect(resetsAfterFormatChange.formatResets
                    == resetsBeforeFormatChange.formatResets + 1);
                expect(resetsAfterFormatChange.explicitResets
                    == resetsBeforeFormatChange.explicitResets);

                coordinator.captureAudioBlock(
                    signal.data(), signal.data(), signal.size(), 96'000.0, 2);
                VisualizationFrame ready;
                expect(waitForFrame(
                    coordinator, ready, [generation = restarted.generation](const auto& candidate) {
                        return candidate.generation == generation && candidate.spectrumValid
                            && candidate.meterValid;
                    }));

                while (coordinator.copyLatestVisualizationFrame(ready)) { }
                coordinator.setCaptureFormat(96'000.0, 2);
                expect(!coordinator.copyLatestVisualizationFrame(ready),
                    "An identical format unnecessarily restarted capture");
            }

            beginTest("Channel-layout changes never retain a stereo spectrum beside a mono meter");
            {
                AnalysisCoordinator coordinator;
                coordinator.setCaptureFormat(48'000.0, 2);
                coordinator.setVisualizationActive(true);

                std::array<float, fftSize> stereo { };
                stereo.fill(0.25F);
                coordinator.captureAudioBlock(
                    stereo.data(), stereo.data(), stereo.size(), 48'000.0, 2);

                VisualizationFrame original;
                expect(waitForFrame(coordinator, original, [](const auto& candidate) {
                    return candidate.spectrumValid && candidate.channelCount == 2;
                }));

                coordinator.setCaptureFormat(48'000.0, 1);
                VisualizationFrame restarted;
                expect(waitForFrame(coordinator, restarted,
                    [generation = original.generation](const auto& candidate) {
                        return candidate.generation > generation && !candidate.spectrumValid
                            && !candidate.meterValid;
                    }));

                constexpr std::array<float, 128> mono { };
                coordinator.captureAudioBlock(mono.data(), nullptr, mono.size(), 48'000.0, 1);
                VisualizationFrame warming;
                expect(waitForFrame(coordinator, warming,
                    [generation = restarted.generation](const auto& candidate) {
                        return candidate.generation == generation && candidate.meterValid
                            && candidate.channelCount == 1;
                    }));
                expect(!warming.spectrumValid);
            }
        };
        runConfigurationTests();

        beginTest("Stale input clears M/S while preserving completed Integrated Loudness");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
            coordinator.setVisualizationActive(true);

            auto toneFrameCursor = std::uint64_t { 0 };
            VisualizationFrame live;
            expect(feedLoudnessTonePeriods(coordinator, 32, -23.0, toneFrameCursor, live));
            expect(live.loudnessMomentaryValid && live.loudnessShortTermValid
                && live.loudnessIntegratedValid);

            VisualizationFrame stale;
            expect(waitForFrame(
                coordinator, stale,
                [sequence = live.loudnessSequence](const auto& candidate) {
                    return candidate.loudnessSequence > sequence
                        && !candidate.loudnessMomentaryValid && !candidate.loudnessShortTermValid
                        && candidate.loudnessIntegratedValid;
                },
                1s));
            expectWithinAbsoluteError(
                stale.loudnessIntegratedLufs, live.loudnessIntegratedLufs, 0.0);
            expect(stale.loudnessIntegratedCapturedFrameEnd
                == live.loudnessIntegratedCapturedFrameEnd);
            expect(coordinator.telemetry().loudness.liveMeasurementClears == 1);
        }

        beginTest("Transport-stop staleness publishes one cleared frame without empty jobs");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, fftSize> signal { };
            signal.fill(1.1F);
            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0);

            VisualizationFrame signalFrame;
            const auto receivedSignal = waitForFrame(coordinator, signalFrame,
                [](const auto& candidate) { return candidate.spectrumValid; });
            expect(receivedSignal);

            VisualizationFrame clearedFrame;
            const auto receivedClear = waitForFrame(
                coordinator, clearedFrame,
                [](const auto& candidate) {
                    return !candidate.spectrumValid && isDisplayFloor(candidate.peakDecibels[0])
                        && isDisplayFloor(candidate.peakDecibels[1])
                        && isDisplayFloor(candidate.rmsDecibels[0])
                        && isDisplayFloor(candidate.rmsDecibels[1]);
                },
                1s);
            expect(receivedClear, "A stopped transport left the last meters frozen");
            expect(clearedFrame.generation == signalFrame.generation);
            expect(clearedFrame.spectrumSequence > signalFrame.spectrumSequence);
            expect(clearedFrame.meterSequence > signalFrame.meterSequence);
            expect(clearedFrame.stereoSequence > signalFrame.stereoSequence);
            expect(!clearedFrame.stereoFieldValid);
            expect(!clearedFrame.stereoCorrelationValid);
            expectWithinAbsoluteError(
                clearedFrame.heldPeakDecibels[0], signalFrame.heldPeakDecibels[0], 0.0001F);
            expect(signalFrame.over[0] && signalFrame.over[1]);
            expect(clearedFrame.over[0] && clearedFrame.over[1]);

            const auto afterClear = coordinator.telemetry();
            expect(afterClear.staleFramesPublished == 1);
            const auto submittedAfterClear = afterClear.scheduler.submitted;

            for (std::size_t request = 0; request < 32; ++request)
                coordinator.requestAnalysis();

            const auto afterRepeatedIdleRequests = coordinator.telemetry();
            expect(afterRepeatedIdleRequests.scheduler.submitted == submittedAfterClear);
            expect(afterRepeatedIdleRequests.staleFramesPublished == 1);
        }

        beginTest("Stale live measurements do not reappear when silent capture resumes");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, 512> signal { };
            signal.fill(1.1F);
            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0);

            VisualizationFrame signalFrame;
            expect(waitForFrame(coordinator, signalFrame,
                [](const auto& candidate) { return candidate.meterValid && candidate.over[0]; }));

            VisualizationFrame clearedFrame;
            expect(waitForFrame(
                coordinator, clearedFrame,
                [sequence = signalFrame.meterSequence](const auto& candidate) {
                    return candidate.meterSequence > sequence
                        && isDisplayFloor(candidate.peakDecibels[0])
                        && isDisplayFloor(candidate.rmsDecibels[0]);
                },
                1s));
            expect(clearedFrame.over[0]);
            expect(!isDisplayFloor(clearedFrame.heldPeakDecibels[0]));

            constexpr std::array<float, 128> silence { };
            coordinator.captureAudioBlock(silence.data(), silence.data(), silence.size(), 48'000.0);

            VisualizationFrame resumedFrame;
            expect(waitForFrame(coordinator, resumedFrame,
                [sequence = clearedFrame.meterSequence](
                    const auto& candidate) { return candidate.meterSequence > sequence; }));
            expect(isDisplayFloor(resumedFrame.peakDecibels[0]));
            expect(isDisplayFloor(resumedFrame.rmsDecibels[0]));
            expectWithinAbsoluteError(
                resumedFrame.heldPeakDecibels[0], clearedFrame.heldPeakDecibels[0], 0.001F);
            expect(resumedFrame.over[0] && resumedFrame.over[1]);
        }

        beginTest("Reactivation begins with a fresh generation and cleared snapshot");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
            coordinator.setVisualizationActive(true);

            auto toneFrameCursor = std::uint64_t { 0 };
            VisualizationFrame activeFrame;
            expect(feedLoudnessTonePeriods(coordinator, 4, -23.0, toneFrameCursor, activeFrame));
            expect(activeFrame.spectrumValid);
            expect(activeFrame.loudnessMomentaryValid);
            expect(activeFrame.loudnessIntegratedValid);

            const auto resetsBeforeReactivation = coordinator.telemetry().loudness;

            coordinator.setVisualizationActive(false);
            coordinator.setVisualizationActive(true);

            VisualizationFrame reopenedFrame;
            const auto receivedFreshGeneration = waitForFrame(coordinator, reopenedFrame,
                [oldGeneration = activeFrame.generation](const auto& candidate) {
                    return candidate.generation > oldGeneration && !candidate.spectrumValid;
                });
            expect(receivedFreshGeneration);
            expect(isDisplayFloor(reopenedFrame.peakDecibels[0]));
            expect(isDisplayFloor(reopenedFrame.peakDecibels[1]));
            expect(isDisplayFloor(reopenedFrame.rmsDecibels[0]));
            expect(isDisplayFloor(reopenedFrame.rmsDecibels[1]));
            expect(isDisplayFloor(reopenedFrame.heldPeakDecibels[0]));
            expect(isDisplayFloor(reopenedFrame.heldPeakDecibels[1]));
            expect(!reopenedFrame.over[0] && !reopenedFrame.over[1]);
            expect(!reopenedFrame.meterValid);
            expect(!reopenedFrame.stereoFieldValid);
            expect(!reopenedFrame.stereoCorrelationValid);
            expect(!reopenedFrame.loudnessMomentaryValid);
            expect(!reopenedFrame.loudnessShortTermValid);
            expect(!reopenedFrame.loudnessIntegratedValid);

            const auto resetsAfterReactivation = coordinator.telemetry().loudness;
            expect(resetsAfterReactivation.generationResets
                == resetsBeforeReactivation.generationResets + 1);
            expect(
                resetsAfterReactivation.explicitResets == resetsBeforeReactivation.explicitResets);
        }

        beginTest("Peak/RMS user reset clears holds and OVER without clearing live values");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, 512> signal { };
            signal.fill(1.1F);
            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0);

            VisualizationFrame beforeReset;
            expect(waitForFrame(coordinator, beforeReset, [](const auto& candidate) {
                return candidate.meterValid && candidate.over[0]
                    && candidate.stereoCorrelationValid;
            }));

            coordinator.resetPeakRms();
            coordinator.resetPeakRms();
            VisualizationFrame afterReset;
            expect(waitForFrame(coordinator, afterReset,
                [sequence = beforeReset.meterSequence](const auto& candidate) {
                    return candidate.meterSequence > sequence && !candidate.over[0]
                        && isDisplayFloor(candidate.heldPeakDecibels[0]);
                }));
            expectWithinAbsoluteError(
                afterReset.peakDecibels[0], beforeReset.peakDecibels[0], 0.0001F);
            expectWithinAbsoluteError(
                afterReset.rmsDecibels[0], beforeReset.rmsDecibels[0], 0.0001F);
            expect(afterReset.stereoSequence == beforeReset.stereoSequence);
            expect(afterReset.stereoCorrelationValid);
            expectWithinAbsoluteError(
                afterReset.stereoCorrelation, beforeReset.stereoCorrelation, 0.0001F);
            expect(coordinator.telemetry().peakRmsUserResets == 2);
        }

        beginTest("Peak/RMS reset before first audio does not fabricate stale state");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);
            coordinator.resetPeakRms();

            const auto deadline = std::chrono::steady_clock::now() + 350ms;
            while (std::chrono::steady_clock::now() < deadline) {
                coordinator.requestAnalysis();
                std::this_thread::sleep_for(2ms);
            }

            const auto telemetry = coordinator.telemetry();
            expect(telemetry.peakRmsUserResets == 1);
            expect(telemetry.meterCapturedFrameEnd == 0);
            expect(telemetry.staleFramesPublished == 0);
        }

        beginTest("Raw capture overflow resets Peak/RMS at the shared discontinuity");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, StereoSampleCapture::framesPerSlot> block { };
            block.fill(1.1F);
            coordinator.captureAudioBlock(block.data(), block.data(), block.size(), 48'000.0);

            VisualizationFrame beforeGap;
            expect(waitForFrame(coordinator, beforeGap,
                [](const auto& candidate) { return candidate.meterValid && candidate.over[0]; }));
            const auto historyResetsBeforeGap = coordinator.telemetry().stereoFieldHistoryResets;

            block.fill(0.1F);
            for (std::size_t index = 0; index < StereoSampleCapture::slotCount + 2; ++index)
                coordinator.captureAudioBlock(block.data(), block.data(), block.size(), 48'000.0);

            auto observedInvalidBoundary = false;
            VisualizationFrame afterGap;
            expect(waitForFrame(coordinator, afterGap,
                [sequence = beforeGap.meterSequence, generation = beforeGap.generation,
                    &observedInvalidBoundary](const auto& candidate) {
                    if (candidate.generation > generation && !candidate.spectrumValid
                        && !candidate.meterValid && !candidate.stereoFieldValid
                        && !candidate.loudnessMomentaryValid && !candidate.loudnessShortTermValid
                        && !candidate.loudnessIntegratedValid) {
                        observedInvalidBoundary = true;
                    }
                    return candidate.meterValid && candidate.meterSequence > sequence
                        && !candidate.over[0];
                }));
            expect(observedInvalidBoundary,
                "Post-gap data was delivered before the invalid capture boundary");
            expect(!afterGap.over[0] && !afterGap.over[1]);
            expectWithinAbsoluteError(afterGap.heldPeakDecibels[0], -20.0F, 0.01F);
            expect(afterGap.generation == beforeGap.generation + 1,
                "Raw and meter reports of one overflow advanced generation more than once");
            const auto afterGapTelemetry = coordinator.telemetry();
            expect(afterGapTelemetry.captureGeneration == afterGap.generation);
            expect(afterGapTelemetry.capture.reclaimedReadyChunks > 0);
            expect(afterGapTelemetry.stereoFieldHistoryResets > historyResetsBeforeGap);
            expect(afterGap.stereoFieldValid);
            expect(afterGap.stereoCapturedFrameEnd == afterGapTelemetry.stereoCapturedFrameEnd);

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount + 2; ++index)
                coordinator.captureAudioBlock(block.data(), block.data(), block.size(), 48'000.0);

            VisualizationFrame afterSecondGap;
            expect(waitForFrame(coordinator, afterSecondGap,
                [generation = afterGap.generation](const auto& candidate) {
                    return candidate.generation > generation && candidate.meterValid;
                }));
            expect(afterSecondGap.generation == afterGap.generation + 1,
                "A distinct later overflow did not advance generation exactly once");
        }

        beginTest("A split host block resets Peak/RMS at the chunk that overflows");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, StereoSampleCapture::framesPerSlot> queued { };
            queued.fill(0.1F);
            for (std::size_t index = 0; index + 1 < StereoSampleCapture::slotCount; ++index)
                coordinator.captureAudioBlock(
                    queued.data(), queued.data(), queued.size(), 48'000.0);

            std::array<float, StereoSampleCapture::framesPerSlot * 2> splitBlock { };
            std::fill_n(splitBlock.begin(), StereoSampleCapture::framesPerSlot, 1.1F);
            std::fill(splitBlock.begin()
                    + static_cast<std::ptrdiff_t>(StereoSampleCapture::framesPerSlot),
                splitBlock.end(), 0.1F);
            coordinator.captureAudioBlock(
                splitBlock.data(), splitBlock.data(), splitBlock.size(), 48'000.0);

            VisualizationFrame frame;
            expect(waitForFrame(
                coordinator, frame, [](const auto& candidate) { return candidate.meterValid; }));
            expect(!frame.over[0] && !frame.over[1]);
            expectWithinAbsoluteError(frame.heldPeakDecibels[0], -20.0F, 0.01F);
            expect(coordinator.telemetry().capture.reclaimedReadyChunks > 0);
        }

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
        runCaptureBoundaryRolloverTests();

        beginTest("A rejected gap boundary is retried before post-gap state");
        {
            AnalysisCoordinator coordinator;
            coordinator.setSpectrumAnalysisConfiguration({ 16384, FftWindow::periodicHann, 60 });
            coordinator.setVisualizationActive(true);

            std::array<float, maximumFftSize> warmup { };
            warmup.fill(0.25F);
            coordinator.captureAudioBlock(warmup.data(), warmup.data(), warmup.size(), 48'000.0);
            VisualizationFrame beforeGap;
            expect(waitForFrame(coordinator, beforeGap,
                [](const auto& candidate) { return candidate.spectrumValid; }));
            while (coordinator.copyLatestVisualizationFrame(beforeGap)) { }

            coordinator.failNextFramePublicationForTesting();
            constexpr std::array<float, 64> shortChunk { };
            for (std::size_t index = 0; index < captureCallsThroughFirstPackedOverflow<64>();
                ++index) {
                coordinator.captureAudioBlock(
                    shortChunk.data(), shortChunk.data(), shortChunk.size(), 48'000.0);
            }

            const auto dropsBefore = coordinator.telemetry().droppedFramePublications;
            const auto failureDeadline = std::chrono::steady_clock::now() + 2s;
            while (coordinator.telemetry().droppedFramePublications == dropsBefore
                && std::chrono::steady_clock::now() < failureDeadline) {
                coordinator.requestAnalysis();
                std::this_thread::sleep_for(1ms);
            }
            expect(coordinator.telemetry().droppedFramePublications == dropsBefore + 1);

            VisualizationFrame rejected;
            expect(!coordinator.copyLatestVisualizationFrame(rejected),
                "A pre-gap frame crossed the new-generation copy fence");

            VisualizationFrame afterGap;
            expect(waitForFrame(
                coordinator, afterGap, [generation = beforeGap.generation](const auto& candidate) {
                    return candidate.generation > generation && !candidate.spectrumValid;
                }));
            expect(afterGap.generation == coordinator.telemetry().captureGeneration);
        }

        beginTest("A format restart supersedes a racing gap rollover");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(48'000.0, 2);
            coordinator.setVisualizationActive(true);

            VisualizationFrame activation;
            expect(coordinator.copyLatestVisualizationFrame(activation));

            WorkerHookBarrier barrier(
                AnalysisCoordinator::WorkerTestOperation::beforeFramePublication);
            coordinator.setWorkerTestHook(&barrier, &WorkerHookBarrier::invoke);

            std::array<float, StereoSampleCapture::framesPerSlot> block { };
            block.fill(0.25F);
            for (std::size_t index = 0; index < StereoSampleCapture::slotCount + 2; ++index)
                coordinator.captureAudioBlock(block.data(), block.data(), block.size(), 48'000.0);

            auto reachedGapBoundary = false;
            const auto boundaryDeadline = std::chrono::steady_clock::now() + 2s;
            while (!reachedGapBoundary && std::chrono::steady_clock::now() < boundaryDeadline) {
                coordinator.requestAnalysis();
                reachedGapBoundary = barrier.waitUntilEntered(2ms);
            }
            expect(reachedGapBoundary, "The worker did not reach the gap boundary publication");
            const auto racingGapGeneration = coordinator.telemetry().captureGeneration;
            expect(racingGapGeneration > activation.generation);

            std::thread formatRestart(
                [&coordinator] { coordinator.setCaptureFormat(96'000.0, 2); });
            const auto closeDeadline = std::chrono::steady_clock::now() + 2s;
            while (coordinator.isVisualizationActive()
                && std::chrono::steady_clock::now() < closeDeadline) {
                std::this_thread::yield();
            }
            expect(!coordinator.isVisualizationActive(),
                "The format restart did not close capture before waiting for the worker");

            barrier.release();
            formatRestart.join();
            coordinator.setWorkerTestHook(nullptr, nullptr);

            VisualizationFrame restarted;
            expect(coordinator.copyLatestVisualizationFrame(restarted));
            expect(restarted.generation > racingGapGeneration);
            expect(!restarted.spectrumValid);
            expect(!restarted.meterValid);
            expect(!restarted.stereoFieldValid);
            expect(!restarted.loudnessMomentaryValid);
            expect(!restarted.loudnessShortTermValid);
            expect(!restarted.loudnessIntegratedValid);
            expect(coordinator.telemetry().captureGeneration == restarted.generation);

            SpectrogramColumn restartMarker;
            expect(coordinator.copyNextSpectrogramColumn(restartMarker));
            expect(restartMarker.resetMarker);
            expect(restartMarker.captureGeneration == restarted.generation);
        }

        beginTest("Deactivation linearizes before a renderer request waiting at the gate");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);
            constexpr std::array<float, 64> samples { };
            coordinator.captureAudioBlock(samples.data(), samples.data(), samples.size(), 48'000.0);

            LifecycleHookBarrier barrier(AnalysisCoordinator::LifecycleTestOperation::deactivate);
            coordinator.setLifecycleTestHook(&barrier, &LifecycleHookBarrier::invoke);

            std::thread closing([&coordinator] { coordinator.setVisualizationActive(false); });
            const auto closeHasGate = barrier.waitUntilEntered();
            expect(closeHasGate);

            std::atomic<bool> requestStarted { false };
            std::atomic<bool> requestReturned { false };
            std::thread requester([&] {
                requestStarted.store(true, std::memory_order_release);
                coordinator.requestAnalysis();
                requestReturned.store(true, std::memory_order_release);
            });

            while (!requestStarted.load(std::memory_order_acquire))
                std::this_thread::yield();
            expect(!requestReturned.load(std::memory_order_acquire));

            barrier.release();
            closing.join();
            requester.join();
            coordinator.setLifecycleTestHook(nullptr, nullptr);

            const auto telemetry = coordinator.telemetry();
            expect(!coordinator.isVisualizationActive());
            expect(telemetry.scheduler.submitted == 0,
                "A renderer request slipped in after deactivation drained the client");
        }

        beginTest("A request linearized before deactivation is cancelled and drained");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);
            constexpr std::array<float, 64> samples { };
            coordinator.captureAudioBlock(samples.data(), samples.data(), samples.size(), 48'000.0);

            LifecycleHookBarrier barrier(AnalysisCoordinator::LifecycleTestOperation::request);
            coordinator.setLifecycleTestHook(&barrier, &LifecycleHookBarrier::invoke);

            std::thread requester([&coordinator] { coordinator.requestAnalysis(); });
            const auto requestHasGate = barrier.waitUntilEntered();
            expect(requestHasGate);

            std::atomic<bool> closeStarted { false };
            std::atomic<bool> closeReturned { false };
            std::thread closing([&] {
                closeStarted.store(true, std::memory_order_release);
                coordinator.setVisualizationActive(false);
                closeReturned.store(true, std::memory_order_release);
            });

            while (!closeStarted.load(std::memory_order_acquire))
                std::this_thread::yield();
            expect(!closeReturned.load(std::memory_order_acquire));

            barrier.release();
            requester.join();
            closing.join();
            coordinator.setLifecycleTestHook(nullptr, nullptr);

            const auto afterClose = coordinator.telemetry();
            expect(!coordinator.isVisualizationActive());
            expect(afterClose.scheduler.submitted == 1);
            const auto submittedAfterClose = afterClose.scheduler.submitted;
            coordinator.requestAnalysis();
            expect(coordinator.telemetry().scheduler.submitted == submittedAfterClose);
        }
#endif
    }

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
private:
    void runCaptureBoundaryRolloverTests()
    {
        runIndependentCaptureBoundaryGateCase(true);
        runIndependentCaptureBoundaryGateCase(false);
        runSustainedOverflowCaptureBoundaryCase();
        runRawAcquisitionRetentionAndCancellationCase();
        runMeterAcquisitionRetentionCase();
        runRecoveryPublicationRetryCase();
        runRecoveryRetryPreservesLaterCaptureCase();
        runPendingRecoverySuppressesStaleClearCase();
        runBoundaryPublicationCancellationCase();
        runBoundaryConfigurationSurvivalCase();
        runStaleBoundaryAcknowledgementCase(
            AnalysisCoordinator::WorkerTestOperation::beforeFrameBoundaryAcknowledgement);
        runStaleBoundaryAcknowledgementCase(
            AnalysisCoordinator::WorkerTestOperation::beforeSpectrogramBoundaryAcknowledgement);
    }

    void runIndependentCaptureBoundaryGateCase(const bool deliverFrameFirst)
    {
        beginTest(deliverFrameFirst
                ? "Capture recovery waits independently for the Spectrogram boundary marker"
                : "Capture recovery waits independently for the visualization boundary frame");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> left { };
        std::array<float, 64> right { };
        left.fill(0.25F);
        right.fill(-0.125F);

        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, left, right, captureCallsThroughFirstPackedOverflow<64>());

        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));
        expect(boundaryGeneration == beforeBoundary.captureGeneration + 1);

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        if (deliverFrameFirst) {
            expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        } else {
            expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));
        }

        constexpr auto deferredRequests = std::uint64_t { 4 };
        const auto beforeDeferredRequests = coordinator.telemetry();
        for (auto request = std::uint64_t { 0 }; request < deferredRequests; ++request)
            coordinator.requestAnalysis();
        const auto afterDeferredRequests = coordinator.telemetry();
        expect(afterDeferredRequests.captureBoundaryRequestsDeferred
            == beforeDeferredRequests.captureBoundaryRequestsDeferred + deferredRequests);
        expect(afterDeferredRequests.emptyAnalysisRequestsAvoided
            == beforeDeferredRequests.emptyAnalysisRequestsAvoided);
        expect(afterDeferredRequests.publishedFrames == beforeDeferredRequests.publishedFrames,
            "Post-gap state was published while one renderer boundary was withheld");

        if (deliverFrameFirst) {
            expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));
        } else {
            expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        }
        expect(isFullyInvalidCaptureBoundary(boundaryFrame));
        expect(boundaryMarker.captureBoundary && boundaryMarker.resetMarker);

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, boundaryGeneration, recovered));
        expect(recovered.meterValid);
        expect(recovered.stereoFieldValid);

        const auto beforeEmptyRequest = coordinator.telemetry();
        coordinator.requestAnalysis();
        const auto afterEmptyRequest = coordinator.telemetry();
        expect(afterEmptyRequest.emptyAnalysisRequestsAvoided
            == beforeEmptyRequest.emptyAnalysisRequestsAvoided + 1);
        expect(afterEmptyRequest.captureBoundaryRequestsDeferred
            == beforeEmptyRequest.captureBoundaryRequestsDeferred);
    }

    void runSustainedOverflowCaptureBoundaryCase()
    {
        beginTest("Sustained overflow remains one episode until post-gap recovery publishes");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> signal { };
        signal.fill(0.2F);
        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());

        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));

        for (std::size_t burst = 0; burst < 3; ++burst) {
            captureRepeated(
                coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());
        }
        const auto capturedFrameEnd = coordinator.telemetry().capture.capturedFrames;
        expect(coordinator.telemetry().captureGeneration == boundaryGeneration,
            "Overflow churn advanced the public generation while the first fence was pending");

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, boundaryGeneration, recovered));
        expect(recovered.meterValid);
        expect(recovered.capturedFrameEnd == capturedFrameEnd);
        expect(recovered.generation == beforeBoundary.captureGeneration + 1);
        expect(coordinator.telemetry().captureGeneration == boundaryGeneration);

        for (std::size_t request = 0; request < 4; ++request)
            coordinator.requestAnalysis();
        expect(coordinator.telemetry().captureGeneration == boundaryGeneration,
            "The completed overflow episode reopened without another audio block");
    }

    void runRawAcquisitionRetentionAndCancellationCase()
    {
        beginTest("A post-gap raw chunk acquired across a gap survives boundary delivery and "
                  "worker cancellation");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        WorkerHookBarrier acquisitionBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeRawAcquisition);
        coordinator.setWorkerTestHook(&acquisitionBarrier, &WorkerHookBarrier::invoke);

        constexpr std::array<float, 64> silence { };
        coordinator.captureAudioBlock(silence.data(), silence.data(), silence.size(), 48'000.0, 2);
        coordinator.requestAnalysis();
        const auto reachedAcquisition = acquisitionBarrier.waitUntilEntered();
        expect(reachedAcquisition, "The worker did not reach the raw acquisition seam");

        captureRepeated(coordinator, silence, silence, captureCallsToFillRawQueue<64>() - 1);
        std::array<float, 64> retainedLeft { };
        std::array<float, 64> retainedRight { };
        retainedLeft.fill(0.8F);
        retainedRight.fill(-0.8F);
        coordinator.captureAudioBlock(
            retainedLeft.data(), retainedRight.data(), retainedLeft.size(), 48'000.0, 2);
        captureRepeated(
            coordinator, retainedLeft, retainedRight, captureCallsToFillRawQueue<64>() - 1);

        const auto beforeBoundary = coordinator.telemetry();
        acquisitionBarrier.release();
        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));
        coordinator.setWorkerTestHook(nullptr, nullptr);

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));

        WorkerHookBarrier cancellationBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeMeterConsumption);
        coordinator.setWorkerTestHook(&cancellationBarrier, &WorkerHookBarrier::invoke);
        const auto processedRetainedRaw = waitUntil([&] {
            coordinator.requestAnalysis();
            return cancellationBarrier.waitUntilEntered(2ms);
        });
        expect(processedRetainedRaw,
            "The recovery worker did not process retained raw input before cancellation");

        const auto jobsStoppedBeforeCancellation = coordinator.telemetry().jobsStopped;
        std::atomic<bool> reconfigurationStarted { false };
        std::atomic<bool> reconfigurationReturned { false };
        std::thread reconfigure([&] {
            reconfigurationStarted.store(true, std::memory_order_release);
            coordinator.setSpectrumAnalysisConfiguration(
                { 2048, FftWindow::fourTermBlackmanHarris, 60 });
            reconfigurationReturned.store(true, std::memory_order_release);
        });
        expect(waitUntil([&] { return reconfigurationStarted.load(std::memory_order_acquire); }));
        std::this_thread::sleep_for(10ms);
        expect(!reconfigurationReturned.load(std::memory_order_acquire),
            "Reconfiguration returned while the recovery worker was blocked");

        cancellationBarrier.release();
        reconfigure.join();
        coordinator.setWorkerTestHook(nullptr, nullptr);
        expect(reconfigurationReturned.load(std::memory_order_acquire));
        expect(coordinator.telemetry().jobsStopped > jobsStoppedBeforeCancellation);

        VisualizationFrame recovered;
        const auto recoveredFromReconfigurationRequest
            = coordinator.copyLatestVisualizationFrame(recovered);
        if (!recoveredFromReconfigurationRequest) {
            expect(waitForCaptureBoundaryRecovery(coordinator, boundaryGeneration, recovered));
        }
        expect(!recovered.captureBoundary && recovered.generation == boundaryGeneration,
            "Cancellation or reconfiguration published partial boundary state");
        expect(recovered.stereoFieldValid);
        const auto retainedPointWasPublished = std::any_of(recovered.stereoFieldPoints.begin(),
            recovered.stereoFieldPoints.begin()
                + static_cast<std::ptrdiff_t>(recovered.stereoFieldPointCount),
            [](const auto& point) { return std::abs(point.horizontal) > 0.7F; });
        expect(retainedPointWasPublished,
            "The destructively acquired raw chunk was absent from the recovered point cloud");
        expect(recovered.generation == boundaryGeneration);
    }

    void runMeterAcquisitionRetentionCase()
    {
        beginTest("A meter endpoint acquired across a gap survives without later audio");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        WorkerHookBarrier acquisitionBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeMeterAcquisition);
        coordinator.setWorkerTestHook(&acquisitionBarrier, &WorkerHookBarrier::invoke);

        constexpr std::array<float, 64> silence { };
        coordinator.captureAudioBlock(silence.data(), silence.data(), silence.size(), 48'000.0, 2);
        coordinator.requestAnalysis();
        const auto reachedAcquisition = acquisitionBarrier.waitUntilEntered();
        expect(reachedAcquisition, "The worker did not reach the meter acquisition seam");

        captureRepeated(coordinator, silence, silence, captureCallsToFillRawQueue<64>());
        std::array<float, 64> finalSignal { };
        finalSignal.fill(0.5F);
        coordinator.captureAudioBlock(
            finalSignal.data(), finalSignal.data(), finalSignal.size(), 48'000.0, 2);

        const auto beforeBoundary = coordinator.telemetry();
        acquisitionBarrier.release();
        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));
        coordinator.setWorkerTestHook(nullptr, nullptr);

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, boundaryGeneration, recovered));
        expect(recovered.meterValid,
            "The destructively consumed post-gap meter endpoint was not recovered");
        expectWithinAbsoluteError(recovered.peakDecibels[0], -6.0206F, 0.02F);
        expect(recovered.generation == boundaryGeneration);
    }

    void runRecoveryPublicationRetryCase()
    {
        beginTest("A rejected recovery frame retries without acknowledging the overflow episode");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> signal { };
        signal.fill(0.3F);
        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());

        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));

        const auto droppedBeforeRecovery = coordinator.telemetry().droppedFramePublications;
        coordinator.failNextFramePublicationForTesting();
        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().droppedFramePublications == droppedBeforeRecovery + 1;
        }));

        VisualizationFrame rejectedRecovery;
        expect(!coordinator.copyLatestVisualizationFrame(rejectedRecovery));
        expect(coordinator.telemetry().captureGeneration == boundaryGeneration);

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, boundaryGeneration, recovered));
        expect(recovered.meterValid);
        expect(coordinator.telemetry().droppedFramePublications == droppedBeforeRecovery + 1);
        expect(coordinator.telemetry().captureGeneration == boundaryGeneration);
    }

    void runRecoveryRetryPreservesLaterCaptureCase()
    {
        beginTest("A cached recovery retry leaves later captured audio pending for follow-up");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> recoverySignal { };
        recoverySignal.fill(0.25F);
        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, recoverySignal, recoverySignal,
            captureCallsThroughFirstPackedOverflow<64>());

        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));

        WorkerHookBarrier recoveryPublicationBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeFramePublication);
        coordinator.setWorkerTestHook(&recoveryPublicationBarrier, &WorkerHookBarrier::invoke);
        coordinator.failNextFramePublicationForTesting();

        const auto beforeFailedRecovery = coordinator.telemetry();
        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().scheduler.submitted
                > beforeFailedRecovery.scheduler.submitted;
        }));
        expect(recoveryPublicationBarrier.waitUntilEntered(),
            "Recovery did not reach publication after applying its retained input");
        expect(coordinator.telemetry().stereoFieldProcessedChunks
                > beforeFailedRecovery.stereoFieldProcessedChunks,
            "Recovery reached publication without applying retained input");

        recoveryPublicationBarrier.release();
        expect(waitUntil([&] {
            const auto telemetry = coordinator.telemetry();
            return telemetry.droppedFramePublications
                == beforeFailedRecovery.droppedFramePublications + 1
                && telemetry.jobsStopped > beforeFailedRecovery.jobsStopped;
        }));
        coordinator.setWorkerTestHook(nullptr, nullptr);

        std::array<float, 64> laterSignal { };
        laterSignal.fill(-0.75F);
        coordinator.captureAudioBlock(
            laterSignal.data(), laterSignal.data(), laterSignal.size(), 48'000.0, 2);
        const auto afterLaterCapture = coordinator.telemetry();
        const auto laterCapturedFrameEnd = afterLaterCapture.capture.capturedFrames;
        expect(afterLaterCapture.latestCaptureRevision
            > afterLaterCapture.lastAnalyzedCaptureRevision);

        const auto beforeRetry = coordinator.telemetry();
        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().scheduler.submitted > beforeRetry.scheduler.submitted;
        }));
        expect(waitUntil([&] {
            const auto telemetry = coordinator.telemetry();
            return telemetry.publishedFrames > beforeRetry.publishedFrames
                && telemetry.jobsCompleted > beforeRetry.jobsCompleted;
        }));

        VisualizationFrame retriedRecovery;
        expect(coordinator.copyLatestVisualizationFrame(retriedRecovery));
        expect(!retriedRecovery.captureBoundary);
        expect(retriedRecovery.generation == boundaryGeneration);
        expect(retriedRecovery.capturedFrameEnd < laterCapturedFrameEnd,
            "The cached recovery frame unexpectedly claimed the later queued audio");

        const auto afterRetry = coordinator.telemetry();
        expect(afterRetry.lastAnalyzedCaptureRevision < afterRetry.latestCaptureRevision,
            "The cached retry incorrectly marked the later capture revision analyzed");

        const auto beforeFollowUp = coordinator.telemetry();
        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().scheduler.submitted > beforeFollowUp.scheduler.submitted;
        }));
        expect(waitUntil([&] {
            const auto telemetry = coordinator.telemetry();
            return telemetry.publishedFrames > beforeFollowUp.publishedFrames
                && telemetry.jobsCompleted > beforeFollowUp.jobsCompleted;
        }));

        VisualizationFrame followUp;
        expect(coordinator.copyLatestVisualizationFrame(followUp));
        expect(!followUp.captureBoundary);
        expect(followUp.generation == boundaryGeneration);
        expect(followUp.capturedFrameEnd >= laterCapturedFrameEnd,
            "Later queued audio did not receive a follow-up analysis publication");

        const auto afterFollowUp = coordinator.telemetry();
        expect(afterFollowUp.capture.capturedFrames == laterCapturedFrameEnd,
            "The follow-up relied on still newer audio");
        expect(afterFollowUp.staleFramesPublished == afterLaterCapture.staleFramesPublished,
            "The follow-up relied on the stale-input timeout");
    }

    void runPendingRecoverySuppressesStaleClearCase()
    {
        beginTest("Pending recovery suppresses stale clear without disabling later staleness");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> signal { };
        signal.fill(0.4F);
        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());

        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));

        WorkerHookBarrier failedPublicationBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeFramePublication);
        coordinator.setWorkerTestHook(&failedPublicationBarrier, &WorkerHookBarrier::invoke);
        coordinator.failNextFramePublicationForTesting();

        const auto beforeFailedRecovery = coordinator.telemetry();
        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().scheduler.submitted
                > beforeFailedRecovery.scheduler.submitted;
        }));
        expect(failedPublicationBarrier.waitUntilEntered(),
            "Recovery did not reach publication after applying valid input");
        expect(coordinator.telemetry().stereoFieldProcessedChunks
                > beforeFailedRecovery.stereoFieldProcessedChunks,
            "The failed recovery publication had not applied valid input");

        failedPublicationBarrier.release();
        expect(waitUntil([&] {
            const auto telemetry = coordinator.telemetry();
            return telemetry.droppedFramePublications
                == beforeFailedRecovery.droppedFramePublications + 1
                && telemetry.jobsStopped > beforeFailedRecovery.jobsStopped;
        }));
        coordinator.setWorkerTestHook(nullptr, nullptr);

        const auto pendingRecovery = coordinator.telemetry();
        std::this_thread::sleep_for(300ms);

        WorkerHookBarrier retryPublicationBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeFramePublication);
        coordinator.setWorkerTestHook(&retryPublicationBarrier, &WorkerHookBarrier::invoke);
        const auto beforeStalePolling = coordinator.telemetry();
        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().scheduler.submitted
                > beforeStalePolling.scheduler.submitted;
        }));
        expect(retryPublicationBarrier.waitUntilEntered(),
            "The cached recovery retry did not reach publication");

        expect(waitUntil([&] {
            coordinator.requestAnalysis();
            return coordinator.telemetry().scheduler.submitted
                >= beforeStalePolling.scheduler.submitted + 2;
        }));
        const auto whileRetryBlocked = coordinator.telemetry();
        expect(whileRetryBlocked.captureGeneration == boundaryGeneration);
        expect(whileRetryBlocked.publishedFrames == beforeStalePolling.publishedFrames);
        expect(whileRetryBlocked.staleFramesPublished == pendingRecovery.staleFramesPublished,
            "Polling armed and published a stale frame while recovery was pending");

        retryPublicationBarrier.release();
        expect(waitUntil([&] {
            const auto telemetry = coordinator.telemetry();
            return telemetry.jobsCompleted >= beforeStalePolling.jobsCompleted + 2;
        }));
        coordinator.setWorkerTestHook(nullptr, nullptr);

        VisualizationFrame recovered;
        expect(coordinator.copyLatestVisualizationFrame(recovered));
        expect(!recovered.captureBoundary && recovered.generation == boundaryGeneration);
        expect(recovered.meterValid);

        const auto afterRecovery = coordinator.telemetry();
        expect(afterRecovery.publishedFrames == beforeStalePolling.publishedFrames + 1,
            "A stale frame followed the cached recovery publication");
        expect(afterRecovery.staleFramesPublished == pendingRecovery.staleFramesPublished,
            "A pending recovery request was misclassified as stale work");

        VisualizationFrame ordinaryStaleClear;
        expect(waitForFrame(
            coordinator, ordinaryStaleClear,
            [sequence = recovered.meterSequence](const auto& candidate) {
                return candidate.meterSequence > sequence && !candidate.spectrumValid
                    && isDisplayFloor(candidate.peakDecibels[0])
                    && isDisplayFloor(candidate.rmsDecibels[0]);
            },
            1s));
        expect(coordinator.telemetry().staleFramesPublished
                == pendingRecovery.staleFramesPublished + 1,
            "Recovery permanently suppressed ordinary stale-input scheduling");
    }

    void runBoundaryPublicationCancellationCase()
    {
        beginTest("Settings cancellation resumes one committed capture-boundary fence");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> signal { };
        signal.fill(0.2F);
        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());

        WorkerHookBarrier publicationBarrier(
            AnalysisCoordinator::WorkerTestOperation::beforeFramePublication);
        coordinator.setWorkerTestHook(&publicationBarrier, &WorkerHookBarrier::invoke);
        coordinator.requestAnalysis();
        expect(publicationBarrier.waitUntilEntered(),
            "The initial capture-boundary frame did not reach its publication seam");

        const auto whilePublicationPaused = coordinator.telemetry();
        const auto committedBoundaryGeneration = whilePublicationPaused.captureGeneration;
        expect(committedBoundaryGeneration > beforeBoundary.captureGeneration,
            "The capture generation was not committed before boundary publication");
        expect(whilePublicationPaused.publishedFrames == beforeBoundary.publishedFrames);

        std::atomic<bool> settingsStarted { false };
        std::atomic<bool> settingsReturned { false };
        std::thread reconfigure([&] {
            settingsStarted.store(true, std::memory_order_release);
            coordinator.setSpectrumAnalysisConfiguration(
                { 2048, FftWindow::fourTermBlackmanHarris, 60 });
            settingsReturned.store(true, std::memory_order_release);
        });
        expect(waitUntil([&] { return settingsStarted.load(std::memory_order_acquire); }));
        std::this_thread::sleep_for(10ms);
        expect(!settingsReturned.load(std::memory_order_acquire),
            "Settings returned while the boundary publication worker was blocked");

        publicationBarrier.release();
        reconfigure.join();
        coordinator.setWorkerTestHook(nullptr, nullptr);
        expect(settingsReturned.load(std::memory_order_acquire));

        const auto afterSettings = coordinator.telemetry();
        expect(afterSettings.jobsStopped > whilePublicationPaused.jobsStopped,
            "Settings did not cancel the paused boundary-publication job");
        expect(afterSettings.captureGeneration == committedBoundaryGeneration,
            "Settings replaced the already committed capture generation");
        expect(afterSettings.configuredFftSize == 2048);

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, committedBoundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, committedBoundaryGeneration, boundaryMarker));
        expect(isFullyInvalidCaptureBoundary(boundaryFrame));
        expect(boundaryFrame.generation != beforeBoundary.captureGeneration,
            "A tagged old-generation frame escaped the cancelled publication");
        expect(boundaryMarker.captureGeneration == committedBoundaryGeneration);

        const auto afterFenceDelivery = coordinator.telemetry();
        expect(afterFenceDelivery.captureGeneration == committedBoundaryGeneration);
        expect(afterFenceDelivery.publishedFrames == beforeBoundary.publishedFrames + 1,
            "Cancellation leaked an extra boundary frame before the committed fence resumed");

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, committedBoundaryGeneration, recovered));
        expect(recovered.generation == committedBoundaryGeneration);
        expect(recovered.meterValid);
    }

    void runBoundaryConfigurationSurvivalCase()
    {
        beginTest("Pending capture fences survive renderer discard and analyzer reconfiguration");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> signal { };
        signal.fill(0.2F);
        const auto beforeBoundary = coordinator.telemetry();
        captureRepeated(coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());

        auto boundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeBoundary.captureGeneration,
            beforeBoundary.publishedFrames, boundaryGeneration));
        const auto boundaryPublishedFrames = coordinator.telemetry().publishedFrames;

        coordinator.discardPendingSpectrogramColumns();
        coordinator.setSpectrumAnalysisConfiguration({ 2048, FftWindow::fiveTermFlatTop, 120 });
        coordinator.setSpectrumTemporalConfiguration(
            { 0.0, 0.0, SpectrumPeakHoldMode::finite, 1.0 });
        coordinator.setSpectrogramFrequencySpacing(0.35);

        const auto afterConfiguration = coordinator.telemetry();
        expect(afterConfiguration.captureGeneration == boundaryGeneration);
        expect(afterConfiguration.publishedFrames == boundaryPublishedFrames,
            "A configuration snapshot replaced the pending tagged boundary frame");

        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        expect(copyCaptureBoundaryFrame(coordinator, boundaryGeneration, boundaryFrame));
        expect(copyCaptureBoundaryMarker(coordinator, boundaryGeneration, boundaryMarker));
        expect(isFullyInvalidCaptureBoundary(boundaryFrame));

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, boundaryGeneration, recovered));
        expect(recovered.meterValid);
        expect(recovered.generation == boundaryGeneration);
        expect(coordinator.telemetry().configuredFftSize == 2048);
    }

    void runStaleBoundaryAcknowledgementCase(
        const AnalysisCoordinator::WorkerTestOperation acknowledgementOperation)
    {
        const auto testsFrameAcknowledgement = acknowledgementOperation
            == AnalysisCoordinator::WorkerTestOperation::beforeFrameBoundaryAcknowledgement;
        beginTest(testsFrameAcknowledgement
                ? "A stale frame acknowledgement cannot clear a restarted generation fence"
                : "A stale Spectrogram acknowledgement cannot clear a restarted generation fence");

        AnalysisCoordinator coordinator;
        coordinator.setCaptureFormat(48'000.0, 2);
        coordinator.setVisualizationActive(true);
        drainRendererState(coordinator);

        std::array<float, 64> signal { };
        signal.fill(0.2F);
        const auto beforeFirstBoundary = coordinator.telemetry();
        captureRepeated(coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>());

        auto firstBoundaryGeneration = std::uint64_t { 0 };
        expect(waitForCaptureBoundaryPublication(coordinator, beforeFirstBoundary.captureGeneration,
            beforeFirstBoundary.publishedFrames, firstBoundaryGeneration));

        WorkerHookBarrier acknowledgementBarrier(acknowledgementOperation);
        coordinator.setWorkerTestHook(&acknowledgementBarrier, &WorkerHookBarrier::invoke);
        std::atomic<bool> staleCopySucceeded { false };
        VisualizationFrame staleFrame;
        SpectrogramColumn staleMarker;
        std::thread staleRenderer([&] {
            const auto copied = testsFrameAcknowledgement
                ? coordinator.copyLatestVisualizationFrame(staleFrame)
                : coordinator.copyNextSpectrogramColumn(staleMarker);
            staleCopySucceeded.store(copied, std::memory_order_release);
        });
        const auto acknowledgementPaused = acknowledgementBarrier.waitUntilEntered();
        expect(acknowledgementPaused,
            "The renderer acknowledgement did not reach the stale-token seam");

        coordinator.setCaptureFormat(96'000.0, 2);
        const auto restartedGeneration = coordinator.telemetry().captureGeneration;
        expect(restartedGeneration > firstBoundaryGeneration);
        drainRendererState(coordinator);

        const auto beforeSecondBoundary = coordinator.telemetry();
        captureRepeated(
            coordinator, signal, signal, captureCallsThroughFirstPackedOverflow<64>(), 96'000.0);
        auto secondBoundaryGeneration = std::uint64_t { 0 };
        expect(
            waitForCaptureBoundaryPublication(coordinator, beforeSecondBoundary.captureGeneration,
                beforeSecondBoundary.publishedFrames, secondBoundaryGeneration));

        acknowledgementBarrier.release();
        staleRenderer.join();
        coordinator.setWorkerTestHook(nullptr, nullptr);
        expect(staleCopySucceeded.load(std::memory_order_acquire));
        if (testsFrameAcknowledgement)
            expect(staleFrame.generation == firstBoundaryGeneration);
        else
            expect(staleMarker.captureGeneration == firstBoundaryGeneration);

        VisualizationFrame secondBoundaryFrame;
        SpectrogramColumn secondBoundaryMarker;
        if (testsFrameAcknowledgement) {
            expect(copyCaptureBoundaryMarker(
                coordinator, secondBoundaryGeneration, secondBoundaryMarker));
        } else {
            expect(copyCaptureBoundaryFrame(
                coordinator, secondBoundaryGeneration, secondBoundaryFrame));
        }

        constexpr auto deferredRequests = std::uint64_t { 3 };
        const auto beforeDeferredRequests = coordinator.telemetry();
        for (auto request = std::uint64_t { 0 }; request < deferredRequests; ++request)
            coordinator.requestAnalysis();
        const auto afterDeferredRequests = coordinator.telemetry();
        expect(afterDeferredRequests.captureBoundaryRequestsDeferred
                == beforeDeferredRequests.captureBoundaryRequestsDeferred + deferredRequests,
            "The stale acknowledgement cleared the newer generation's delivery token");
        expect(afterDeferredRequests.publishedFrames == beforeDeferredRequests.publishedFrames);

        if (testsFrameAcknowledgement) {
            expect(copyCaptureBoundaryFrame(
                coordinator, secondBoundaryGeneration, secondBoundaryFrame));
        } else {
            expect(copyCaptureBoundaryMarker(
                coordinator, secondBoundaryGeneration, secondBoundaryMarker));
        }
        expect(isFullyInvalidCaptureBoundary(secondBoundaryFrame));

        VisualizationFrame recovered;
        expect(waitForCaptureBoundaryRecovery(coordinator, secondBoundaryGeneration, recovered));
        expect(recovered.generation == secondBoundaryGeneration);
        expect(recovered.meterValid);
    }
#endif
};

AnalysisCoordinatorTests analysisCoordinatorTests;
} // namespace
} // namespace audio_insight
