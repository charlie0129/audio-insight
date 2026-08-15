// SPDX-License-Identifier: AGPL-3.0-or-later

#include "FrameLatencyHistory.h"

#include <algorithm>
#include <utility>

namespace audio_insight {
namespace {
constexpr std::uint8_t gpuCompletionPublished = 1U << 0U;
constexpr std::uint8_t presentationPublished = 1U << 1U;
constexpr std::uint8_t timingSampleEmitted = 1U << 2U;
constexpr std::uint8_t requiredTimingEvents = gpuCompletionPublished | presentationPublished;

constexpr std::uint8_t presentationUnknown = 0;
constexpr std::uint8_t presentationSkipped = 1;
constexpr std::uint8_t presentationPresented = 2;
} // namespace

FrameLatencySample classifyFrameLatency(const std::uint64_t sequence,
    const std::uint64_t callbackNanoseconds, const std::uint64_t cpuReadyNanoseconds,
    const std::uint64_t gpuStartNanoseconds, const std::uint64_t gpuEndNanoseconds,
    const std::uint64_t presentedNanoseconds) noexcept
{
    FrameLatencySample sample;
    sample.sequence = sequence;
    sample.presentedHostTimestampNanoseconds = presentedNanoseconds;

    if (sequence == 0 || callbackNanoseconds == 0 || presentedNanoseconds == 0
        || presentedNanoseconds < callbackNanoseconds) {
        return sample;
    }

    sample.totalNanoseconds = presentedNanoseconds - callbackNanoseconds;
    sample.totalValid = true;

    if (cpuReadyNanoseconds == 0 || gpuStartNanoseconds == 0 || gpuEndNanoseconds == 0
        || cpuReadyNanoseconds < callbackNanoseconds || gpuStartNanoseconds < cpuReadyNanoseconds
        || gpuEndNanoseconds < gpuStartNanoseconds || presentedNanoseconds < gpuEndNanoseconds) {
        return sample;
    }

    sample.cpuEncodeNanoseconds = cpuReadyNanoseconds - callbackNanoseconds;
    sample.submitQueueWaitNanoseconds = gpuStartNanoseconds - cpuReadyNanoseconds;
    sample.gpuExecutionNanoseconds = gpuEndNanoseconds - gpuStartNanoseconds;
    sample.compositorWaitNanoseconds = presentedNanoseconds - gpuEndNanoseconds;
    sample.componentsValid = true;
    return sample;
}

FrameLatencySubmission::FrameLatencySubmission(
    const std::uint64_t sequence, const std::uint64_t callbackNanoseconds) noexcept
    : sequence_(sequence), callbackNanoseconds_(callbackNanoseconds)
{
}

void FrameLatencySubmission::setCpuReadyTimestamp(const std::uint64_t cpuReadyNanoseconds) noexcept
{
    cpuReadyNanoseconds_.store(cpuReadyNanoseconds, std::memory_order_release);
}

bool FrameLatencySubmission::recordGpuCompletion(const std::uint64_t gpuStartNanoseconds,
    const std::uint64_t gpuEndNanoseconds, FrameLatencySample& sample) noexcept
{
    gpuStartNanoseconds_.store(gpuStartNanoseconds, std::memory_order_relaxed);
    gpuEndNanoseconds_.store(gpuEndNanoseconds, std::memory_order_relaxed);
    return publishTimingEvent(gpuCompletionPublished, sample);
}

bool FrameLatencySubmission::recordPresentation(
    const std::uint64_t presentedNanoseconds, FrameLatencySample& sample) noexcept
{
    presentedNanoseconds_.store(presentedNanoseconds, std::memory_order_relaxed);
    return publishTimingEvent(presentationPublished, sample);
}

PresentationOutcomeTransition FrameLatencySubmission::classifySkipped() noexcept
{
    auto expected = presentationUnknown;

    if (presentationOutcome_.compare_exchange_strong(
            expected, presentationSkipped, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return PresentationOutcomeTransition::newlySkipped;
    }

    return PresentationOutcomeTransition::none;
}

PresentationOutcomeTransition FrameLatencySubmission::classifyPresented() noexcept
{
    auto current = presentationOutcome_.load(std::memory_order_acquire);

    for (;;) {
        if (current == presentationPresented)
            return PresentationOutcomeTransition::none;

        const auto transition = current == presentationSkipped
            ? PresentationOutcomeTransition::upgradedSkippedToPresented
            : PresentationOutcomeTransition::newlyPresented;

        if (presentationOutcome_.compare_exchange_weak(current, presentationPresented,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return transition;
        }
    }
}

bool FrameLatencySubmission::publishTimingEvent(
    const std::uint8_t event, FrameLatencySample& sample) noexcept
{
    auto state = static_cast<std::uint8_t>(
        timingState_.fetch_or(event, std::memory_order_acq_rel) | event);

    while ((state & requiredTimingEvents) == requiredTimingEvents
        && (state & timingSampleEmitted) == 0) {
        const auto desired = static_cast<std::uint8_t>(state | timingSampleEmitted);

        if (timingState_.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
            sample = classifyFrameLatency(sequence_, callbackNanoseconds_,
                cpuReadyNanoseconds_.load(std::memory_order_acquire),
                gpuStartNanoseconds_.load(std::memory_order_acquire),
                gpuEndNanoseconds_.load(std::memory_order_acquire),
                presentedNanoseconds_.load(std::memory_order_acquire));
            return true;
        }
    }

    return false;
}

bool FrameLatencyHistory::record(FrameLatencySample sample) noexcept
{
    if (sample.sequence == 0 || sample.presentedHostTimestampNanoseconds == 0)
        return false;

    const auto lessByPresentation = [](const FrameLatencySample& left,
                                        const FrameLatencySample& right) {
        if (left.presentedHostTimestampNanoseconds != right.presentedHostTimestampNanoseconds) {
            return left.presentedHostTimestampNanoseconds < right.presentedHostTimestampNanoseconds;
        }

        return left.sequence < right.sequence;
    };
    auto begin = samples_.begin();
    auto end = begin + static_cast<std::ptrdiff_t>(count_);

    if (std::find_if(begin, end,
            [&sample](const FrameLatencySample& existing) {
                return existing.sequence == sample.sequence;
            })
        != end) {
        return false;
    }

    auto insertion = std::lower_bound(begin, end, sample, lessByPresentation);

    if (count_ == samples_.size()) {
        if (!lessByPresentation(samples_.front(), sample))
            return false;

        std::move(begin + 1, end, begin);
        --count_;
        end = begin + static_cast<std::ptrdiff_t>(count_);
        insertion = std::lower_bound(begin, end, sample, lessByPresentation);
    }

    std::move_backward(insertion, end, end + 1);
    *insertion = sample;
    ++count_;
    return true;
}

std::size_t FrameLatencyHistory::snapshot(
    std::array<FrameLatencySample, frameLatencyHistoryCapacity>& destination) const noexcept
{
    std::copy_n(samples_.begin(), count_, destination.begin());
    return count_;
}
} // namespace audio_insight
