// SPDX-License-Identifier: AGPL-3.0-or-later

#include "LoudnessAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace audio_insight {
namespace {
constexpr double headFilterFrequencyHz = 1681.974450955533;
constexpr double headFilterGainDecibels = 3.999843853973347;
constexpr double headFilterQ = 0.7071752369554196;
constexpr double headFilterShelfExponent = 0.4996667741545416;
constexpr double highPassFilterFrequencyHz = 38.13547087602444;
constexpr double highPassFilterQ = 0.5003270373238773;
constexpr double minimumSupportedSampleRate = 2.0 * headFilterFrequencyHz;
constexpr double maximumSupportedSampleRate = 768'000.0;

[[nodiscard]] std::uint64_t incrementWithoutWrap(const std::uint64_t value) noexcept
{
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1;
}
} // namespace

LoudnessAnalyzer::LoudnessAnalyzer()
    : integrationEnergyIndex_(static_cast<std::size_t>(integrationBlockCapacity))
{
    output_.integrationBlockCapacity = integrationBlockCapacity;
    publishIntegrationIndexStatistics();
}

LoudnessAnalyzer::ProcessResult LoudnessAnalyzer::process(
    const CapturedStereoChunkView& chunk) noexcept
{
    telemetryInputChunks_.fetch_add(1, std::memory_order_relaxed);

    if (!isChunkMetadataValid(chunk)) {
        applyFullReset(ResetReason::invalid);
        return { };
    }

    const auto chunkFrameStart = chunk.capturedFrameEnd - chunk.frameCount;
    if (initialized_) {
        if (chunk.channelCount != channelCount_
            || sampleRatesDiffer(chunk.sampleRate, sampleRate_)) {
            applyFullReset(ResetReason::formatChange);
        } else if (chunk.generation != previousGeneration_) {
            applyFullReset(ResetReason::generationChange);
        } else if (chunk.followsDiscontinuity
            || !isContinuousWithPrevious(chunk, chunkFrameStart)) {
            applyFullReset(ResetReason::discontinuity);
        }
    }

    if (!initialized_)
        configure(chunk);

    ProcessResult result;
    result.accepted = true;
    telemetryInputFrames_.fetch_add(chunk.frameCount, std::memory_order_relaxed);

    for (std::size_t frame = 0; frame < chunk.frameCount; ++frame) {
        const auto leftInput
            = std::isfinite(chunk.left[frame]) ? static_cast<double>(chunk.left[frame]) : 0.0;
        const auto left = filterSample(0, leftInput);

        auto energy = left * left;
        if (channelCount_ == 2) {
            const auto rightInput
                = std::isfinite(chunk.right[frame]) ? static_cast<double>(chunk.right[frame]) : 0.0;
            const auto right = filterSample(1, rightInput);
            energy += right * right;
        }

        if (!std::isfinite(energy) || energy < 0.0)
            energy = 0.0;

        appendEnergy(partialMeasurementHop_, energy);
        appendEnergy(partialIntegrationHop_, energy);

        const auto capturedFrameEnd = chunkFrameStart + frame + 1;
        if (--measurementFramesUntilCompletion_ == 0) {
            appendMeasurementHop(partialMeasurementHop_, capturedFrameEnd);
            partialMeasurementHop_ = { };
            measurementFramesUntilCompletion_ = advancePeriodScheduler(measurementPeriodScheduler_);
            ++result.measurementCompletions;
        }

        if (--integrationFramesUntilCompletion_ == 0) {
            if (appendIntegrationHop(partialIntegrationHop_, capturedFrameEnd))
                ++result.integrationBlockCompletions;
            partialIntegrationHop_ = { };
            integrationFramesUntilCompletion_ = advancePeriodScheduler(integrationPeriodScheduler_);
        }
    }

    previousGeneration_ = chunk.generation;
    previousSequence_ = chunk.sequence;
    previousCapturedFrameEnd_ = chunk.capturedFrameEnd;
    telemetryCapturedFrameEnd_.store(chunk.capturedFrameEnd, std::memory_order_relaxed);
    return result;
}

void LoudnessAnalyzer::reset() noexcept
{
    applyFullReset(ResetReason::explicitReset);
}

void LoudnessAnalyzer::resetForLifecycle() noexcept
{
    applyFullReset(ResetReason::generationChange);
}

void LoudnessAnalyzer::resetForDiscontinuity() noexcept
{
    applyFullReset(ResetReason::discontinuity);
}

void LoudnessAnalyzer::resetForFormatChange() noexcept
{
    applyFullReset(ResetReason::formatChange);
}

