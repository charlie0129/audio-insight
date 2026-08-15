// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/LoudnessAnalyzer.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace audio_insight {
namespace {
constexpr double referenceSampleRate = 48'000.0;
constexpr double referenceToneFrequency = 1'000.0;

double energyForLufs(const double loudness)
{
    return std::pow(10.0, (loudness - LoudnessAnalyzer::loudnessOffsetLufs) / 10.0);
}

struct IndexedGateEvaluation final {
    LoudnessAnalyzer::IntegratedGateResult result;
    IntegratedLoudnessIndex::Statistics statistics;
    bool insertedEveryValue = true;
};

IndexedGateEvaluation evaluateIndexedGate(const std::span<const double> energies)
{
    IntegratedLoudnessIndex index(std::max<std::size_t>(energies.size(), 1));
    const auto absoluteThreshold = energyForLufs(LoudnessAnalyzer::absoluteGateLufs);
    auto absoluteSum = 0.0;
    auto absoluteCount = std::uint64_t { 0 };
    auto insertedEveryValue = true;
    for (const auto energy : energies) {
        if (std::isfinite(energy) && energy > absoluteThreshold) {
            insertedEveryValue = index.insert(energy) && insertedEveryValue;
            absoluteSum += energy;
            ++absoluteCount;
        }
    }

    LoudnessAnalyzer::IntegratedGateResult result;
    result.absoluteGatedBlockCount = absoluteCount;
    if (absoluteCount != 0 && std::isfinite(absoluteSum) && absoluteSum > 0.0) {
        const auto relativeThreshold = (absoluteSum / static_cast<double>(absoluteCount)) * 0.1;
        result.relativeGateLufs
            = LoudnessAnalyzer::loudnessOffsetLufs + (10.0 * std::log10(relativeThreshold));
        const auto relative
            = index.queryGreaterThan(std::max(absoluteThreshold, relativeThreshold));
        result.relativeGatedBlockCount = relative.count;
        if (relative.count != 0 && std::isfinite(relative.sum) && relative.sum > 0.0) {
            result.integratedLufs = LoudnessAnalyzer::loudnessOffsetLufs
                + (10.0 * std::log10(relative.sum / static_cast<double>(relative.count)));
        }
    }
    return { result, index.statistics(), insertedEveryValue };
}

struct ReferenceBiquadState final {
    double firstDelay = 0.0;
    double secondDelay = 0.0;
};

double processReferenceBiquad(const double input,
    const LoudnessAnalyzer::BiquadCoefficients& coefficients, ReferenceBiquadState& state)
{
    const auto output = (coefficients.b0 * input) + state.firstDelay;
    const auto nextFirstDelay
        = (coefficients.b1 * input) - (coefficients.a1 * output) + state.secondDelay;
    const auto nextSecondDelay = (coefficients.b2 * input) - (coefficients.a2 * output);
    state.firstDelay = nextFirstDelay;
    state.secondDelay = nextSecondDelay;
    return output;
}

std::vector<double> referenceMonoEnergies(
    const std::vector<float>& samples, const double sampleRate)
{
    const auto coefficients = LoudnessAnalyzer::coefficientsForSampleRate(sampleRate);
    ReferenceBiquadState headState;
    ReferenceBiquadState highPassState;
    std::vector<double> energies;
    energies.reserve(samples.size());

    for (const auto sample : samples) {
        const auto head = processReferenceBiquad(sample, coefficients.headFilter, headState);
        const auto filtered
            = processReferenceBiquad(head, coefficients.highPassFilter, highPassState);
        energies.push_back(filtered * filtered);
    }

    return energies;
}

double literalWindowLufs(
    const std::vector<double>& energies, const std::size_t frameEnd, const std::size_t windowFrames)
{
    const auto first = frameEnd - windowFrames;
    auto sum = 0.0;
    for (auto frame = first; frame < frameEnd; ++frame)
        sum += energies[frame];

    return LoudnessAnalyzer::loudnessOffsetLufs
        + (10.0 * std::log10(sum / static_cast<double>(windowFrames)));
}

struct StreamCursor final {
    explicit StreamCursor(const double rate, const std::uint32_t channels)
        : sampleRate(rate), channelCount(channels)
    {
    }

    LoudnessAnalyzer::ProcessResult feed(LoudnessAnalyzer& analyzer, const float* const left,
        const float* const right, const std::size_t frameCount,
        const bool followsDiscontinuity = false)
    {
        capturedFrameEnd += frameCount;
        const CapturedStereoChunkView chunk { left, right, frameCount, generation, sequence,
            capturedFrameEnd, sampleRate, followsDiscontinuity, channelCount };
        ++sequence;
        return analyzer.process(chunk);
    }

