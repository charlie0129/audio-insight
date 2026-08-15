// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PresentedFrameHistory.h"

#include <algorithm>
#include <utility>

namespace audio_insight {
bool PresentedFrameHistory::recordPresentation(
    const std::uint64_t sequence, const std::uint64_t presentedHostTimestampNanoseconds) noexcept
{
    if (sequence == 0 || presentedHostTimestampNanoseconds == 0)
        return false;

    const auto lessByTimestamp = [](const TimestampSample& sample, const std::uint64_t timestamp) {
        return sample.nanoseconds < timestamp;
    };
    auto begin = timestamps_.begin();
    auto end = begin + static_cast<std::ptrdiff_t>(count_);
    auto insertion
        = std::lower_bound(begin, end, presentedHostTimestampNanoseconds, lessByTimestamp);

    // A drawable has one actual presentation time. Treat a repeated timestamp
    // as a duplicate callback instead of manufacturing a zero-length interval.
    if (insertion != end && insertion->nanoseconds == presentedHostTimestampNanoseconds)
        return false;

    if (count_ == timestamps_.size()) {
        // A callback can be delayed indefinitely. Once its timestamp has fallen
        // outside the newest retained window, ignoring it leaves that window
        // exact and avoids displacing newer pacing data.
        if (presentedHostTimestampNanoseconds <= timestamps_.front().nanoseconds)
            return false;

        std::move(begin + 1, end, begin);
        --count_;
        end = begin + static_cast<std::ptrdiff_t>(count_);
        insertion
            = std::lower_bound(begin, end, presentedHostTimestampNanoseconds, lessByTimestamp);
    }

    std::move_backward(insertion, end, end + 1);
    *insertion = { sequence, presentedHostTimestampNanoseconds };
    ++count_;
    return true;
}

std::size_t PresentedFrameHistory::snapshotIntervals(
    std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>& destination)
    const noexcept
{
    auto destinationCount = std::size_t { 0 };

    for (std::size_t index = 1; index < count_; ++index) {
        const auto previous = timestamps_[index - 1].nanoseconds;
        const auto current = timestamps_[index].nanoseconds;

        if (current <= previous)
            continue;

        destination[destinationCount++] = { timestamps_[index].sequence, current - previous };
    }

    return destinationCount;
}

std::uint64_t PresentedFrameHistory::latestTimestampNanoseconds() const noexcept
{
    return count_ != 0 ? timestamps_[count_ - 1].nanoseconds : 0;
}

std::uint64_t PresentedFrameHistory::latestIntervalNanoseconds() const noexcept
{
    if (count_ < 2)
        return 0;

    return timestamps_[count_ - 1].nanoseconds - timestamps_[count_ - 2].nanoseconds;
}
} // namespace audio_insight