void LoudnessAnalyzer::resetIntegration() noexcept
{
    clearIntegrationState();
    telemetryIntegrationResets_.fetch_add(1, std::memory_order_relaxed);
    publishStateChange();
}

void LoudnessAnalyzer::clearLiveMeasurementsPreservingIntegration() noexcept
{
    clearFiltersAndPartialHops();
    telemetryLiveMeasurementClears_.fetch_add(1, std::memory_order_relaxed);
    publishStateChange();
}

LoudnessAnalyzer::Statistics LoudnessAnalyzer::statistics() const noexcept
{
    Statistics result;
    result.inputChunks = telemetryInputChunks_.load(std::memory_order_relaxed);
    result.inputFrames = telemetryInputFrames_.load(std::memory_order_relaxed);
    result.measurementCompletions
        = telemetryMeasurementCompletions_.load(std::memory_order_relaxed);
    result.integrationBlockCompletions
        = telemetryIntegrationBlockCompletions_.load(std::memory_order_relaxed);
    result.fullResets = telemetryFullResets_.load(std::memory_order_relaxed);
    result.explicitResets = telemetryExplicitResets_.load(std::memory_order_relaxed);
    result.generationResets = telemetryGenerationResets_.load(std::memory_order_relaxed);
    result.discontinuityResets = telemetryDiscontinuityResets_.load(std::memory_order_relaxed);
    result.formatResets = telemetryFormatResets_.load(std::memory_order_relaxed);
    result.invalidInputResets = telemetryInvalidInputResets_.load(std::memory_order_relaxed);
    result.integrationResets = telemetryIntegrationResets_.load(std::memory_order_relaxed);
    result.liveMeasurementClears = telemetryLiveMeasurementClears_.load(std::memory_order_relaxed);
    result.integrationCapacityOverflows
        = telemetryIntegrationCapacityOverflows_.load(std::memory_order_relaxed);
    result.integrationBlocksSinceReset
        = telemetryIntegrationBlocksSinceReset_.load(std::memory_order_relaxed);
    result.absoluteGatedBlocks = telemetryAbsoluteGatedBlocks_.load(std::memory_order_relaxed);
    result.relativeGatedBlocks = telemetryRelativeGatedBlocks_.load(std::memory_order_relaxed);
    result.integrationIndexReservedBytes
        = telemetryIntegrationIndexReservedBytes_.load(std::memory_order_relaxed);
    result.integrationIndexLeafNodes
        = telemetryIntegrationIndexLeafNodes_.load(std::memory_order_relaxed);
    result.integrationIndexInternalNodes
        = telemetryIntegrationIndexInternalNodes_.load(std::memory_order_relaxed);
    result.integrationIndexLeafCapacity
        = telemetryIntegrationIndexLeafCapacity_.load(std::memory_order_relaxed);
    result.integrationIndexInternalCapacity
        = telemetryIntegrationIndexInternalCapacity_.load(std::memory_order_relaxed);
    result.integrationIndexTreeHeight
        = telemetryIntegrationIndexTreeHeight_.load(std::memory_order_relaxed);
    result.integrationIndexQueries
        = telemetryIntegrationIndexQueries_.load(std::memory_order_relaxed);
    result.integrationIndexLastNodeVisits
        = telemetryIntegrationIndexLastNodeVisits_.load(std::memory_order_relaxed);
    result.integrationIndexMaximumNodeVisits
        = telemetryIntegrationIndexMaximumNodeVisits_.load(std::memory_order_relaxed);
    result.integrationIndexLastAggregateReads
        = telemetryIntegrationIndexLastAggregateReads_.load(std::memory_order_relaxed);
    result.integrationIndexMaximumAggregateReads
        = telemetryIntegrationIndexMaximumAggregateReads_.load(std::memory_order_relaxed);
    result.integrationIndexLastBoundaryValueReads
        = telemetryIntegrationIndexLastBoundaryValueReads_.load(std::memory_order_relaxed);
    result.integrationIndexMaximumBoundaryValueReads
        = telemetryIntegrationIndexMaximumBoundaryValueReads_.load(std::memory_order_relaxed);
    result.stateSequence = telemetryStateSequence_.load(std::memory_order_relaxed);
    result.capturedFrameEnd = telemetryCapturedFrameEnd_.load(std::memory_order_relaxed);
    result.integrationBlockCapacity
        = telemetryIntegrationBlockCapacity_.load(std::memory_order_relaxed);
    result.integrationCapacityExceeded
        = telemetryIntegrationCapacityExceeded_.load(std::memory_order_relaxed) != 0;
    return result;
}

