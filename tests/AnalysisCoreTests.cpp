// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/SpectrumAnalyzer.h"
#include "analysis/StereoMeterAccumulator.h"
#include "analysis/StereoSampleCapture.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

namespace audio_insight {
namespace {
class StereoSampleCaptureTests final : public juce::UnitTest {
public:
    StereoSampleCaptureTests() : UnitTest("Stereo sample capture", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Continuous chunks preserve plugin sequence and frame time");
        {
            StereoSampleCapture capture;
            std::array<float, 64> left { };
            std::array<float, 64> right { };
            left.fill(0.25F);
            right.fill(-0.5F);

            for (std::size_t chunk = 0; chunk < 3; ++chunk) {
                for (std::size_t quarter = 0; quarter < 4; ++quarter) {
                    const auto result
                        = capture.publishBlock(left.data(), right.data(), left.size(), 48000.0, 7);
                    expect(result.attemptedChunks == (quarter == 0 ? 1 : 0));
                    expect(result.publishedChunks == (quarter == 3 ? 1 : 0));
                    expect(result.droppedIncomingChunks == 0);
                    expect(result.captureDiscontinuityRevision == 0);
                    expect(!result.beganCaptureDiscontinuity);
                }
            }

            for (std::uint64_t expectedSequence = 1; expectedSequence <= 3; ++expectedSequence) {
                StereoSampleCapture::ReadHandle handle;
                expect(capture.tryAcquireOldest(handle));
                expect(handle.view().sequence == expectedSequence);
                expect(handle.view().frameCount == StereoSampleCapture::framesPerSlot);
                expect(handle.view().capturedFrameEnd
                    == expectedSequence * StereoSampleCapture::framesPerSlot);
                expect(!handle.view().followsDiscontinuity);
                expect(handle.view().captureDiscontinuityRevision == 0);
                expect(handle.view().captureLifecycleGeneration == 7);
                expectWithinAbsoluteError(handle.view().left[200], 0.25F, 1.0e-7F);
                expectWithinAbsoluteError(handle.view().right[200], -0.5F, 1.0e-7F);
            }

            const auto telemetry = capture.telemetry();
            expect(telemetry.attemptedChunks == 3);
            expect(telemetry.publishedChunks == 3);
            expect(telemetry.lostChunks() == 0);
            expect(telemetry.readyHighWaterMark == 3);
            expect(telemetry.readyFrameHighWaterMark == 3 * StereoSampleCapture::framesPerSlot);
            expect(telemetry.partialFrames == 0);
        }

        beginTest("Small host blocks pack into frame-bounded storage without a false overflow");
        {
            StereoSampleCapture capture;
            std::array<float, 64> block { };
            block.fill(0.25F);

            // 4,800 frames models 100 ms without analysis service at 48 kHz.
            for (std::size_t callback = 0; callback < 75; ++callback) {
                const auto publication
                    = capture.publishBlock(block.data(), block.data(), block.size(), 48'000.0, 1);
                expect(!publication.beganCaptureDiscontinuity);
            }

            const auto telemetry = capture.telemetry();
            expect(telemetry.capturedFrames == 4'800);
            expect(telemetry.publishedChunks == 18);
            expect(telemetry.readyFrames == 4'608);
            expect(telemetry.partialFrames == 192);
            expect(telemetry.bufferedFrameCapacity == 32'768);
            expect(telemetry.reclaimedReadyChunks == 0);
            expect(telemetry.droppedIncomingChunks == 0);
            expect(telemetry.overflowEpisodes == 0);
        }

        beginTest("Packing preserves sample order across uneven host callbacks");
        {
            StereoSampleCapture capture;
            std::array<float, 31> firstLeft { };
            std::array<float, 31> firstRight { };
            std::array<float, 100> secondLeft { };
            std::array<float, 100> secondRight { };
            std::array<float, 125> thirdLeft { };
            std::array<float, 125> thirdRight { };
            const auto fillRange = [](auto& left, auto& right, const std::size_t start) {
                for (std::size_t index = 0; index < left.size(); ++index) {
                    left[index] = static_cast<float>(start + index);
                    right[index] = -left[index];
                }
            };
            fillRange(firstLeft, firstRight, 0);
            fillRange(secondLeft, secondRight, firstLeft.size());
            fillRange(thirdLeft, thirdRight, firstLeft.size() + secondLeft.size());

            expect(capture
                       .publishBlock(
                           firstLeft.data(), firstRight.data(), firstLeft.size(), 48'000.0, 1)
                       .publishedChunks
                == 0);
            expect(capture
                       .publishBlock(
                           secondLeft.data(), secondRight.data(), secondLeft.size(), 48'000.0, 1)
                       .publishedChunks
                == 0);
            expect(capture
                       .publishBlock(
                           thirdLeft.data(), thirdRight.data(), thirdLeft.size(), 48'000.0, 1)
                       .publishedChunks
                == 1);

            StereoSampleCapture::ReadHandle handle;
            expect(capture.tryAcquireOldest(handle));
            expect(handle.view().frameCount == StereoSampleCapture::framesPerSlot);
            for (std::size_t index = 0; index < handle.view().frameCount; ++index) {
                expectWithinAbsoluteError(
                    handle.view().left[index], static_cast<float>(index), 0.0F);
                expectWithinAbsoluteError(
                    handle.view().right[index], -static_cast<float>(index), 0.0F);
            }
        }

        beginTest("Overflow never overwrites reading storage and is observable");
        {
            StereoSampleCapture capture;
            std::array<float, StereoSampleCapture::framesPerSlot> sample { };

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index) {
                sample.fill(static_cast<float>(index + 1));
                expect(capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1)
                           .publishedChunks
                    == 1);
            }

            std::array<StereoSampleCapture::ReadHandle, StereoSampleCapture::slotCount> held;
            expect(capture.tryAcquireOldest(held[0]));
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);

            auto firstGapRevision = std::uint64_t { 0 };
            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index) {
                sample.fill(100.0F + static_cast<float>(index));
                const auto publication
                    = capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1);
                expect(publication.publishedChunks == 1);
                expect(publication.beganCaptureDiscontinuity == (index == 0));
                if (index == 0)
                    firstGapRevision = publication.captureDiscontinuityRevision;
                expect(publication.captureDiscontinuityRevision == firstGapRevision);
            }

            // The producer reclaimed only ready slots; the held payload is intact.
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);
            auto telemetry = capture.telemetry();
            expect(telemetry.reclaimedReadyChunks == StereoSampleCapture::slotCount);
            expect(telemetry.droppedIncomingChunks == 0);

            for (std::size_t index = 1; index < held.size(); ++index)
                expect(capture.tryAcquireOldest(held[index]));

            expect(held[1].view().followsDiscontinuity);
            expect(held[1].view().captureDiscontinuityRevision == firstGapRevision);
            telemetry = capture.telemetry();
            expect(telemetry.consumerDiscontinuities == 1);

            // With every fixed slot being read there is no safe reclaim target.
            sample.fill(999.0F);
            const auto dropped
                = capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1);
            expect(dropped.droppedIncomingChunks == 1);
            expect(dropped.captureDiscontinuityRevision == firstGapRevision);
            expect(!dropped.beganCaptureDiscontinuity);
            expect(capture.telemetry().droppedIncomingChunks == 1);
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);

            held[0].release();
            const auto afterDrop
                = capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1);
            expect(afterDrop.publishedChunks == 1);
            expect(afterDrop.captureDiscontinuityRevision == dropped.captureDiscontinuityRevision);
            expect(!afterDrop.beganCaptureDiscontinuity);

