// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/StereoFieldAnalyzer.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace audio_insight {
namespace {
CapturedStereoChunkView makeChunk(const float* const left, const float* const right,
    const std::size_t frameCount, const std::uint64_t sequence,
    const std::uint64_t capturedFrameEnd, const double sampleRate = 4.0,
    const std::uint64_t generation = 1, const std::uint32_t channelCount = 2,
    const bool followsDiscontinuity = false)
{
    return { left, right, frameCount, generation, sequence, capturedFrameEnd, sampleRate,
        followsDiscontinuity, channelCount };
}

class StereoFieldAnalyzerTests final : public juce::UnitTest {
public:
    StereoFieldAnalyzerTests() : UnitTest("Stereo field analyzer", "audio-insight")
    {
    }

    void runTest() override
    {
        testConventionalOrientation();
        testFixedScale();
        testBoundedWindowAndAges();
        testHistoryExpiration();
        testContinuityResets();
        testMalformedMetadata();
    }

private:
    void expectSinglePoint(const float left, const float right, const float expectedHorizontal,
        const float expectedVertical)
    {
        StereoFieldAnalyzer analyzer;
        VisualizationFrame frame;
        expect(analyzer.process(makeChunk(&left, &right, 1, 1, 1)));
        expect(analyzer.writeFrame(frame));
        expect(frame.stereoFieldValid);
        expect(frame.stereoFieldPointCount == 1);
        expectWithinAbsoluteError(
            frame.stereoFieldPoints[0].horizontal, expectedHorizontal, 1.0e-7F);
        expectWithinAbsoluteError(frame.stereoFieldPoints[0].vertical, expectedVertical, 1.0e-7F);
        expectWithinAbsoluteError(frame.stereoFieldPoints[0].normalizedAge, 0.0F, 1.0e-7F);
    }

    void testConventionalOrientation()
    {
        beginTest("Stereo rotation gives conventional anti-phase and channel orientation");
        expectSinglePoint(0.8F, -0.8F, -0.8F, 0.0F);
        expectSinglePoint(1.0F, 0.0F, -0.5F, 0.5F);
        expectSinglePoint(0.0F, 1.0F, 0.5F, 0.5F);

        beginTest("Mono accepts a null right channel and remains vertical");
        {
            StereoFieldAnalyzer analyzer;
            constexpr std::array<float, 2> mono { 0.75F, -0.25F };
            VisualizationFrame frame;
            expect(analyzer.process(makeChunk(mono.data(), nullptr, mono.size(), 1, 2, 8.0, 1, 1)));
            expect(analyzer.writeFrame(frame));
            expect(frame.stereoMono);
            expect(frame.channelCount == 1);
            expect(frame.stereoFieldPointCount == 2);
            for (std::size_t index = 0; index < mono.size(); ++index) {
                expectWithinAbsoluteError(frame.stereoFieldPoints[index].horizontal, 0.0F, 1.0e-7F);
                expectWithinAbsoluteError(
                    frame.stereoFieldPoints[index].vertical, mono[index], 1.0e-7F);
            }
        }
    }

    void testFixedScale()
    {
        beginTest("Quiet input stays quiet without auto-normalization or clamping");
        constexpr float left = 0.001F;
        constexpr float right = -0.0005F;
        StereoFieldAnalyzer analyzer;
        VisualizationFrame frame;
        expect(analyzer.process(makeChunk(&left, &right, 1, 1, 1)));
        expect(analyzer.writeFrame(frame));
        expectWithinAbsoluteError(frame.stereoFieldPoints[0].horizontal, -0.00075F, 1.0e-8F);
        expectWithinAbsoluteError(frame.stereoFieldPoints[0].vertical, 0.00025F, 1.0e-8F);

        constexpr float outOfRange = 3.0F;
        expect(analyzer.process(makeChunk(&outOfRange, &outOfRange, 1, 2, 2)));
        expect(analyzer.writeFrame(frame));
        expect(frame.stereoFieldPoints[frame.stereoFieldPointCount - 1].vertical > 1.0F);
    }

