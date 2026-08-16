// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IntegratedLoudnessIndex.h"
#include "StereoSampleCapture.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace audio_insight {
/** One immutable worker-side Loudness endpoint. */
struct LoudnessMeasurement final {
    double momentaryLufs = -std::numeric_limits<double>::infinity();
    double shortTermLufs = -std::numeric_limits<double>::infinity();
    double integratedLufs = -std::numeric_limits<double>::infinity();
    double relativeGateLufs = -std::numeric_limits<double>::infinity();

    std::uint64_t stateSequence = 0;
    std::uint64_t measurementCompletionCount = 0;
    std::uint64_t integrationBlockCount = 0;
    std::uint64_t absoluteGatedBlockCount = 0;
    std::uint64_t relativeGatedBlockCount = 0;
    std::uint64_t measurementCapturedFrameEnd = 0;
    std::uint64_t integratedCapturedFrameEnd = 0;
    std::uint64_t integrationBlockCapacity = 0;
    std::uint64_t generation = 0;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;

    bool momentaryValid = false;
    bool shortTermValid = false;
    bool integratedValid = false;
    bool integrationCapacityExceeded = false;
};

/**
    Bounded worker-owned ITU-R BS.1770-5 / EBU R128 Loudness analysis.

    Mono and stereo input are K-weighted in source order. Momentary and
    Short-term use fixed 400 ms and 3 s rectangular windows, while Integrated
    uses 400 ms blocks completed every 100 ms. Exact Integrated block energies
    are retained for a bounded 24-hour interval. Exceeding that interval makes
    I unavailable rather than silently rolling or approximating it.

    This class belongs on an existing non-real-time analysis worker. It neither
    allocates nor locks while processing a chunk, but it is not an audio-callback
    facility and is not thread-safe.
*/
class LoudnessAnalyzer final {
public:
    static constexpr double loudnessOffsetLufs = -0.691;
    static constexpr double absoluteGateLufs = -70.0;
    static constexpr double relativeGateOffsetLu = -10.0;
    static constexpr double measurementPeriodSeconds = 0.100;
    static constexpr double momentaryWindowSeconds = 0.400;
    static constexpr double shortTermWindowSeconds = 3.000;
    static constexpr std::uint64_t integrationBlockCapacity = 24ULL * 60ULL * 60ULL * 10ULL;

    struct BiquadCoefficients final {
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    struct KWeightingCoefficients final {
        BiquadCoefficients headFilter;
        BiquadCoefficients highPassFilter;
        bool valid = false;
    };

    struct ProcessResult final {
        std::uint64_t measurementCompletions = 0;
        std::uint64_t integrationBlockCompletions = 0;
        bool accepted = false;
        bool integrationResetApplied = false;

        [[nodiscard]] bool producedVisibleState() const noexcept
        {
            return measurementCompletions != 0 || integrationBlockCompletions != 0;
        }
    };

    struct IntegratedGateResult final {
        double integratedLufs = -std::numeric_limits<double>::infinity();
        double relativeGateLufs = -std::numeric_limits<double>::infinity();
        std::uint64_t absoluteGatedBlockCount = 0;
        std::uint64_t relativeGatedBlockCount = 0;
    };

