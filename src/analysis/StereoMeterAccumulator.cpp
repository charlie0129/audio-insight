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
    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}
} // namespace

StereoMeterAccumulator::StereoMeterAccumulator() noexcept = default;

StereoMeterAccumulator::PublishResult StereoMeterAccumulator::publishBlock(const float* const left,
    const float* const right, const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation) noexcept
{
    PublishResult result;
    if (frameCount == 0)
        return result;

    result.sequence = nextSequence_++;
    capturedFrameCursor_ += frameCount;
    attemptedBlocks_.fetch_add(1, std::memory_order_relaxed);

    const auto aggregate = measure(
        left, right, frameCount, sampleRate, generation, result.sequence, capturedFrameCursor_);

    if (publishToFreeSlot(aggregate)) {
        result.published = true;
        publishedBlocks_.fetch_add(1, std::memory_order_relaxed);
        updateReadyHighWaterMark();
        return result;
    }

    if (coalesceIntoNewestReady(aggregate)) {
        result.published = true;
        result.coalesced = true;
        publishedBlocks_.fetch_add(1, std::memory_order_relaxed);
        coalescedBlocks_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // A consumer may have claimed the newest slot between our bounded scans.
    // Take one final bounded pass over newly freed storage, then drop safely.
    if (publishToFreeSlot(aggregate)) {
        result.published = true;
        publishedBlocks_.fetch_add(1, std::memory_order_relaxed);
        updateReadyHighWaterMark();
        return result;
    }

    result.dropped = true;
    droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

bool StereoMeterAccumulator::consumeLatest(StereoMeterReading& destination) noexcept
{
    std::array<Aggregate, slotCount * 2> acquired { };
    std::size_t acquiredCount = 0;

    // Repeat a fixed number of complete scans. This catches slots published into
    // an index already visited during the first scan without ever waiting.
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (auto& slot : slots_) {
            auto expected = SlotState::ready;
            if (!slot.state.compare_exchange_strong(expected, SlotState::reading,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                continue;
            }

            acquired[acquiredCount++] = slot.aggregate;
            slot.state.store(SlotState::free, std::memory_order_release);
        }
    }

    if (acquiredCount == 0)
        return false;

    const auto acquiredEnd = acquired.begin() + static_cast<std::ptrdiff_t>(acquiredCount);
    const auto newestGeneration = std::max_element(
        acquired.begin(), acquiredEnd, [](const Aggregate& left, const Aggregate& right) {
            return left.generation < right.generation;
        })->generation;

    std::sort(acquired.begin(), acquiredEnd, [](const Aggregate& left, const Aggregate& right) {
        if (left.generation != right.generation)
            return left.generation < right.generation;
        return left.firstSequence < right.firstSequence;
    });

    Aggregate combined;
    bool hasCombined = false;
    bool followsDiscontinuity = false;

    for (auto iterator = acquired.begin(); iterator != acquiredEnd; ++iterator) {
        const auto& value = *iterator;
        if (value.generation != newestGeneration)
            continue;

        if (!hasCombined) {
            combined = value;
            hasCombined = true;
            followsDiscontinuity = value.containsSequenceGap;
            continue;
        }

        const auto isContiguous = combined.lastSequence != std::numeric_limits<std::uint64_t>::max()
            && value.firstSequence == combined.lastSequence + 1;
        if (!isContiguous || value.containsSequenceGap
            || sampleRatesDiffer(combined.sampleRate, value.sampleRate)) {
            // RMS integration and peak hold restart at a missing interval. Keep
            // the newest contiguous segment rather than blending across a gap.
            combined = value;
            followsDiscontinuity = true;
            continue;
        }

        merge(combined, value);
    }

    const auto sequenceSpan = combined.lastSequence >= combined.firstSequence
        ? (combined.lastSequence - combined.firstSequence) + 1
        : 0;
    followsDiscontinuity = followsDiscontinuity || combined.containsSequenceGap
        || sequenceSpan != combined.representedBlocks;

    if (consumerHasPreviousSequence_ && combined.generation == consumerPreviousGeneration_)
        followsDiscontinuity
            = followsDiscontinuity || combined.firstSequence != consumerPreviousSequence_ + 1;

    if (followsDiscontinuity)
        consumerDiscontinuities_.fetch_add(1, std::memory_order_relaxed);

    consumerHasPreviousSequence_ = true;
    consumerPreviousGeneration_ = combined.generation;
    consumerPreviousSequence_ = combined.lastSequence;

    StereoMeterReading reading;
    reading.generation = combined.generation;
    reading.firstSequence = combined.firstSequence;
    reading.lastSequence = combined.lastSequence;
    reading.capturedFrameEnd = combined.capturedFrameEnd;
    reading.representedBlocks = combined.representedBlocks;
    reading.representedFrames = combined.frameCount;
    reading.sampleRate = combined.sampleRate;
    reading.followsDiscontinuity = followsDiscontinuity;

    for (std::size_t channel = 0; channel < 2; ++channel) {
        reading.peakLinear[channel] = combined.peak[channel];
        reading.rmsLinear[channel] = combined.frameCount > 0
            ? static_cast<float>(std::sqrt(
                  combined.sumSquares[channel] / static_cast<double>(combined.frameCount)))
            : 0.0F;
        reading.peakDecibels[channel] = linearToDecibels(reading.peakLinear[channel]);
        reading.rmsDecibels[channel] = linearToDecibels(reading.rmsLinear[channel]);
    }

    destination = reading;
    return true;
}

bool StereoMeterAccumulator::consumeInto(VisualizationFrame& destination) noexcept
{
    StereoMeterReading reading;
    if (!consumeLatest(reading))
        return false;

    destination.peakDecibels = reading.peakDecibels;
    destination.rmsDecibels = reading.rmsDecibels;
    return true;
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

void StereoMeterAccumulator::discardPending() noexcept
{
    for (auto& slot : slots_) {
        const auto state = slot.state.load(std::memory_order_acquire);
        assert(state != SlotState::writing && state != SlotState::reading);

        if (state == SlotState::ready)
            slot.state.store(SlotState::free, std::memory_order_release);
    }

    consumerHasPreviousSequence_ = false;
    consumerPreviousGeneration_ = 0;
    consumerPreviousSequence_ = 0;
}

StereoMeterAccumulator::Aggregate StereoMeterAccumulator::measure(const float* const left,
    const float* const right, const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation, const std::uint64_t sequence,
    const std::uint64_t capturedFrameEnd) noexcept
{
    Aggregate result;
    result.frameCount = frameCount;
    result.representedBlocks = 1;
    result.generation = generation;
    result.firstSequence = sequence;
    result.lastSequence = sequence;
    result.capturedFrameEnd = capturedFrameEnd;
    result.sampleRate = sampleRate;

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const std::array<float, 2> samples {
            left != nullptr && std::isfinite(left[frame]) ? left[frame] : 0.0F,
            right != nullptr && std::isfinite(right[frame]) ? right[frame] : 0.0F
        };

        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto magnitude = std::abs(samples[channel]);
            result.peak[channel] = std::max(result.peak[channel], magnitude);
            result.sumSquares[channel]
                += static_cast<double>(samples[channel]) * static_cast<double>(samples[channel]);
        }
    }

    return result;
}

void StereoMeterAccumulator::merge(Aggregate& destination, const Aggregate& source) noexcept
{
    if (destination.generation != source.generation) {
        if (source.generation > destination.generation)
            destination = source;
        return;
    }

    destination.containsSequenceGap = destination.containsSequenceGap || source.containsSequenceGap;
    destination.firstSequence = std::min(destination.firstSequence, source.firstSequence);
    destination.lastSequence = std::max(destination.lastSequence, source.lastSequence);
    destination.capturedFrameEnd = std::max(destination.capturedFrameEnd, source.capturedFrameEnd);
    destination.representedBlocks += source.representedBlocks;
    destination.frameCount += source.frameCount;
    const auto sourceIsNewer = source.lastSequence >= destination.lastSequence;
    if (sourceIsNewer)
        destination.sampleRate = source.sampleRate;

    for (std::size_t channel = 0; channel < 2; ++channel) {
        destination.peak[channel] = std::max(destination.peak[channel], source.peak[channel]);
        destination.sumSquares[channel] += source.sumSquares[channel];
    }
}

float StereoMeterAccumulator::linearToDecibels(const float value) noexcept
{
    constexpr auto floorLinear = 1.0e-6F; // -120 dBFS
    if (!std::isfinite(value) || value <= floorLinear)
        return minimumDisplayDecibels;

    return 20.0F * std::log10(value);
}

bool StereoMeterAccumulator::publishToFreeSlot(const Aggregate& aggregate) noexcept
{
    for (auto& slot : slots_) {
        auto expected = SlotState::free;
        if (!slot.state.compare_exchange_strong(expected, SlotState::writing,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            continue;
        }

        slot.aggregate = aggregate;
        slot.state.store(SlotState::ready, std::memory_order_release);
        return true;
    }

    return false;
}

bool StereoMeterAccumulator::coalesceIntoNewestReady(const Aggregate& aggregate) noexcept
{
    std::size_t newestIndex = slots_.size();
    std::uint64_t newestSequence = 0;

    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state.load(std::memory_order_acquire) != SlotState::ready)
            continue;

        if (newestIndex == slots_.size() || slots_[index].aggregate.lastSequence > newestSequence) {
            newestIndex = index;
            newestSequence = slots_[index].aggregate.lastSequence;
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

    const auto sameGeneration = slot.aggregate.generation == aggregate.generation;
    const auto contiguous = slot.aggregate.lastSequence != std::numeric_limits<std::uint64_t>::max()
        && slot.aggregate.lastSequence + 1 == aggregate.firstSequence;
    const auto sameSampleRate = !sampleRatesDiffer(slot.aggregate.sampleRate, aggregate.sampleRate);
    if (sameGeneration && contiguous && sameSampleRate) {
        merge(slot.aggregate, aggregate);
    } else {
        // Across lifecycle/sequence discontinuities, begin a fresh RMS interval.
        slot.aggregate = aggregate;
        slot.aggregate.containsSequenceGap = sameGeneration && (!contiguous || !sameSampleRate);
    }

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
