// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SharedAnalysisScheduler.h"
#include "StereoMeterAccumulator.h"
#include "StereoSampleCapture.h"
#include "core/VisualizationDataSource.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace audio_insight
{
/** Non-real-time diagnostics for one plugin instance's analysis pipeline. */
struct AnalysisTelemetry
{
    StereoSampleCapture::Telemetry capture;
    StereoMeterAccumulator::Telemetry meters;
    SharedAnalysisScheduler::Counters scheduler;

    std::uint64_t jobsStarted = 0;
    std::uint64_t jobsCompleted = 0;
    std::uint64_t jobsStopped = 0;
    std::uint64_t ignoredGenerationChunks = 0;
    std::uint64_t publishedFrames = 0;
    std::uint64_t droppedFramePublications = 0;
    std::uint64_t lastJobNanoseconds = 0;
    std::uint64_t maximumJobNanoseconds = 0;
    std::uint64_t spectrumTransforms = 0;
    std::uint64_t lastJobSpectrumTransforms = 0;
    std::uint64_t maximumJobSpectrumTransforms = 0;
    std::uint64_t backlogDiscardedFrames = 0;
    std::uint64_t spectrumCapturedFrameEnd = 0;
    std::uint64_t meterCapturedFrameEnd = 0;
    std::uint64_t latestCaptureRevision = 0;
    std::uint64_t lastAnalyzedCaptureRevision = 0;
    std::uint64_t emptyAnalysisRequestsAvoided = 0;
    std::uint64_t staleFramesPublished = 0;
};

/**
    Per-instance bridge between the audio callback, shared analysis workers, and
    the renderer.

    captureAudioBlock() is the only audio-thread entry point. It performs only
    bounded measurement and copies into preallocated lock-free storage; it never
    requests or wakes a worker. All VisualizationDataSource calls are strictly
    non-real-time.
*/
class AnalysisCoordinator final : public VisualizationDataSource
{
public:
    AnalysisCoordinator();
    ~AnalysisCoordinator() override;

    AnalysisCoordinator(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator& operator=(const AnalysisCoordinator&) = delete;

    /** Audio-thread entry point. Null channels are treated as silence. */
    void captureAudioBlock(const float* left,
                           const float* right,
                           std::size_t frameCount,
                           double sampleRate) noexcept;

    void requestAnalysis() noexcept override;
    void setVisualizationActive(bool shouldBeActive) noexcept override;
    [[nodiscard]] bool
    copyLatestVisualizationFrame(VisualizationFrame& destination) const noexcept override;

    [[nodiscard]] bool isVisualizationActive() const noexcept;
    [[nodiscard]] AnalysisTelemetry telemetry() const noexcept;

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
    enum class LifecycleTestOperation
    {
        request,
        activate,
        deactivate
    };

    using LifecycleTestHook = void (*)(void*, LifecycleTestOperation) noexcept;

    /** Installs a synchronous test seam invoked while the non-real-time gate is held. */
    void setLifecycleTestHook(void* context, LifecycleTestHook hook) noexcept;
#endif

private:
    struct State;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    SharedAnalysisScheduler::Ptr scheduler_;
    std::shared_ptr<State> state_;
    std::shared_ptr<SharedAnalysisScheduler::Client> client_;

    // Zero means inactive. A non-zero value is the generation stamped by the
    // audio producer without any scheduler interaction.
    std::atomic<std::uint64_t> captureGeneration_ { 0 };
    std::atomic<std::int64_t> nextAnalysisRequestNanoseconds_ { 0 };

    // Accessed only while lifecycleMutex_ is held.
    std::uint64_t lastRequestedCaptureRevision_ = 0;
    std::uint64_t lastObservedCaptureRevision_ = 0;
    std::int64_t lastObservedCaptureNanoseconds_ = 0;
    bool staleClearRequested_ = false;

    // Serialises renderer requests against non-real-time lifecycle changes so
    // no request can slip in after deactivation has drained the client.
    mutable std::mutex lifecycleMutex_;

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
    void* lifecycleTestHookContext_ = nullptr;
    LifecycleTestHook lifecycleTestHook_ = nullptr;
#endif
};
} // namespace audio_insight