    struct Statistics final {
        std::uint64_t inputChunks = 0;
        std::uint64_t inputFrames = 0;
        std::uint64_t measurementCompletions = 0;
        std::uint64_t integrationBlockCompletions = 0;
        std::uint64_t fullResets = 0;
        std::uint64_t explicitResets = 0;
        std::uint64_t generationResets = 0;
        std::uint64_t discontinuityResets = 0;
        std::uint64_t formatResets = 0;
        std::uint64_t invalidInputResets = 0;
        std::uint64_t integrationResets = 0;
        std::uint64_t liveMeasurementClears = 0;
        std::uint64_t integrationCapacityOverflows = 0;
        std::uint64_t integrationBlocksSinceReset = 0;
        std::uint64_t absoluteGatedBlocks = 0;
        std::uint64_t relativeGatedBlocks = 0;
        std::uint64_t integrationIndexReservedBytes = 0;
        std::uint64_t integrationIndexLeafNodes = 0;
        std::uint64_t integrationIndexInternalNodes = 0;
        std::uint64_t integrationIndexLeafCapacity = 0;
        std::uint64_t integrationIndexInternalCapacity = 0;
        std::uint64_t integrationIndexTreeHeight = 0;
        std::uint64_t integrationIndexQueries = 0;
        std::uint64_t integrationIndexLastNodeVisits = 0;
        std::uint64_t integrationIndexMaximumNodeVisits = 0;
        std::uint64_t integrationIndexLastAggregateReads = 0;
        std::uint64_t integrationIndexMaximumAggregateReads = 0;
        std::uint64_t integrationIndexLastBoundaryValueReads = 0;
        std::uint64_t integrationIndexMaximumBoundaryValueReads = 0;
        std::uint64_t stateSequence = 0;
        std::uint64_t capturedFrameEnd = 0;
        std::uint64_t integrationBlockCapacity = 0;
        bool integrationCapacityExceeded = false;
    };

    LoudnessAnalyzer();

    LoudnessAnalyzer(const LoudnessAnalyzer&) = delete;
    LoudnessAnalyzer& operator=(const LoudnessAnalyzer&) = delete;

    /**
        Consumes one continuous source-order raw capture chunk.

        When the optional reset boundary lies strictly inside the chunk,
        Integrated processing restarts at that exact captured-frame boundary.
        K-weighting and the live M/S windows remain continuous across it.
    */
    [[nodiscard]] ProcessResult process(const CapturedStereoChunkView& chunk,
        std::optional<std::uint64_t> integrationResetCapturedFrameEnd = std::nullopt) noexcept;

    /**
        Full capture/lifecycle reset. Every measurement, filter, and partial
        window is invalidated immediately.
    */
    void reset() noexcept;

    /** Coordinator-classified full lifecycle/generation reset. */
    void resetForLifecycle() noexcept;

    /** Coordinator-classified full raw-capture discontinuity reset. */
    void resetForDiscontinuity() noexcept;

    /** Coordinator-classified full sample-rate/channel-layout reset. */
    void resetForFormatChange() noexcept;

    /**
        Starts a new Integrated measurement without disturbing ready M or S.

        The first new gating block begins with the next supplied sample, so no
        pre-reset energy can enter the new integration.
    */
    void resetIntegration() noexcept;

    /**
        Invalidates M/S and clears stale filters and partial windows while
        retaining completed Integrated blocks and its current value.
    */
    void clearLiveMeasurementsPreservingIntegration() noexcept;

    [[nodiscard]] const LoudnessMeasurement& current() const noexcept
    {
        return output_;
    }
    [[nodiscard]] Statistics statistics() const noexcept;

    /** Returns the BS.1770-equivalent two-stage coefficients for a sample rate. */
    [[nodiscard]] static KWeightingCoefficients coefficientsForSampleRate(
        double sampleRate) noexcept;

    /** Exact two-pass BS.1770 absolute/relative gate reducer for block energies. */
    [[nodiscard]] static IntegratedGateResult reduceIntegratedBlockEnergies(
        std::span<const double> meanSquareBlocks) noexcept;

    /** Closed-form cumulative 100 ms boundary used by the fractional scheduler. */
    [[nodiscard]] static std::uint64_t measurementBoundaryFrameForTesting(
        double sampleRate, std::uint64_t completionCount) noexcept;

