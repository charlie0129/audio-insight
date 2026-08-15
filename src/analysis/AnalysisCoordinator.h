// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SharedAnalysisScheduler.h"
#include "StereoMeterAccumulator.h"
#include "StereoSampleCapture.h"
#include "core/SpectrumAnalysisConfiguration.h"
#include "core/SpectrumTemporalConfiguration.h"
#include "core/VisualizationDataSource.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace audio_insight {
/** Non-real-time diagnostics for one plugin instance's analysis pipeline. */
struct AnalysisTelemetry {
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
    std::uint64_t peakRmsUserResets = 0;
    std::uint64_t spectrumUserClears = 0;
    std::uint64_t fftConfigurationChanges = 0;
    std::uint64_t spectrumTemporalConfigurationChanges = 0;
    std::uint64_t fftGeneration = 0;
    std::uint32_t configuredFftSize = 0;
    std::uint32_t configuredFftWindow = 0;
    std::uint32_t requestedFftSliceRateHz = 0;
};

/**
    Per-instance bridge between the audio callback, shared analysis workers, and
    the renderer.

    captureAudioBlock() is the only audio-thread entry point. It performs only
    bounded measurement and copies into preallocated lock-free storage; it never
    requests or wakes a worker. All VisualizationDataSource calls are strictly
    non-real-time.
*/
class AnalysisCoordinator final : public VisualizationDataSource {
public:
    AnalysisCoordinator();
    ~AnalysisCoordinator() override;

    AnalysisCoordinator(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator& operator=(const AnalysisCoordinator&) = delete;

    /** Audio-thread entry point. Null channels are treated as silence. */
    void captureAudioBlock(const float* left, const float* right, std::size_t frameCount,
        double sampleRate, std::uint32_t channelCount = 2) noexcept;

    /**
        Records a host format at a non-real-time boundary.

        The caller must ensure that audio callbacks are quiescent, as JUCE does
        around prepareToPlay(). A changed active format starts a fresh capture
        generation so snapshots never combine analyzers from different formats.
    */
    void setCaptureFormat(double sampleRate, std::uint32_t channelCount) noexcept;

    /**
        Applies worker-side FFT settings without changing the capture generation
        or resetting Peak/RMS. This is strictly a non-audio-thread operation.
    */
    void setSpectrumAnalysisConfiguration(SpectrumAnalysisConfiguration configuration) noexcept;

    /** Applies worker-side averaging/hold settings without advancing the FFT generation. */
    void setSpectrumTemporalConfiguration(SpectrumTemporalConfiguration configuration) noexcept;

    void requestAnalysis() noexcept override;
    void setVisualizationActive(bool shouldBeActive) noexcept override;
    void resetSpectrum() noexcept override;
    void resetPeakRms() noexcept override;
    [[nodiscard]] bool copyLatestVisualizationFrame(
        VisualizationFrame& destination) const noexcept override;

    [[nodiscard]] bool isVisualizationActive() const noexcept;
    [[nodiscard]] AnalysisTelemetry telemetry() const noexcept;

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
    enum class LifecycleTestOperation { request, activate, deactivate };

    using LifecycleTestHook = void (*)(void*, LifecycleTestOperation) noexcept;

    /** Installs a synchronous test seam invoked while the non-real-time gate is held. */
    void setLifecycleTestHook(void* context, LifecycleTestHook hook) noexcept;
#endif

private:
    struct State;

    [[nodiscard]] bool restartActiveGenerationLocked(bool discardPendingCapture);
    [[nodiscard]] static std::uint64_t nextNonzeroGeneration(std::uint64_t& counter) noexcept;

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
    double configuredSampleRate_ = 0.0;
    std::uint32_t configuredChannelCount_ = 0;
    bool hasConfiguredFormat_ = false;
    bool staleClearRequested_ = false;
    SpectrumAnalysisConfiguration spectrumConfiguration_;
    SpectrumTemporalConfiguration spectrumTemporalConfiguration_;
    std::int64_t analysisRequestPeriodNanoseconds_ = 16'666'667;
    std::uint64_t captureGenerationCounter_ = 0;
    std::uint64_t fftGenerationCounter_ = 1;

    // Serialises renderer requests against non-real-time lifecycle changes so
    // no request can slip in after deactivation has drained the client.
    mutable std::mutex lifecycleMutex_;

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
    void* lifecycleTestHookContext_ = nullptr;
    LifecycleTestHook lifecycleTestHook_ = nullptr;
#endif
};
} // namespace audio_insight