LoudnessAnalyzer::KWeightingCoefficients LoudnessAnalyzer::coefficientsForSampleRate(
    const double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= minimumSupportedSampleRate
        || sampleRate > maximumSupportedSampleRate) {
        return { };
    }

    const auto headK = std::tan(std::numbers::pi * headFilterFrequencyHz / sampleRate);
    const auto headHighGain = std::pow(10.0, headFilterGainDecibels / 20.0);
    const auto headMiddleGain = std::pow(headHighGain, headFilterShelfExponent);
    const auto headDenominator = 1.0 + (headK / headFilterQ) + (headK * headK);

    KWeightingCoefficients result;
    result.headFilter.b0 = (headHighGain + (headMiddleGain * headK / headFilterQ) + (headK * headK))
        / headDenominator;
    result.headFilter.b1 = 2.0 * ((headK * headK) - headHighGain) / headDenominator;
    result.headFilter.b2 = (headHighGain - (headMiddleGain * headK / headFilterQ) + (headK * headK))
        / headDenominator;
    result.headFilter.a1 = 2.0 * ((headK * headK) - 1.0) / headDenominator;
    result.headFilter.a2 = (1.0 - (headK / headFilterQ) + (headK * headK)) / headDenominator;

    const auto highPassK = std::tan(std::numbers::pi * highPassFilterFrequencyHz / sampleRate);
    const auto highPassDenominator = 1.0 + (highPassK / highPassFilterQ) + (highPassK * highPassK);
    result.highPassFilter.b0 = 1.0;
    result.highPassFilter.b1 = -2.0;
    result.highPassFilter.b2 = 1.0;
    result.highPassFilter.a1 = 2.0 * ((highPassK * highPassK) - 1.0) / highPassDenominator;
    result.highPassFilter.a2
        = (1.0 - (highPassK / highPassFilterQ) + (highPassK * highPassK)) / highPassDenominator;
    result.valid = std::isfinite(result.headFilter.b0) && std::isfinite(result.headFilter.b1)
        && std::isfinite(result.headFilter.b2) && std::isfinite(result.headFilter.a1)
        && std::isfinite(result.headFilter.a2) && std::isfinite(result.highPassFilter.a1)
        && std::isfinite(result.highPassFilter.a2);
    return result;
}

LoudnessAnalyzer::IntegratedGateResult LoudnessAnalyzer::reduceIntegratedBlockEnergies(
    const std::span<const double> meanSquareBlocks) noexcept
{
    const auto absoluteThreshold = lufsToEnergy(absoluteGateLufs);
    auto absoluteEnergySum = 0.0;
    auto absoluteCount = std::uint64_t { 0 };
    for (const auto energy : meanSquareBlocks) {
        if (std::isfinite(energy) && energy > absoluteThreshold) {
            absoluteEnergySum += energy;
            ++absoluteCount;
        }
    }

    IntegratedGateResult result;
    result.absoluteGatedBlockCount = absoluteCount;
    if (absoluteCount == 0 || !std::isfinite(absoluteEnergySum) || absoluteEnergySum <= 0.0)
        return result;

    const auto absoluteMean = absoluteEnergySum / static_cast<double>(absoluteCount);
    const auto relativeThreshold = absoluteMean * 0.1;
    result.relativeGateLufs = energyToLufs(relativeThreshold);

    auto relativeEnergySum = 0.0;
    auto relativeCount = std::uint64_t { 0 };
    for (const auto energy : meanSquareBlocks) {
        if (std::isfinite(energy) && energy > absoluteThreshold && energy > relativeThreshold) {
            relativeEnergySum += energy;
            ++relativeCount;
        }
    }

    result.relativeGatedBlockCount = relativeCount;
    if (relativeCount != 0 && std::isfinite(relativeEnergySum) && relativeEnergySum > 0.0) {
        result.integratedLufs
            = energyToLufs(relativeEnergySum / static_cast<double>(relativeCount));
    }
    return result;
}

bool LoudnessAnalyzer::setIntegrationBlockCapacityForTesting(const std::uint64_t capacity) noexcept
{
    if (capacity == 0 || capacity > integrationBlockCapacity || integrationBlockCount_ != 0)
        return false;

    effectiveIntegrationBlockCapacity_ = capacity;
    output_.integrationBlockCapacity = capacity;
    telemetryIntegrationBlockCapacity_.store(capacity, std::memory_order_relaxed);
    publishStateChange();
    return true;
}