    /** Narrows the 24-hour limit before integration starts, for bounded tests only. */
    [[nodiscard]] bool setIntegrationBlockCapacityForTesting(std::uint64_t capacity) noexcept;

private:
    static constexpr std::size_t momentaryHopCount = 4;
    static constexpr std::size_t shortTermHopCount = 30;
    static constexpr std::size_t measurementHopCapacity = shortTermHopCount + 1;
    static constexpr std::size_t integrationHopCapacity = momentaryHopCount + 1;
    static constexpr std::size_t hopEdgeSampleCapacity = 2;
    enum class ResetReason {
        explicitReset,
        generationChange,
        discontinuity,
        formatChange,
        invalid
    };

    struct BiquadState final {
        double firstDelay = 0.0;
        double secondDelay = 0.0;

        void clear() noexcept
        {
            firstDelay = 0.0;
            secondDelay = 0.0;
        }
    };

    struct ChannelFilterState final {
        BiquadState head;
        BiquadState highPass;

        void clear() noexcept
        {
            head.clear();
            highPass.clear();
        }
    };

    struct EnergyHop final {
        double sum = 0.0;
        std::uint64_t frameCount = 0;
        std::array<double, hopEdgeSampleCapacity> firstSamples { };
        std::array<double, hopEdgeSampleCapacity> trailingSamples { };
    };

    struct PeriodScheduler final {
        std::uint64_t completionCount = 0;
        std::uint64_t roundedBoundaryFrames = 0;
    };

    [[nodiscard]] static bool isChunkMetadataValid(const CapturedStereoChunkView& chunk) noexcept;
    [[nodiscard]] static bool sampleRatesDiffer(double left, double right) noexcept;
    [[nodiscard]] static double lufsToEnergy(double loudness) noexcept;
    [[nodiscard]] static double energyToLufs(double meanSquare) noexcept;
    [[nodiscard]] static double processBiquad(
        double input, const BiquadCoefficients& coefficients, BiquadState& state) noexcept;

    [[nodiscard]] bool isContinuousWithPrevious(
        const CapturedStereoChunkView& chunk, std::uint64_t chunkFrameStart) const noexcept;
    void configure(const CapturedStereoChunkView& chunk) noexcept;
    void applyFullReset(ResetReason reason) noexcept;
    void clearAllTemporalState() noexcept;
    void clearFiltersAndPartialHops() noexcept;
    void clearMeasurementState() noexcept;
    void clearIntegrationState() noexcept;
    void publishIntegrationIndexStatistics() noexcept;
    void publishStateChange() noexcept;
    void resetPeriodScheduler(
        PeriodScheduler& scheduler, std::uint64_t& framesUntilCompletion) const noexcept;
    [[nodiscard]] std::uint64_t advancePeriodScheduler(PeriodScheduler& scheduler) const noexcept;
    [[nodiscard]] double filterSample(std::size_t channel, double sample) noexcept;
    static void appendEnergy(EnergyHop& hop, double energy) noexcept;
    [[nodiscard]] static double sumFirstSamples(const EnergyHop& hop, std::size_t count) noexcept;
    [[nodiscard]] static double sumLastSamples(const EnergyHop& hop, std::size_t count) noexcept;
    void appendMeasurementHop(EnergyHop hop, std::uint64_t capturedFrameEnd) noexcept;
    [[nodiscard]] bool appendIntegrationHop(EnergyHop hop, std::uint64_t capturedFrameEnd) noexcept;
    [[nodiscard]] EnergyHop sumLatestMeasurementHops(std::size_t count) const noexcept;
    [[nodiscard]] EnergyHop sumLatestIntegrationHops(std::size_t count) const noexcept;
    [[nodiscard]] bool measurementWindow(
        std::size_t hopCount, std::uint64_t windowFrames, EnergyHop& result) const noexcept;
    [[nodiscard]] bool integrationWindow(
        std::size_t hopCount, std::uint64_t windowFrames, EnergyHop& result) const noexcept;
    void addIntegrationBlock(double meanSquare, std::uint64_t capturedFrameEnd) noexcept;
    void updateIntegratedOutput() noexcept;

