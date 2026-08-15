// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/SpectrumAnalyzer.h"
#include "analysis/StereoMeterAccumulator.h"
#include "analysis/StereoSampleCapture.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

class SpectrumAnalyzerTests final : public juce::UnitTest {
public:
    SpectrumAnalyzerTests() : UnitTest("Spectrum analyzer", "audio-insight")
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

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            const CapturedStereoChunkView chunk { left.data(), right.data(), left.size(), 2, 1,
                fftSize, 48000.0, false };
            expect(analyzer.process(chunk, frame));
            expect(frame.spectrumValid);
            expect(frame.generation == 2);
            expect(frame.fftGeneration == 1);
            expect(frame.capturedFrameEnd == fftSize);
            expect(frame.spectrumFftSize == fftSize);
            expect(frame.spectrumBinCount == spectrumBinCount);
            expectWithinAbsoluteError(frame.spectrumDecibels[sineBin], 0.0F, 0.1F);

            const auto maximum = std::max_element(frame.spectrumDecibels.begin() + 1,
                frame.spectrumDecibels.begin()
                    + static_cast<std::ptrdiff_t>(frame.spectrumBinCount));
            expect(static_cast<std::size_t>(maximum - frame.spectrumDecibels.begin()) == sineBin);
        }

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

        beginTest("Temporal averaging seeds immediately and averages calibrated power");
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
                { true, 1'000.0, SpectrumPeakHoldMode::off, 2.0 }, &averagedFrame));
            expect(averaged.process({ fullScale.data(), nullptr, fullScale.size(), generation, 1,
                                        configuredSize, sampleRate, false, 1 },
                averagedFrame));
            expectWithinAbsoluteError(averagedFrame.spectrumDecibels[0], 0.0F, 0.001F);

            expect(averaged.process({ silence.data(), nullptr, silence.size(), generation, 2,
                                        configuredSize * 2, sampleRate, false, 1 },
                averagedFrame));
            const auto expectedPower = std::exp(-(1.0 / 15.0));
            const auto expectedDecibels = static_cast<float>(10.0 * std::log10(expectedPower));
            expectWithinAbsoluteError(averagedFrame.spectrumDecibels[0], expectedDecibels, 0.001F);

            SpectrumAnalyzer immediate;
            VisualizationFrame immediateFrame;
            expect(immediate.reconfigure(
                { configuredSize, FftWindow::rectangular, 15 }, 2, &immediateFrame));
            expect(immediate.reconfigureTemporal(
                { false, 1'000.0, SpectrumPeakHoldMode::off, 2.0 }, &immediateFrame));
            expect(immediate.process({ fullScale.data(), nullptr, fullScale.size(), generation, 1,
                                         configuredSize, sampleRate, false, 1 },
                immediateFrame));
            expect(immediate.process({ silence.data(), nullptr, silence.size(), generation, 2,
                                         configuredSize * 2, sampleRate, false, 1 },
                immediateFrame));
            expectEquals(immediateFrame.spectrumDecibels[0], minimumSpectrumDecibels);
        }

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
                { true, 2'000.0, SpectrumPeakHoldMode::finite, 0.25 }, &frame));
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, 1, configuredSize,
                                        sampleRate, false, 1 },
                frame));
            expect(analyzer.process({ fullScale.data(), nullptr, fullScale.size(), 1, 2,
                                        configuredSize * 2, sampleRate, false, 1 },
                frame));
            expect(frame.spectrumPeakHoldValid);
            expectWithinAbsoluteError(frame.spectrumPeakHoldDecibels[0], 0.0F, 0.001F);
            expect(frame.spectrumDecibels[0] < -14.0F);

            for (std::uint64_t sequence = 3; sequence <= 6; ++sequence) {
                expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, sequence,
                                            configuredSize * sequence, sampleRate, false, 1 },
                    frame));
            }

            // Four 1/15-second silent intervals exceed the 0.25-second hold by 1/60 second.
            expectWithinAbsoluteError(frame.spectrumPeakHoldDecibels[0], -0.2F, 0.002F);
        }

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
                { true, 75.0, SpectrumPeakHoldMode::infinite, 2.0 }, &frame));
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
            expect(analyzer.reconfigureTemporal(
                { false, 75.0, SpectrumPeakHoldMode::off, 2.0 }, &frame));
            expect(!frame.spectrumValid);
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 1, 3,
                                        configuredSize + (hopSize * 2), sampleRate, false, 1 },
                frame));
            expect(analyzer.statistics().transforms == transformsBefore + 1);
            expect(!frame.spectrumPeakHoldValid);
        }

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
