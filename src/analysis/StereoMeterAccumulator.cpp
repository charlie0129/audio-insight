// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StereoMeterAccumulator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace audio_insight {
namespace {
[[nodiscard]] bool sampleRatesDiffer(const double left, const double right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return true;

    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}

[[nodiscard]] std::uint64_t saturatingAdd(
    const std::uint64_t left, const std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}
} // namespace

StereoMeterAccumulator::StereoMeterAccumulator() noexcept = default;

StereoMeterAccumulator::PublishResult StereoMeterAccumulator::publishBlock(const float* const left,
    const float* const right, const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation, const std::uint32_t channelCount,
    const bool followsDiscontinuity) noexcept
{
    PublishResult result;
    if (frameCount == 0)
        return result;

    result.sequence = nextSequence_++;
    capturedFrameCursor_ += frameCount;
    attemptedBlocks_.fetch_add(1, std::memory_order_relaxed);

    const auto reading = measureEndpoint(left, right, frameCount, sampleRate, generation,
        result.sequence, capturedFrameCursor_, channelCount, followsDiscontinuity);
    const auto hasCorrelationEndpoint = reading.valid && reading.channelCount == 2;
    if (hasCorrelationEndpoint)
        correlationProcessedSamples_.fetch_add(frameCount, std::memory_order_relaxed);

    if (publishToFreeSlot(reading)) {
        result.published = true;
        publishedBlocks_.fetch_add(1, std::memory_order_relaxed);
        if (hasCorrelationEndpoint)
            correlationPublishedEndpoints_.fetch_add(1, std::memory_order_relaxed);
        updateReadyHighWaterMark();
        return result;
    }

    if (coalesceIntoNewestReady(reading)) {
        result.published = true;
        result.coalesced = true;
        publishedBlocks_.fetch_add(1, std::memory_order_relaxed);
        coalescedBlocks_.fetch_add(1, std::memory_order_relaxed);
        if (hasCorrelationEndpoint)
            correlationPublishedEndpoints_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // A consumer may have claimed the newest slot between our bounded scans.
    // Take one final bounded pass over newly freed storage, then drop safely.
    if (publishToFreeSlot(reading)) {
        result.published = true;
        publishedBlocks_.fetch_add(1, std::memory_order_relaxed);
        if (hasCorrelationEndpoint)
            correlationPublishedEndpoints_.fetch_add(1, std::memory_order_relaxed);
        updateReadyHighWaterMark();
        return result;
    }

    result.dropped = true;
    droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool StereoMeterAccumulator::consumeLatest(StereoMeterReading& destination) noexcept
{
    std::array<StereoMeterReading, slotCount * 2> acquired { };
    std::size_t acquiredCount = 0;
    std::uint64_t acquiredCorrelationEndpoints = 0;

    // Repeat a fixed number of complete scans. This catches slots published into
    // an index already visited during the first scan without ever waiting.
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (auto& slot : slots_) {
            auto expected = SlotState::ready;
            if (!slot.state.compare_exchange_strong(expected, SlotState::reading,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                continue;
            }

            acquired[acquiredCount++] = slot.reading;
            if (slot.reading.valid && slot.reading.channelCount == 2)
                ++acquiredCorrelationEndpoints;
            slot.state.store(SlotState::free, std::memory_order_release);
        }
    }

    if (acquiredCount == 0)
        return false;

    correlationConsumedEndpoints_.fetch_add(
        acquiredCorrelationEndpoints, std::memory_order_relaxed);

    const auto acquiredEnd = acquired.begin() + static_cast<std::ptrdiff_t>(acquiredCount);
    std::sort(acquired.begin(), acquiredEnd,
        [](const StereoMeterReading& left, const StereoMeterReading& right) {
            return left.lastSequence < right.lastSequence;
        });

    StereoMeterReading combined;
    auto hasCombined = false;
    for (auto iterator = acquired.begin(); iterator != acquiredEnd; ++iterator) {
        auto value = *iterator;
        if (!hasCombined) {
            combined = value;
            hasCombined = true;
            continue;
        }

        const auto contiguous = combined.lastSequence != std::numeric_limits<std::uint64_t>::max()
            && value.firstSequence == combined.lastSequence + 1;
        if (contiguous && formatsMatch(combined, value) && !value.followsDiscontinuity) {
            prependRepresentedMetadata(value, combined);
            combined = value;
        } else {
            value.followsDiscontinuity = true;
            combined = value;
        }
    }

    if (consumerHasPreviousSequence_) {
        const auto contiguous
            = consumerPreviousSequence_ != std::numeric_limits<std::uint64_t>::max()
            && combined.firstSequence == consumerPreviousSequence_ + 1;
        const auto sameFormat = consumerPreviousValid_ && combined.valid
            && consumerPreviousGeneration_ == combined.generation
            && consumerPreviousChannelCount_ == combined.channelCount
            && !sampleRatesDiffer(consumerPreviousSampleRate_, combined.sampleRate);
        combined.followsDiscontinuity = combined.followsDiscontinuity || !contiguous || !sameFormat;
    }

    if (combined.followsDiscontinuity)
        consumerDiscontinuities_.fetch_add(1, std::memory_order_relaxed);

    consumerHasPreviousSequence_ = true;
    consumerPreviousValid_ = combined.valid;
    consumerPreviousGeneration_ = combined.generation;
    consumerPreviousSequence_ = combined.lastSequence;
    consumerPreviousChannelCount_ = combined.channelCount;
    consumerPreviousSampleRate_ = combined.sampleRate;

    destination = combined;
    return true;
}

bool StereoMeterAccumulator::consumeInto(VisualizationFrame& destination) noexcept
{
    StereoMeterReading reading;
    if (!consumeLatest(reading))
        return false;

    destination.peakDecibels = reading.peakDecibels;
    destination.rmsDecibels = reading.rmsDecibels;
    destination.heldPeakDecibels = reading.heldPeakDecibels;
    destination.over = reading.over;
    destination.stereoCorrelation = reading.correlation;
    destination.stereoCorrelationValid = reading.correlationValid;
    destination.stereoMono = reading.valid && reading.channelCount == 1;
    destination.channelCount = reading.channelCount;
    destination.sampleRate = reading.sampleRate;
    destination.meterSequence = reading.lastSequence;
    destination.meterValid = reading.valid;
    return true;
}

std::uint64_t StereoMeterAccumulator::requestUserReset() noexcept
{
    return requestedUserResetEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint64_t StereoMeterAccumulator::requestLiveClear() noexcept
{
    return requestedLiveClearEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint64_t StereoMeterAccumulator::requestedUserResetEpoch() const noexcept
{
    return requestedUserResetEpoch_.load(std::memory_order_acquire);
}

std::uint64_t StereoMeterAccumulator::requestedLiveClearEpoch() const noexcept
{
    return requestedLiveClearEpoch_.load(std::memory_order_acquire);
}

StereoMeterAccumulator::Telemetry StereoMeterAccumulator::telemetry() const noexcept
{
    Telemetry result;
    result.attemptedBlocks = attemptedBlocks_.load(std::memory_order_relaxed);
    result.publishedBlocks = publishedBlocks_.load(std::memory_order_relaxed);
    result.coalescedBlocks = coalescedBlocks_.load(std::memory_order_relaxed);
    result.droppedBlocks = droppedBlocks_.load(std::memory_order_relaxed);
    result.consumerDiscontinuities = consumerDiscontinuities_.load(std::memory_order_relaxed);
    result.readyHighWaterMark = readyHighWaterMark_.load(std::memory_order_relaxed);

    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == SlotState::ready)
            ++result.readySlots;

    return result;
}

StereoMeterAccumulator::CorrelationTelemetry
StereoMeterAccumulator::correlationTelemetry() const noexcept
{
    return {
        correlationProcessedSamples_.load(std::memory_order_relaxed),
        correlationPublishedEndpoints_.load(std::memory_order_relaxed),
        correlationConsumedEndpoints_.load(std::memory_order_relaxed),
        correlationStateResets_.load(std::memory_order_relaxed),
    };
}

void StereoMeterAccumulator::discardPending() noexcept
{
    for (auto& slot : slots_) {
        const auto state = slot.state.load(std::memory_order_acquire);
        assert(state != SlotState::writing && state != SlotState::reading);

        if (state == SlotState::ready)
            slot.state.store(SlotState::free, std::memory_order_release);
    }

    consumerHasPreviousSequence_ = false;
    consumerPreviousValid_ = false;
    consumerPreviousGeneration_ = 0;
    consumerPreviousSequence_ = 0;
    consumerPreviousChannelCount_ = 0;
    consumerPreviousSampleRate_ = 0.0;
}

StereoMeterReading StereoMeterAccumulator::measureEndpoint(const float* const left,
    const float* const right, const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation, const std::uint64_t sequence,
    const std::uint64_t capturedFrameEnd, const std::uint32_t channelCount,
    const bool followsDiscontinuity) noexcept
{
    const auto previous = ballistics_.current();
    const auto formatChanged = previous.valid
        && (previous.generation != generation || previous.channelCount != channelCount
            || sampleRatesDiffer(previous.sampleRate, sampleRate));
    auto correlationWasReset
        = previous.valid && previous.channelCount == 2 && (formatChanged || followsDiscontinuity);

    const auto requestedUserReset = requestedUserResetEpoch_.load(std::memory_order_acquire);
    if (requestedUserReset != appliedUserResetEpoch_) {
        ballistics_.userReset();
        appliedUserResetEpoch_ = requestedUserReset;
    }

    const auto requestedLiveClear = requestedLiveClearEpoch_.load(std::memory_order_acquire);
    if (requestedLiveClear != appliedLiveClearEpoch_) {
        ballistics_.clearLiveMeasurements();
        appliedLiveClearEpoch_ = requestedLiveClear;
        correlationWasReset = correlationWasReset || (previous.valid && previous.channelCount == 2);
    }

    const auto endpoint = ballistics_.processBlock(
        left, right, frameCount, sampleRate, generation, channelCount, followsDiscontinuity);
    if (correlationWasReset)
        correlationStateResets_.fetch_add(1, std::memory_order_relaxed);

    StereoMeterReading reading;
    reading.peakLinear = endpoint.liveSamplePeakLinear;
    reading.rmsLinear = endpoint.rmsLinear;
    reading.heldPeakLinear = endpoint.heldSamplePeakLinear;
    reading.rmsMeanSquare = endpoint.rmsMeanSquare;
    reading.peakDecibels = endpoint.liveSamplePeakDecibels;
    reading.rmsDecibels = endpoint.rmsDecibels;
    reading.heldPeakDecibels = endpoint.heldSamplePeakDecibels;
    reading.over = endpoint.over;
    reading.crossMeanProduct = endpoint.crossMeanProduct;
    reading.correlation = endpoint.correlation;
    reading.generation = generation;
    reading.firstSequence = sequence;
    reading.lastSequence = sequence;
    reading.capturedFrameEnd = capturedFrameEnd;
    reading.representedBlocks = 1;
    reading.representedFrames = frameCount;
    reading.appliedUserResetEpoch = appliedUserResetEpoch_;
    reading.appliedLiveClearEpoch = appliedLiveClearEpoch_;
    reading.channelCount = endpoint.channelCount;
    reading.sampleRate = endpoint.sampleRate;
    reading.rawCaptureDiscontinuity = followsDiscontinuity;
    reading.followsDiscontinuity = followsDiscontinuity || formatChanged;
    reading.valid = endpoint.valid;
    reading.correlationValid = endpoint.correlationValid;
    return reading;
}

void StereoMeterAccumulator::prependRepresentedMetadata(
    StereoMeterReading& newer, const StereoMeterReading& older) noexcept
{
    newer.firstSequence = older.firstSequence;
    newer.representedBlocks = saturatingAdd(older.representedBlocks, newer.representedBlocks);
    newer.representedFrames = saturatingAdd(older.representedFrames, newer.representedFrames);
    newer.rawCaptureDiscontinuity = newer.rawCaptureDiscontinuity || older.rawCaptureDiscontinuity;
    newer.followsDiscontinuity = newer.followsDiscontinuity || older.followsDiscontinuity;
}

#if defined(JUCE_UNIT_TESTS) && JUCE_UNIT_TESTS
void StereoMeterAccumulator::skipNextEndpointSequenceForTesting() noexcept
{
    ++nextSequence_;
}
#endif

bool StereoMeterAccumulator::formatsMatch(
    const StereoMeterReading& left, const StereoMeterReading& right) noexcept
{
    return left.valid && right.valid && left.generation == right.generation
        && left.channelCount == right.channelCount
        && !sampleRatesDiffer(left.sampleRate, right.sampleRate);
}

bool StereoMeterAccumulator::publishToFreeSlot(const StereoMeterReading& reading) noexcept
{
    for (auto& slot : slots_) {
        auto expected = SlotState::free;
        if (!slot.state.compare_exchange_strong(expected, SlotState::writing,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            continue;
        }

        slot.reading = reading;
        slot.state.store(SlotState::ready, std::memory_order_release);
        return true;
    }

    return false;
}

bool StereoMeterAccumulator::coalesceIntoNewestReady(const StereoMeterReading& reading) noexcept
{
    std::size_t newestIndex = slots_.size();
    std::uint64_t newestSequence = 0;

    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
            continue;

        if (newestIndex == slots_.size() || slots_[index].reading.lastSequence > newestSequence) {
            newestIndex = index;
            newestSequence = slots_[index].reading.lastSequence;
        }
    }

    if (newestIndex == slots_.size())
        return false;

    auto& slot = slots_[newestIndex];
    auto expected = SlotState::ready;
    if (!slot.state.compare_exchange_strong(
            expected, SlotState::writing, std::memory_order_acquire, std::memory_order_relaxed)) {
        return false;
    }

    auto replacement = reading;
    const auto contiguous = slot.reading.lastSequence != std::numeric_limits<std::uint64_t>::max()
        && reading.firstSequence == slot.reading.lastSequence + 1;
    if (contiguous && formatsMatch(slot.reading, reading) && !reading.followsDiscontinuity) {
        prependRepresentedMetadata(replacement, slot.reading);
    } else {
        replacement.followsDiscontinuity = true;
    }

    slot.reading = replacement;
    slot.state.store(SlotState::ready, std::memory_order_release);
    return true;
}

void StereoMeterAccumulator::updateReadyHighWaterMark() noexcept
{
    std::uint32_t readyCount = 0;
    for (const auto& slot : slots_)
        if (slot.state.load(std::memory_order_acquire) == SlotState::ready)
            ++readyCount;

    if (readyCount > producerReadyHighWaterMark_) {
        producerReadyHighWaterMark_ = readyCount;
        readyHighWaterMark_.store(readyCount, std::memory_order_relaxed);
    }
}
} // namespace audio_insight
