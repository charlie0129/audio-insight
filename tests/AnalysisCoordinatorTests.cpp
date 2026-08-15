// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/AnalysisCoordinator.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numbers>
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

[[nodiscard]] bool isDisplayFloor(const float value) noexcept
{
    return std::abs(value - minimumDisplayDecibels) < 0.0001F;
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

    [[nodiscard]] bool waitUntilEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return entered; });
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
            expectWithinAbsoluteError(frame.rmsDecibels[0], -9.0309F, 0.02F);
            expectWithinAbsoluteError(frame.rmsDecibels[1], -15.0515F, 0.02F);

            const auto activeTelemetry = coordinator.telemetry();
            expect(activeTelemetry.capture.attemptedChunks == 2);
            expect(activeTelemetry.meters.attemptedBlocks == 1);
            expect(activeTelemetry.jobsCompleted >= 1);
            expect(activeTelemetry.spectrumCapturedFrameEnd == fftSize);
            expect(activeTelemetry.meterCapturedFrameEnd == fftSize);
            expect(activeTelemetry.latestCaptureRevision == 1);
            expect(activeTelemetry.lastAnalyzedCaptureRevision == 1);

            const auto submittedBeforeEmptyRequests = activeTelemetry.scheduler.submitted;
            for (std::size_t request = 0; request < 32; ++request)
                coordinator.requestAnalysis();

            const auto afterEmptyRequests = coordinator.telemetry();
            expect(afterEmptyRequests.scheduler.submitted == submittedBeforeEmptyRequests);
            expect(afterEmptyRequests.emptyAnalysisRequestsAvoided
                >= activeTelemetry.emptyAnalysisRequestsAvoided + 32);

            coordinator.setVisualizationActive(false);
            const auto beforeClosedCapture = coordinator.telemetry();
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

        beginTest("Transport-stop staleness publishes one cleared frame without empty jobs");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, fftSize> signal { };
            signal.fill(0.5F);
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

            const auto afterClear = coordinator.telemetry();
            expect(afterClear.staleFramesPublished == 1);
            const auto submittedAfterClear = afterClear.scheduler.submitted;

            for (std::size_t request = 0; request < 32; ++request)
                coordinator.requestAnalysis();

            const auto afterRepeatedIdleRequests = coordinator.telemetry();
            expect(afterRepeatedIdleRequests.scheduler.submitted == submittedAfterClear);
            expect(afterRepeatedIdleRequests.staleFramesPublished == 1);
        }

        beginTest("Reactivation begins with a fresh generation and cleared snapshot");
        {
            AnalysisCoordinator coordinator;
            coordinator.setVisualizationActive(true);

            std::array<float, fftSize> signal { };
            signal.fill(0.25F);
            coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0);

            VisualizationFrame activeFrame;
            expect(waitForFrame(coordinator, activeFrame,
                [](const auto& candidate) { return candidate.spectrumValid; }));

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