std::uint64_t LoudnessAnalyzer::measurementBoundaryFrameForTesting(
    const double sampleRate, const std::uint64_t completionCount) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= minimumSupportedSampleRate
        || sampleRate > maximumSupportedSampleRate || completionCount == 0) {
        return 0;
    }

    // Express every 100 ms boundary as an integer number of tenths. Reusing
    // this same rational definition for 1, 4, and 30 tenths keeps the first
    // hop, Momentary window, and Short-term window identical even when a
    // fractional sample rate lies beside a half-sample rounding boundary.
    const auto boundary
        = (static_cast<long double>(sampleRate) * static_cast<long double>(completionCount))
        / 10.0L;
    const auto roundedBoundary = std::floor(boundary + 0.5L);
    if (!std::isfinite(roundedBoundary)
        || roundedBoundary >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(roundedBoundary);
}

bool LoudnessAnalyzer::isChunkMetadataValid(const CapturedStereoChunkView& chunk) noexcept
{
    return chunk.frameCount != 0 && chunk.left != nullptr
        && (chunk.channelCount == 1 || chunk.channelCount == 2)
        && (chunk.channelCount == 1 || chunk.right != nullptr) && chunk.generation != 0
        && chunk.sequence != 0 && chunk.capturedFrameEnd >= chunk.frameCount
        && coefficientsForSampleRate(chunk.sampleRate).valid;
}

bool LoudnessAnalyzer::sampleRatesDiffer(const double left, const double right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return true;

    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}

double LoudnessAnalyzer::energyToLufs(const double meanSquare) noexcept
{
    return std::isfinite(meanSquare) && meanSquare > 0.0
        ? loudnessOffsetLufs + (10.0 * std::log10(meanSquare))
        : -std::numeric_limits<double>::infinity();
}

double LoudnessAnalyzer::lufsToEnergy(const double loudness) noexcept
{
    return std::pow(10.0, (loudness - loudnessOffsetLufs) / 10.0);
}

double LoudnessAnalyzer::processBiquad(
    const double input, const BiquadCoefficients& coefficients, BiquadState& state) noexcept
{
    const auto output = (coefficients.b0 * input) + state.firstDelay;
    const auto nextFirstDelay
        = (coefficients.b1 * input) - (coefficients.a1 * output) + state.secondDelay;
    const auto nextSecondDelay = (coefficients.b2 * input) - (coefficients.a2 * output);

    if (!std::isfinite(output) || !std::isfinite(nextFirstDelay)
        || !std::isfinite(nextSecondDelay)) {
        state.clear();
        return 0.0;
    }

    state.firstDelay = nextFirstDelay;
    state.secondDelay = nextSecondDelay;
    return output;
}

bool LoudnessAnalyzer::isContinuousWithPrevious(
    const CapturedStereoChunkView& chunk, const std::uint64_t chunkFrameStart) const noexcept
{
    return previousSequence_ != std::numeric_limits<std::uint64_t>::max()
        && chunk.sequence == previousSequence_ + 1 && chunkFrameStart == previousCapturedFrameEnd_;
}

void LoudnessAnalyzer::configure(const CapturedStereoChunkView& chunk) noexcept
{
    coefficients_ = coefficientsForSampleRate(chunk.sampleRate);
    sampleRate_ = chunk.sampleRate;
    channelCount_ = chunk.channelCount;
    measurementPeriodFrames_ = measurementBoundaryFrameForTesting(sampleRate_, 1);
    momentaryWindowFrames_ = measurementBoundaryFrameForTesting(sampleRate_, 4);
    shortTermWindowFrames_ = measurementBoundaryFrameForTesting(sampleRate_, 30);
    resetPeriodScheduler(measurementPeriodScheduler_, measurementFramesUntilCompletion_);
    resetPeriodScheduler(integrationPeriodScheduler_, integrationFramesUntilCompletion_);
    previousGeneration_ = chunk.generation;
    previousSequence_ = chunk.sequence - 1;
    previousCapturedFrameEnd_ = chunk.capturedFrameEnd - chunk.frameCount;
    initialized_ = true;

    output_.generation = chunk.generation;
    output_.channelCount = chunk.channelCount;
    output_.sampleRate = chunk.sampleRate;
    publishStateChange();
}

