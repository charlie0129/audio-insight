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

template <typename Predicate>
bool waitForFrame(AnalysisCoordinator& coordinator, VisualizationFrame& frame,
    Predicate&& predicate, const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        coordinator.requestAnalysis();
        if (coordinator.copyLatestVisualizationFrame(frame) && predicate(frame))
            return true;

        std::this_thread::sleep_for(2ms);
    }

    return false;
}

template <typename Predicate>
bool waitForFrameWithoutRequest(AnalysisCoordinator& coordinator, VisualizationFrame& frame,
    Predicate&& predicate, const std::chrono::milliseconds timeout = 500ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (coordinator.copyLatestVisualizationFrame(frame) && predicate(frame))
            return true;

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
    std::array<float, loudnessMeasurementPeriodFrames> signal { };

    for (std::size_t period = 0; period < periodCount; ++period) {
        fillLoudnessTone(signal, peakDecibels, toneFrameCursor);
        const auto expectedCapturedFrameEnd
            = coordinator.telemetry().capture.capturedFrames + signal.size();
        coordinator.captureAudioBlock(signal.data(), channelCount == 2 ? signal.data() : nullptr,
            signal.size(), loudnessTestSampleRate, channelCount);
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
            expect(activeTelemetry.capture.attemptedChunks == 2);
            expect(activeTelemetry.meters.attemptedBlocks == 2);
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
            expect(activeTelemetry.stereoFieldProcessedChunks == 2);
            expect(activeTelemetry.stereoFieldProcessedFrames == fftSize);
            expect(activeTelemetry.stereoFieldSelectedPoints == frame.stereoFieldPointCount);
            expect(activeTelemetry.stereoCorrelationProcessedSamples == fftSize);
            expect(activeTelemetry.stereoCorrelationPublishedEndpoints == 2);
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

        beginTest("Coordinator publishes standards-based M, S, and I Loudness snapshots");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
            coordinator.setVisualizationActive(true);

            auto toneFrameCursor = std::uint64_t { 0 };
            VisualizationFrame frame;
            expect(feedLoudnessTonePeriods(coordinator, 30, -23.0, toneFrameCursor, frame));
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
            expect(telemetry.loudness.measurementCompletions == 30);
            expect(telemetry.loudness.integrationBlockCompletions == 27);
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
            expect(feedLoudnessTonePeriods(coordinator, 30, -18.0, toneFrameCursor, beforeReset));
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
            expect(feedLoudnessTonePeriods(coordinator, 30, -10.0, toneFrameCursor, frame));
            expect(frame.loudnessIntegratedValid);

            std::array<float, loudnessMeasurementPeriodFrames * 2> queuedBeforeReset { };
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
            coordinator.captureAudioBlock(
                afterReset.data(), afterReset.data(), afterReset.size(), loudnessTestSampleRate, 2);

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

        beginTest("FFT and presentation mapping changes preserve Loudness state");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
            coordinator.setVisualizationActive(true);

            auto toneFrameCursor = std::uint64_t { 0 };
            VisualizationFrame beforeChanges;
            expect(feedLoudnessTonePeriods(coordinator, 30, -20.0, toneFrameCursor, beforeChanges));
            const auto loudnessBefore = coordinator.telemetry().loudnessMeasurement;

            coordinator.setSpectrumAnalysisConfiguration(
                { 2048, FftWindow::fourTermBlackmanHarris, 120 });
            coordinator.setSpectrumTemporalConfiguration(
                { true, 125.0, SpectrumPeakHoldMode::finite, 1.0 });
            coordinator.setSpectrogramFrequencySpacing(0.35);

            VisualizationFrame afterChanges;
            expect(waitForFrame(coordinator, afterChanges,
                [generation = beforeChanges.generation](const auto& candidate) {
                    return candidate.generation == generation && candidate.loudnessMomentaryValid
                        && candidate.loudnessShortTermValid && candidate.loudnessIntegratedValid;
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
            coordinator.captureAudioBlock(
                nextPeriod.data(), nextPeriod.data(), nextPeriod.size(), loudnessTestSampleRate, 2);
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

            std::array<float, 128> trigger { };
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
            for (std::size_t index = 0; index < StereoSampleCapture::slotCount + 4; ++index) {
                coordinator.captureAudioBlock(queuedBeforeReset.data(), queuedBeforeReset.data(),
                    queuedBeforeReset.size(), loudnessTestSampleRate, 2);
            }

            const auto resetBoundary = coordinator.telemetry().capture.capturedFrames;
            coordinator.resetLoudness();
            barrier.release();

            VisualizationFrame afterGap;
            expect(waitForFrame(
                coordinator, afterGap,
                [sequence = beforeGap.loudnessSequence, resetBoundary](const auto& candidate) {
                    return candidate.loudnessSequence > sequence
                        && candidate.capturedFrameEnd >= resetBoundary
                        && candidate.loudnessMomentaryValid;
                },
                2s));
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
                expect(invalidated.stereoCorrelationValid == beforeChange.stereoCorrelationValid);
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

            std::array<float, StereoSampleCapture::framesPerSlot> quieterSignal { };
            quieterSignal.fill(0.25F);
            coordinator.captureAudioBlock(
                quieterSignal.data(), quieterSignal.data(), quieterSignal.size(), 48'000.0);

            VisualizationFrame warmedUp;
            expect(waitForFrame(coordinator, warmedUp,
                [captureGeneration = beforeChange.generation,
                    fftGeneration = invalidated.fftGeneration](const auto& candidate) {
                    return candidate.generation == captureGeneration
                        && candidate.fftGeneration == fftGeneration && candidate.spectrumValid
                        && candidate.spectrumFftSize == 1024 && candidate.spectrumBinCount == 513;
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
            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0);

            VisualizationFrame beforeChange;
            expect(waitForFrame(coordinator, beforeChange, [](const auto& candidate) {
                return candidate.spectrumValid && candidate.meterValid && candidate.over[0];
            }));
            const auto telemetryBefore = coordinator.telemetry();

            coordinator.setSpectrumTemporalConfiguration(
                { true, 250.0, SpectrumPeakHoldMode::infinite, 2.0 });

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

            std::array<float, 800> nextHop { };
            nextHop.fill(0.25F);
            coordinator.captureAudioBlock(nextHop.data(), nextHop.data(), nextHop.size(), 48'000.0);

            VisualizationFrame warmed;
            expect(waitForFrame(coordinator, warmed,
                [sequence = invalidated.spectrumSequence](const auto& candidate) {
                    return candidate.spectrumSequence > sequence && candidate.spectrumValid
                        && candidate.spectrumPeakHoldValid;
                }));

            coordinator.resetSpectrum();
            coordinator.resetSpectrum();
            VisualizationFrame cleared;
            expect(waitForFrameWithoutRequest(
                coordinator, cleared, [sequence = warmed.spectrumSequence](const auto& candidate) {
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

            std::array<float, StereoSampleCapture::framesPerSlot> samples { };
            samples.fill(0.25F);
            coordinator.captureAudioBlock(samples.data(), samples.data(), samples.size(), 48'000.0);

            VisualizationFrame frame;
            expect(waitForFrame(
                coordinator, frame, [](const auto& candidate) { return candidate.spectrumValid; }));
            expect(frame.spectrumFftSize == 1024);
            expect(frame.spectrumBinCount == 513);

            const auto telemetry = coordinator.telemetry();
            expect(telemetry.capture.reclaimedReadyChunks == 0);
            expect(telemetry.capture.droppedIncomingChunks == 0);
            expect(telemetry.backlogDiscardedFrames == 0,
                "A single ordinary 2048-frame capture slot was truncated as backlog");
            expect(telemetry.spectrumTransforms == 1);
            expect(telemetry.spectrumCapturedFrameEnd == 1024);
            expect(telemetry.meterCapturedFrameEnd == StereoSampleCapture::framesPerSlot);
        }

        beginTest("A capture gap publishes a sequenced invalid Spectrum before warm-up");
        {
            AnalysisCoordinator coordinator;
            coordinator.setSpectrumAnalysisConfiguration({ 16384, FftWindow::periodicHann, 60 });
            coordinator.setVisualizationActive(true);

            std::array<float, maximumFftSize> initialSignal { };
            initialSignal.fill(0.25F);
            coordinator.captureAudioBlock(
                initialSignal.data(), initialSignal.data(), initialSignal.size(), 48'000.0);

            VisualizationFrame valid;
            expect(waitForFrame(
                coordinator, valid, [](const auto& candidate) { return candidate.spectrumValid; }));

            constexpr std::array<float, 64> shortChunk { };
            for (std::size_t chunk = 0; chunk < StereoSampleCapture::slotCount + 1; ++chunk) {
                coordinator.captureAudioBlock(
                    shortChunk.data(), shortChunk.data(), shortChunk.size(), 48'000.0);
            }

            VisualizationFrame invalidated;
            expect(waitForFrame(coordinator, invalidated,
                [capturedFrameEnd = valid.capturedFrameEnd](const auto& candidate) {
                    return candidate.capturedFrameEnd > capturedFrameEnd
                        && !candidate.spectrumValid;
                }));
            expect(invalidated.generation == valid.generation);
            expect(invalidated.fftGeneration == valid.fftGeneration);
            expect(invalidated.spectrumSequence > valid.spectrumSequence,
                "Capture-gap invalidation reused the preceding valid Spectrum sequence");
            expect(coordinator.telemetry().capture.reclaimedReadyChunks > 0);
        }

        beginTest("A meter-only endpoint gap preserves continuous Stereo field history");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, 192> left { };
            std::array<float, 192> right { };
            left.fill(0.5F);
            right.fill(-0.25F);
            coordinator.captureAudioBlock(left.data(), right.data(), left.size(), 48'000.0);

            VisualizationFrame first;
            expect(waitForFrame(coordinator, first, [expectedEnd = left.size()](const auto& frame) {
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
                coordinator.setSpectrumAnalysisConfiguration({ 1024, FftWindow::periodicHann, 15 });
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
            expect(
                telemetryAfter.fftConfigurationChanges == telemetryBefore.fftConfigurationChanges);
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
            const auto receivedSpectrum = waitForFrame(
                coordinator, frame, [](const auto& candidate) { return candidate.spectrumValid; });
            expect(receivedSpectrum, "The bounded backlog job did not publish its newest window");

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
            expect(
                freshnessTelemetry.meterCapturedFrameEnd == spectrumEndpoint + shortBlock.size());
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
                expectWithinAbsoluteError(frame.stereoFieldPoints[point].horizontal, 0.0F, 1.0e-7F);
        }

        beginTest("Sample-rate changes start a clean capture generation");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(48'000.0, 2);
            coordinator.setVisualizationActive(true);

            std::array<float, fftSize> signal { };
            signal.fill(0.25F);
            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0, 2);

            VisualizationFrame original;
            expect(waitForFrame(coordinator, original, [](const auto& candidate) {
                return candidate.spectrumValid && candidate.meterValid;
            }));

            const auto resetsBeforeFormatChange = coordinator.telemetry().loudness;

            coordinator.setCaptureFormat(96'000.0, 2);
            VisualizationFrame restarted;
            expect(waitForFrame(
                coordinator, restarted, [generation = original.generation](const auto& candidate) {
                    return candidate.generation > generation && !candidate.spectrumValid
                        && !candidate.meterValid && !candidate.stereoFieldValid
                        && !candidate.stereoCorrelationValid;
                }));

            constexpr std::array<float, 128> shortSignal { };
            coordinator.captureAudioBlock(
                shortSignal.data(), shortSignal.data(), shortSignal.size(), 96'000.0, 2);

            VisualizationFrame warming;
            expect(waitForFrame(
                coordinator, warming, [generation = restarted.generation](const auto& candidate) {
                    return candidate.generation == generation && candidate.meterValid
                        && !candidate.spectrumValid;
                }));
            expectWithinAbsoluteError(warming.sampleRate, 96'000.0, 0.001);

            const auto resetsAfterFormatChange = coordinator.telemetry().loudness;
            expect(
                resetsAfterFormatChange.formatResets == resetsBeforeFormatChange.formatResets + 1);
            expect(
                resetsAfterFormatChange.explicitResets == resetsBeforeFormatChange.explicitResets);

            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 96'000.0, 2);
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
            coordinator.captureAudioBlock(stereo.data(), stereo.data(), stereo.size(), 48'000.0, 2);

            VisualizationFrame original;
            expect(waitForFrame(coordinator, original, [](const auto& candidate) {
                return candidate.spectrumValid && candidate.channelCount == 2;
            }));

            coordinator.setCaptureFormat(48'000.0, 1);
            VisualizationFrame restarted;
            expect(waitForFrame(
                coordinator, restarted, [generation = original.generation](const auto& candidate) {
                    return candidate.generation > generation && !candidate.spectrumValid
                        && !candidate.meterValid;
                }));

            constexpr std::array<float, 128> mono { };
            coordinator.captureAudioBlock(mono.data(), nullptr, mono.size(), 48'000.0, 1);
            VisualizationFrame warming;
            expect(waitForFrame(
                coordinator, warming, [generation = restarted.generation](const auto& candidate) {
                    return candidate.generation == generation && candidate.meterValid
                        && candidate.channelCount == 1;
                }));
            expect(!warming.spectrumValid);
        }

        beginTest("Stale input clears M/S while preserving completed Integrated Loudness");
        {
            AnalysisCoordinator coordinator;
            coordinator.setCaptureFormat(loudnessTestSampleRate, 2);
            coordinator.setVisualizationActive(true);

            auto toneFrameCursor = std::uint64_t { 0 };
            VisualizationFrame live;
            expect(feedLoudnessTonePeriods(coordinator, 30, -23.0, toneFrameCursor, live));
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

            VisualizationFrame afterGap;
            expect(waitForFrame(
                coordinator, afterGap, [sequence = beforeGap.meterSequence](const auto& candidate) {
                    return candidate.meterSequence > sequence && !candidate.over[0];
                }));
            expect(!afterGap.over[0] && !afterGap.over[1]);
            expectWithinAbsoluteError(afterGap.heldPeakDecibels[0], -20.0F, 0.01F);
            const auto afterGapTelemetry = coordinator.telemetry();
            expect(afterGapTelemetry.capture.reclaimedReadyChunks > 0);
            expect(afterGapTelemetry.stereoFieldHistoryResets > historyResetsBeforeGap);
            expect(afterGap.stereoFieldValid);
            expect(afterGap.stereoCapturedFrameEnd == afterGapTelemetry.stereoCapturedFrameEnd);
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
};

AnalysisCoordinatorTests analysisCoordinatorTests;
} // namespace
} // namespace audio_insight
