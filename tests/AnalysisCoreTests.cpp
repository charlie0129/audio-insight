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

namespace audio_insight
{
namespace
{
class StereoSampleCaptureTests final : public juce::UnitTest
{
public:
    StereoSampleCaptureTests() : UnitTest("Stereo sample capture", "audio-insight") {}

    void runTest() override
    {
        beginTest("Continuous chunks preserve plugin sequence and frame time");
        {
            StereoSampleCapture capture;
            constexpr std::array<float, 4> left{0.1F, 0.2F, 0.3F, 0.4F};
            constexpr std::array<float, 4> right{-0.1F, -0.2F, -0.3F, -0.4F};

            for (std::size_t block = 0; block < 3; ++block)
            {
                const auto result =
                    capture.publishBlock(left.data(), right.data(), left.size(), 48000.0, 7);
                expect(result.publishedChunks == 1);
                expect(result.droppedIncomingChunks == 0);
            }

            for (std::uint64_t expectedSequence = 1; expectedSequence <= 3; ++expectedSequence)
            {
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
            std::array<float, 1> sample{};

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index)
            {
                sample[0] = static_cast<float>(index + 1);
                expect(capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1)
                           .publishedChunks == 1);
            }

            std::array<StereoSampleCapture::ReadHandle, StereoSampleCapture::slotCount> held;
            expect(capture.tryAcquireOldest(held[0]));
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);

            for (std::size_t index = 0; index < StereoSampleCapture::slotCount; ++index)
            {
                sample[0] = 100.0F + static_cast<float>(index);
                expect(capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1)
                           .publishedChunks == 1);
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
            const auto dropped =
                capture.publishBlock(sample.data(), sample.data(), sample.size(), 48000.0, 1);
            expect(dropped.droppedIncomingChunks == 1);
            expect(capture.telemetry().droppedIncomingChunks == 1);
            expectWithinAbsoluteError(held[0].view().left[0], 1.0F, 1.0e-7F);
        }
    }
};

class StereoMeterAccumulatorTests final : public juce::UnitTest
{
public:
    StereoMeterAccumulatorTests() : UnitTest("Stereo meter accumulator", "audio-insight") {}

    void runTest() override
    {
        beginTest("Sample peak and RMS use the represented frame count");
        {
            StereoMeterAccumulator meters;
            constexpr std::array<float, 4> left{0.5F, -0.25F, 0.0F, 0.0F};
            constexpr std::array<float, 4> right{0.25F, 0.25F, -0.25F, -0.25F};

            const auto publication =
                meters.publishBlock(left.data(), right.data(), left.size(), 48000.0, 4);
            expect(publication.published);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            expectWithinAbsoluteError(reading.peakLinear[0], 0.5F, 1.0e-6F);
            expectWithinAbsoluteError(reading.peakLinear[1], 0.25F, 1.0e-6F);
            expectWithinAbsoluteError(reading.rmsLinear[0], std::sqrt(0.3125F / 4.0F), 1.0e-6F);
            expectWithinAbsoluteError(reading.rmsLinear[1], 0.25F, 1.0e-6F);
            expectWithinAbsoluteError(reading.peakDecibels[0], -6.0206F, 1.0e-3F);
            expect(reading.representedFrames == left.size());
            expect(!reading.followsDiscontinuity);
        }

        beginTest("Bounded coalescing retains the largest recent peak");
        {
            StereoMeterAccumulator meters;
            std::array<float, 1> low{0.1F};
            std::array<float, 1> high{0.9F};
            std::array<float, 1> tail{0.2F};

            for (std::size_t index = 0; index < StereoMeterAccumulator::slotCount; ++index)
                expect(
                    meters.publishBlock(low.data(), low.data(), low.size(), 48000.0, 1).published);

            expect(
                meters.publishBlock(high.data(), high.data(), high.size(), 48000.0, 1).coalesced);
            expect(
                meters.publishBlock(tail.data(), tail.data(), tail.size(), 48000.0, 1).coalesced);

            StereoMeterReading reading;
            expect(meters.consumeLatest(reading));
            expectWithinAbsoluteError(reading.peakLinear[0], 0.9F, 1.0e-6F);
            const auto expectedRms = std::sqrt((8.0F * 0.01F + 0.81F + 0.04F) / 10.0F);
            expectWithinAbsoluteError(reading.rmsLinear[0], expectedRms, 1.0e-6F);
            expect(reading.representedBlocks == 10);
            expect(!reading.followsDiscontinuity);

            const auto telemetry = meters.telemetry();
            expect(telemetry.coalescedBlocks == 2);
            expect(telemetry.droppedBlocks == 0);
            expect(telemetry.readyHighWaterMark == StereoMeterAccumulator::slotCount);
        }
    }
};

