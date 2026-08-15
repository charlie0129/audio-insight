// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
inline constexpr std::size_t presentedFrameIntervalHistoryCapacity = 240;

struct PresentedFrameIntervalSample {
    std::uint64_t sequence = 0;
    std::uint64_t nanoseconds = 0;
};

/**
    Fixed-capacity history of actual drawable presentation timestamps.

    recordPresentation() accepts callbacks in any order and retains the newest
    241 distinct timestamps. snapshotIntervals() derives at most 240 exact,
    chronological intervals between adjacent retained presentations.

    This type does not synchronize its own access. The Metal backend protects it
    with a small non-audio-thread lock because drawable presentation handlers may
    run concurrently.
*/
class PresentedFrameHistory final {
public:
    [[nodiscard]] bool recordPresentation(
        std::uint64_t sequence, std::uint64_t presentedHostTimestampNanoseconds) noexcept;

    [[nodiscard]] std::size_t snapshotIntervals(
        std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>&
            destination) const noexcept;

    [[nodiscard]] std::uint64_t latestTimestampNanoseconds() const noexcept;
    [[nodiscard]] std::uint64_t latestIntervalNanoseconds() const noexcept;

private:
    struct TimestampSample {
        std::uint64_t sequence = 0;
        std::uint64_t nanoseconds = 0;
    };

    static constexpr std::size_t timestampCapacity = presentedFrameIntervalHistoryCapacity + 1;

    std::array<TimestampSample, timestampCapacity> timestamps_ { };
    std::size_t count_ = 0;
};
} // namespace audio_insight
