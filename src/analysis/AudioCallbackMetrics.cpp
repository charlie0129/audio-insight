// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AudioCallbackMetrics.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <mach/mach_time.h>

namespace audio_insight {
namespace {
[[nodiscard]] std::uint64_t saturatingAdd(
    const std::uint64_t left, const std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] std::uint64_t callbackBudgetNanoseconds(
    const std::uint32_t frameCount, const double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return 0;

    constexpr auto nanosecondsPerSecond = 1'000'000'000.0L;
    constexpr auto blockDurationBudgetProportion = 0.02L;
    const auto blockDurationNanoseconds
        = (static_cast<long double>(frameCount) * nanosecondsPerSecond) / sampleRate;
    auto budget = blockDurationNanoseconds * blockDurationBudgetProportion;
    if (frameCount == 128)
        budget = std::min(budget, 25'000.0L);

    if (!std::isfinite(budget) || budget <= 0.0L)
        return 0;

    const auto roundedBudget = std::round(budget);
    const auto maximum = static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    if (roundedBudget >= maximum)
        return std::numeric_limits<std::uint64_t>::max();

    return static_cast<std::uint64_t>(roundedBudget);
}
} // namespace

MachContinuousTimebase MachContinuousTimebase::system() noexcept
{
    mach_timebase_info_data_t information { };
    const auto status = mach_timebase_info(&information);
    return { information.numer, information.denom,
        status == KERN_SUCCESS && information.numer != 0 && information.denom != 0 };
}

bool MachContinuousTimebase::durationNanoseconds(const std::uint64_t startTicks,
    const std::uint64_t endTicks, std::uint64_t& destination) const noexcept
{
    destination = 0;
    if (!valid || numerator == 0 || denominator == 0 || endTicks < startTicks)
        return false;

    const auto elapsedTicks = endTicks - startTicks;
    if (numerator == denominator) {
        destination = elapsedTicks;
        return true;
    }

    const auto scaled = static_cast<unsigned __int128>(elapsedTicks) * numerator;
    const auto nanoseconds = scaled / denominator;
    if (nanoseconds > std::numeric_limits<std::uint64_t>::max())
        return false;

    destination = static_cast<std::uint64_t>(nanoseconds);
    return true;
}

std::uint64_t readMachContinuousTime() noexcept
{
    return mach_continuous_time();
}

AudioCallbackMetrics::AudioCallbackMetrics(const MachContinuousTimebase timebase) noexcept
    : timebase_(timebase)
{
}

void AudioCallbackMetrics::configureSampleRate(const double sampleRate) noexcept
{
    for (auto index = std::size_t { 0 }; index < blocks_.size(); ++index) {
        blocks_[index].budgetNanoseconds.store(
            callbackBudgetNanoseconds(trackedAudioCallbackBlockSizes[index], sampleRate),
            std::memory_order_release);
    }
}

AudioCallbackMetrics::CallbackToken AudioCallbackMetrics::beginCallback(
    const std::uint32_t frameCount, const std::uint64_t startTicks) noexcept
{
    if (activeCallbacks_.fetch_add(1, std::memory_order_acq_rel) != 0)
        concurrentCallbackViolations_.fetch_add(1, std::memory_order_relaxed);

    return { startTicks, frameCount };
}

void AudioCallbackMetrics::finishCallback(
    const CallbackToken token, const std::uint64_t endTicks) noexcept
{
    callbackCount_.fetch_add(1, std::memory_order_relaxed);
    processedFrames_.fetch_add(token.frameCount, std::memory_order_relaxed);

    const auto index = blockIndex(token.frameCount);
    if (index < blocks_.size())
        blocks_[index].callbackCount.fetch_add(1, std::memory_order_relaxed);
    else
        untrackedBlockSizeCallbacks_.fetch_add(1, std::memory_order_relaxed);

    std::uint64_t durationNanoseconds = 0;
    if (!timebase_.durationNanoseconds(token.startTicks, endTicks, durationNanoseconds)) {
        timingUnavailable_.fetch_add(1, std::memory_order_relaxed);
        if (timebase_.valid && endTicks < token.startTicks)
            clockAnomalyViolations_.fetch_add(1, std::memory_order_relaxed);
        activeCallbacks_.fetch_sub(1, std::memory_order_release);
        return;
    }

    timingSamples_.fetch_add(1, std::memory_order_relaxed);
    if (index < blocks_.size()) {
        auto& block = blocks_[index];
        block.timingSamples.fetch_add(1, std::memory_order_relaxed);
        block.durationHistogram[durationBucket(durationNanoseconds)].fetch_add(
            1, std::memory_order_relaxed);

        const auto budget = block.budgetNanoseconds.load(std::memory_order_acquire);
        if (budget != 0 && durationNanoseconds > budget) {
            block.budgetExceeded.fetch_add(1, std::memory_order_relaxed);
            budgetExceeded_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    activeCallbacks_.fetch_sub(1, std::memory_order_release);
}

AudioCallbackTelemetry AudioCallbackMetrics::telemetry() const noexcept
{
    AudioCallbackTelemetry result;
    result.callbackCount = callbackCount_.load(std::memory_order_relaxed);
    result.processedFrames = processedFrames_.load(std::memory_order_relaxed);
    result.timingSamples = timingSamples_.load(std::memory_order_relaxed);
    result.timingUnavailable = timingUnavailable_.load(std::memory_order_relaxed);
    result.budgetExceeded = budgetExceeded_.load(std::memory_order_relaxed);
    result.untrackedBlockSizeCallbacks
        = untrackedBlockSizeCallbacks_.load(std::memory_order_relaxed);
    result.concurrentCallbackViolations
        = concurrentCallbackViolations_.load(std::memory_order_relaxed);
    result.clockAnomalyViolations = clockAnomalyViolations_.load(std::memory_order_relaxed);
    result.rtSafetyViolationCount
        = saturatingAdd(result.concurrentCallbackViolations, result.clockAnomalyViolations);
    result.detectorCoverageFlags
        = concurrentCallbackDetection | (timebase_.valid ? monotonicClockAnomalyDetection : 0U);
    result.clockAvailable = timebase_.valid;

    for (auto index = std::size_t { 0 }; index < blocks_.size(); ++index) {
        const auto& source = blocks_[index];
        auto& destination = result.trackedBlocks[index];
        destination.callbackCount = source.callbackCount.load(std::memory_order_relaxed);
        destination.timingSamples = source.timingSamples.load(std::memory_order_relaxed);
        destination.budgetExceeded = source.budgetExceeded.load(std::memory_order_relaxed);
        destination.budgetNanoseconds = source.budgetNanoseconds.load(std::memory_order_acquire);
        for (auto bucket = std::size_t { 0 }; bucket < destination.durationHistogram.size();
            ++bucket) {
            destination.durationHistogram[bucket]
                = source.durationHistogram[bucket].load(std::memory_order_relaxed);
        }
    }

    return result;
}

std::size_t AudioCallbackMetrics::blockIndex(const std::uint32_t frameCount) noexcept
{
    const auto match = std::find(
        trackedAudioCallbackBlockSizes.begin(), trackedAudioCallbackBlockSizes.end(), frameCount);
    return static_cast<std::size_t>(match - trackedAudioCallbackBlockSizes.begin());
}

std::size_t AudioCallbackMetrics::durationBucket(const std::uint64_t nanoseconds) noexcept
{
    return static_cast<std::size_t>(
        std::min<std::uint64_t>(nanoseconds / audioCallbackDurationHistogramBucketNanoseconds,
            audioCallbackDurationHistogramOverflowBucket));
}
} // namespace audio_insight