    double sampleRate;
    std::uint32_t channelCount;
    std::uint64_t generation = 1;
    std::uint64_t sequence = 1;
    std::uint64_t capturedFrameEnd = 0;
    std::uint64_t toneFrame = 0;
};

void feedTone(LoudnessAnalyzer& analyzer, StreamCursor& cursor, std::uint64_t frameCount,
    const double peakDecibels, const double frequency = referenceToneFrequency,
    const std::size_t maximumChunkFrames = 2'048, const float rightPolarity = 1.0F)
{
    const auto amplitude = static_cast<float>(std::pow(10.0, peakDecibels / 20.0));
    std::vector<float> left(maximumChunkFrames);
    std::vector<float> right(maximumChunkFrames);

    while (frameCount != 0) {
        const auto frames
            = static_cast<std::size_t>(std::min<std::uint64_t>(frameCount, maximumChunkFrames));
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto phase = 2.0 * std::numbers::pi * frequency
                * static_cast<double>(cursor.toneFrame + frame) / cursor.sampleRate;
            left[frame] = amplitude * static_cast<float>(std::sin(phase));
            right[frame] = rightPolarity * left[frame];
        }

        const auto* rightInput = cursor.channelCount == 2 ? right.data() : nullptr;
        const auto result = cursor.feed(analyzer, left.data(), rightInput, frames);
        juce::ignoreUnused(result);
        cursor.toneFrame += frames;
        frameCount -= frames;
    }
}

void feedSilence(LoudnessAnalyzer& analyzer, StreamCursor& cursor, std::uint64_t frameCount,
    const std::size_t maximumChunkFrames = 2'048)
{
    std::vector<float> silence(maximumChunkFrames, 0.0F);
    while (frameCount != 0) {
        const auto frames
            = static_cast<std::size_t>(std::min<std::uint64_t>(frameCount, maximumChunkFrames));
        const auto* rightInput = cursor.channelCount == 2 ? silence.data() : nullptr;
        const auto result = cursor.feed(analyzer, silence.data(), rightInput, frames);
        juce::ignoreUnused(result);
        cursor.toneFrame += frames;
        frameCount -= frames;
    }
}

void feedConstant(LoudnessAnalyzer& analyzer, StreamCursor& cursor, std::uint64_t frameCount,
    const float value, const std::size_t maximumChunkFrames = 2'048)
{
    std::vector<float> samples(maximumChunkFrames, value);
    while (frameCount != 0) {
        const auto frames
            = static_cast<std::size_t>(std::min<std::uint64_t>(frameCount, maximumChunkFrames));
        const auto* rightInput = cursor.channelCount == 2 ? samples.data() : nullptr;
        const auto result = cursor.feed(analyzer, samples.data(), rightInput, frames);
        juce::ignoreUnused(result);
        cursor.toneFrame += frames;
        frameCount -= frames;
    }
}

class LoudnessAnalyzerTests final : public juce::UnitTest {
public:
    LoudnessAnalyzerTests() : UnitTest("Loudness analyzer", "audio-insight")
    {
    }

    void runTest() override
    {
        testPublishedCoefficientsAndAlignmentTone();
        testExactGateReducer();
        testIndexedGateEquivalence();
        testIndexedGateStructureAndCapacity();
        testStartupValidityAndCadence();
        testExactMonoSummation();
        testArbitraryRateAndPartitionInvariance();
        testScopedResets();
        testIntegrationResetExcludesPriorSamples();
        testLiveClearPreservesIntegration();
        testPublishedRelativeGateSequence();
        testAbsoluteGateAndCapacityBound();
        testConcurrentStatisticsRead();
    }

private:
    void expectMatchingGateResults(const LoudnessAnalyzer::IntegratedGateResult& indexed,
        const LoudnessAnalyzer::IntegratedGateResult& bruteForce,
        const double loudnessTolerance = 1.0e-10)
    {
        expect(indexed.absoluteGatedBlockCount == bruteForce.absoluteGatedBlockCount);
        expect(indexed.relativeGatedBlockCount == bruteForce.relativeGatedBlockCount);

        const auto expectMatchingLoudness
            = [this, loudnessTolerance](const double actual, const double expected) {
                  if (std::isfinite(expected))
                      expectWithinAbsoluteError(actual, expected, loudnessTolerance);
                  else
                      expect(std::isinf(actual) && std::signbit(actual) == std::signbit(expected));
              };
        expectMatchingLoudness(indexed.relativeGateLufs, bruteForce.relativeGateLufs);
        expectMatchingLoudness(indexed.integratedLufs, bruteForce.integratedLufs);
    }

    void testPublishedCoefficientsAndAlignmentTone()
    {
        beginTest("Published BS.1770-5 48 kHz K-weighting coefficients are reproduced");

        // ITU-R BS.1770-5 (11/2023), Annex 1, Tables 1 and 2 publish these
        // coefficients for the two K-weighting stages at 48 kHz.
        const auto coefficients = LoudnessAnalyzer::coefficientsForSampleRate(referenceSampleRate);
        expect(coefficients.valid);
        expectWithinAbsoluteError(coefficients.headFilter.b0, 1.53512485958697, 1.0e-13);
        expectWithinAbsoluteError(coefficients.headFilter.b1, -2.69169618940638, 1.0e-13);
        expectWithinAbsoluteError(coefficients.headFilter.b2, 1.19839281085285, 1.0e-13);
        expectWithinAbsoluteError(coefficients.headFilter.a1, -1.69065929318241, 1.0e-13);
        expectWithinAbsoluteError(coefficients.headFilter.a2, 0.73248077421585, 1.0e-13);
        expectWithinAbsoluteError(coefficients.highPassFilter.b0, 1.0, 0.0);
        expectWithinAbsoluteError(coefficients.highPassFilter.b1, -2.0, 0.0);
        expectWithinAbsoluteError(coefficients.highPassFilter.b2, 1.0, 0.0);
        expectWithinAbsoluteError(coefficients.highPassFilter.a1, -1.99004745483398, 1.0e-13);
        expectWithinAbsoluteError(coefficients.highPassFilter.a2, 0.99007225036621, 1.0e-13);
        expect(!LoudnessAnalyzer::coefficientsForSampleRate(0.0).valid);

        beginTest("EBU Tech 3341 stereo alignment tone meets M, S, and I tolerance");

        // EBU Tech 3341 (2016), section 6, Table 1, test case 1 specifies a
        // 20 s in-phase stereo 1 kHz sine at -23 dBFS peak per channel and
        // requires M, S, and I = -23.0 +/- 0.1 LUFS.
        LoudnessAnalyzer analyzer;
        StreamCursor cursor(referenceSampleRate, 2);
        feedTone(analyzer, cursor, 20 * static_cast<std::uint64_t>(referenceSampleRate), -23.0);

        const auto& output = analyzer.current();
        expect(output.momentaryValid);
        expect(output.shortTermValid);
        expect(output.integratedValid);
        expectWithinAbsoluteError(output.momentaryLufs, -23.0, 0.1);
        expectWithinAbsoluteError(output.shortTermLufs, -23.0, 0.1);
        expectWithinAbsoluteError(output.integratedLufs, -23.0, 0.1);
        expect(output.measurementCompletionCount == 200);
        expect(output.integrationBlockCount == 197);

        const auto statistics = analyzer.statistics();
        expect(statistics.measurementCompletions == 200);
        expect(statistics.integrationBlockCompletions == 197);
        expect(statistics.absoluteGatedBlocks == 197);
        expect(statistics.relativeGatedBlocks == 197);
        expect(statistics.integrationIndexReservedBytes < 8ULL * 1024ULL * 1024ULL);
        expect(statistics.integrationIndexLeafNodes == 1);
        expect(statistics.integrationIndexInternalNodes == 0);
        expect(statistics.integrationIndexTreeHeight == 1);
        expect(statistics.integrationIndexQueries == 197);
        expect(statistics.integrationIndexLastNodeVisits == 1);
        expect(statistics.integrationIndexMaximumNodeVisits == 1);
        expect(statistics.integrationIndexLastAggregateReads == 0);
        expect(statistics.integrationIndexMaximumAggregateReads == 0);
        expect(statistics.integrationIndexLastBoundaryValueReads <= 197);
        expect(statistics.integrationIndexMaximumBoundaryValueReads <= 197);
    }

    void testExactGateReducer()
    {
        beginTest("Integrated reducer applies the two gates once without iteration");

        // ITU-R BS.1770-5 Annex 1 equations (5)-(7) first form the
        // absolute-gated mean, subtract exactly 10 LU once, then apply both
        // strict gates. These three block levels retain 0 and -13 LUFS and
        // produce the analytically calculated result below.
        const std::array nonIterativeBlocks {
            energyForLufs(0.0),
            energyForLufs(-13.0),
            energyForLufs(-20.0),
        };
        const auto nonIterative
            = LoudnessAnalyzer::reduceIntegratedBlockEnergies(nonIterativeBlocks);
        expect(nonIterative.absoluteGatedBlockCount == 3);
        expect(nonIterative.relativeGatedBlockCount == 2);
        expectWithinAbsoluteError(nonIterative.integratedLufs, -2.7979159374972604, 1.0e-12);
        expectWithinAbsoluteError(nonIterative.relativeGateLufs, -14.51766749819016, 1.0e-12);

        beginTest("A block exactly equal to the relative gate is excluded");
        // mean(19, 1) / 10 is exactly 1 in linear energy. The strict `>`
        // comparison must therefore retain only 19, not the equal-energy 1.
        constexpr std::array<double, 2> relativeEquality { 19.0, 1.0 };
        const auto strictRelative
            = LoudnessAnalyzer::reduceIntegratedBlockEnergies(relativeEquality);
        expect(strictRelative.absoluteGatedBlockCount == 2);
        expect(strictRelative.relativeGatedBlockCount == 1);
        expectWithinAbsoluteError(
            strictRelative.relativeGateLufs, LoudnessAnalyzer::loudnessOffsetLufs, 1.0e-14);
        expectWithinAbsoluteError(strictRelative.integratedLufs,
            LoudnessAnalyzer::loudnessOffsetLufs + (10.0 * std::log10(19.0)), 1.0e-14);

        beginTest("A block exactly equal to the absolute gate is excluded");
        const auto absoluteThreshold = energyForLufs(LoudnessAnalyzer::absoluteGateLufs);
        const std::array absoluteEquality {
            absoluteThreshold,
            std::nextafter(absoluteThreshold, std::numeric_limits<double>::infinity()),
            0.0,
        };
        const auto strictAbsolute
            = LoudnessAnalyzer::reduceIntegratedBlockEnergies(absoluteEquality);
        expect(strictAbsolute.absoluteGatedBlockCount == 1);
        expect(strictAbsolute.relativeGatedBlockCount == 1);

        beginTest("Exact members near one former quantization bin all pass when required");
        std::array<double, 100> closeBlocks { };
        closeBlocks.fill(energyForLufs(-20.01));
        closeBlocks.back() = energyForLufs(9.525);
        const auto closeResult = LoudnessAnalyzer::reduceIntegratedBlockEnergies(closeBlocks);
        expect(closeResult.absoluteGatedBlockCount == closeBlocks.size());
        expect(closeResult.relativeGatedBlockCount == closeBlocks.size());
        expectWithinAbsoluteError(closeResult.integratedLufs, -10.02103382503129, 1.0e-12);

        beginTest("Exact gating has no artificial upper-LUFS ceiling");
        const std::array extremeBlock { energyForLufs(150.0) };
        const auto extreme = LoudnessAnalyzer::reduceIntegratedBlockEnergies(extremeBlock);
        expect(extreme.absoluteGatedBlockCount == 1);
        expect(extreme.relativeGatedBlockCount == 1);
        expectWithinAbsoluteError(extreme.integratedLufs, 150.0, 1.0e-12);
    }

    void testIndexedGateEquivalence()
    {
        beginTest("Indexed Integrated gating matches randomized brute-force histories");
        std::mt19937_64 generator(0x1770'5EB0'0129ULL);
        std::uniform_int_distribution<std::size_t> historyLength(1, 4'096);
        std::uniform_real_distribution<double> loudness(-100.0, 180.0);
        for (auto iteration = 0; iteration < 64; ++iteration) {
            std::vector<double> history(historyLength(generator));
            for (auto index = std::size_t { 0 }; index < history.size(); ++index) {
                history[index] = energyForLufs(loudness(generator));
                if (index % 127 == 0)
                    history[index] = energyForLufs(LoudnessAnalyzer::absoluteGateLufs);
                else if (index % 211 == 0)
                    history[index] = std::numeric_limits<double>::quiet_NaN();
                else if (index % 307 == 0)
                    history[index] = std::numeric_limits<double>::infinity();
                else if (index % 401 == 0)
                    history[index] = -1.0;
            }

            const auto indexed = evaluateIndexedGate(history);
            const auto bruteForce = LoudnessAnalyzer::reduceIntegratedBlockEnergies(history);
            expect(indexed.insertedEveryValue);
            expectMatchingGateResults(indexed.result, bruteForce);
            expect(indexed.statistics.lastQueryBoundaryValueReads
                <= IntegratedLoudnessIndex::leafValueCapacity);
        }

        beginTest("Indexed gates preserve strict threshold equality and exact extremes");
        const auto absoluteThreshold = energyForLufs(LoudnessAnalyzer::absoluteGateLufs);
        const std::array thresholdEdges {
            absoluteThreshold,
            std::nextafter(absoluteThreshold, std::numeric_limits<double>::infinity()),
            19.0,
            1.0,
            energyForLufs(150.0),
        };
        auto indexed = evaluateIndexedGate(thresholdEdges);
        auto bruteForce = LoudnessAnalyzer::reduceIntegratedBlockEnergies(thresholdEdges);
        expect(indexed.insertedEveryValue);
        expectMatchingGateResults(indexed.result, bruteForce);

        constexpr std::array<double, 2> relativeEquality { 19.0, 1.0 };
        indexed = evaluateIndexedGate(relativeEquality);
        bruteForce = LoudnessAnalyzer::reduceIntegratedBlockEnergies(relativeEquality);
        expect(indexed.result.relativeGatedBlockCount == 1);
        expectMatchingGateResults(indexed.result, bruteForce, 1.0e-14);

        std::array<double, 100> formerQuantizationCounterexample { };
        formerQuantizationCounterexample.fill(energyForLufs(-20.01));
        formerQuantizationCounterexample.back() = energyForLufs(9.525);
        indexed = evaluateIndexedGate(formerQuantizationCounterexample);
        bruteForce
            = LoudnessAnalyzer::reduceIntegratedBlockEnergies(formerQuantizationCounterexample);
        expect(indexed.result.relativeGatedBlockCount == formerQuantizationCounterexample.size());
        expectMatchingGateResults(indexed.result, bruteForce);

        beginTest("Ascending, descending, duplicate, and alternating orders remain equivalent");
        std::vector<double> ascending(32'768);
        for (auto index = std::size_t { 0 }; index < ascending.size(); ++index) {
            ascending[index]
                = energyForLufs(-69.0 + (120.0 * static_cast<double>(index) / ascending.size()));
        }
        auto descending = ascending;
        std::reverse(descending.begin(), descending.end());

        std::vector<double> duplicates(ascending.size());
        const std::array duplicateValues {
            energyForLufs(-69.0),
            energyForLufs(-40.0),
            energyForLufs(-23.0),
            energyForLufs(-13.0),
            energyForLufs(-3.0),
        };
        for (auto index = std::size_t { 0 }; index < duplicates.size(); ++index)
            duplicates[index] = duplicateValues[index % duplicateValues.size()];

        std::vector<double> alternating;
        alternating.reserve(ascending.size());
        for (auto index = std::size_t { 0 }; index < ascending.size() / 2; ++index) {
            alternating.push_back(ascending[index]);
            alternating.push_back(ascending[ascending.size() - 1 - index]);
        }

        for (const auto* history : { &ascending, &descending, &duplicates, &alternating }) {
            indexed = evaluateIndexedGate(*history);
            bruteForce = LoudnessAnalyzer::reduceIntegratedBlockEnergies(*history);
            expect(indexed.insertedEveryValue);
            expectMatchingGateResults(indexed.result, bruteForce);
        }
    }

    void testIndexedGateStructureAndCapacity()
    {
        beginTest("The full 24-hour index stays bounded in time and reserved memory");
        IntegratedLoudnessIndex index(
            static_cast<std::size_t>(LoudnessAnalyzer::integrationBlockCapacity));
        const auto initial = index.statistics();
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
        // Lock the accepted reference arm64 ABI footprint while keeping the
        // portable arena ceiling below for other architectures.
        expect(initial.reservedBytes == 7'606'712);
#endif
        expect(initial.reservedBytes < 8ULL * 1024ULL * 1024ULL);
        expect(initial.leafNodeCount == 1);
        expect(initial.internalNodeCount == 0);

        auto expectedSum = 0.0;
        auto expectedCount = std::uint64_t { 0 };
        constexpr auto threshold = 1.4321;
        auto insertedEveryBlock = true;
        for (auto block = std::uint64_t { 0 }; block < LoudnessAnalyzer::integrationBlockCapacity;
            ++block) {
            const auto value = 1.0 + (static_cast<double>(block) * 1.0e-6);
            insertedEveryBlock = index.insert(value) && insertedEveryBlock;
            if (value > threshold) {
                expectedSum += value;
                ++expectedCount;
            }
        }
        expect(insertedEveryBlock);
        const auto beforeRejectedInsert = index.queryGreaterThan(threshold);
        const auto structureBeforeRejectedInsert = index.statistics();
        expect(!index.insert(2.0));

        const auto reduced = index.queryGreaterThan(threshold);
        const auto full = index.statistics();
        expect(reduced.count == beforeRejectedInsert.count);
        expectWithinAbsoluteError(reduced.sum, beforeRejectedInsert.sum, 0.0);
        expect(full.valueCount == structureBeforeRejectedInsert.valueCount);
        expect(full.leafNodeCount == structureBeforeRejectedInsert.leafNodeCount);
        expect(full.internalNodeCount == structureBeforeRejectedInsert.internalNodeCount);
        expect(full.treeHeight == structureBeforeRejectedInsert.treeHeight);
        expect(reduced.count == expectedCount);
        expectWithinAbsoluteError(
            reduced.sum, expectedSum, std::numeric_limits<double>::epsilon() * expectedSum * 32.0);
        expect(full.valueCount == LoudnessAnalyzer::integrationBlockCapacity);
        expect(full.reservedBytes == initial.reservedBytes);
        expect(full.reservedBytes < 8ULL * 1024ULL * 1024ULL);
        expect(full.leafNodeCount <= full.leafNodeCapacity);
        expect(full.internalNodeCount <= full.internalNodeCapacity);
        expect(full.treeHeight <= 4);
        expect(full.lastQueryNodeVisits == full.treeHeight);
        expect(full.lastQueryAggregateReads
            <= IntegratedLoudnessIndex::internalChildCapacity * (full.treeHeight - 1));
        expect(full.lastQueryBoundaryValueReads <= IntegratedLoudnessIndex::leafValueCapacity);

        beginTest("Reset retains the arena and clears every indexed value");
        index.clear();
        const auto cleared = index.statistics();
        expect(cleared.valueCount == 0);
        expect(cleared.queryCount == 0);
        expect(cleared.leafNodeCount == 1);
        expect(cleared.internalNodeCount == 0);
        expect(cleared.treeHeight == 1);
        expect(cleared.leafNodeCapacity == initial.leafNodeCapacity);
        expect(cleared.internalNodeCapacity == initial.internalNodeCapacity);
        expect(cleared.reservedBytes == initial.reservedBytes);
        expect(index.insert(3.0));
        expect(index.insert(3.0));
        const auto duplicateQuery = index.queryGreaterThan(3.0);
        expect(duplicateQuery.count == 0);
    }

    void testStartupValidityAndCadence()
    {
        beginTest("Readings remain unavailable until each literal window is complete");
        LoudnessAnalyzer analyzer;
        StreamCursor cursor(referenceSampleRate, 2);
        constexpr auto periodFrames = std::uint64_t { 4'800 };

        feedSilence(analyzer, cursor, 3 * periodFrames);
        auto output = analyzer.current();
        expect(!output.momentaryValid);
        expect(!output.shortTermValid);
        expect(!output.integratedValid);
        expect(output.measurementCompletionCount == 3);

        feedSilence(analyzer, cursor, periodFrames);
        output = analyzer.current();
        expect(output.momentaryValid);
        expect(!output.shortTermValid);
        expect(output.integratedValid);
        expect(std::isinf(output.momentaryLufs) && output.momentaryLufs < 0.0);
        expect(std::isinf(output.integratedLufs) && output.integratedLufs < 0.0);
        expect(output.integrationBlockCount == 1);

        feedSilence(analyzer, cursor, 26 * periodFrames);
        output = analyzer.current();
        expect(output.shortTermValid);
        expect(std::isinf(output.shortTermLufs) && output.shortTermLufs < 0.0);
        expect(output.measurementCompletionCount == 30);
        expect(output.integrationBlockCount == 27);
    }

    void testExactMonoSummation()
    {
        beginTest("Mono is summed once and stereo power is independent of phase polarity");
        LoudnessAnalyzer mono;
        LoudnessAnalyzer inPhaseStereo;
        LoudnessAnalyzer antiPhaseStereo;
        StreamCursor monoCursor(referenceSampleRate, 1);
        StreamCursor inPhaseCursor(referenceSampleRate, 2);
        StreamCursor antiPhaseCursor(referenceSampleRate, 2);
        constexpr auto frames = std::uint64_t { 4 * 48'000 };

        // BS.1770-5 Annex 1 Notes 1/2 state that a 997 Hz, 0 dBFS-peak
        // sine in one unit-weight channel reads -3.01 LKFS. At -20 dBFS it
        // therefore reads -23.01, while two identical unit-weight channels
        // add 3.0103 dB.
        feedTone(mono, monoCursor, frames, -20.0, 997.0);
        feedTone(inPhaseStereo, inPhaseCursor, frames, -20.0, 997.0);
        feedTone(antiPhaseStereo, antiPhaseCursor, frames, -20.0, 997.0, 2'048, -1.0F);

        const auto& monoOutput = mono.current();
        const auto& inPhaseOutput = inPhaseStereo.current();
        const auto& antiPhaseOutput = antiPhaseStereo.current();
        expect(monoOutput.channelCount == 1);
        expect(monoOutput.shortTermValid && inPhaseOutput.shortTermValid
            && antiPhaseOutput.shortTermValid);
        expectWithinAbsoluteError(monoOutput.momentaryLufs, -23.01, 0.05);
        expectWithinAbsoluteError(
            inPhaseOutput.momentaryLufs - monoOutput.momentaryLufs, 3.0102999566, 0.001);
        expectWithinAbsoluteError(
            inPhaseOutput.integratedLufs - monoOutput.integratedLufs, 3.0102999566, 0.001);
        expectWithinAbsoluteError(
            antiPhaseOutput.momentaryLufs, inPhaseOutput.momentaryLufs, 1.0e-12);
        expectWithinAbsoluteError(
            antiPhaseOutput.integratedLufs, inPhaseOutput.integratedLufs, 1.0e-12);
    }

    void testArbitraryRateAndPartitionInvariance()
    {
        constexpr double unusualSampleRate = 44'117.0;
        constexpr auto unusualPeriodFrames = std::uint64_t { 4'412 };

        beginTest("Fractional cadence has exact cumulative boundaries with no long-run drift");
        expect(LoudnessAnalyzer::measurementBoundaryFrameForTesting(unusualSampleRate, 1) == 4'412);
        expect(
            LoudnessAnalyzer::measurementBoundaryFrameForTesting(unusualSampleRate, 4) == 17'647);
        expect(
            LoudnessAnalyzer::measurementBoundaryFrameForTesting(unusualSampleRate, 30) == 132'351);
        expect(LoudnessAnalyzer::measurementBoundaryFrameForTesting(unusualSampleRate, 36'000)
            == 158'821'200);

        const auto beyondSignedCompletion
            = (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 4'800) + 1;
        const auto beyondSignedBoundary = LoudnessAnalyzer::measurementBoundaryFrameForTesting(
            referenceSampleRate, beyondSignedCompletion);
        expect(beyondSignedBoundary
            > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        expect(beyondSignedBoundary < std::numeric_limits<std::uint64_t>::max());

        auto accumulatedFrames = std::uint64_t { 0 };
        auto previousBoundary = std::uint64_t { 0 };
        auto allHopSizesValid = true;
        for (std::uint64_t completion = 1; completion <= 36'000; ++completion) {
            const auto boundary = LoudnessAnalyzer::measurementBoundaryFrameForTesting(
                unusualSampleRate, completion);
            const auto hopFrames = boundary - previousBoundary;
            allHopSizesValid = allHopSizesValid && (hopFrames == 4'411 || hopFrames == 4'412);
            accumulatedFrames += hopFrames;
            previousBoundary = boundary;
        }
        expect(allHopSizesValid);
        expect(accumulatedFrames == 158'821'200);

        beginTest("Fractional hops trim and extend exact nonstationary 400 ms windows");
        const auto verifyImpulseWindow
            = [this](const double sampleRate, const std::uint64_t completion,
                  const std::size_t impulseFrame) {
                  const auto frameEnd = static_cast<std::size_t>(
                      LoudnessAnalyzer::measurementBoundaryFrameForTesting(sampleRate, completion));
                  const auto windowFrames = static_cast<std::size_t>(
                      LoudnessAnalyzer::measurementBoundaryFrameForTesting(sampleRate, 4));
                  std::vector<float> samples(frameEnd, 0.0F);
                  samples[impulseFrame] = 0.5F;
                  const auto energies = referenceMonoEnergies(samples, sampleRate);

                  LoudnessAnalyzer analyzer;
                  StreamCursor cursor(sampleRate, 1);
                  expect(cursor.feed(analyzer, samples.data(), nullptr, samples.size()).accepted);
                  expect(analyzer.current().momentaryValid);
                  expectWithinAbsoluteError(analyzer.current().momentaryLufs,
                      literalWindowLufs(energies, frameEnd, windowFrames), 1.0e-10);
              };

        // At 44,111 Hz completion 5, the latest four rounded hops contain
        // 17,645 frames while the literal 400 ms window has 17,644. The
        // impulse is the one oldest sample that must be trimmed.
        verifyImpulseWindow(44'111.0, 5, 4'411);
        // At 44,117 Hz completion 9, four rounded hops contain 17,646 frames
        // while the literal window has 17,647. The impulse is the preceding
        // sample that must be extended into the window.
        verifyImpulseWindow(44'117.0, 9, 22'058);

        beginTest("Half-sample fractional rates share one rational cadence and window definition");
        const auto expectReadinessAtBoundary
            = [this](const double sampleRate, const std::uint64_t completion) {
                  LoudnessAnalyzer analyzer;
                  StreamCursor cursor(sampleRate, 1);
                  const auto boundary = LoudnessAnalyzer::measurementBoundaryFrameForTesting(
                      sampleRate, completion);
                  feedSilence(analyzer, cursor, boundary - 1, 2'048);

                  if (completion == 1) {
                      expect(analyzer.current().measurementCompletionCount == 0);
                  } else if (completion == 4) {
                      expect(!analyzer.current().momentaryValid);
                      expect(!analyzer.current().integratedValid);
                  } else if (completion == 30) {
                      expect(!analyzer.current().shortTermValid);
                  }

                  feedSilence(analyzer, cursor, 1, 1);
                  expect(analyzer.current().measurementCompletionCount == completion);
                  if (completion == 4) {
                      expect(analyzer.current().momentaryValid);
                      expect(analyzer.current().integratedValid);
                  } else if (completion == 30) {
                      expect(analyzer.current().shortTermValid);
                  }
              };

        expectReadinessAtBoundary(211'666.24999999997, 4);
        expectReadinessAtBoundary(342'510.8333333333, 30);
        expectReadinessAtBoundary(400'554.99999999994, 1);
        // This rate reproduces the old B30-versus-round(Fs*3) disagreement on
        // Apple arm64, where long double has binary64 precision.
        expectReadinessAtBoundary(3'364.4999999999995, 30);

        beginTest("Literal 400 ms and 3 s windows become valid on their nearest-sample endpoints");
        LoudnessAnalyzer boundaryAnalyzer;
        StreamCursor boundaryCursor(unusualSampleRate, 1);
        feedSilence(boundaryAnalyzer, boundaryCursor, 17'646, 733);
        expect(!boundaryAnalyzer.current().momentaryValid);
        expect(!boundaryAnalyzer.current().integratedValid);
        feedSilence(boundaryAnalyzer, boundaryCursor, 1, 1);
        expect(boundaryAnalyzer.current().momentaryValid);
        expect(boundaryAnalyzer.current().integratedValid);
        expect(boundaryAnalyzer.current().integratedCapturedFrameEnd == 17'647);

        feedSilence(boundaryAnalyzer, boundaryCursor, 132'350 - 17'647, 733);
        expect(!boundaryAnalyzer.current().shortTermValid);
        feedSilence(boundaryAnalyzer, boundaryCursor, 1, 1);
        expect(boundaryAnalyzer.current().shortTermValid);
        expect(boundaryAnalyzer.current().measurementCapturedFrameEnd == 132'351);

        beginTest("K-weighting supports a non-table sample rate");
        LoudnessAnalyzer unusualRateAnalyzer;
        StreamCursor unusualCursor(unusualSampleRate, 1);
        feedTone(unusualRateAnalyzer, unusualCursor, 35 * unusualPeriodFrames, -20.0, 997.0, 733);
        const auto& unusualOutput = unusualRateAnalyzer.current();
        expect(unusualOutput.shortTermValid);
        expectWithinAbsoluteError(unusualOutput.momentaryLufs, -23.01, 0.08);
        expectWithinAbsoluteError(unusualOutput.integratedLufs, -23.01, 0.08);
        expect(unusualOutput.measurementCompletionCount == 35);

        beginTest("Identical samples are invariant to raw capture partitioning");
        constexpr std::size_t frameCount = 4 * 48'000;
        std::vector<float> left(frameCount);
        std::vector<float> right(frameCount);
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            const auto time = static_cast<double>(frame) / referenceSampleRate;
            left[frame] = static_cast<float>(0.17 * std::sin(2.0 * std::numbers::pi * 997.0 * time)
                + 0.04 * std::sin(2.0 * std::numbers::pi * 83.0 * time));
            right[frame] = static_cast<float>(0.11 * std::sin(2.0 * std::numbers::pi * 733.0 * time)
                - 0.03 * std::sin(2.0 * std::numbers::pi * 151.0 * time));
        }

        LoudnessAnalyzer wholeAnalyzer;
        StreamCursor wholeCursor(referenceSampleRate, 2);
        expect(wholeCursor.feed(wholeAnalyzer, left.data(), right.data(), frameCount).accepted);

        LoudnessAnalyzer splitAnalyzer;
        StreamCursor splitCursor(referenceSampleRate, 2);
        constexpr std::array<std::size_t, 8> partitionPattern {
            1,
            37,
            2'048,
            4'801,
            511,
            97,
            8'192,
            1'003,
        };
        auto offset = std::size_t { 0 };
        auto partition = std::size_t { 0 };
        while (offset < frameCount) {
            const auto frames = std::min(
                partitionPattern[partition % partitionPattern.size()], frameCount - offset);
            expect(
                splitCursor.feed(splitAnalyzer, left.data() + offset, right.data() + offset, frames)
                    .accepted);
            offset += frames;
            ++partition;
        }

        const auto& whole = wholeAnalyzer.current();
        const auto& split = splitAnalyzer.current();
        expect(whole.momentaryValid == split.momentaryValid);
        expect(whole.shortTermValid == split.shortTermValid);
        expect(whole.integratedValid == split.integratedValid);
        expectWithinAbsoluteError(whole.momentaryLufs, split.momentaryLufs, 1.0e-12);
        expectWithinAbsoluteError(whole.shortTermLufs, split.shortTermLufs, 1.0e-12);
        expectWithinAbsoluteError(whole.integratedLufs, split.integratedLufs, 1.0e-12);
        expect(whole.measurementCompletionCount == split.measurementCompletionCount);
        expect(whole.integrationBlockCount == split.integrationBlockCount);
        expect(whole.absoluteGatedBlockCount == split.absoluteGatedBlockCount);
        expect(whole.relativeGatedBlockCount == split.relativeGatedBlockCount);
    }

    void testScopedResets()
    {
        beginTest("Sequence discontinuity invalidates all temporal Loudness state immediately");
        LoudnessAnalyzer discontinuityAnalyzer;
        StreamCursor discontinuityCursor(referenceSampleRate, 2);
        feedTone(discontinuityAnalyzer, discontinuityCursor, 3 * 48'000, -23.0);
        expect(discontinuityAnalyzer.current().shortTermValid);
        const auto sequenceBeforeGap = discontinuityAnalyzer.current().stateSequence;
        ++discontinuityCursor.sequence;
        feedTone(discontinuityAnalyzer, discontinuityCursor, 4'800, -23.0);
        expect(!discontinuityAnalyzer.current().momentaryValid);
        expect(!discontinuityAnalyzer.current().shortTermValid);
        expect(!discontinuityAnalyzer.current().integratedValid);
        expect(discontinuityAnalyzer.current().stateSequence > sequenceBeforeGap);
        expect(discontinuityAnalyzer.statistics().discontinuityResets == 1);

        beginTest("Channel format and lifecycle generation changes perform full resets");
        LoudnessAnalyzer formatAnalyzer;
        StreamCursor formatCursor(referenceSampleRate, 2);
        feedTone(formatAnalyzer, formatCursor, 3 * 48'000, -23.0);
        formatCursor.channelCount = 1;
        feedTone(formatAnalyzer, formatCursor, 4'800, -20.0, 997.0);
        expect(!formatAnalyzer.current().momentaryValid);
        expect(formatAnalyzer.current().channelCount == 1);
        expect(formatAnalyzer.statistics().formatResets == 1);

        ++formatCursor.generation;
        feedTone(formatAnalyzer, formatCursor, 4'800, -20.0, 997.0);
        expect(!formatAnalyzer.current().momentaryValid);
        expect(formatAnalyzer.statistics().generationResets == 1);

        const auto stateBeforeExplicitReset = formatAnalyzer.current().stateSequence;
        formatAnalyzer.reset();
        expect(!formatAnalyzer.current().momentaryValid);
        expect(!formatAnalyzer.current().integratedValid);
        expect(formatAnalyzer.current().stateSequence > stateBeforeExplicitReset);
        expect(formatAnalyzer.statistics().explicitResets == 1);

        beginTest("Malformed input resets immediately and is rejected");
        LoudnessAnalyzer malformedAnalyzer;
        constexpr float sample = 0.0F;
        const CapturedStereoChunkView malformed {
            &sample,
            nullptr,
            1,
            1,
            1,
            1,
            referenceSampleRate,
            false,
            2,
        };
        const auto stateBeforeMalformed = malformedAnalyzer.current().stateSequence;
        const auto result = malformedAnalyzer.process(malformed);
        expect(!result.accepted);
        expect(malformedAnalyzer.current().stateSequence > stateBeforeMalformed);
        expect(malformedAnalyzer.statistics().invalidInputResets == 1);

        beginTest("Coordinator-classified reset APIs increment only their matching reasons");
        LoudnessAnalyzer classifiedAnalyzer;
        classifiedAnalyzer.resetForLifecycle();
        classifiedAnalyzer.resetForDiscontinuity();
        classifiedAnalyzer.resetForFormatChange();
        const auto classified = classifiedAnalyzer.statistics();
        expect(classified.fullResets == 3);
        expect(classified.generationResets == 1);
        expect(classified.discontinuityResets == 1);
        expect(classified.formatResets == 1);
        expect(classified.explicitResets == 0);
    }

    void testIntegrationResetExcludesPriorSamples()
    {
        beginTest("Independent integration reset retains M/S and excludes every prior hop");
        LoudnessAnalyzer analyzer;
        StreamCursor cursor(referenceSampleRate, 2);
        feedTone(analyzer, cursor, 3 * 48'000, -10.0);
        const auto beforeReset = analyzer.current();
        expect(beforeReset.momentaryValid && beforeReset.shortTermValid
            && beforeReset.integratedValid);
        expectWithinAbsoluteError(beforeReset.integratedLufs, -10.0, 0.1);

        analyzer.resetIntegration();
        auto output = analyzer.current();
        expect(output.momentaryValid);
        expect(output.shortTermValid);
        expectWithinAbsoluteError(output.momentaryLufs, beforeReset.momentaryLufs, 0.0);
        expectWithinAbsoluteError(output.shortTermLufs, beforeReset.shortTermLufs, 0.0);
        expect(!output.integratedValid);
        expect(output.integrationBlockCount == 0);
        expect(output.stateSequence > beforeReset.stateSequence);

        constexpr auto integrationFrames = std::uint64_t { 19'200 };
        feedTone(analyzer, cursor, integrationFrames - 1, -30.0);
        expect(!analyzer.current().integratedValid);
        feedTone(analyzer, cursor, 1, -30.0);
        output = analyzer.current();
        expect(output.integratedValid);
        expect(output.integrationBlockCount == 1);
        expect(output.integratedCapturedFrameEnd == cursor.capturedFrameEnd);
        expectWithinAbsoluteError(output.integratedLufs, -30.0, 0.15);
        expect(analyzer.statistics().integrationResets == 1);
    }

    void testLiveClearPreservesIntegration()
    {
        beginTest("Stale live clear invalidates M/S but retains completed Integrated state");
        LoudnessAnalyzer analyzer;
        StreamCursor cursor(referenceSampleRate, 2);
        feedTone(analyzer, cursor, (3 * 48'000) + 2'400, -23.0);
        const auto beforeClear = analyzer.current();
        expect(beforeClear.momentaryValid && beforeClear.shortTermValid
            && beforeClear.integratedValid);

        analyzer.clearLiveMeasurementsPreservingIntegration();
        auto output = analyzer.current();
        expect(!output.momentaryValid);
        expect(!output.shortTermValid);
        expect(output.integratedValid);
        expectWithinAbsoluteError(output.integratedLufs, beforeClear.integratedLufs, 0.0);
        expect(output.integrationBlockCount == beforeClear.integrationBlockCount);
        expect(output.stateSequence > beforeClear.stateSequence);

        feedTone(analyzer, cursor, 19'199, -23.0);
        output = analyzer.current();
        expect(!output.momentaryValid);
        expect(!output.shortTermValid);
        expect(output.integrationBlockCount == beforeClear.integrationBlockCount);

        feedTone(analyzer, cursor, 1, -23.0);
        output = analyzer.current();
        expect(output.momentaryValid);
        expect(!output.shortTermValid);
        expect(output.integratedValid);
        expect(output.integrationBlockCount == beforeClear.integrationBlockCount + 1);
        expect(analyzer.statistics().liveMeasurementClears == 1);
    }

    void testPublishedRelativeGateSequence()
    {
        beginTest("EBU Tech 3341 relative-gating sequence rejects low-level sections");

        // EBU Tech 3341 (2016), section 6, Table 1, test case 3 specifies
        // in-phase stereo 1 kHz tones: 10 s at -36 dBFS, 60 s at -23 dBFS,
        // then 10 s at -36 dBFS. Its required I result is -23.0 +/- 0.1 LUFS.
        LoudnessAnalyzer analyzer;
        StreamCursor cursor(referenceSampleRate, 2);
        feedTone(analyzer, cursor, 10 * 48'000, -36.0, referenceToneFrequency, 4'800);
        feedTone(analyzer, cursor, 60 * 48'000, -23.0, referenceToneFrequency, 4'800);
        feedTone(analyzer, cursor, 10 * 48'000, -36.0, referenceToneFrequency, 4'800);

        const auto& output = analyzer.current();
        expect(output.integratedValid);
        expectWithinAbsoluteError(output.integratedLufs, -23.0, 0.1);
        expect(output.relativeGatedBlockCount < output.absoluteGatedBlockCount);
        expect(output.relativeGateLufs > -35.0);
    }

    void testAbsoluteGateAndCapacityBound()
    {
        beginTest("The strict -70 LUFS absolute gate yields completed negative infinity");
        LoudnessAnalyzer quietAnalyzer;
        StreamCursor quietCursor(referenceSampleRate, 2);
        feedTone(quietAnalyzer, quietCursor, 48'000, -71.0);
        const auto& quiet = quietAnalyzer.current();
        expect(quiet.integratedValid);
        expect(std::isinf(quiet.integratedLufs) && quiet.integratedLufs < 0.0);
        expect(quiet.absoluteGatedBlockCount == 0);
        expect(quiet.relativeGatedBlockCount == 0);

        beginTest("Signals immediately below and above the absolute threshold separate");
        LoudnessAnalyzer belowThresholdAnalyzer;
        LoudnessAnalyzer aboveThresholdAnalyzer;
        StreamCursor belowThresholdCursor(referenceSampleRate, 2);
        StreamCursor aboveThresholdCursor(referenceSampleRate, 2);
        feedTone(belowThresholdAnalyzer, belowThresholdCursor, 4 * 48'000, -70.02, 997.0);
        feedTone(aboveThresholdAnalyzer, aboveThresholdCursor, 4 * 48'000, -69.98, 997.0);
        expect(belowThresholdAnalyzer.current().absoluteGatedBlockCount == 0);
        expect(aboveThresholdAnalyzer.current().absoluteGatedBlockCount != 0);
        expect(std::isfinite(aboveThresholdAnalyzer.current().integratedLufs));

        beginTest("Finite float extremes remain exact above +100 LUFS");
        LoudnessAnalyzer extremeAnalyzer;
        StreamCursor extremeCursor(referenceSampleRate, 2);
        feedConstant(extremeAnalyzer, extremeCursor, 19'200, std::numeric_limits<float>::max());
        const auto& extreme = extremeAnalyzer.current();
        expect(extreme.integratedValid);
        expect(std::isfinite(extreme.integratedLufs));
        expect(extreme.integratedLufs > 100.0);
        expect(extreme.integrationBlockCount == 1);
        expect(extreme.absoluteGatedBlockCount == 1);
        expect(extreme.relativeGatedBlockCount == 1);

        beginTest("The exact 24-hour capacity fails closed at its first excess block");
        expect(LoudnessAnalyzer::integrationBlockCapacity == 864'000);
        LoudnessAnalyzer capacityAnalyzer;
        expect(capacityAnalyzer.setIntegrationBlockCapacityForTesting(2));
        StreamCursor capacityCursor(referenceSampleRate, 2);
        feedTone(capacityAnalyzer, capacityCursor, 19'200, -23.0);
        expect(capacityAnalyzer.current().integratedValid);
        expect(capacityAnalyzer.current().integrationBlockCount == 1);
        feedTone(capacityAnalyzer, capacityCursor, 4'800, -23.0);
        expect(capacityAnalyzer.current().integratedValid);
        expect(capacityAnalyzer.current().integrationBlockCount == 2);
        feedTone(capacityAnalyzer, capacityCursor, 4'800, -23.0);
        auto capacityOutput = capacityAnalyzer.current();
        expect(capacityOutput.momentaryValid);
        expect(!capacityOutput.integratedValid);
        expect(capacityOutput.integrationCapacityExceeded);
        expect(capacityOutput.integrationBlockCapacity == 2);
        expect(capacityOutput.integrationBlockCount == 3);

        feedTone(capacityAnalyzer, capacityCursor, 4'800, -23.0);
        auto capacityStatistics = capacityAnalyzer.statistics();
        expect(capacityStatistics.integrationCapacityOverflows == 1);
        expect(capacityStatistics.integrationCapacityExceeded);
        expect(capacityStatistics.integrationBlockCapacity == 2);

        capacityAnalyzer.resetIntegration();
        capacityOutput = capacityAnalyzer.current();
        expect(!capacityOutput.integratedValid);
        expect(!capacityOutput.integrationCapacityExceeded);
        expect(capacityOutput.integrationBlockCapacity == 2);
        expect(capacityAnalyzer.statistics().integrationCapacityOverflows == 1);
    }

    void testConcurrentStatisticsRead()
    {
        beginTest("Telemetry statistics may be read concurrently with worker processing");
        LoudnessAnalyzer analyzer;
        StreamCursor cursor(referenceSampleRate, 2);
        constexpr std::size_t frameCount = 10 * 48'000;
        std::vector<float> samples(frameCount);
        for (std::size_t frame = 0; frame < samples.size(); ++frame) {
            samples[frame] = static_cast<float>(0.1
                * std::sin(2.0 * std::numbers::pi * referenceToneFrequency
                    * static_cast<double>(frame) / referenceSampleRate));
        }

        std::atomic<bool> stop { false };
        std::atomic<std::uint64_t> observedSequence { 0 };
        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto snapshot = analyzer.statistics();
                auto previous = observedSequence.load(std::memory_order_relaxed);
                while (snapshot.stateSequence > previous
                    && !observedSequence.compare_exchange_weak(previous, snapshot.stateSequence,
                        std::memory_order_relaxed, std::memory_order_relaxed)) { }
            }
        });

        expect(cursor.feed(analyzer, samples.data(), samples.data(), samples.size()).accepted);
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        const auto statistics = analyzer.statistics();
        expect(statistics.inputChunks == 1);
        expect(statistics.inputFrames == frameCount);
        expect(statistics.measurementCompletions == 100);
        expect(statistics.integrationBlockCompletions == 97);
        expect(observedSequence.load(std::memory_order_relaxed) <= statistics.stateSequence);
    }
};

static LoudnessAnalyzerTests loudnessAnalyzerTests;
} // namespace
} // namespace audio_insight
