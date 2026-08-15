// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/HannSpectrumAnalyzer.h"
#include "analysis/StereoMeterAccumulator.h"
#include "analysis/StereoSampleCapture.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

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
            constexpr std::array<float, 4> left { 0.1F, 0.2F, 0.3F, 0.4F };
            constexpr std::array<float, 4> right { -0.1F, -0.2F, -0.3F, -0.4F };

            for (std::size_t block = 0; block < 3; ++block) {
                const auto result
                    = capture.publishBlock(left.data(), right.data(), left.size(), 48000.0, 7);
                expect(result.publishedChunks == 1);
                expect(result.droppedIncomingChunks == 0);
            }

            for (std::uint64_t expectedSequence = 1; expectedSequence <= 3; ++expectedSequence) {
                StereoSampleCapture::ReadHandle handle;
                expect(capture.tryAcquireOldest(handle));
                expect(handle.view().sequence == expectedSequence);
                expect(handle.view().capturedFrameEnd == expectedSequence * left.size());
                expect(!handle.view().followsDiscontinuity);
                expectWithinAbsoluteError(handle.view().left[2], left[2], 1.0e-7F);
                expectWithinAbsoluteError(handle.view().right[2], right[2], 1.0e-7F);
            }

            const auto telemetry = capture.telemetry();
            expect(telemetry.attemptedChunks == 3);
            expect(telemetry.publishedChunks == 3);
            expect(telemetry.lostChunks() == 0);
            expect(telemetry.readyHighWaterMark == 3);
        }

        beginTest("Overflow never overwrites reading storage and is observable");
        {
            StereoSampleCapture capture;
            std::array<float, 1> sample { };

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index) {
                sample[0] = static_cast<float>(index + 1);
                expect(capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1)
                           .publishedChunks
                    == 1);
            }

            std::array<StereoSampleCapture::ReadHandle, StereoSampleCapture::slotCount> held;
            expect(capture.tryAcquireOldest(held[0]));
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index) {
                sample[0] = 100.0F + static_cast<float>(index);
                expect(capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1)
                           .publishedChunks
                    == 1);
            }

            // The producer reclaimed only ready slots; the held payload is intact.
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);
            auto telemetry = capture.telemetry();
            expect(telemetry.reclaimedReadyChunks == StereoSampleCapture::slotCount);
            expect(telemetry.droppedIncomingChunks == 0);

            for (std::size_t index = 1; index < held.size(); ++index)
                expect(capture.tryAcquireOldest(held[index]));

            expect(held[1].view().followsDiscontinuity);
            telemetry = capture.telemetry();
            expect(telemetry.consumerDiscontinuities == 1);

            // With every fixed slot being read there is no safe reclaim target.
            sample[0] = 999.0F;
            const auto dropped
                = capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1);
            expect(dropped.droppedIncomingChunks == 1);
            expect(capture.telemetry().droppedIncomingChunks == 1);
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);
        }

        beginTest("Mono layout metadata is preserved without duplicating its samples");
        {
            StereoSampleCapture capture;
            constexpr std::array<float, 3> mono { 0.25F, -0.5F, 0.75F };

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

        beginTest("Unsupported channel metadata is preserved for downstream rejection");
        {
            StereoSampleCapture capture;
            constexpr std::array<float, 1> sample { 0.25F };
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

            expect(meters.publishBlock(low.data(), low.data(), low.size(), 1'000.0, 1, 2, true)
                    .published);
            StereoMeterReading afterGap;
            expect(meters.consumeLatest(afterGap));
            expect(afterGap.followsDiscontinuity);
            expect(!afterGap.over[0] && !afterGap.over[1]);
            expectWithinAbsoluteError(afterGap.peakDecibels[0], -20.0F, 0.001F);
            expectWithinAbsoluteError(afterGap.heldPeakDecibels[0], -20.0F, 0.001F);
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
    }
};

class HannSpectrumAnalyzerTests final : public juce::UnitTest {
public:
    HannSpectrumAnalyzerTests() : UnitTest("Hann spectrum analyzer", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("A bin-centred full-scale sine produces the expected dB bin");
        {
            constexpr std::size_t sineBin = 128;
            std::array<float, fftSize> left { };
            std::array<float, fftSize> right { };

            for (std::size_t frame = 0; frame < fftSize; ++frame) {
                left[frame] = std::sin(
                    static_cast<float>(2.0 * std::numbers::pi * static_cast<double>(sineBin)
                        * static_cast<double>(frame) / static_cast<double>(fftSize)));
            }

            HannSpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            const CapturedStereoChunkView chunk { left.data(), right.data(), left.size(), 2, 1,
                fftSize, 48000.0, false };
            expect(analyzer.process(chunk, frame));
            expect(frame.spectrumValid);
            expect(frame.generation == 2);
            expect(frame.capturedFrameEnd == fftSize);
            expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], 0.0F, 0.1F);

            const auto maximum = std::max_element(
                frame.spectrumDecibels.begin() + 1, frame.spectrumDecibels.end());
            expect(static_cast<std::size_t>(maximum - frame.spectrumDecibels.begin()) == sineBin);
        }

        beginTest("A chunk sequence gap clears overlap before another transform");
        {
            std::array<float, fftSize> initial { };
            for (std::size_t frame = 0; frame < fftSize; ++frame)
                initial[frame] = std::sin(static_cast<float>(2.0 * std::numbers::pi * 64.0
                    * static_cast<double>(frame) / static_cast<double>(fftSize)));

            HannSpectrumAnalyzer analyzer;
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

        beginTest("An invalid first captured-frame range cannot underflow timing");
        {
            std::array<float, 16> samples { };
            HannSpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            frame.spectrumValid = true;

            expect(!analyzer.process({ samples.data(), samples.data(), samples.size(), 1, 1,
                                         samples.size() - 1, 48000.0, false },
                frame));
            expect(!frame.spectrumValid);
            expect(analyzer.statistics().sequenceGapResets == 1);
        }

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

            HannSpectrumAnalyzer analyzer;
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
HannSpectrumAnalyzerTests hannSpectrumAnalyzerTests;
} // namespace
} // namespace audio_insight