void LoudnessAnalyzer::applyFullReset(const ResetReason reason) noexcept
{
    clearAllTemporalState();
    coefficients_ = { };
    sampleRate_ = 0.0;
    channelCount_ = 0;
    measurementPeriodFrames_ = 0;
    momentaryWindowFrames_ = 0;
    shortTermWindowFrames_ = 0;
    measurementPeriodScheduler_ = { };
    integrationPeriodScheduler_ = { };
    measurementFramesUntilCompletion_ = 0;
    integrationFramesUntilCompletion_ = 0;
    previousGeneration_ = 0;
    previousSequence_ = 0;
    previousCapturedFrameEnd_ = 0;
    initialized_ = false;

    telemetryFullResets_.fetch_add(1, std::memory_order_relaxed);
    switch (reason) {
    case ResetReason::explicitReset:
        telemetryExplicitResets_.fetch_add(1, std::memory_order_relaxed);
        break;
    case ResetReason::generationChange:
        telemetryGenerationResets_.fetch_add(1, std::memory_order_relaxed);
        break;
    case ResetReason::discontinuity:
        telemetryDiscontinuityResets_.fetch_add(1, std::memory_order_relaxed);
        break;
    case ResetReason::formatChange:
        telemetryFormatResets_.fetch_add(1, std::memory_order_relaxed);
        break;
    case ResetReason::invalid:
        telemetryInvalidInputResets_.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    publishStateChange();
    telemetryCapturedFrameEnd_.store(0, std::memory_order_relaxed);
}

void LoudnessAnalyzer::clearAllTemporalState() noexcept
{
    for (auto& state : filterStates_)
        state.clear();

    clearMeasurementState();
    clearIntegrationState();
    output_.generation = 0;
    output_.channelCount = 0;
    output_.sampleRate = 0.0;
}

void LoudnessAnalyzer::clearFiltersAndPartialHops() noexcept
{
    for (auto& state : filterStates_)
        state.clear();

    clearMeasurementState();
    integrationHops_.fill({ });
    integrationHopWriteIndex_ = 0;
    integrationHopCount_ = 0;
    partialIntegrationHop_ = { };
    if (initialized_)
        resetPeriodScheduler(integrationPeriodScheduler_, integrationFramesUntilCompletion_);
    else
        integrationFramesUntilCompletion_ = 0;
}

void LoudnessAnalyzer::clearMeasurementState() noexcept
{
    measurementHops_.fill({ });
    measurementHopWriteIndex_ = 0;
    measurementHopCount_ = 0;
    partialMeasurementHop_ = { };
    if (initialized_)
        resetPeriodScheduler(measurementPeriodScheduler_, measurementFramesUntilCompletion_);
    else
        measurementFramesUntilCompletion_ = 0;
    measurementCompletionCount_ = 0;

    output_.momentaryLufs = -std::numeric_limits<double>::infinity();
    output_.shortTermLufs = -std::numeric_limits<double>::infinity();
    output_.momentaryValid = false;
    output_.shortTermValid = false;
    output_.measurementCompletionCount = 0;
    output_.measurementCapturedFrameEnd = 0;
}

void LoudnessAnalyzer::clearIntegrationState() noexcept
{
    integrationHops_.fill({ });
    integrationEnergyIndex_.clear();
    integrationHopWriteIndex_ = 0;
    integrationHopCount_ = 0;
    partialIntegrationHop_ = { };
    if (initialized_)
        resetPeriodScheduler(integrationPeriodScheduler_, integrationFramesUntilCompletion_);
    else
        integrationFramesUntilCompletion_ = 0;
    absoluteGatedEnergySum_ = 0.0;
    absoluteGatedBlockCount_ = 0;
    relativeGatedBlockCount_ = 0;
    integrationBlockCount_ = 0;
    integrationCapacityExceeded_ = false;
    telemetryIntegrationBlocksSinceReset_.store(0, std::memory_order_relaxed);
    telemetryAbsoluteGatedBlocks_.store(0, std::memory_order_relaxed);
    telemetryRelativeGatedBlocks_.store(0, std::memory_order_relaxed);
    telemetryIntegrationCapacityExceeded_.store(0, std::memory_order_relaxed);

    output_.integratedLufs = -std::numeric_limits<double>::infinity();
    output_.relativeGateLufs = -std::numeric_limits<double>::infinity();
    output_.integratedValid = false;
    output_.integrationBlockCount = 0;
    output_.absoluteGatedBlockCount = 0;
    output_.relativeGatedBlockCount = 0;
    output_.integratedCapturedFrameEnd = 0;
    output_.integrationBlockCapacity = effectiveIntegrationBlockCapacity_;
    output_.integrationCapacityExceeded = false;
    publishIntegrationIndexStatistics();
}

void LoudnessAnalyzer::publishIntegrationIndexStatistics() noexcept
{
    const auto statistics = integrationEnergyIndex_.statistics();
    telemetryIntegrationIndexReservedBytes_.store(
        statistics.reservedBytes, std::memory_order_relaxed);
    telemetryIntegrationIndexLeafNodes_.store(statistics.leafNodeCount, std::memory_order_relaxed);
    telemetryIntegrationIndexInternalNodes_.store(
        statistics.internalNodeCount, std::memory_order_relaxed);
    telemetryIntegrationIndexLeafCapacity_.store(
        statistics.leafNodeCapacity, std::memory_order_relaxed);
    telemetryIntegrationIndexInternalCapacity_.store(
        statistics.internalNodeCapacity, std::memory_order_relaxed);
    telemetryIntegrationIndexTreeHeight_.store(statistics.treeHeight, std::memory_order_relaxed);
    telemetryIntegrationIndexQueries_.store(statistics.queryCount, std::memory_order_relaxed);
    telemetryIntegrationIndexLastNodeVisits_.store(
        statistics.lastQueryNodeVisits, std::memory_order_relaxed);
    telemetryIntegrationIndexMaximumNodeVisits_.store(
        statistics.maximumQueryNodeVisits, std::memory_order_relaxed);
    telemetryIntegrationIndexLastAggregateReads_.store(
        statistics.lastQueryAggregateReads, std::memory_order_relaxed);
    telemetryIntegrationIndexMaximumAggregateReads_.store(
        statistics.maximumQueryAggregateReads, std::memory_order_relaxed);
    telemetryIntegrationIndexLastBoundaryValueReads_.store(
        statistics.lastQueryBoundaryValueReads, std::memory_order_relaxed);
    telemetryIntegrationIndexMaximumBoundaryValueReads_.store(
        statistics.maximumQueryBoundaryValueReads, std::memory_order_relaxed);
}

void LoudnessAnalyzer::publishStateChange() noexcept
{
    stateSequence_ = incrementWithoutWrap(stateSequence_);
    output_.stateSequence = stateSequence_;
    telemetryStateSequence_.store(stateSequence_, std::memory_order_relaxed);
}

void LoudnessAnalyzer::resetPeriodScheduler(
    PeriodScheduler& scheduler, std::uint64_t& framesUntilCompletion) const noexcept
{
    scheduler.completionCount = 1;
    scheduler.roundedBoundaryFrames = measurementPeriodFrames_;
    framesUntilCompletion = measurementPeriodFrames_;
}

std::uint64_t LoudnessAnalyzer::advancePeriodScheduler(PeriodScheduler& scheduler) const noexcept
{
    scheduler.completionCount = incrementWithoutWrap(scheduler.completionCount);
    const auto nextBoundary
        = measurementBoundaryFrameForTesting(sampleRate_, scheduler.completionCount);
    const auto frames = nextBoundary > scheduler.roundedBoundaryFrames
        ? nextBoundary - scheduler.roundedBoundaryFrames
        : 1;
    scheduler.roundedBoundaryFrames = nextBoundary;
    return frames;
}

double LoudnessAnalyzer::filterSample(const std::size_t channel, const double sample) noexcept
{
    auto& state = filterStates_[channel];
    const auto head = processBiquad(sample, coefficients_.headFilter, state.head);
    return processBiquad(head, coefficients_.highPassFilter, state.highPass);
}

void LoudnessAnalyzer::appendEnergy(EnergyHop& hop, const double energy) noexcept
{
    if (hop.frameCount < hop.firstSamples.size())
        hop.firstSamples[static_cast<std::size_t>(hop.frameCount)] = energy;
    hop.trailingSamples[static_cast<std::size_t>(hop.frameCount % hop.trailingSamples.size())]
        = energy;
    hop.sum += energy;
    ++hop.frameCount;
}

double LoudnessAnalyzer::sumFirstSamples(const EnergyHop& hop, const std::size_t count) noexcept
{
    auto result = 0.0;
    for (std::size_t index = 0; index < count; ++index)
        result += hop.firstSamples[index];
    return result;
}

double LoudnessAnalyzer::sumLastSamples(const EnergyHop& hop, const std::size_t count) noexcept
{
    auto result = 0.0;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto index
            = static_cast<std::size_t>((hop.frameCount - 1 - offset) % hop.trailingSamples.size());
        result += hop.trailingSamples[index];
    }
    return result;
}