class HannSpectrumAnalyzerTests final : public juce::UnitTest
{
public:
    HannSpectrumAnalyzerTests() : UnitTest("Hann spectrum analyzer", "audio-insight") {}

    void runTest() override
    {
        beginTest("A bin-centred full-scale sine produces the expected dB bin");
        {
            constexpr std::size_t sineBin = 128;
            std::array<float, fftSize> left{};
            std::array<float, fftSize> right{};

            for (std::size_t frame = 0; frame < fftSize; ++frame)
            {
                left[frame] = std::sin(
                    static_cast<float>(2.0 * std::numbers::pi * static_cast<double>(sineBin) *
                                       static_cast<double>(frame) / static_cast<double>(fftSize)));
            }

            HannSpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            const CapturedStereoChunkView chunk{left.data(), right.data(), left.size(), 2,
                                                1,           fftSize,      48000.0,     false};
            expect(analyzer.process(chunk, frame));
            expect(frame.spectrumValid);
            expect(frame.generation == 2);
            expect(frame.capturedFrameEnd == fftSize);
            expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], 0.0F, 0.1F);

            const auto maximum =
                std::max_element(frame.spectrumDecibels.begin() + 1, frame.spectrumDecibels.end());
            expect(static_cast<std::size_t>(maximum - frame.spectrumDecibels.begin()) == sineBin);
        }

        beginTest("A chunk sequence gap clears overlap before another transform");
        {
            std::array<float, fftSize> initial{};
            for (std::size_t frame = 0; frame < fftSize; ++frame)
                initial[frame] = std::sin(
                    static_cast<float>(2.0 * std::numbers::pi * 64.0 * static_cast<double>(frame) /
                                       static_cast<double>(fftSize)));

            HannSpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.process(
                {initial.data(), initial.data(), initial.size(), 1, 1, fftSize, 48000.0, false},
                frame));
            expect(frame.spectrumValid);

            std::array<float, 64> afterGap{};
            expect(!analyzer.process({afterGap.data(), afterGap.data(), afterGap.size(), 1, 3,
                                      fftSize + afterGap.size(), 48000.0, true},
                                     frame));
            expect(!frame.spectrumValid);
            expect(analyzer.statistics().sequenceGapResets == 1);

            std::array<float, fftSize - afterGap.size()> refill{};
            expect(analyzer.process(
                {refill.data(), refill.data(), refill.size(), 1, 4, fftSize * 2, 48000.0, false},
                frame));
            expect(frame.spectrumValid);
            expect(frame.capturedFrameEnd == fftSize * 2);
            expect(analyzer.statistics().transforms == 2);
        }

        beginTest("An invalid first captured-frame range cannot underflow timing");
        {
            std::array<float, 16> samples{};
            HannSpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            frame.spectrumValid = true;

            expect(!analyzer.process({samples.data(), samples.data(), samples.size(), 1, 1,
                                      samples.size() - 1, 48000.0, false},
                                     frame));
            expect(!frame.spectrumValid);
            expect(analyzer.statistics().sequenceGapResets == 1);
        }
    }
};

StereoSampleCaptureTests stereoSampleCaptureTests;
StereoMeterAccumulatorTests stereoMeterAccumulatorTests;
HannSpectrumAnalyzerTests hannSpectrumAnalyzerTests;
} // namespace
} // namespace audio_insight