    KWeightingCoefficients coefficients_;
    std::array<ChannelFilterState, 2> filterStates_ { };
    std::array<EnergyHop, measurementHopCapacity> measurementHops_ { };
    std::array<EnergyHop, integrationHopCapacity> integrationHops_ { };
    IntegratedLoudnessIndex integrationEnergyIndex_;

    LoudnessMeasurement output_;
    EnergyHop partialMeasurementHop_;
    EnergyHop partialIntegrationHop_;
    double absoluteGatedEnergySum_ = 0.0;
    std::uint64_t absoluteGatedBlockCount_ = 0;
    std::uint64_t relativeGatedBlockCount_ = 0;
    std::uint64_t integrationBlockCount_ = 0;
    std::uint64_t effectiveIntegrationBlockCapacity_ = integrationBlockCapacity;
    std::uint64_t stateSequence_ = 0;
    std::uint64_t measurementCompletionCount_ = 0;
    std::uint64_t previousGeneration_ = 0;
    std::uint64_t previousSequence_ = 0;
    std::uint64_t previousCapturedFrameEnd_ = 0;
    std::uint64_t measurementFramesUntilCompletion_ = 0;
    std::uint64_t integrationFramesUntilCompletion_ = 0;
    std::uint64_t measurementPeriodFrames_ = 0;
    std::uint64_t momentaryWindowFrames_ = 0;
    std::uint64_t shortTermWindowFrames_ = 0;
    PeriodScheduler measurementPeriodScheduler_;
    PeriodScheduler integrationPeriodScheduler_;
    std::size_t measurementHopWriteIndex_ = 0;
    std::size_t measurementHopCount_ = 0;
    std::size_t integrationHopWriteIndex_ = 0;
    std::size_t integrationHopCount_ = 0;
    std::uint32_t channelCount_ = 0;
    double sampleRate_ = 0.0;
    bool initialized_ = false;
    bool integrationCapacityExceeded_ = false;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    std::atomic<std::uint64_t> telemetryInputChunks_ { 0 };
    std::atomic<std::uint64_t> telemetryInputFrames_ { 0 };
    std::atomic<std::uint64_t> telemetryMeasurementCompletions_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationBlockCompletions_ { 0 };
    std::atomic<std::uint64_t> telemetryFullResets_ { 0 };
    std::atomic<std::uint64_t> telemetryExplicitResets_ { 0 };
    std::atomic<std::uint64_t> telemetryGenerationResets_ { 0 };
    std::atomic<std::uint64_t> telemetryDiscontinuityResets_ { 0 };
    std::atomic<std::uint64_t> telemetryFormatResets_ { 0 };
    std::atomic<std::uint64_t> telemetryInvalidInputResets_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationResets_ { 0 };
    std::atomic<std::uint64_t> telemetryLiveMeasurementClears_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationCapacityOverflows_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationBlocksSinceReset_ { 0 };
    std::atomic<std::uint64_t> telemetryAbsoluteGatedBlocks_ { 0 };
    std::atomic<std::uint64_t> telemetryRelativeGatedBlocks_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexReservedBytes_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexLeafNodes_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexInternalNodes_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexLeafCapacity_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexInternalCapacity_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexTreeHeight_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexQueries_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexLastNodeVisits_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexMaximumNodeVisits_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexLastAggregateReads_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexMaximumAggregateReads_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexLastBoundaryValueReads_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationIndexMaximumBoundaryValueReads_ { 0 };
    std::atomic<std::uint64_t> telemetryStateSequence_ { 0 };
    std::atomic<std::uint64_t> telemetryCapturedFrameEnd_ { 0 };
    std::atomic<std::uint64_t> telemetryIntegrationBlockCapacity_ { integrationBlockCapacity };
    std::atomic<std::uint64_t> telemetryIntegrationCapacityExceeded_ { 0 };
};
} // namespace audio_insight