void LoudnessAnalyzer::appendMeasurementHop(
    const EnergyHop hop, const std::uint64_t capturedFrameEnd) noexcept
{
    measurementHops_[measurementHopWriteIndex_] = hop;
    measurementHopWriteIndex_ = (measurementHopWriteIndex_ + 1) % measurementHops_.size();
    measurementHopCount_ = std::min(measurementHopCount_ + 1, measurementHops_.size());
    measurementCompletionCount_ = incrementWithoutWrap(measurementCompletionCount_);

    EnergyHop momentary;
    if (measurementWindow(momentaryHopCount, momentaryWindowFrames_, momentary)) {
        output_.momentaryLufs
            = energyToLufs(momentary.sum / static_cast<double>(momentary.frameCount));
        output_.momentaryValid = true;
    } else {
        output_.momentaryLufs = -std::numeric_limits<double>::infinity();
        output_.momentaryValid = false;
    }

    EnergyHop shortTerm;
    if (measurementWindow(shortTermHopCount, shortTermWindowFrames_, shortTerm)) {
        output_.shortTermLufs
            = energyToLufs(shortTerm.sum / static_cast<double>(shortTerm.frameCount));
        output_.shortTermValid = true;
    } else {
        output_.shortTermLufs = -std::numeric_limits<double>::infinity();
        output_.shortTermValid = false;
    }

    output_.measurementCompletionCount = measurementCompletionCount_;
    output_.measurementCapturedFrameEnd = capturedFrameEnd;
    telemetryMeasurementCompletions_.fetch_add(1, std::memory_order_relaxed);
    publishStateChange();
}

