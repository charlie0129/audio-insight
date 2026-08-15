// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/PeakRmsBallistics.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

namespace audio_insight {
namespace {
constexpr double testSampleRate = 1'000.0;

template <std::size_t size> std::array<float, size> filledBlock(const float value)
{
    std::array<float, size> result { };
    result.fill(value);
    return result;
}

float linearToDecibels(const double value)
{
    return value > 0.0 ? static_cast<float>(20.0 * std::log10(value)) : minimumDisplayDecibels;
}

class PeakRmsBallisticsTests final : public juce::UnitTest {
public:
    PeakRmsBallisticsTests() : UnitTest("Peak/RMS ballistics", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Full-scale sine RMS converges to -3.01 dBFS without an AES17 offset");
        {
            PeakRmsBallistics ballistics;
            std::array<float, 1'000> sine { };
            for (std::size_t sample = 0; sample < sine.size(); ++sample) {
                sine[sample] = std::sin(static_cast<float>(
                    2.0 * std::numbers::pi * 100.0 * static_cast<double>(sample) / testSampleRate));
            }

            PeakRmsBallisticsFrame frame;
            for (auto block = 0; block < 20; ++block) {
                frame = ballistics.processBlock(
                    sine.data(), sine.data(), sine.size(), testSampleRate, 1, 2);
            }

            expect(frame.valid);
            expectWithinAbsoluteError(frame.rmsDecibels[0], -3.0103F, 0.02F);
            expectWithinAbsoluteError(frame.rmsDecibels[1], -3.0103F, 0.02F);
        }

        beginTest("Source order changes the endpoint of a non-stationary signal");
        {
            const auto high = filledBlock<100>(1.0F);
            const auto silence = filledBlock<900>(0.0F);
            PeakRmsBallistics highThenSilence;
            PeakRmsBallistics silenceThenHigh;

            static_cast<void>(highThenSilence.processBlock(
                high.data(), high.data(), high.size(), testSampleRate, 1, 2));
            const auto decayed = highThenSilence.processBlock(
                silence.data(), silence.data(), silence.size(), testSampleRate, 1, 2);

            static_cast<void>(silenceThenHigh.processBlock(
                silence.data(), silence.data(), silence.size(), testSampleRate, 1, 2));
            const auto recent = silenceThenHigh.processBlock(
                high.data(), high.data(), high.size(), testSampleRate, 1, 2);

            const auto rmsDecay
                = std::exp(-1.0 / (testSampleRate * PeakRmsBallistics::rmsTimeConstantSeconds));
            const auto highPowerAfter100 = 1.0 - std::pow(rmsDecay, 100.0);
            const auto expectedOldPower = highPowerAfter100 * std::pow(rmsDecay, 900.0);

            expectWithinAbsoluteError(
                decayed.rmsLinear[0], static_cast<float>(std::sqrt(expectedOldPower)), 1.0e-6F);
            expectWithinAbsoluteError(
                recent.rmsLinear[0], static_cast<float>(std::sqrt(highPowerAfter100)), 1.0e-6F);
            expect(decayed.rmsLinear[0] < recent.rmsLinear[0]);
            expectWithinAbsoluteError(decayed.liveSamplePeakDecibels[0], -18.0F, 0.001F);
            expectWithinAbsoluteError(recent.liveSamplePeakDecibels[0], 0.0F, 0.0001F);
        }

        beginTest("Identical samples produce the same endpoint across block partitions");
        {
            std::array<float, 1'000> combined { };
            combined.fill(0.0F);
            std::fill_n(combined.begin(), 100, 1.0F);

            PeakRmsBallistics oneBlock;
            PeakRmsBallistics splitBlocks;
            const auto whole = oneBlock.processBlock(
                combined.data(), combined.data(), combined.size(), testSampleRate, 1, 2);
            static_cast<void>(splitBlocks.processBlock(
                combined.data(), combined.data(), 100, testSampleRate, 1, 2));
            const auto split = splitBlocks.processBlock(
                combined.data() + 100, combined.data() + 100, 900, testSampleRate, 1, 2);

            expectWithinAbsoluteError(
                whole.liveSamplePeakLinear[0], split.liveSamplePeakLinear[0], 1.0e-7F);
            expectWithinAbsoluteError(whole.rmsLinear[0], split.rmsLinear[0], 1.0e-7F);
            expectWithinAbsoluteError(
                whole.heldSamplePeakLinear[0], split.heldSamplePeakLinear[0], 1.0e-7F);

            const auto tail = filledBlock<1'250>(0.0F);
            const auto wholeTail = oneBlock.processBlock(
                tail.data(), tail.data(), tail.size(), testSampleRate, 1, 2);
            const auto splitTail = splitBlocks.processBlock(
                tail.data(), tail.data(), tail.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(
                wholeTail.heldSamplePeakLinear[0], splitTail.heldSamplePeakLinear[0], 1.0e-7F);
        }

        beginTest("Peak releases at 20 dB per second and hold decays after exactly two seconds");
        {
            PeakRmsBallistics ballistics;
            constexpr std::array<float, 1> peak { 1.0F };
            const auto oneSecondSilence = filledBlock<1'000>(0.0F);
            const auto oneEighthSecondSilence = filledBlock<125>(0.0F);

            static_cast<void>(ballistics.processBlock(
                peak.data(), peak.data(), peak.size(), testSampleRate, 1, 2));
            auto frame = ballistics.processBlock(oneSecondSilence.data(), oneSecondSilence.data(),
                oneSecondSilence.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(frame.liveSamplePeakDecibels[0], -20.0F, 0.001F);
            expectWithinAbsoluteError(frame.heldSamplePeakDecibels[0], 0.0F, 0.0001F);

            frame = ballistics.processBlock(oneSecondSilence.data(), oneSecondSilence.data(),
                oneSecondSilence.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(frame.heldSamplePeakDecibels[0], 0.0F, 0.0001F);

            frame = ballistics.processBlock(oneEighthSecondSilence.data(),
                oneEighthSecondSilence.data(), oneEighthSecondSilence.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(frame.heldSamplePeakDecibels[0], -2.5F, 0.001F);
        }

        beginTest("A later lower peak overtakes a decayed hold and starts a new hold");
        {
            PeakRmsBallistics ballistics;
            constexpr std::array<float, 1> fullScale { 1.0F };
            constexpr std::array<float, 1> lower { 0.2F };
            const auto threeSecondSilence = filledBlock<3'000>(0.0F);

            static_cast<void>(ballistics.processBlock(
                fullScale.data(), fullScale.data(), fullScale.size(), testSampleRate, 1, 2));
            auto frame = ballistics.processBlock(threeSecondSilence.data(),
                threeSecondSilence.data(), threeSecondSilence.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(frame.heldSamplePeakDecibels[0], -20.0F, 0.001F);

            frame = ballistics.processBlock(
                lower.data(), lower.data(), lower.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(
                frame.heldSamplePeakDecibels[0], linearToDecibels(0.2), 0.001F);

            const auto oneSecondSilence = filledBlock<1'000>(0.0F);
            frame = ballistics.processBlock(oneSecondSilence.data(), oneSecondSilence.data(),
                oneSecondSilence.size(), testSampleRate, 1, 2);
            expectWithinAbsoluteError(
                frame.heldSamplePeakDecibels[0], linearToDecibels(0.2), 0.001F);
        }

        beginTest("User reset and stale clear affect disjoint temporal state");
        {
            PeakRmsBallistics ballistics;
            const auto over = filledBlock<100>(1.1F);
            auto frame = ballistics.processBlock(
                over.data(), over.data(), over.size(), testSampleRate, 1, 2);
            const auto liveBeforeReset = frame.liveSamplePeakLinear;
            const auto rmsBeforeReset = frame.rmsLinear;

            ballistics.userReset();
            frame = ballistics.current();
            expect(!frame.over[0] && !frame.over[1]);
            expectEquals(frame.heldSamplePeakDecibels[0], minimumDisplayDecibels);
            expectEquals(frame.heldSamplePeakDecibels[1], minimumDisplayDecibels);
            expectEquals(frame.liveSamplePeakLinear[0], liveBeforeReset[0]);
            expectEquals(frame.rmsLinear[0], rmsBeforeReset[0]);

            constexpr std::array<float, 1> overAgain { 1.0F };
            frame = ballistics.processBlock(
                overAgain.data(), overAgain.data(), overAgain.size(), testSampleRate, 1, 2);
            const auto heldBeforeClear = frame.heldSamplePeakLinear;
            expect(frame.over[0] && frame.over[1]);

            ballistics.clearLiveMeasurements();
            frame = ballistics.current();
            expectEquals(frame.liveSamplePeakDecibels[0], minimumDisplayDecibels);
            expectEquals(frame.rmsDecibels[0], minimumDisplayDecibels);
            expectEquals(frame.heldSamplePeakLinear[0], heldBeforeClear[0]);
            expect(frame.over[0] && frame.over[1]);
        }

        beginTest("Mono ignores the unused right channel and keeps it invalid");
        {
            PeakRmsBallistics ballistics;
            const auto mono = filledBlock<100>(0.5F);
            const auto ignoredRight = filledBlock<100>(2.0F);
            const auto frame = ballistics.processBlock(
                mono.data(), ignoredRight.data(), mono.size(), testSampleRate, 1, 1);

            expect(frame.valid);
            expect(frame.channelCount == 1);
            expect(frame.channelValid[0]);
            expect(!frame.channelValid[1]);
            expectEquals(frame.liveSamplePeakDecibels[1], minimumDisplayDecibels);
            expectEquals(frame.rmsDecibels[1], minimumDisplayDecibels);
            expectEquals(frame.heldSamplePeakDecibels[1], minimumDisplayDecibels);
            expect(!frame.over[1]);
        }

        beginTest("Lifecycle, format, and discontinuity boundaries reset before current input");
        {
            const auto verifyReset = [this](const double sampleRate, const std::uint64_t generation,
                                         const std::uint32_t channelCount,
                                         const bool followsDiscontinuity) {
                PeakRmsBallistics ballistics;
                const auto seed = filledBlock<1'000>(1.1F);
                static_cast<void>(ballistics.processBlock(
                    seed.data(), seed.data(), seed.size(), testSampleRate, 1, 2));

                constexpr std::array<float, 1> low { 0.1F };
                const auto frame = ballistics.processBlock(low.data(), low.data(), low.size(),
                    sampleRate, generation, channelCount, followsDiscontinuity);
                const auto coefficient
                    = std::exp(-1.0 / (sampleRate * PeakRmsBallistics::rmsTimeConstantSeconds));
                const auto expectedRms = 0.1 * std::sqrt(1.0 - coefficient);

                expect(frame.valid);
                expect(!frame.over[0]);
                expectWithinAbsoluteError(frame.liveSamplePeakDecibels[0], -20.0F, 0.001F);
                expectWithinAbsoluteError(frame.heldSamplePeakDecibels[0], -20.0F, 0.001F);
                expectWithinAbsoluteError(
                    frame.rmsLinear[0], static_cast<float>(expectedRms), 1.0e-6F);
            };

            verifyReset(testSampleRate, 1, 2, true);
            verifyReset(testSampleRate, 2, 2, false);
            verifyReset(2'000.0, 1, 2, false);
            verifyReset(testSampleRate, 1, 1, false);
        }

        beginTest("Invalid metadata clears the model safely");
        {
            PeakRmsBallistics ballistics;
            const auto samples = filledBlock<16>(0.5F);
            static_cast<void>(ballistics.processBlock(
                samples.data(), samples.data(), samples.size(), testSampleRate, 1, 2));

            auto frame
                = ballistics.processBlock(samples.data(), samples.data(), 0, testSampleRate, 1, 2);
            expect(!frame.valid);
            expect(frame.channelCount == 0);
            expectEquals(frame.liveSamplePeakDecibels[0], minimumDisplayDecibels);

            expect(!ballistics
                    .processBlock(
                        samples.data(), samples.data(), samples.size(), testSampleRate, 1, 0)
                    .valid);
            expect(!ballistics
                    .processBlock(
                        samples.data(), samples.data(), samples.size(), testSampleRate, 1, 3)
                    .valid);
            expect(!ballistics
                    .processBlock(
                        samples.data(), samples.data(), samples.size(), testSampleRate, 0, 2)
                    .valid);
            expect(
                !ballistics.processBlock(samples.data(), samples.data(), samples.size(), 0.0, 1, 2)
                    .valid);
            expect(!ballistics
                    .processBlock(samples.data(), samples.data(), samples.size(),
                        std::numeric_limits<double>::quiet_NaN(), 1, 2)
                    .valid);
            expect(!ballistics
                    .processBlock(samples.data(), samples.data(), samples.size(),
                        std::numeric_limits<double>::infinity(), 1, 2)
                    .valid);
        }
    }
};

static PeakRmsBallisticsTests peakRmsBallisticsTests;
} // namespace
} // namespace audio_insight
