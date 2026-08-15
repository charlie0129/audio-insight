// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/AudioCallbackMetrics.h"

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace audio_insight {
namespace {
class AudioCallbackMetricsTests final : public juce::UnitTest {
public:
    AudioCallbackMetricsTests() : UnitTest("Audio-callback metrics", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Mach tick conversion is deterministic and rejects invalid intervals");
        {
            std::uint64_t duration = 99;
            const MachContinuousTimebase identity { 1, 1, true };
            expect(identity.durationNanoseconds(100, 3'100, duration));
            expectEquals(duration, std::uint64_t { 3'000 });

            const MachContinuousTimebase fractional { 125, 3, true };
            expect(fractional.durationNanoseconds(10, 13, duration));
            expectEquals(duration, std::uint64_t { 125 });

            expect(!identity.durationNanoseconds(101, 100, duration));
            expectEquals(duration, std::uint64_t { 0 });
            expect(!MachContinuousTimebase { 0, 0, false }.durationNanoseconds(1, 2, duration));
            expect(!MachContinuousTimebase { std::numeric_limits<std::uint32_t>::max(), 1, true }
                    .durationNanoseconds(0, std::numeric_limits<std::uint64_t>::max(), duration));
        }

        beginTest("Tracked block budgets and lifetime counters are exact");
        {
            AudioCallbackMetrics metrics({ 1, 1, true });
            metrics.configureSampleRate(48'000.0);

            const auto equalToBudget = metrics.beginCallback(128, 1'000);
            metrics.finishCallback(equalToBudget, 26'000);
            const auto overBudget = metrics.beginCallback(128, 50'000);
            metrics.finishCallback(overBudget, 75'001);
            const auto otherTracked = metrics.beginCallback(64, 100'000);
            metrics.finishCallback(otherTracked, 126'668);
            const auto untracked = metrics.beginCallback(192, 200'000);
            metrics.finishCallback(untracked, 201'000);

            const auto telemetry = metrics.telemetry();
            expectEquals(telemetry.callbackCount, std::uint64_t { 4 });
            expectEquals(telemetry.processedFrames, std::uint64_t { 512 });
            expectEquals(telemetry.timingSamples, std::uint64_t { 4 });
            expectEquals(telemetry.timingUnavailable, std::uint64_t { 0 });
            expectEquals(telemetry.budgetExceeded, std::uint64_t { 2 });
            expectEquals(telemetry.untrackedBlockSizeCallbacks, std::uint64_t { 1 });

            const auto& block64 = telemetry.trackedBlocks[0];
            expectEquals(block64.callbackCount, std::uint64_t { 1 });
            expectEquals(block64.timingSamples, std::uint64_t { 1 });
            expectEquals(block64.budgetNanoseconds, std::uint64_t { 26'667 });
            expectEquals(block64.budgetExceeded, std::uint64_t { 1 });

            const auto& block128 = telemetry.trackedBlocks[1];
            expectEquals(block128.callbackCount, std::uint64_t { 2 });
            expectEquals(block128.timingSamples, std::uint64_t { 2 });
            expectEquals(block128.budgetNanoseconds, std::uint64_t { 25'000 });
            expectEquals(block128.budgetExceeded, std::uint64_t { 1 });

            expectEquals(telemetry.trackedBlocks[2].budgetNanoseconds, std::uint64_t { 106'667 });
            expectEquals(telemetry.trackedBlocks[3].budgetNanoseconds, std::uint64_t { 213'333 });
            expectEquals(telemetry.trackedBlocks[4].budgetNanoseconds, std::uint64_t { 426'667 });
        }

        beginTest("Duration histograms use one-microsecond bins plus explicit overflow");
        {
            AudioCallbackMetrics metrics({ 1, 1, true });
            constexpr std::uint64_t durations[] {
                0,
                999,
                1'000,
                1'023'999,
                1'024'000,
                9'000'000,
            };
            for (const auto duration : durations) {
                const auto token = metrics.beginCallback(256, 10);
                metrics.finishCallback(token, 10 + duration);
            }

            const auto telemetry = metrics.telemetry();
            const auto& histogram = telemetry.trackedBlocks[2].durationHistogram;
            expectEquals(histogram[0], std::uint64_t { 2 });
            expectEquals(histogram[1], std::uint64_t { 1 });
            expectEquals(histogram[1'023], std::uint64_t { 1 });
            expectEquals(
                histogram[audioCallbackDurationHistogramOverflowBucket], std::uint64_t { 2 });
        }

        beginTest("Bounded detector coverage is explicit and honest");
        {
            AudioCallbackMetrics metrics({ 1, 1, true });
            const auto first = metrics.beginCallback(64, 100);
            const auto concurrent = metrics.beginCallback(64, 200);
            metrics.finishCallback(concurrent, 300);
            metrics.finishCallback(first, 50);

            const auto telemetry = metrics.telemetry();
            expect(telemetry.detectorActive);
            expect(telemetry.clockAvailable);
            expect(!telemetry.allocationDetectorActive);
            expect(!telemetry.lockWaitDetectorActive);
            expect(telemetry.detectorCoverageFlags
                == static_cast<std::uint32_t>(
                    concurrentCallbackDetection | monotonicClockAnomalyDetection));
            expectEquals(telemetry.concurrentCallbackViolations, std::uint64_t { 1 });
            expectEquals(telemetry.clockAnomalyViolations, std::uint64_t { 1 });
            expectEquals(telemetry.rtSafetyViolationCount, std::uint64_t { 2 });
            expectEquals(telemetry.timingSamples, std::uint64_t { 1 });
            expectEquals(telemetry.timingUnavailable, std::uint64_t { 1 });
        }

        beginTest("Unavailable timing does not claim clock-anomaly coverage");
        {
            AudioCallbackMetrics metrics({ 0, 0, false });
            const auto token = metrics.beginCallback(64, 200);
            metrics.finishCallback(token, 100);

            const auto telemetry = metrics.telemetry();
            expect(telemetry.detectorActive);
            expect(!telemetry.clockAvailable);
            expect(telemetry.detectorCoverageFlags
                == static_cast<std::uint32_t>(concurrentCallbackDetection));
            expectEquals(telemetry.timingUnavailable, std::uint64_t { 1 });
            expectEquals(telemetry.clockAnomalyViolations, std::uint64_t { 0 });
        }
    }
};

AudioCallbackMetricsTests audioCallbackMetricsTests;
} // namespace
} // namespace audio_insight