bool LoudnessAnalyzer::appendIntegrationHop(
    const EnergyHop hop, const std::uint64_t capturedFrameEnd) noexcept
{
    integrationHops_[integrationHopWriteIndex_] = hop;
    integrationHopWriteIndex_ = (integrationHopWriteIndex_ + 1) % integrationHops_.size();
    integrationHopCount_ = std::min(integrationHopCount_ + 1, integrationHops_.size());
    if (integrationHopCount_ < momentaryHopCount)
        return false;

    EnergyHop block;
    if (!integrationWindow(momentaryHopCount, momentaryWindowFrames_, block))
        return false;
    addIntegrationBlock(block.sum / static_cast<double>(block.frameCount), capturedFrameEnd);
    telemetryIntegrationBlockCompletions_.fetch_add(1, std::memory_order_relaxed);
    publishStateChange();
    return true;
}

LoudnessAnalyzer::EnergyHop LoudnessAnalyzer::sumLatestMeasurementHops(
    const std::size_t count) const noexcept
{
    EnergyHop result;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto index = (measurementHopWriteIndex_ + measurementHops_.size() - 1 - offset)
            % measurementHops_.size();
        result.sum += measurementHops_[index].sum;
        result.frameCount += measurementHops_[index].frameCount;
    }
    return result;
}

LoudnessAnalyzer::EnergyHop LoudnessAnalyzer::sumLatestIntegrationHops(
    const std::size_t count) const noexcept
{
    EnergyHop result;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto index = (integrationHopWriteIndex_ + integrationHops_.size() - 1 - offset)
            % integrationHops_.size();
        result.sum += integrationHops_[index].sum;
        result.frameCount += integrationHops_[index].frameCount;
    }
    return result;
}

bool LoudnessAnalyzer::measurementWindow(
    const std::size_t hopCount, const std::uint64_t windowFrames, EnergyHop& result) const noexcept
{
    if (measurementHopCount_ < hopCount)
        return false;

    result = sumLatestMeasurementHops(hopCount);
    if (result.frameCount == windowFrames)
        return true;

    const auto oldestIncludedIndex
        = (measurementHopWriteIndex_ + measurementHops_.size() - hopCount)
        % measurementHops_.size();
    if (result.frameCount > windowFrames) {
        const auto trim = result.frameCount - windowFrames;
        if (trim > hopEdgeSampleCapacity)
            return false;
        result.sum -= sumFirstSamples(
            measurementHops_[oldestIncludedIndex], static_cast<std::size_t>(trim));
    } else {
        const auto extend = windowFrames - result.frameCount;
        if (extend > hopEdgeSampleCapacity || measurementHopCount_ <= hopCount)
            return false;
        const auto precedingIndex
            = (oldestIncludedIndex + measurementHops_.size() - 1) % measurementHops_.size();
        result.sum
            += sumLastSamples(measurementHops_[precedingIndex], static_cast<std::size_t>(extend));
    }

    result.frameCount = windowFrames;
    return true;
}