    void testBoundedWindowAndAges()
    {
        beginTest("48 kHz history uses stride three and exact chronological ages");
        constexpr std::size_t windowFrames = 12'000;
        std::vector<float> left(windowFrames);
        std::vector<float> right(windowFrames);
        for (std::size_t frame = 0; frame < windowFrames; ++frame) {
            left[frame] = static_cast<float>(frame) / static_cast<float>(windowFrames);
            right[frame] = left[frame];
        }

        StereoFieldAnalyzer analyzer;
        VisualizationFrame frame;
        expect(analyzer.process(
            makeChunk(left.data(), right.data(), left.size(), 1, windowFrames, 48'000.0)));
        expect(analyzer.writeFrame(frame));
        expect(frame.stereoPointStrideFrames == 3);
        expect(frame.stereoFieldPointCount == 4'000);
        expect(frame.stereoFieldPointCount <= maximumStereoFieldPointCount);
        expectWithinAbsoluteError(frame.stereoFieldPoints.front().vertical, 0.0F, 1.0e-7F);
        expectWithinAbsoluteError(
            frame.stereoFieldPoints.front().normalizedAge, 11'999.0F / 12'000.0F, 1.0e-7F);
        const auto& newest = frame.stereoFieldPoints[frame.stereoFieldPointCount - 1];
        expectWithinAbsoluteError(newest.vertical, 11'997.0F / 12'000.0F, 1.0e-7F);
        expectWithinAbsoluteError(newest.normalizedAge, 2.0F / 12'000.0F, 1.0e-7F);

        constexpr std::array<float, 3> next { 0.75F, 0.5F, 0.25F };
        expect(analyzer.process(makeChunk(
            next.data(), next.data(), next.size(), 2, windowFrames + next.size(), 48'000.0)));
        expect(analyzer.writeFrame(frame));
        expect(frame.stereoFieldPointCount == 4'000);
        expectWithinAbsoluteError(
            frame.stereoFieldPoints.front().vertical, 3.0F / 12'000.0F, 1.0e-7F);
        expectWithinAbsoluteError(
            frame.stereoFieldPoints[frame.stereoFieldPointCount - 1].vertical, 0.75F, 1.0e-7F);
    }

    void testHistoryExpiration()
    {
        beginTest("Advancing capture expires samples older than 250 ms");
        StereoFieldAnalyzer analyzer;
        constexpr std::array<float, 4> first { 1.0F, 2.0F, 3.0F, 4.0F };
        constexpr std::array<float, 4> second { 5.0F, 6.0F, 7.0F, 8.0F };
        expect(analyzer.process(makeChunk(first.data(), first.data(), first.size(), 1, 4, 16.0)));
        expect(
            analyzer.process(makeChunk(second.data(), second.data(), second.size(), 2, 8, 16.0)));

        VisualizationFrame frame;
        expect(analyzer.writeFrame(frame));
        expect(frame.stereoFieldPointCount == 4);
        for (std::size_t index = 0; index < second.size(); ++index)
            expectWithinAbsoluteError(
                frame.stereoFieldPoints[index].vertical, second[index], 1.0e-7F);
        expectWithinAbsoluteError(frame.stereoFieldPoints.front().normalizedAge, 0.75F, 1.0e-7F);
        expectWithinAbsoluteError(frame.stereoFieldPoints[3].normalizedAge, 0.0F, 1.0e-7F);
    }

    void testContinuityResets()
    {
        beginTest("Sequence, frame, format, generation, and explicit gaps reset history");
        StereoFieldAnalyzer analyzer;
        constexpr float sample = 0.25F;
        VisualizationFrame frame;
        expect(analyzer.process(makeChunk(&sample, &sample, 1, 1, 1)));
        auto expectedResets = analyzer.statistics().historyResets;

        const auto expectReset
            = [this, &analyzer, &frame, &expectedResets](const CapturedStereoChunkView& chunk) {
                  expect(analyzer.process(chunk));
                  expect(analyzer.writeFrame(frame));
                  ++expectedResets;
                  expect(analyzer.statistics().historyResets == expectedResets);
                  expect(frame.stereoFieldPointCount == 1);
              };

        expectReset(makeChunk(&sample, &sample, 1, 3, 2));
        expectReset(makeChunk(&sample, &sample, 1, 4, 4));
        expectReset(makeChunk(&sample, &sample, 1, 5, 5, 8.0));
        expectReset(makeChunk(&sample, &sample, 1, 6, 6, 8.0, 2));
        expectReset(makeChunk(&sample, &sample, 1, 7, 7, 8.0, 2, 2, true));
    }

    void testMalformedMetadata()
    {
        beginTest("Malformed metadata clears state without overflowing frame arithmetic");
        StereoFieldAnalyzer analyzer;
        constexpr float sample = 0.25F;
        VisualizationFrame frame;
        expect(analyzer.process(makeChunk(&sample, &sample, 1, 1, 1)));

        expect(!analyzer.process(
            makeChunk(&sample, &sample, 1, 2, 2, std::numeric_limits<double>::max())));
        expect(!analyzer.writeFrame(frame));
        expect(!frame.stereoFieldValid);
        expect(frame.stereoFieldPointCount == 0);

        expect(!analyzer.process(makeChunk(&sample, nullptr, 1, 3, 3, 48'000.0, 1, 2)));
        expect(!analyzer.process(makeChunk(&sample, &sample, 1, 4, 0)));
        const auto statistics = analyzer.statistics();
        expect(statistics.invalidChunks == 3);
        expect(statistics.pointCount == 0);
        expect(statistics.capturedFrameEnd == 0);
    }
};

static StereoFieldAnalyzerTests stereoFieldAnalyzerTests;
} // namespace
} // namespace audio_insight