            StereoSampleCapture::ReadHandle afterDropHandle;
            expect(capture.tryAcquireOldest(afterDropHandle));
            expect(afterDropHandle.view().captureDiscontinuityRevision
                == dropped.captureDiscontinuityRevision);
        }

        beginTest("Overflow does not reclassify an older surviving slot as post-gap input");
        {
            StereoSampleCapture capture;
            std::array<float, StereoSampleCapture::framesPerSlot> sample { };

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index) {
                sample.fill(static_cast<float>(index + 1));
                expect(capture
                           .publishBlock(
                               sample.data(), sample.data(), sample.size(), 48'000.0, 1, 2, 9)
                           .publishedChunks
                    == 1);
            }

            sample.fill(100.0F);
            const auto overflow = capture.publishBlock(
                sample.data(), sample.data(), sample.size(), 48'000.0, 1, 2, 9);
            expect(overflow.reclaimedReadyChunks == 1);
            expect(overflow.beganCaptureDiscontinuity);
            expect(overflow.captureDiscontinuityRevision != 0);
            expect(
                capture.captureDiscontinuityRevision(9) == overflow.captureDiscontinuityRevision);

            StereoSampleCapture::ReadHandle survivingPreGap;
            expect(capture.tryAcquireOldest(survivingPreGap));
            expect(survivingPreGap.view().sequence == 2);
            expect(survivingPreGap.view().captureDiscontinuityRevision == 0,
                "Acquisition inherited the later producer-side gap revision");
            expectWithinAbsoluteError(survivingPreGap.view().left[0], 2.0F, 1.0e-7F);
        }

        beginTest("A packed overflow reports its exact host-block frame boundary");
        {
            StereoSampleCapture capture;
            std::array<float, StereoSampleCapture::framesPerSlot> full { };
            full.fill(0.1F);
            for (std::size_t index = 0; index + 1 < StereoSampleCapture::slotCount; ++index) {
                static_cast<void>(
                    capture.publishBlock(full.data(), full.data(), full.size(), 48'000.0, 1));
            }

            std::array<float, StereoSampleCapture::framesPerSlot / 2> half { };
            half.fill(0.2F);
            const auto partial
                = capture.publishBlock(half.data(), half.data(), half.size(), 48'000.0, 1);
            expect(partial.publishedChunks == 0);
            expect(capture.telemetry().partialFrames == half.size());

            const auto crossing
                = capture.publishBlock(full.data(), full.data(), full.size(), 48'000.0, 1);
            expect(crossing.beganCaptureDiscontinuity);
            expect(crossing.firstDiscontinuityFrameOffset == half.size());
            expect(crossing.precedingCaptureDiscontinuityRevision == 0);
            expect(crossing.captureDiscontinuityRevision != 0);
            expect(crossing.reclaimedReadyChunks == 1);
            expect(capture.telemetry().overflowEpisodes == 1);
        }

        beginTest("Mono layout metadata is preserved without duplicating its samples");
        {
            StereoSampleCapture capture;
            std::array<float, StereoSampleCapture::framesPerSlot> mono { };
            mono.fill(0.25F);
            mono[2] = 0.75F;

            expect(capture.publishBlock(mono.data(), nullptr, mono.size(), 48'000.0, 3, 1)
                       .publishedChunks
                == 1);

            StereoSampleCapture::ReadHandle monoHandle;
            expect(capture.tryAcquireOldest(monoHandle));
            expect(monoHandle.view().channelCount == 1);
            expectWithinAbsoluteError(monoHandle.view().left[2], mono[2], 1.0e-7F);
            expectWithinAbsoluteError(monoHandle.view().right[2], 0.0F, 1.0e-7F);

            expect(capture.publishBlock(mono.data(), mono.data(), mono.size(), 48'000.0, 3, 2)
                       .publishedChunks
                == 1);
            StereoSampleCapture::ReadHandle stereoHandle;
            expect(capture.tryAcquireOldest(stereoHandle));
            expect(stereoHandle.view().channelCount == 2);
            expect(stereoHandle.view().followsDiscontinuity);
        }

        beginTest("Raw discontinuity revisions stay within one capture lifecycle");
        {
            StereoSampleCapture capture;
            std::array<float, StereoSampleCapture::framesPerSlot> sample { };
            sample.fill(0.25F);
            constexpr auto oldLifecycle = std::uint64_t { 41 };
            constexpr auto newLifecycle = std::uint64_t { 73 };

            StereoSampleCapture::PublishResult oldPublication;
            for (std::size_t index = 0; index <= StereoSampleCapture::slotCount; ++index) {
                oldPublication = capture.publishBlock(
                    sample.data(), sample.data(), sample.size(), 48'000.0, 1, 2, oldLifecycle);
            }
            expect(oldPublication.captureDiscontinuityRevision != 0);
            expect(capture.captureDiscontinuityRevision(oldLifecycle)
                == oldPublication.captureDiscontinuityRevision);
            expect(capture.captureDiscontinuityRevision(newLifecycle) == 0);

            std::array<float, StereoSampleCapture::framesPerSlot / 2> partial { };
            const auto oldPartial = capture.publishBlock(
                partial.data(), partial.data(), partial.size(), 48'000.0, 1, 2, oldLifecycle);
            expect(oldPartial.publishedChunks == 0);
            expect(capture.telemetry().partialFrames == partial.size());

            const auto newPublication = capture.publishBlock(
                sample.data(), sample.data(), sample.size(), 48'000.0, 2, 2, newLifecycle);
            expect(newPublication.captureDiscontinuityRevision == 0);
            expect(!newPublication.beganCaptureDiscontinuity);

            StereoSampleCapture::ReadHandle handle;
            expect(capture.tryAcquireOldest(handle));
            expect(handle.view().captureLifecycleGeneration == newLifecycle);
            expect(handle.view().captureDiscontinuityRevision == 0);
        }

        beginTest("Unsupported channel metadata is preserved for downstream rejection");
        {
            StereoSampleCapture capture;
            std::array<float, StereoSampleCapture::framesPerSlot> sample { };
            sample.fill(0.25F);
            expect(capture.publishBlock(sample.data(), sample.data(), sample.size(), 48'000.0, 4, 3)
                       .publishedChunks
                == 1);

            StereoSampleCapture::ReadHandle handle;
            expect(capture.tryAcquireOldest(handle));
            expect(handle.view().channelCount == 3);
        }
    }
};

class StereoMeterAccumulatorTests final : public juce::UnitTest {
public:
    StereoMeterAccumulatorTests() : UnitTest("Stereo meter accumulator", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Published readings are complete producer-time endpoints");
        {
            StereoMeterAccumulator meters;
            std::array<float, 100> left { };
            std::array<float, 100> right { };
            left.fill(0.5F);
            right.fill(0.25F);

            const auto publication
                = meters.publishBlock(left.data(), right.data(), left.size(), 1'000.0, 4);
            expect(publication.published);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            expectWithinAbsoluteError(reading.peakLinear[0], 0.5F, 1.0e-6F);
            expectWithinAbsoluteError(reading.peakLinear[1], 0.25F, 1.0e-6F);
            const auto rmsIntegration = std::sqrt(1.0 - std::exp(-100.0 / 300.0));
            expectWithinAbsoluteError(
                reading.rmsLinear[0], static_cast<float>(0.5 * rmsIntegration), 1.0e-6F);
            expectWithinAbsoluteError(
                reading.rmsLinear[1], static_cast<float>(0.25 * rmsIntegration), 1.0e-6F);
            expectWithinAbsoluteError(reading.peakDecibels[0], -6.0206F, 1.0e-3F);
            expectWithinAbsoluteError(reading.heldPeakDecibels[0], -6.0206F, 1.0e-3F);
            expect(reading.correlationValid);
            expectWithinAbsoluteError(reading.correlation, 1.0F, 1.0e-6F);
            expect(reading.rmsMeanSquare[0] > reading.rmsMeanSquare[1]);
            expect(reading.crossMeanProduct > 0.0);
            expect(reading.representedFrames == left.size());
            expect(reading.representedBlocks == 1);
            expect(reading.valid);
            expect(!reading.followsDiscontinuity);
        }

        beginTest("Forced endpoint coalescing exactly matches sequential consumption");
        {
            StereoMeterAccumulator sequential;
            StereoMeterAccumulator coalesced;
            std::array<float, 100> block { };
            StereoMeterReading sequentialEndpoint;

            for (std::size_t index = 0; index < 10; ++index) {
                const auto level = index == 0 ? 1.0F : (index == 8 ? 0.2F : 0.0F);
                block.fill(level);
                expect(sequential.publishBlock(block.data(), block.data(), block.size(), 1'000.0, 1)
                        .published);
                expect(sequential.consumeLatest(sequentialEndpoint));

                expect(coalesced.publishBlock(block.data(), block.data(), block.size(), 1'000.0, 1)
                        .published);
            }

            StereoMeterReading coalescedEndpoint;
            expect(coalesced.consumeLatest(coalescedEndpoint));
            expectWithinAbsoluteError(
                coalescedEndpoint.peakLinear[0], sequentialEndpoint.peakLinear[0], 1.0e-7F);
            expectWithinAbsoluteError(
                coalescedEndpoint.rmsLinear[0], sequentialEndpoint.rmsLinear[0], 1.0e-7F);
            expectWithinAbsoluteError(
                coalescedEndpoint.heldPeakLinear[0], sequentialEndpoint.heldPeakLinear[0], 1.0e-7F);
            expect(coalescedEndpoint.over == sequentialEndpoint.over);
            expectWithinAbsoluteError(
                coalescedEndpoint.rmsMeanSquare[0], sequentialEndpoint.rmsMeanSquare[0], 1.0e-14);
            expectWithinAbsoluteError(
                coalescedEndpoint.crossMeanProduct, sequentialEndpoint.crossMeanProduct, 1.0e-14);
            expect(coalescedEndpoint.correlationValid == sequentialEndpoint.correlationValid);
            expectWithinAbsoluteError(
                coalescedEndpoint.correlation, sequentialEndpoint.correlation, 1.0e-7F);
            expect(coalescedEndpoint.representedBlocks == 10);
            expect(coalescedEndpoint.representedFrames == 1'000);
            expect(!coalescedEndpoint.followsDiscontinuity);

            const auto telemetry = coalesced.telemetry();
            expect(telemetry.coalescedBlocks == 2);
            expect(telemetry.droppedBlocks == 0);
            expect(telemetry.readyHighWaterMark == StereoMeterAccumulator::slotCount);
        }

        beginTest("User-reset epochs count every request and apply at a block boundary");
        {
            StereoMeterAccumulator meters;
            std::array<float, 100> over { };
            over.fill(1.1F);
            expect(
                meters.publishBlock(over.data(), over.data(), over.size(), 1'000.0, 1).published);

            StereoMeterReading latched;
            expect(meters.consumeLatest(latched));
            expect(latched.over[0] && latched.over[1]);

            expect(meters.requestUserReset() == 1);
            expect(meters.requestUserReset() == 2);
            expect(meters.requestedUserResetEpoch() == 2);

            std::array<float, 100> belowNominal { };
            belowNominal.fill(0.5F);
            expect(meters
                    .publishBlock(
                        belowNominal.data(), belowNominal.data(), belowNominal.size(), 1'000.0, 1)
                    .published);

            StereoMeterReading reset;
            expect(meters.consumeLatest(reset));
            expect(reset.appliedUserResetEpoch == 2);
            expect(!reset.over[0] && !reset.over[1]);
            expectWithinAbsoluteError(reset.heldPeakLinear[0], 0.5F, 1.0e-6F);
        }

        beginTest("Stale live-clear cannot resurrect a pre-request endpoint");
        {
            StereoMeterAccumulator meters;
            std::array<float, 100> over { };
            over.fill(1.1F);
            expect(
                meters.publishBlock(over.data(), over.data(), over.size(), 1'000.0, 1).published);

            expect(meters.requestLiveClear() == 1);
            expect(meters.requestLiveClear() == 2);
            expect(meters.requestedLiveClearEpoch() == 2);

            std::array<float, 100> silence { };
            expect(meters.publishBlock(silence.data(), silence.data(), silence.size(), 1'000.0, 1)
                    .published);

            StereoMeterReading cleared;
            expect(meters.consumeLatest(cleared));
            expect(cleared.appliedLiveClearEpoch == 2);
            expectEquals(cleared.peakDecibels[0], minimumDisplayDecibels);
            expectEquals(cleared.rmsDecibels[0], minimumDisplayDecibels);
            expect(cleared.heldPeakLinear[0] > 1.0F);
            expect(cleared.over[0] && cleared.over[1]);

            expect(meters.publishBlock(silence.data(), silence.data(), silence.size(), 1'000.0, 1)
                    .published);
            StereoMeterReading later;
            expect(meters.consumeLatest(later));
            expect(later.appliedLiveClearEpoch == 2);
            expectEquals(later.peakDecibels[0], minimumDisplayDecibels);
            expectEquals(later.rmsDecibels[0], minimumDisplayDecibels);
            expect(later.heldPeakLinear[0] > 1.0F);
            expect(later.over[0]);
        }

        beginTest("Capture discontinuity resets endpoint state before current samples");
        {
            StereoMeterAccumulator meters;
            constexpr std::array<float, 1> over { 1.1F };
            constexpr std::array<float, 1> low { 0.1F };
            expect(
                meters.publishBlock(over.data(), over.data(), over.size(), 1'000.0, 1).published);
            StereoMeterReading beforeGap;
            expect(meters.consumeLatest(beforeGap));
            expect(beforeGap.over[0]);

            expect(
                meters.publishBlock(low.data(), low.data(), low.size(), 1'000.0, 1, 2, true, 7, 99)
                    .published);
            StereoMeterReading afterGap;
            expect(meters.consumeLatest(afterGap));
            expect(afterGap.followsDiscontinuity);
            expect(afterGap.captureDiscontinuityRevision == 7);
            expect(afterGap.captureLifecycleGeneration == 99);
            expect(!afterGap.over[0] && !afterGap.over[1]);
            expectWithinAbsoluteError(afterGap.peakDecibels[0], -20.0F, 0.001F);
            expectWithinAbsoluteError(afterGap.heldPeakDecibels[0], -20.0F, 0.001F);

            expect(
                meters.publishBlock(low.data(), low.data(), low.size(), 1'000.0, 2, 2, false, 7, 99)
                    .published);
            StereoMeterReading sameSegment;
            expect(meters.consumeLatest(sameSegment));
            expect(!sameSegment.followsDiscontinuity);
            expect(sameSegment.generation == 2);
            expect(sameSegment.captureDiscontinuityRevision == 7);
            expect(sameSegment.captureLifecycleGeneration == 99);
        }

        beginTest("Mono meter readings preserve the real channel count");
        {
            StereoMeterAccumulator meters;
            std::array<float, 100> mono { };
            std::array<float, 100> ignoredRight { };
            mono.fill(0.5F);
            ignoredRight.fill(2.0F);
            expect(meters.publishBlock(mono.data(), ignoredRight.data(), mono.size(), 1'000.0, 8, 1)
                    .published);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            expect(reading.valid);
            expect(reading.channelCount == 1);
            expectWithinAbsoluteError(reading.peakLinear[0], 0.5F, 1.0e-6F);
            expectWithinAbsoluteError(reading.peakLinear[1], 0.0F, 1.0e-6F);
            expectWithinAbsoluteError(reading.rmsLinear[1], 0.0F, 1.0e-6F);
            expect(!reading.over[1]);
            expect(!reading.correlationValid);
        }

        beginTest("A newer invalid endpoint supersedes queued valid state");
        {
            StereoMeterAccumulator meters;
            constexpr std::array<float, 1> sample { 0.5F };
            expect(meters.publishBlock(sample.data(), sample.data(), sample.size(), 1'000.0, 1)
                    .published);
            expect(meters.publishBlock(sample.data(), sample.data(), sample.size(), 1'000.0, 1, 3)
                    .published);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            expect(!reading.valid);
            expect(reading.channelCount == 0);
            expect(reading.lastSequence == 2);
            expect(reading.followsDiscontinuity);
        }

        beginTest("Meter-only sequence loss stays distinct from a raw-capture discontinuity");
        {
            StereoMeterAccumulator meters;
            constexpr std::array<float, 4> samples { 0.25F, -0.25F, 0.5F, -0.5F };
            expect(
                meters.publishBlock(samples.data(), samples.data(), samples.size(), 48'000.0, 1, 2)
                    .published);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            expect(!reading.followsDiscontinuity);
            expect(!reading.rawCaptureDiscontinuity);

            meters.skipNextEndpointSequenceForTesting();
            expect(
                meters.publishBlock(samples.data(), samples.data(), samples.size(), 48'000.0, 1, 2)
                    .published);
            expect(meters.consumeLatest(reading));
            expect(reading.followsDiscontinuity);
            expect(!reading.rawCaptureDiscontinuity);

            expect(meters
                    .publishBlock(
                        samples.data(), samples.data(), samples.size(), 48'000.0, 1, 2, true)
                    .published);
            expect(meters.consumeLatest(reading));
            expect(reading.followsDiscontinuity);
            expect(reading.rawCaptureDiscontinuity);
        }

        beginTest("Correlation telemetry separates all-sample input from endpoint handoff");
        {
            StereoMeterAccumulator meters;
            constexpr std::array<float, 4> left { 0.25F, 0.5F, -0.25F, -0.5F };
            constexpr std::array<float, 4> right { 0.125F, 0.25F, -0.125F, -0.25F };
            expect(meters.publishBlock(left.data(), right.data(), left.size(), 48'000.0, 1, 2)
                    .published);

            auto telemetry = meters.correlationTelemetry();
            expect(telemetry.processedSamples == left.size());
            expect(telemetry.publishedEndpoints == 1);
            expect(telemetry.consumedEndpoints == 0);
            expect(telemetry.stateResets == 0);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            telemetry = meters.correlationTelemetry();
            expect(telemetry.consumedEndpoints == 1);

            expect(meters.publishBlock(left.data(), right.data(), left.size(), 48'000.0, 1, 2, true)
                    .published);
            expect(meters.correlationTelemetry().stateResets == 1);

            expect(meters.requestUserReset() == 1);
            expect(meters.publishBlock(left.data(), right.data(), left.size(), 48'000.0, 1, 2)
                    .published);
            expect(meters.correlationTelemetry().stateResets == 1);

            expect(meters.requestLiveClear() == 1);
            expect(meters.publishBlock(left.data(), right.data(), left.size(), 48'000.0, 1, 2)
                    .published);
            expect(meters.correlationTelemetry().stateResets == 2);
        }
    }
};

class SpectrumAnalyzerTests final : public juce::UnitTest {
public:
    SpectrumAnalyzerTests() : UnitTest("Spectrum analyzer", "audio-insight")
    {
    }

    void runTest() override
    {
        testBinCentredFullScaleSine();
        testSupportedSizesAndWindows();
        testDcAndNyquistNormalization();
        testSliceRates();
        testStereoGreaterMagnitude();
        testTemporalAveraging();
        testFinitePeakHold();
        testInfinitePeakHoldAndClear();
        testChunkSequenceGap();
        testInvalidCapturedFrameRange();
        testMonoInput();
    }

private:
    void testBinCentredFullScaleSine()
    {
        beginTest("A bin-centred full-scale sine produces the expected dB bin");
        constexpr std::size_t sineBin = 128;
        std::array<float, fftSize> left { };
        std::array<float, fftSize> right { };

        for (std::size_t frame = 0; frame < fftSize; ++frame) {
            left[frame]
                = std::sin(static_cast<float>(2.0 * std::numbers::pi * static_cast<double>(sineBin)
                    * static_cast<double>(frame) / static_cast<double>(fftSize)));
        }

        SpectrumAnalyzer analyzer;
        VisualizationFrame frame;
        const CapturedStereoChunkView chunk { left.data(), right.data(), left.size(), 2, 1, fftSize,
            48000.0, false };
        expect(analyzer.process(chunk, frame));
        expect(frame.spectrumValid);
        expect(frame.generation == 2);
        expect(frame.fftGeneration == 1);
        expect(frame.capturedFrameEnd == fftSize);
        expect(frame.spectrumFftSize == fftSize);
        expect(frame.spectrumBinCount == spectrumBinCount);
        expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], 0.0F, 0.1F);

        const auto maximum = std::max_element(frame.spectrumDecibels.begin() + 1,
            frame.spectrumDecibels.begin() + static_cast<std::ptrdiff_t>(frame.spectrumBinCount));
        expect(static_cast<std::size_t>(maximum - frame.spectrumDecibels.begin()) == sineBin);
    }

    void testSupportedSizesAndWindows()
    {
        beginTest("Every supported size and window calibrates a full-scale bin to zero dB");
        {
            constexpr std::array supportedFftSizes {
                std::size_t { 1024 },
                std::size_t { 2048 },
                std::size_t { 4096 },
                std::size_t { 8192 },
                std::size_t { 16384 },
            };
            constexpr std::array supportedWindows { FftWindow::rectangular, FftWindow::periodicHann,
                FftWindow::fourTermBlackmanHarris, FftWindow::fiveTermFlatTop };
            constexpr std::size_t sineBin = 37;

            SpectrumAnalyzer analyzer;
            std::array<float, maximumFftSize> signal { };
            auto fftGeneration = std::uint64_t { 2 };

            for (const auto configuredSize : supportedFftSizes) {
                for (const auto window : supportedWindows) {
                    const SpectrumAnalysisConfiguration configuration { configuredSize, window,
                        60 };
                    VisualizationFrame frame;
                    expect(analyzer.reconfigure(configuration, fftGeneration, &frame));
                    expect(!frame.spectrumValid);
                    expect(frame.fftGeneration == fftGeneration);
                    expect(frame.spectrumFftSize == configuredSize);
                    expect(frame.spectrumBinCount == (configuredSize / 2) + 1);

                    for (std::size_t sample = 0; sample < configuredSize; ++sample) {
                        signal[sample] = std::sin(static_cast<float>(2.0 * std::numbers::pi
                            * static_cast<double>(sineBin) * static_cast<double>(sample)
                            / static_cast<double>(configuredSize)));
                    }

                    expect(analyzer.process({ signal.data(), nullptr, configuredSize, 7, 1,
                                                configuredSize, 48'000.0, false, 1 },
                        frame));
                    expect(frame.spectrumValid);
                    expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], 0.0F, 0.015F);
                    ++fftGeneration;
                }
            }
        }
    }

    void testDcAndNyquistNormalization()
    {
        beginTest("DC and Nyquist bins use edge rather than doubled one-sided normalization");
        {
            constexpr std::array supportedFftSizes {
                std::size_t { 1024 },
                std::size_t { 2048 },
                std::size_t { 4096 },
                std::size_t { 8192 },
                std::size_t { 16384 },
            };
            constexpr std::array supportedWindows { FftWindow::rectangular, FftWindow::periodicHann,
                FftWindow::fourTermBlackmanHarris, FftWindow::fiveTermFlatTop };

            SpectrumAnalyzer analyzer;
            std::array<float, maximumFftSize> signal { };
            auto fftGeneration = std::uint64_t { 32 };

            for (const auto configuredSize : supportedFftSizes) {
                for (const auto window : supportedWindows) {
                    const SpectrumAnalysisConfiguration configuration { configuredSize, window,
                        60 };
                    VisualizationFrame frame;
                    expect(analyzer.reconfigure(configuration, fftGeneration, &frame));

                    std::fill_n(signal.begin(), configuredSize, 1.0F);
                    expect(analyzer.process({ signal.data(), nullptr, configuredSize, 8, 1,
                                                configuredSize, 48'000.0, false, 1 },
                        frame));
                    expectWithinAbsoluteError(frame.spectrumDecibels[0], 0.0F, 0.015F);

                    analyzer.reset(&frame);
                    for (std::size_t sample = 0; sample < configuredSize; ++sample)
                        signal[sample] = sample % 2 == 0 ? 1.0F : -1.0F;

                    expect(analyzer.process({ signal.data(), nullptr, configuredSize, 8, 2,
                                                configuredSize * 2, 48'000.0, false, 1 },
                        frame));
                    expectWithinAbsoluteError(
                        frame.spectrumDecibels[configuredSize / 2], 0.0F, 0.015F);
                    ++fftGeneration;
                }
            }
        }
    }

    void testSliceRates()
    {
        beginTest("Slice rates set independent sample-domain transform hops");
        {
            constexpr std::array supportedRates { 15, 30, 60, 120 };
            constexpr auto configuredSize = std::size_t { 1024 };
            constexpr auto sampleRate = 48'000.0;
            std::array<float, configuredSize> warmup { };
            std::vector<float> oneSecond(static_cast<std::size_t>(sampleRate));

            for (const auto rate : supportedRates) {
                SpectrumAnalyzer analyzer;
                const SpectrumAnalysisConfiguration configuration { configuredSize,
                    FftWindow::periodicHann, rate };
                VisualizationFrame frame;
                expect(analyzer.reconfigure(configuration, 2, &frame));
                expect(analyzer.process({ warmup.data(), nullptr, warmup.size(), 1, 1,
                                            warmup.size(), sampleRate, false, 1 },
                    frame));

                const auto transformsBefore = analyzer.statistics().transforms;
                expect(
                    analyzer.process({ oneSecond.data(), nullptr, oneSecond.size(), 1, 2,
                                         warmup.size() + oneSecond.size(), sampleRate, false, 1 },
                        frame));
                expect(analyzer.hopSize()
                    == static_cast<std::size_t>(std::llround(sampleRate / rate)));
                expect(analyzer.statistics().transforms - transformsBefore
                    == static_cast<std::uint64_t>(rate));
            }
        }
    }

    void testStereoGreaterMagnitude()
    {
        beginTest("Stereo keeps the greater magnitude independently in every bin");
        {
            constexpr auto leftBin = std::size_t { 41 };
            constexpr auto rightBin = std::size_t { 113 };
            std::array<float, fftSize> left { };
            std::array<float, fftSize> right { };
            for (std::size_t sample = 0; sample < fftSize; ++sample) {
                const auto phase = 2.0 * std::numbers::pi * static_cast<double>(sample)
                    / static_cast<double>(fftSize);
                left[sample] = 0.25F * std::sin(static_cast<float>(leftBin * phase));
                right[sample] = 0.5F * std::sin(static_cast<float>(rightBin * phase));
            }

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.process(
                { left.data(), right.data(), left.size(), 3, 1, fftSize, 48'000.0, false, 2 },
                frame));
            expectWithinAbsoluteError(frame.spectrumDecibels[leftBin], -12.0412F, 0.02F);
            expectWithinAbsoluteError(frame.spectrumDecibels[rightBin], -6.0206F, 0.02F);
        }
    }

    void testTemporalAveraging()
    {
        beginTest("Attack follows rises immediately while Release averages calibrated power");
        {
            constexpr auto configuredSize = std::size_t { 1024 };
            constexpr auto sampleRate = 15'360.0;
            constexpr auto generation = std::uint64_t { 9 };
            std::array<float, configuredSize> fullScale { };
            std::array<float, configuredSize> silence { };
            fullScale.fill(1.0F);

            SpectrumAnalyzer averaged;
            VisualizationFrame averagedFrame;
            expect(averaged.reconfigure(
                { configuredSize, FftWindow::rectangular, 15 }, 2, &averagedFrame));
            expect(averaged.reconfigureTemporal(
                { 0.0, 1'000.0, SpectrumPeakHoldMode::off, 2.0 }, &averagedFrame));
            expect(averaged.process({ silence.data(), nullptr, silence.size(), generation, 1,
                                        configuredSize, sampleRate, false, 1 },
                averagedFrame));
            expectEquals(averagedFrame.spectrumDecibels[0], minimumSpectrumDecibels);

            expect(averaged.process({ fullScale.data(), nullptr, fullScale.size(), generation, 2,
                                        configuredSize * 2, sampleRate, false, 1 },
                averagedFrame));
            expectWithinAbsoluteError(averagedFrame.spectrumDecibels[0], 0.0F, 0.001F);

            expect(averaged.process({ silence.data(), nullptr, silence.size(), generation, 3,
                                        configuredSize * 3, sampleRate, false, 1 },
                averagedFrame));
            const auto expectedPower = std::exp(-(1.0 / 15.0));
            const auto expectedDecibels = static_cast<float>(10.0 * std::log10(expectedPower));
            expectWithinAbsoluteError(averagedFrame.spectrumDecibels[0], expectedDecibels, 0.001F);
        }

        beginTest("Positive Attack averages rises while Release Off follows falls immediately");
        {
            constexpr auto configuredSize = std::size_t { 1024 };
            constexpr auto sampleRate = 15'360.0;
            constexpr auto generation = std::uint64_t { 10 };
            std::array<float, configuredSize> fullScale { };
            std::array<float, configuredSize> silence { };
            fullScale.fill(1.0F);

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.reconfigure({ configuredSize, FftWindow::rectangular, 15 }, 2, &frame));
            expect(analyzer.reconfigureTemporal(
                { 500.0, 0.0, SpectrumPeakHoldMode::off, 2.0 }, &frame));
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), generation, 1,
                                        configuredSize, sampleRate, false, 1 },
                frame));
            expect(analyzer.process({ fullScale.data(), nullptr, fullScale.size(), generation, 2,
                                        configuredSize * 2, sampleRate, false, 1 },
                frame));
            const auto expectedPower = 1.0 - std::exp(-(1.0 / 15.0) / 0.5);
            const auto expectedDecibels = static_cast<float>(10.0 * std::log10(expectedPower));
            expectWithinAbsoluteError(frame.spectrumDecibels[0], expectedDecibels, 0.001F);

            expect(analyzer.process({ silence.data(), nullptr, silence.size(), generation, 3,
                                        configuredSize * 3, sampleRate, false, 1 },
                frame));
            expectEquals(frame.spectrumDecibels[0], minimumSpectrumDecibels);
        }

        beginTest("Both directions Off publish each raw FFT snapshot");
        {
            constexpr auto configuredSize = std::size_t { 1024 };
            constexpr auto sampleRate = 15'360.0;
            std::array<float, configuredSize> fullScale { };
            std::array<float, configuredSize> silence { };
            fullScale.fill(1.0F);

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.reconfigure({ configuredSize, FftWindow::rectangular, 15 }, 2, &frame));
            expect(
                analyzer.reconfigureTemporal({ 0.0, 0.0, SpectrumPeakHoldMode::off, 2.0 }, &frame));
            expect(analyzer.process({ fullScale.data(), nullptr, fullScale.size(), 11, 1,
                                        configuredSize, sampleRate, false, 1 },
                frame));
            expectWithinAbsoluteError(frame.spectrumDecibels[0], 0.0F, 0.001F);
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 11, 2,
                                        configuredSize * 2, sampleRate, false, 1 },
                frame));
            expectEquals(frame.spectrumDecibels[0], minimumSpectrumDecibels);
        }

        beginTest("Temporal configuration accepts Off or the direction-specific ranges");
        {
            expect(SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 0.0, 0.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 5.0, 25.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 500.0, 2'000.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(!SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 1.0, 250.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(!SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 501.0, 250.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(!SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 0.0, 1.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(!SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 0.0, 2'001.0, SpectrumPeakHoldMode::off, 2.0 }));
            expect(!SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { std::numeric_limits<double>::quiet_NaN(), 250.0, SpectrumPeakHoldMode::off,
                    2.0 }));
            expect(!SpectrumAnalyzer::isSupportedTemporalConfiguration(
                { 0.0, std::numeric_limits<double>::infinity(), SpectrumPeakHoldMode::off, 2.0 }));
        }
    }

    void testFinitePeakHold()
    {
        beginTest("Peak hold uses unsmoothed power and finite hold decays at twelve dB per second");
        {
            constexpr auto configuredSize = std::size_t { 1024 };
            constexpr auto sampleRate = 15'360.0;
            std::array<float, configuredSize> silence { };
            std::array<float, configuredSize> fullScale { };
            fullScale.fill(1.0F);

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.reconfigure({ configuredSize, FftWindow::rectangular, 15 }, 2, &frame));
            expect(analyzer.reconfigureTemporal(
                { 500.0, 2'000.0, SpectrumPeakHoldMode::finite, 0.25 }, &frame));
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, 1, configuredSize,
                                        sampleRate, false, 1 },
                frame));
            expect(analyzer.process({ fullScale.data(), nullptr, fullScale.size(), 1, 2,
                                        configuredSize * 2, sampleRate, false, 1 },
                frame));
            expect(frame.spectrumPeakHoldValid);
            expectWithinAbsoluteError(frame.spectrumPeakHoldDecibels[0], 0.0F, 0.001F);
            expect(frame.spectrumDecibels[0] < -8.0F);

            for (std::uint64_t sequence = 3; sequence <= 6; ++sequence) {
                expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, sequence,
                                            configuredSize * sequence, sampleRate, false, 1 },
                    frame));
            }

            // Four 1/15-second silent intervals exceed the 0.25-second hold by 1/60 second.
            expectWithinAbsoluteError(frame.spectrumPeakHoldDecibels[0], -0.2F, 0.002F);
        }
    }

    void testInfinitePeakHoldAndClear()
    {
        beginTest("Infinite peak hold and user Clear preserve FFT overlap");
        {
            constexpr auto configuredSize = std::size_t { 1024 };
            constexpr auto sampleRate = 48'000.0;
            constexpr auto hopSize = std::size_t { 800 };
            std::array<float, configuredSize> fullScale { };
            std::array<float, hopSize> silence { };
            fullScale.fill(1.0F);

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.reconfigure({ configuredSize, FftWindow::rectangular, 60 }, 2, &frame));
            expect(analyzer.reconfigureTemporal(
                { 75.0, 75.0, SpectrumPeakHoldMode::infinite, 2.0 }, &frame));
            expect(analyzer.process({ fullScale.data(), nullptr, fullScale.size(), 1, 1,
                                        configuredSize, sampleRate, false, 1 },
                frame));
            expectWithinAbsoluteError(frame.spectrumPeakHoldDecibels[0], 0.0F, 0.001F);

            analyzer.clearTemporalState(&frame);
            expect(!frame.spectrumValid);
            expect(!frame.spectrumPeakHoldValid);
            expect(analyzer.statistics().userClears == 1);

            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, 2,
                                        configuredSize + hopSize, sampleRate, false, 1 },
                frame));
            expect(frame.spectrumValid);
            expect(frame.spectrumPeakHoldValid);

            const auto transformsBefore = analyzer.statistics().transforms;
            expect(
                analyzer.reconfigureTemporal({ 0.0, 0.0, SpectrumPeakHoldMode::off, 2.0 }, &frame));
            expect(!frame.spectrumValid);
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, 3,
                                        configuredSize + (hopSize * 2), sampleRate, false, 1 },
                frame));
            expect(analyzer.statistics().transforms == transformsBefore + 1);
            expect(!frame.spectrumPeakHoldValid);
        }
    }

    void testChunkSequenceGap()
    {
        beginTest("A chunk sequence gap clears overlap before another transform");
        {
            std::array<float, fftSize> initial { };
            for (std::size_t frame = 0; frame < fftSize; ++frame)
                initial[frame] = std::sin(static_cast<float>(2.0 * std::numbers::pi * 64.0
                    * static_cast<double>(frame) / static_cast<double>(fftSize)));

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.process(
                { initial.data(), initial.data(), initial.size(), 1, 1, fftSize, 48000.0, false },
                frame));
            expect(frame.spectrumValid);

            std::array<float, 64> afterGap { };
            expect(!analyzer.process({ afterGap.data(), afterGap.data(), afterGap.size(), 1, 3,
                                         fftSize + afterGap.size(), 48000.0, true },
                frame));
            expect(!frame.spectrumValid);
            expect(analyzer.statistics().sequenceGapResets == 1);

            std::array<float, fftSize - afterGap.size()> refill { };
            expect(analyzer.process(
                { refill.data(), refill.data(), refill.size(), 1, 4, fftSize * 2, 48000.0, false },
                frame));
            expect(frame.spectrumValid);
            expect(frame.capturedFrameEnd == fftSize * 2);
            expect(analyzer.statistics().transforms == 2);
        }
    }

    void testInvalidCapturedFrameRange()
    {
        beginTest("An invalid first captured-frame range cannot underflow timing");
        {
            std::array<float, 16> samples { };
            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            frame.spectrumValid = true;

            expect(!analyzer.process({ samples.data(), samples.data(), samples.size(), 1, 1,
                                         samples.size() - 1, 48000.0, false },
                frame));
            expect(!frame.spectrumValid);
            expect(analyzer.statistics().sequenceGapResets == 1);
        }
    }

    void testMonoInput()
    {
        beginTest("Mono is accepted without a synthetic right channel");
        {
            constexpr std::size_t sineBin = 32;
            std::array<float, fftSize> mono { };
            for (std::size_t frame = 0; frame < mono.size(); ++frame) {
                mono[frame] = 0.5F
                    * std::sin(
                        static_cast<float>(2.0 * std::numbers::pi * static_cast<double>(sineBin)
                            * static_cast<double>(frame) / static_cast<double>(fftSize)));
            }

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.process(
                { mono.data(), nullptr, mono.size(), 4, 1, fftSize, 48'000.0, false, 1 }, frame));
            expect(frame.channelCount == 1);
            expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], -6.0206F, 0.1F);
        }
    }
};

StereoSampleCaptureTests stereoSampleCaptureTests;
StereoMeterAccumulatorTests stereoMeterAccumulatorTests;
SpectrumAnalyzerTests spectrumAnalyzerTests;
} // namespace
} // namespace audio_insight