bool LoudnessAnalyzer::integrationWindow(
    const std::size_t hopCount, const std::uint64_t windowFrames, EnergyHop& result) const noexcept
{
    if (integrationHopCount_ < hopCount)
        return false;

    result = sumLatestIntegrationHops(hopCount);
    if (result.frameCount == windowFrames)
        return true;

    const auto oldestIncludedIndex
        = (integrationHopWriteIndex_ + integrationHops_.size() - hopCount)
        % integrationHops_.size();
    if (result.frameCount > windowFrames) {
        const auto trim = result.frameCount - windowFrames;
        if (trim > hopEdgeSampleCapacity)
            return false;
        result.sum -= sumFirstSamples(
            integrationHops_[oldestIncludedIndex], static_cast<std::size_t>(trim));
    } else {
        const auto extend = windowFrames - result.frameCount;
        if (extend > hopEdgeSampleCapacity || integrationHopCount_ <= hopCount)
            return false;
        const auto precedingIndex
            = (oldestIncludedIndex + integrationHops_.size() - 1) % integrationHops_.size();
        result.sum
            += sumLastSamples(integrationHops_[precedingIndex], static_cast<std::size_t>(extend));
    }

    result.frameCount = windowFrames;
    return true;
}

void LoudnessAnalyzer::addIntegrationBlock(
    const double meanSquare, const std::uint64_t capturedFrameEnd) noexcept
{
    integrationBlockCount_ = incrementWithoutWrap(integrationBlockCount_);
    telemetryIntegrationBlocksSinceReset_.store(integrationBlockCount_, std::memory_order_relaxed);
    output_.integrationBlockCount = integrationBlockCount_;
    output_.integratedCapturedFrameEnd = capturedFrameEnd;

    if (integrationCapacityExceeded_)
        return;

    if (integrationBlockCount_ > effectiveIntegrationBlockCapacity_) {
        integrationCapacityExceeded_ = true;
        output_.integrationCapacityExceeded = true;
        output_.integratedValid = false;
        output_.integratedLufs = -std::numeric_limits<double>::infinity();
        output_.relativeGateLufs = -std::numeric_limits<double>::infinity();
        telemetryIntegrationCapacityExceeded_.store(1, std::memory_order_relaxed);
        telemetryIntegrationCapacityOverflows_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto absoluteThreshold = lufsToEnergy(absoluteGateLufs);
    if (std::isfinite(meanSquare) && meanSquare > absoluteThreshold) {
        if (!integrationEnergyIndex_.insert(meanSquare)) {
            integrationCapacityExceeded_ = true;
            output_.integrationCapacityExceeded = true;
            output_.integratedValid = false;
            output_.integratedLufs = -std::numeric_limits<double>::infinity();
            output_.relativeGateLufs = -std::numeric_limits<double>::infinity();
            telemetryIntegrationCapacityExceeded_.store(1, std::memory_order_relaxed);
            telemetryIntegrationCapacityOverflows_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        absoluteGatedEnergySum_ += meanSquare;
        absoluteGatedBlockCount_ = incrementWithoutWrap(absoluteGatedBlockCount_);
    }
    updateIntegratedOutput();
}

void LoudnessAnalyzer::updateIntegratedOutput() noexcept
{
    auto integratedLufs = -std::numeric_limits<double>::infinity();
    auto relativeGateLufs = -std::numeric_limits<double>::infinity();
    relativeGatedBlockCount_ = 0;

    if (absoluteGatedBlockCount_ != 0 && std::isfinite(absoluteGatedEnergySum_)
        && absoluteGatedEnergySum_ > 0.0) {
        const auto absoluteMean
            = absoluteGatedEnergySum_ / static_cast<double>(absoluteGatedBlockCount_);
        const auto relativeThreshold = absoluteMean * 0.1;
        relativeGateLufs = energyToLufs(relativeThreshold);
        const auto threshold = std::max(lufsToEnergy(absoluteGateLufs), relativeThreshold);
        const auto relative = integrationEnergyIndex_.queryGreaterThan(threshold);
        relativeGatedBlockCount_ = relative.count;
        if (relative.count != 0 && std::isfinite(relative.sum) && relative.sum > 0.0) {
            integratedLufs = energyToLufs(relative.sum / static_cast<double>(relative.count));
        }
    }

    output_.integratedValid = integrationBlockCount_ != 0;
    output_.integratedLufs = integratedLufs;
    output_.relativeGateLufs = relativeGateLufs;
    output_.absoluteGatedBlockCount = absoluteGatedBlockCount_;
    output_.relativeGatedBlockCount = relativeGatedBlockCount_;
    telemetryAbsoluteGatedBlocks_.store(absoluteGatedBlockCount_, std::memory_order_relaxed);
    telemetryRelativeGatedBlocks_.store(relativeGatedBlockCount_, std::memory_order_relaxed);
    publishIntegrationIndexStatistics();
}
} // namespace audio_insight
