// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/PresentedFrameHistory.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
namespace {
class PresentedFrameHistoryTests final : public juce::UnitTest {
public:
    PresentedFrameHistoryTests() : UnitTest("Presented-frame history", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Out-of-order callbacks produce timestamp-ordered intervals");
        {
            PresentedFrameHistory history;
            expect(history.recordPresentation(1, 1'000));
            expect(history.recordPresentation(3, 3'600));

            std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>
                intervals { };
            auto count = history.snapshotIntervals(intervals);
            expectEquals(count, std::size_t { 1 });
            expectEquals(intervals[0].sequence, std::uint64_t { 3 });
            expectEquals(intervals[0].nanoseconds, std::uint64_t { 2'600 });

            expect(history.recordPresentation(2, 2'100));
            count = history.snapshotIntervals(intervals);
            expectEquals(count, std::size_t { 2 });
            expectEquals(intervals[0].sequence, std::uint64_t { 2 });
            expectEquals(intervals[0].nanoseconds, std::uint64_t { 1'100 });
            expectEquals(intervals[1].sequence, std::uint64_t { 3 });
            expectEquals(intervals[1].nanoseconds, std::uint64_t { 1'500 });
            expectEquals(history.latestTimestampNanoseconds(), std::uint64_t { 3'600 });
            expectEquals(history.latestIntervalNanoseconds(), std::uint64_t { 1'500 });
        }

        beginTest("The bounded window retains the newest 241 presentations");
        {
            PresentedFrameHistory history;
            constexpr auto extraPresentations = std::size_t { 19 };
            constexpr auto presentationCount
                = presentedFrameIntervalHistoryCapacity + 1 + extraPresentations;

            for (std::size_t index = 1; index <= presentationCount; ++index) {
                expect(history.recordPresentation(
                    static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(index) * 1'000));
            }

            // An arbitrarily delayed callback older than the retained window
            // cannot displace any of the newest samples.
            expect(!history.recordPresentation(999'999, 500));

            std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>
                intervals { };
            const auto count = history.snapshotIntervals(intervals);
            expectEquals(count, presentedFrameIntervalHistoryCapacity);
            expectEquals(
                intervals.front().sequence, static_cast<std::uint64_t>(extraPresentations + 2));
            expectEquals(intervals.front().nanoseconds, std::uint64_t { 1'000 });
            expectEquals(intervals.back().sequence, static_cast<std::uint64_t>(presentationCount));
            expectEquals(intervals.back().nanoseconds, std::uint64_t { 1'000 });
            expectEquals(history.latestTimestampNanoseconds(),
                static_cast<std::uint64_t>(presentationCount) * 1'000);
        }

        beginTest("A delayed timestamp still inside the full window is inserted exactly");
        {
            PresentedFrameHistory history;
            expect(history.recordPresentation(1, 1'000));

            for (std::size_t index = 3; index <= presentedFrameIntervalHistoryCapacity + 2;
                ++index) {
                expect(history.recordPresentation(
                    static_cast<std::uint64_t>(index), static_cast<std::uint64_t>(index) * 1'000));
            }

            // The window is full with t1 and t3...t242. Inserting delayed t2
            // must evict t1, shift the window, and produce exact t2...t242
            // intervals rather than dropping or reordering a sample.
            expect(history.recordPresentation(2, 2'000));

            std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>
                intervals { };
            const auto count = history.snapshotIntervals(intervals);
            expectEquals(count, presentedFrameIntervalHistoryCapacity);
            expectEquals(intervals.front().sequence, std::uint64_t { 3 });
            expectEquals(intervals.front().nanoseconds, std::uint64_t { 1'000 });
            expectEquals(intervals.back().sequence,
                static_cast<std::uint64_t>(presentedFrameIntervalHistoryCapacity + 2));
            expectEquals(intervals.back().nanoseconds, std::uint64_t { 1'000 });
        }

        beginTest("Duplicate and invalid presentation records are ignored");
        {
            PresentedFrameHistory history;
            expect(!history.recordPresentation(0, 1'000));
            expect(!history.recordPresentation(1, 0));
            expect(history.recordPresentation(1, 1'000));
            expect(!history.recordPresentation(2, 1'000));
            expect(history.recordPresentation(3, 2'000));

            std::array<PresentedFrameIntervalSample, presentedFrameIntervalHistoryCapacity>
                intervals { };
            const auto count = history.snapshotIntervals(intervals);
            expectEquals(count, std::size_t { 1 });
            expectEquals(intervals[0].sequence, std::uint64_t { 3 });
            expectEquals(intervals[0].nanoseconds, std::uint64_t { 1'000 });
        }
    }
};

static PresentedFrameHistoryTests presentedFrameHistoryTests;
} // namespace
} // namespace audio_insight
