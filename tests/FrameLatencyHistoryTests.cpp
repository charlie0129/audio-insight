// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/FrameLatencyHistory.h"

#include <juce_core/juce_core.h>

#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace audio_insight {
namespace {
class FrameLatencyHistoryTests final : public juce::UnitTest {
public:
    FrameLatencyHistoryTests() : UnitTest("Frame-latency history", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Ordered endpoints produce an exact latency decomposition");
        {
            const auto sample = classifyFrameLatency(7, 1'000, 1'150, 1'400, 1'650, 2'000);
            expectEquals(sample.sequence, std::uint64_t { 7 });
            expectEquals(sample.presentedHostTimestampNanoseconds, std::uint64_t { 2'000 });
            expect(sample.totalValid);
            expect(sample.componentsValid);
            expectEquals(sample.cpuEncodeNanoseconds, std::uint64_t { 150 });
            expectEquals(sample.submitQueueWaitNanoseconds, std::uint64_t { 250 });
            expectEquals(sample.gpuExecutionNanoseconds, std::uint64_t { 250 });
            expectEquals(sample.compositorWaitNanoseconds, std::uint64_t { 350 });
            expectEquals(sample.totalNanoseconds, std::uint64_t { 1'000 });
            expectEquals(sample.cpuEncodeNanoseconds + sample.submitQueueWaitNanoseconds
                    + sample.gpuExecutionNanoseconds + sample.compositorWaitNanoseconds,
                sample.totalNanoseconds);
        }

        beginTest("Equal endpoints preserve valid zero-duration components");
        {
            const auto sample = classifyFrameLatency(1, 500, 500, 500, 500, 500);
            expect(sample.totalValid);
            expect(sample.componentsValid);
            expectEquals(sample.totalNanoseconds, std::uint64_t { 0 });
            expectEquals(sample.cpuEncodeNanoseconds, std::uint64_t { 0 });
            expectEquals(sample.submitQueueWaitNanoseconds, std::uint64_t { 0 });
            expectEquals(sample.gpuExecutionNanoseconds, std::uint64_t { 0 });
            expectEquals(sample.compositorWaitNanoseconds, std::uint64_t { 0 });
        }

        beginTest("Invalid or unavailable endpoints retain only measurements that are valid");
        {
            const auto missingGpu = classifyFrameLatency(2, 1'000, 1'100, 0, 0, 2'000);
            expect(missingGpu.totalValid);
            expect(!missingGpu.componentsValid);
            expectEquals(missingGpu.totalNanoseconds, std::uint64_t { 1'000 });

            const auto gpuBeforeCpu = classifyFrameLatency(3, 1'000, 1'300, 1'200, 1'500, 2'000);
            expect(gpuBeforeCpu.totalValid);
            expect(!gpuBeforeCpu.componentsValid);

            const auto presentationBeforeCallback
                = classifyFrameLatency(4, 2'000, 2'100, 2'200, 2'300, 1'900);
            expect(!presentationBeforeCallback.totalValid);
            expect(!presentationBeforeCallback.componentsValid);
            expectEquals(presentationBeforeCallback.presentedHostTimestampNanoseconds,
                std::uint64_t { 1'900 });

            expect(!classifyFrameLatency(0, 1, 1, 1, 1, 1).totalValid);
            expect(!classifyFrameLatency(5, 0, 1, 1, 1, 1).totalValid);
        }

        beginTest("Completion and presentation correlate exactly once in either order");
        {
            FrameLatencySubmission completionFirst(11, 1'000);
            completionFirst.setCpuReadyTimestamp(1'100);
            FrameLatencySample sample;
            expect(!completionFirst.recordGpuCompletion(1'200, 1'400, sample));
            expect(completionFirst.recordPresentation(2'000, sample));
            expect(sample.componentsValid);
            expectEquals(sample.sequence, std::uint64_t { 11 });
            expect(!completionFirst.recordPresentation(2'000, sample));

            FrameLatencySubmission presentationFirst(12, 2'000);
            presentationFirst.setCpuReadyTimestamp(2'100);
            expect(!presentationFirst.recordPresentation(3'000, sample));
            expect(presentationFirst.recordGpuCompletion(2'300, 2'500, sample));
            expect(sample.componentsValid);
            expectEquals(sample.sequence, std::uint64_t { 12 });
            expect(!presentationFirst.recordGpuCompletion(2'300, 2'500, sample));
        }

        beginTest("Concurrent callbacks publish one complete sample and a stable outcome");
        {
            constexpr auto iterations = std::size_t { 10'000 };
            auto startBarrier = std::barrier { 3 };
            auto finishBarrier = std::barrier { 3 };
            FrameLatencySubmission* submission = nullptr;
            std::uint64_t baseTimestamp = 0;
            FrameLatencySample completionSample;
            FrameLatencySample presentationSample;
            bool completionEmitted = false;
            bool presentationEmitted = false;
            auto skippedTransition = PresentationOutcomeTransition::none;
            auto presentedTransition = PresentationOutcomeTransition::none;

            std::thread completionThread([&] {
                for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                    startBarrier.arrive_and_wait();
                    completionEmitted = submission->recordGpuCompletion(
                        baseTimestamp + 400, baseTimestamp + 600, completionSample);
                    skippedTransition = submission->classifySkipped();
                    finishBarrier.arrive_and_wait();
                }
            });
            std::thread presentationThread([&] {
                for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                    startBarrier.arrive_and_wait();
                    presentationEmitted
                        = submission->recordPresentation(baseTimestamp + 1'000, presentationSample);
                    presentedTransition = submission->classifyPresented();
                    finishBarrier.arrive_and_wait();
                }
            });

            auto firstEmissionFailure = iterations;
            auto firstPublicationFailure = iterations;
            auto firstOutcomeFailure = iterations;

            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                const auto sequence = static_cast<std::uint64_t>(iteration) + 100;
                baseTimestamp = (static_cast<std::uint64_t>(iteration) + 1) * 10'000;
                FrameLatencySubmission currentSubmission(sequence, baseTimestamp);
                currentSubmission.setCpuReadyTimestamp(baseTimestamp + 100);
                submission = &currentSubmission;
                completionSample = { };
                presentationSample = { };
                completionEmitted = false;
                presentationEmitted = false;
                skippedTransition = PresentationOutcomeTransition::none;
                presentedTransition = PresentationOutcomeTransition::none;

                startBarrier.arrive_and_wait();
                finishBarrier.arrive_and_wait();

                const auto emittedExactlyOnce = completionEmitted != presentationEmitted;

                if (!emittedExactlyOnce && firstEmissionFailure == iterations)
                    firstEmissionFailure = iteration;

                if (emittedExactlyOnce) {
                    const auto& sample = completionEmitted ? completionSample : presentationSample;
                    const auto fullyPublished = sample.sequence == sequence
                        && sample.presentedHostTimestampNanoseconds == baseTimestamp + 1'000
                        && sample.cpuEncodeNanoseconds == 100
                        && sample.submitQueueWaitNanoseconds == 300
                        && sample.gpuExecutionNanoseconds == 200
                        && sample.compositorWaitNanoseconds == 400
                        && sample.totalNanoseconds == 1'000 && sample.totalValid
                        && sample.componentsValid;

                    if (!fullyPublished && firstPublicationFailure == iterations)
                        firstPublicationFailure = iteration;
                }

                const auto outcomeIsStable
                    = (skippedTransition == PresentationOutcomeTransition::newlySkipped
                          && presentedTransition
                              == PresentationOutcomeTransition::upgradedSkippedToPresented)
                    || (skippedTransition == PresentationOutcomeTransition::none
                        && presentedTransition == PresentationOutcomeTransition::newlyPresented);
                const auto rejectsLaterChanges
                    = currentSubmission.classifySkipped() == PresentationOutcomeTransition::none
                    && currentSubmission.classifyPresented() == PresentationOutcomeTransition::none;

                if ((!outcomeIsStable || !rejectsLaterChanges) && firstOutcomeFailure == iterations)
                    firstOutcomeFailure = iteration;
            }

            completionThread.join();
            presentationThread.join();
            expect(firstEmissionFailure == iterations,
                "Sample emission race failed at iteration "
                    + juce::String(static_cast<juce::int64>(firstEmissionFailure)));
            expect(firstPublicationFailure == iterations,
                "A sample exposed partially published fields at iteration "
                    + juce::String(static_cast<juce::int64>(firstPublicationFailure)));
            expect(firstOutcomeFailure == iterations,
                "Presentation outcome race failed at iteration "
                    + juce::String(static_cast<juce::int64>(firstOutcomeFailure)));
        }

        beginTest("A real presentation deterministically supersedes a provisional skip");
        {
            FrameLatencySubmission failedFirst(21, 1'000);
            expect(failedFirst.classifySkipped() == PresentationOutcomeTransition::newlySkipped);
            expect(failedFirst.classifyPresented()
                == PresentationOutcomeTransition::upgradedSkippedToPresented);
            expect(failedFirst.classifySkipped() == PresentationOutcomeTransition::none);
            expect(failedFirst.classifyPresented() == PresentationOutcomeTransition::none);

            FrameLatencySubmission presentedFirst(22, 1'000);
            expect(presentedFirst.classifyPresented()
                == PresentationOutcomeTransition::newlyPresented);
            expect(presentedFirst.classifySkipped() == PresentationOutcomeTransition::none);
        }

        beginTest("History follows actual presentation order, not callback or sequence order");
        {
            FrameLatencyHistory history;
            expect(history.record(classifyFrameLatency(3, 1'000, 1'100, 1'200, 1'300, 3'000)));
            expect(history.record(classifyFrameLatency(1, 500, 600, 700, 800, 1'000)));
            expect(history.record(classifyFrameLatency(2, 700, 800, 900, 1'000, 2'000)));

            std::array<FrameLatencySample, frameLatencyHistoryCapacity> samples { };
            const auto count = history.snapshot(samples);
            expectEquals(count, std::size_t { 3 });
            expectEquals(samples[0].sequence, std::uint64_t { 1 });
            expectEquals(samples[1].sequence, std::uint64_t { 2 });
            expectEquals(samples[2].sequence, std::uint64_t { 3 });
        }

        beginTest("The bounded history retains the latest 240 actual presentations");
        {
            FrameLatencyHistory history;
            constexpr auto extraSamples = std::size_t { 17 };
            constexpr auto sampleCount = frameLatencyHistoryCapacity + extraSamples;

            for (std::size_t index = 1; index <= sampleCount; ++index) {
                const auto timestamp = static_cast<std::uint64_t>(index) * 1'000;
                expect(history.record(
                    classifyFrameLatency(static_cast<std::uint64_t>(index), timestamp - 500,
                        timestamp - 400, timestamp - 300, timestamp - 200, timestamp)));
            }

            auto stale = classifyFrameLatency(999'999, 1, 2, 3, 4, 5);
            expect(!history.record(stale));

            std::array<FrameLatencySample, frameLatencyHistoryCapacity> samples { };
            const auto count = history.snapshot(samples);
            expectEquals(count, frameLatencyHistoryCapacity);
            expectEquals(samples.front().sequence, std::uint64_t { extraSamples + 1 });
            expectEquals(samples.back().sequence, static_cast<std::uint64_t>(sampleCount));
        }

        beginTest("Invalid identities and duplicate samples are rejected");
        {
            FrameLatencyHistory history;
            auto invalidSequence = classifyFrameLatency(0, 1, 1, 1, 1, 1);
            expect(!history.record(invalidSequence));

            auto invalidPresentation = classifyFrameLatency(1, 1, 1, 1, 1, 0);
            expect(!history.record(invalidPresentation));

            const auto valid = classifyFrameLatency(1, 1, 1, 1, 1, 2);
            expect(history.record(valid));
            expect(!history.record(valid));
        }
    }
};

static FrameLatencyHistoryTests frameLatencyHistoryTests;
} // namespace
} // namespace audio_insight
