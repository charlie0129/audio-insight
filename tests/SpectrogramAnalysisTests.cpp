// SPDX-License-Identifier: AGPL-3.0-or-later

#include "analysis/AnalysisCoordinator.h"
#include "analysis/SpectrogramColumnMapper.h"
#include "analysis/SpectrogramColumnQueue.h"
#include "analysis/SpectrumAnalyzer.h"
#include "analysis/SpectrumTransformSink.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <thread>
#include <utility>
#include <vector>

namespace audio_insight {
namespace {
using namespace std::chrono_literals;

struct RecordedTransform final {
    std::vector<float> powerBins;
    std::uint64_t captureGeneration = 0;
    std::uint64_t fftGeneration = 0;
    std::uint64_t rawTransformSequence = 0;
    std::uint64_t resetEpoch = 0;
    std::uint64_t capturedFrameEnd = 0;
    double sampleRate = 0.0;
    std::uint32_t fftSize = 0;
    std::uint32_t hopSizeFrames = 0;
    std::uint32_t requestedSliceRateHz = 0;
    std::uint32_t channelCount = 0;
};

class RecordingTransformSink final : public SpectrumTransformSink {
public:
    void consumeSpectrumTransform(const SpectrumTransformView& transform) noexcept override
    {
        try {
            RecordedTransform record;
            record.powerBins.assign(transform.powerBins,
                transform.powerBins + static_cast<std::ptrdiff_t>(transform.binCount));
            record.captureGeneration = transform.captureGeneration;
            record.fftGeneration = transform.fftGeneration;
            record.rawTransformSequence = transform.rawTransformSequence;
            record.resetEpoch = transform.resetEpoch;
            record.capturedFrameEnd = transform.capturedFrameEnd;
            record.sampleRate = transform.sampleRate;
            record.fftSize = transform.fftSize;
            record.hopSizeFrames = transform.hopSizeFrames;
            record.requestedSliceRateHz = transform.requestedSliceRateHz;
            record.channelCount = transform.channelCount;
            transforms.push_back(std::move(record));
        } catch (...) {
            allocationFailed = true;
        }
    }

    std::vector<RecordedTransform> transforms;
    bool allocationFailed = false;
};

[[nodiscard]] SpectrumTransformView makeTransformView(const float* const powerBins,
    const std::uint32_t fftSizeToUse, const double sampleRate,
    const std::uint64_t rawTransformSequence = 1) noexcept
{
    return { powerBins, (fftSizeToUse / 2U) + 1U, 7, 11, rawTransformSequence, 13, fftSizeToUse,
        sampleRate, fftSizeToUse, static_cast<std::uint32_t>(std::llround(sampleRate / 60.0)), 60,
        2 };
}

[[nodiscard]] std::size_t expectedUsableBinCount(
    const std::uint32_t fftSizeToUse, const double sampleRate) noexcept
{
    const auto binWidth = sampleRate / static_cast<double>(fftSizeToUse);
    const auto first = static_cast<std::size_t>(std::ceil(20.0 / binWidth));
    const auto last
        = static_cast<std::size_t>(std::floor(std::min(20'000.0, sampleRate * 0.5) / binWidth));
    return (last - first) + 1;
}

[[nodiscard]] double frequencyUnit(
    const double frequency, const double maximumFrequency, const double spacing) noexcept
{
    const auto linear = (frequency - 20.0) / (maximumFrequency - 20.0);
    const auto logarithmic = std::log(frequency / 20.0) / std::log(maximumFrequency / 20.0);
    return ((1.0 - spacing) * linear) + (spacing * logarithmic);
}

template <typename Predicate>
bool waitForTelemetry(AnalysisCoordinator& coordinator, Predicate&& predicate,
    const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        coordinator.requestAnalysis();
        if (predicate(coordinator.telemetry()))
            return true;

        std::this_thread::sleep_for(1ms);
    }

    return false;
}

[[nodiscard]] std::vector<SpectrogramColumn> drainSpectrogramColumns(
    AnalysisCoordinator& coordinator)
{
    std::vector<SpectrogramColumn> result;
    SpectrogramColumn column;
    while (coordinator.copyNextSpectrogramColumn(column))
        result.push_back(column);
    return result;
}

class SpectrumTransformSinkTests final : public juce::UnitTest {
public:
    SpectrumTransformSinkTests() : UnitTest("Spectrum transform sink", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Every transform in one process call reaches the synchronous raw sink");
        {
            constexpr auto configuredFftSize = std::size_t { 1024 };
            constexpr auto hopSize = std::size_t { 400 };
            constexpr auto transformCount = std::size_t { 4 };
            constexpr auto frameCount = configuredFftSize + ((transformCount - 1) * hopSize);
            constexpr auto sampleRate = 48'000.0;

            std::array<float, frameCount> samples { };
            for (std::size_t index = 0; index < samples.size(); ++index) {
                samples[index] = std::sin(static_cast<float>(2.0 * std::numbers::pi * 31.0
                    * static_cast<double>(index) / static_cast<double>(configuredFftSize)));
            }

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(analyzer.reconfigure(
                { configuredFftSize, FftWindow::rectangular, 120 }, 5, &frame));

            RecordingTransformSink sink;
            expect(analyzer.process({ samples.data(), nullptr, samples.size(), 9, 1, samples.size(),
                                        sampleRate, false, 1 },
                frame, &sink));
            expect(!sink.allocationFailed);
            expect(sink.transforms.size() == transformCount);
            expect(analyzer.statistics().transforms == transformCount);
            expect(frame.spectrumCapturedFrameEnd == samples.size());

            for (std::size_t index = 0; index < sink.transforms.size(); ++index) {
                const auto& transform = sink.transforms[index];
                expect(transform.rawTransformSequence == index + 1);
                expect(transform.capturedFrameEnd == configuredFftSize + (index * hopSize));
                expect(transform.captureGeneration == 9);
                expect(transform.fftGeneration == 5);
                expect(transform.fftSize == configuredFftSize);
                expect(transform.hopSizeFrames == hopSize);
                expect(transform.requestedSliceRateHz == 120);
                expect(transform.channelCount == 1);
                expect(transform.powerBins.size() == (configuredFftSize / 2) + 1);
            }
        }

        beginTest("Raw power is unsmoothed and survives only Spectrum-temporal resets");
        {
            constexpr auto configuredFftSize = std::size_t { 1024 };
            constexpr auto sampleRate = 15'360.0;
            std::array<float, configuredFftSize> fullScaleDc { };
            std::array<float, configuredFftSize> silence { };
            fullScaleDc.fill(1.0F);

            SpectrumAnalyzer analyzer;
            VisualizationFrame frame;
            expect(
                analyzer.reconfigure({ configuredFftSize, FftWindow::rectangular, 15 }, 3, &frame));
            expect(analyzer.reconfigureTemporal(
                { 0.0, 1'000.0, SpectrumPeakHoldMode::off, 2.0 }, &frame));

            RecordingTransformSink sink;
            expect(analyzer.process({ fullScaleDc.data(), nullptr, fullScaleDc.size(), 4, 1,
                                        configuredFftSize, sampleRate, false, 1 },
                frame, &sink));
            expect(analyzer.process({ silence.data(), nullptr, silence.size(), 4, 2,
                                        configuredFftSize * 2, sampleRate, false, 1 },
                frame, &sink));
            expect(sink.transforms.size() == 2);
            expectWithinAbsoluteError(sink.transforms[0].powerBins[0], 1.0F, 0.0001F);
            expectWithinAbsoluteError(sink.transforms[1].powerBins[0], 0.0F, 0.0001F);

            const auto expectedAveragedPower = std::exp(-(1.0 / 15.0));
            expectWithinAbsoluteError(frame.spectrumDecibels[0],
                static_cast<float>(10.0 * std::log10(expectedAveragedPower)), 0.001F);
            expect(frame.spectrumDecibels[0] > minimumSpectrumDecibels);

            const auto cachedSequence = sink.transforms.back().rawTransformSequence;
            const auto cachedResetEpoch = sink.transforms.back().resetEpoch;
            expect(
                analyzer.reconfigureTemporal({ 0.0, 0.0, SpectrumPeakHoldMode::off, 2.0 }, &frame));
            expect(analyzer.emitLatestRawTransform(sink));
            expect(sink.transforms.back().rawTransformSequence == cachedSequence);
            expect(sink.transforms.back().resetEpoch == cachedResetEpoch);
            expectWithinAbsoluteError(sink.transforms.back().powerBins[0], 0.0F, 0.0001F);

            analyzer.clearTemporalState(&frame);
            expect(analyzer.emitLatestRawTransform(sink));
            expect(sink.transforms.back().rawTransformSequence == cachedSequence);
            expect(sink.transforms.back().resetEpoch == cachedResetEpoch);

            analyzer.reset(&frame);
            expect(analyzer.resetEpoch() > cachedResetEpoch);
            expect(!analyzer.emitLatestRawTransform(sink));
        }
    }
};

class SpectrogramColumnMapperTests final : public juce::UnitTest {
public:
    SpectrogramColumnMapperTests() : UnitTest("Spectrogram column mapper", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Row count is bounded by usable bins and the 1024-row limit");
        {
            std::array<float, maximumSpectrumBinCount> power { };
            power.fill(1.0e-18F);

            constexpr std::array cases {
                std::pair { std::uint32_t { 1024 }, 48'000.0 },
                std::pair { std::uint32_t { 4096 }, 48'000.0 },
                std::pair { std::uint32_t { 1024 }, 32'000.0 },
            };

            auto sequence = std::uint64_t { 1 };
            for (const auto& [fftSizeToUse, sampleRate] : cases) {
                SpectrogramColumnMapper mapper;
                SpectrogramColumn column;
                const auto transform = makeTransformView(power.data(), fftSizeToUse, sampleRate);
                expect(mapper.map(transform, sequence++, false, column));
                expect(column.rowCount
                    == std::min<std::size_t>(maximumSpectrogramRowCount,
                        expectedUsableBinCount(fftSizeToUse, sampleRate)));
                expect(column.binCount == (fftSizeToUse / 2U) + 1U);
                expect(!column.resetMarker);
            }
        }

        beginTest("Mapping clears a capture-boundary tag on a reused destination");
        {
            constexpr auto fftSizeToUse = std::uint32_t { 1024 };
            std::array<float, maximumSpectrumBinCount> power { };
            power.fill(1.0e-18F);

            SpectrogramColumnMapper mapper;
            expectWithinAbsoluteError(mapper.frequencySpacing(), 0.8, 1.0e-12);
            SpectrogramColumn column;
            column.captureBoundary = true;
            expect(mapper.map(
                makeTransformView(power.data(), fftSizeToUse, 48'000.0), 1, false, column));
            expect(!column.captureBoundary);
        }

        beginTest("Linear, intermediate, and logarithmic spacing use uniform shared-u rows");
        {
            constexpr auto fftSizeToUse = std::uint32_t { 4096 };
            constexpr auto sampleRate = 48'000.0;
            constexpr auto maximumFrequency = 20'000.0;
            constexpr auto targetFrequency = 1'000.0;
            const auto binWidth = sampleRate / static_cast<double>(fftSizeToUse);
            const auto targetBin
                = static_cast<std::size_t>(std::llround(targetFrequency / binWidth));
            const auto binFrequency = static_cast<double>(targetBin) * binWidth;

            std::array<float, maximumSpectrumBinCount> power { };
            power.fill(1.0e-18F);
            power[targetBin] = 1.0F;

            SpectrogramColumnMapper mapper;
            std::array<std::size_t, 3> targetRows { };
            constexpr std::array spacings { 0.0, 0.5, 1.0 };
            for (std::size_t index = 0; index < spacings.size(); ++index) {
                expect(mapper.setFrequencySpacing(spacings[index], index + 2));
                SpectrogramColumn column;
                expect(mapper.map(makeTransformView(power.data(), fftSizeToUse, sampleRate),
                    index + 1, false, column));

                const auto unit = frequencyUnit(binFrequency, maximumFrequency, spacings[index]);
                targetRows[index] = std::min<std::size_t>(column.rowCount - 1,
                    static_cast<std::size_t>(std::floor(unit * column.rowCount)));
                expectWithinAbsoluteError(column.decibels[targetRows[index]], 0.0F, 0.0001F);
                expect(column.mappingGeneration == index + 2);
            }

            expect(targetRows[0] < targetRows[1]);
            expect(targetRows[1] < targetRows[2]);
        }

        beginTest("The greatest calibrated bin power wins inside a row interval");
        {
            constexpr auto fftSizeToUse = std::uint32_t { 4096 };
            constexpr auto sampleRate = 48'000.0;
            const auto binWidth = sampleRate / static_cast<double>(fftSizeToUse);
            const auto rowCount = maximumSpectrogramRowCount;
            const auto firstBin = static_cast<std::size_t>(std::ceil(20.0 / binWidth));
            const auto lastBin = static_cast<std::size_t>(std::floor(20'000.0 / binWidth));

            auto lowerBin = std::size_t { 0 };
            auto upperBin = std::size_t { 0 };
            auto sharedRow = std::size_t { 0 };
            for (auto bin = firstBin; bin < lastBin; ++bin) {
                const auto firstUnit
                    = frequencyUnit(static_cast<double>(bin) * binWidth, 20'000.0, 0.0);
                const auto secondUnit
                    = frequencyUnit(static_cast<double>(bin + 1) * binWidth, 20'000.0, 0.0);
                const auto firstRow = std::min(
                    rowCount - 1, static_cast<std::size_t>(std::floor(firstUnit * rowCount)));
                const auto secondRow = std::min(
                    rowCount - 1, static_cast<std::size_t>(std::floor(secondUnit * rowCount)));
                if (firstRow == secondRow) {
                    lowerBin = bin;
                    upperBin = bin + 1;
                    sharedRow = firstRow;
                    break;
                }
            }
            expect(lowerBin != 0);

            std::array<float, maximumSpectrumBinCount> power { };
            power.fill(1.0e-18F);
            power[lowerBin] = 0.01F;
            power[upperBin] = 0.25F;

            SpectrogramColumnMapper mapper;
            expect(mapper.setFrequencySpacing(0.0, 2));
            SpectrogramColumn column;
            expect(mapper.map(
                makeTransformView(power.data(), fftSizeToUse, sampleRate), 1, false, column));
            expectWithinAbsoluteError(column.decibels[sharedRow], -6.0206F, 0.001F);
        }

        beginTest("An empty row interpolates power before conversion to decibels");
        {
            constexpr auto fftSizeToUse = std::uint32_t { 1024 };
            constexpr auto sampleRate = 48'000.0;
            const auto binWidth = sampleRate / static_cast<double>(fftSizeToUse);
            const auto rowCount = expectedUsableBinCount(fftSizeToUse, sampleRate);
            const auto firstBin = static_cast<std::size_t>(std::ceil(20.0 / binWidth));
            const auto lastBin = static_cast<std::size_t>(std::floor(20'000.0 / binWidth));

            std::array<bool, maximumSpectrogramRowCount> occupiedRows { };
            for (auto bin = firstBin; bin <= lastBin; ++bin) {
                const auto unit = frequencyUnit(static_cast<double>(bin) * binWidth, 20'000.0, 1.0);
                const auto row
                    = std::min(rowCount - 1, static_cast<std::size_t>(std::floor(unit * rowCount)));
                occupiedRows[row] = true;
            }

            auto targetRow = rowCount;
            auto interpolationLowerBin = std::size_t { 0 };
            auto interpolationUpperBin = std::size_t { 0 };
            auto interpolationWeight = 0.0;
            for (std::size_t row = 0; row < rowCount; ++row) {
                if (occupiedRows[row])
                    continue;

                const auto centreUnit
                    = (static_cast<double>(row) + 0.5) / static_cast<double>(rowCount);
                const auto centreFrequency
                    = 20.0 * std::exp(centreUnit * std::log(20'000.0 / 20.0));
                const auto fractionalBin = centreFrequency / binWidth;
                const auto lower = static_cast<std::size_t>(std::floor(fractionalBin));
                const auto upper = static_cast<std::size_t>(std::ceil(fractionalBin));
                const auto weight = fractionalBin - static_cast<double>(lower);
                if (lower != upper && weight > 0.2 && weight < 0.8) {
                    targetRow = row;
                    interpolationLowerBin = lower;
                    interpolationUpperBin = upper;
                    interpolationWeight = weight;
                    break;
                }
            }
            expect(targetRow != rowCount);

            std::array<float, maximumSpectrumBinCount> power { };
            power.fill(1.0e-18F);
            power[interpolationLowerBin] = 0.01F;
            power[interpolationUpperBin] = 1.0F;

            SpectrogramColumnMapper mapper;
            expect(mapper.setFrequencySpacing(1.0, 2));
            SpectrogramColumn column;
            expect(mapper.map(
                makeTransformView(power.data(), fftSizeToUse, sampleRate), 1, false, column));
            const auto interpolatedPower = 0.01 + (interpolationWeight * (1.0 - 0.01));
            const auto expectedDecibels = static_cast<float>(10.0 * std::log10(interpolatedPower));
            const auto incorrectlyInterpolatedDecibels
                = static_cast<float>((1.0 - interpolationWeight) * -20.0);
            expectWithinAbsoluteError(column.decibels[targetRow], expectedDecibels, 0.001F);
            expect(std::abs(column.decibels[targetRow] - incorrectlyInterpolatedDecibels) > 1.0F);
        }
    }
};

class SpectrogramColumnQueueTests final : public juce::UnitTest {
public:
    SpectrogramColumnQueueTests() : UnitTest("Spectrogram column queue", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Overflow reclaims the oldest ready column and preserves FIFO order");
        {
            SpectrogramColumnQueue queue;
            SpectrogramColumn source;
            for (std::uint64_t sequence = 1; sequence <= SpectrogramColumnQueue::capacity;
                ++sequence) {
                source.sequence = sequence;
                source.decibels[0] = static_cast<float>(sequence);
                const auto publication = queue.publish(source);
                expect(publication.published);
                expect(!publication.reclaimedOldestReady);
            }

            source.sequence = SpectrogramColumnQueue::capacity + 1;
            source.decibels[0] = static_cast<float>(source.sequence);
            const auto overflowPublication = queue.publish(source);
            expect(overflowPublication.published);
            expect(overflowPublication.reclaimedOldestReady);
            expect(!overflowPublication.droppedIncoming);

            SpectrogramColumn destination;
            for (std::uint64_t expectedSequence = 2;
                expectedSequence <= SpectrogramColumnQueue::capacity + 1; ++expectedSequence) {
                expect(queue.copyNext(destination));
                expect(destination.sequence == expectedSequence);
                expectWithinAbsoluteError(
                    destination.decibels[0], static_cast<float>(expectedSequence), 0.0001F);
            }
            expect(!queue.copyNext(destination));

            const auto telemetry = queue.telemetry();
            expect(telemetry.attemptedColumns == SpectrogramColumnQueue::capacity + 1);
            expect(telemetry.publishedColumns == SpectrogramColumnQueue::capacity + 1);
            expect(telemetry.reclaimedReadyColumns == 1);
            expect(telemetry.droppedIncomingColumns == 0);
            expect(telemetry.consumedColumns == SpectrogramColumnQueue::capacity);
            expect(telemetry.readyHighWaterMark == SpectrogramColumnQueue::capacity);
            expect(telemetry.readyColumns == 0);
        }

        beginTest("Discard retires only pending ready columns and reports the loss");
        {
            SpectrogramColumnQueue queue;
            SpectrogramColumn column;
            for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
                column.sequence = sequence;
                expect(queue.publish(column).published);
            }

            SpectrogramColumn consumed;
            expect(queue.copyNext(consumed));
            expect(consumed.sequence == 1);
            queue.discardPending();
            expect(!queue.copyNext(consumed));

            const auto telemetry = queue.telemetry();
            expect(telemetry.consumedColumns == 1);
            expect(telemetry.discardedReadyColumns == 2);
            expect(telemetry.readyColumns == 0);
        }

        beginTest("Selective discard preserves only the matching tagged reset marker");
        {
            constexpr auto boundaryGeneration = std::uint64_t { 42 };
            SpectrogramColumnQueue queue;
            SpectrogramColumn column;

            column.sequence = 1;
            column.captureGeneration = boundaryGeneration;
            column.resetMarker = true;
            column.captureBoundary = true;
            expect(queue.publish(column).published);

            column.sequence = 2;
            column.resetMarker = false;
            expect(queue.publish(column).published);

            column.sequence = 3;
            column.captureGeneration = boundaryGeneration + 1;
            column.resetMarker = true;
            expect(queue.publish(column).published);

            column.sequence = 4;
            column.captureGeneration = boundaryGeneration;
            column.captureBoundary = false;
            expect(queue.publish(column).published);

            queue.discardPendingExceptCaptureBoundary(boundaryGeneration);

            SpectrogramColumn preserved;
            expect(queue.copyNext(preserved));
            expect(preserved.sequence == 1);
            expect(preserved.captureGeneration == boundaryGeneration);
            expect(preserved.resetMarker);
            expect(preserved.captureBoundary);
            expect(!queue.copyNext(preserved));

            const auto telemetry = queue.telemetry();
            expect(telemetry.consumedColumns == 1);
            expect(telemetry.discardedReadyColumns == 3);
            expect(telemetry.readyColumns == 0);
        }

        beginTest("Overflow reclaims an ordinary column before a capture-boundary marker");
        {
            constexpr auto boundaryGeneration = std::uint64_t { 17 };
            SpectrogramColumnQueue queue;

            SpectrogramColumn marker;
            marker.sequence = 1;
            marker.captureGeneration = boundaryGeneration;
            marker.resetMarker = true;
            marker.captureBoundary = true;
            expect(queue.publish(marker).published);

            SpectrogramColumn ordinary;
            for (std::uint64_t sequence = 2; sequence <= SpectrogramColumnQueue::capacity;
                ++sequence) {
                ordinary.sequence = sequence;
                expect(queue.publish(ordinary).published);
            }

            ordinary.sequence = SpectrogramColumnQueue::capacity + 1;
            const auto overflowPublication = queue.publish(ordinary);
            expect(overflowPublication.published);
            expect(overflowPublication.reclaimedOldestReady);
            expect(!overflowPublication.droppedIncoming);

            SpectrogramColumn destination;
            expect(queue.copyNext(destination));
            expect(destination.sequence == marker.sequence);
            expect(destination.captureGeneration == boundaryGeneration);
            expect(destination.resetMarker);
            expect(destination.captureBoundary);

            for (std::uint64_t expectedSequence = 3;
                expectedSequence <= SpectrogramColumnQueue::capacity + 1; ++expectedSequence) {
                expect(queue.copyNext(destination));
                expect(destination.sequence == expectedSequence);
                expect(!destination.captureBoundary);
            }
            expect(!queue.copyNext(destination));

            const auto telemetry = queue.telemetry();
            expect(telemetry.reclaimedReadyColumns == 1);
            expect(telemetry.droppedIncomingColumns == 0);
            expect(telemetry.consumedColumns == SpectrogramColumnQueue::capacity);
            expect(telemetry.readyColumns == 0);
        }
    }
};

class SpectrogramCoordinatorTests final : public juce::UnitTest {
public:
    SpectrogramCoordinatorTests() : UnitTest("Spectrogram coordinator", "audio-insight")
    {
    }

    void runTest() override
    {
        testEveryFftBecomesAColumnAndTemporalChangesStayScoped();
        testControlMarkersWithoutAudio();
        testDiscontinuityClearsQueuedHistory();
    }

private:
    void expectAndDrainActivationMarker(AnalysisCoordinator& coordinator)
    {
        SpectrogramColumn marker;
        expect(coordinator.copyNextSpectrogramColumn(marker));
        expect(marker.resetMarker);
        expect(marker.sequence != 0);
        expect(marker.captureGeneration != 0);
        expect(marker.resetEpoch != 0);
        expect(marker.mappingGeneration != 0);
        expect(marker.rowCount == 0);
        expect(marker.rawTransformSequence == 0);
        static_cast<void>(drainSpectrogramColumns(coordinator));
    }

    void testEveryFftBecomesAColumnAndTemporalChangesStayScoped()
    {
        beginTest("One worker job publishes every FFT column and remaps cached raw power");
        AnalysisCoordinator coordinator;
        coordinator.setSpectrumAnalysisConfiguration({ 1024, FftWindow::rectangular, 120 });
        coordinator.setVisualizationActive(true);
        expectAndDrainActivationMarker(coordinator);

        std::array<float, 2048> signal { };
        for (std::size_t index = 0; index < signal.size(); ++index) {
            signal[index] = std::sin(static_cast<float>(
                2.0 * std::numbers::pi * 31.0 * static_cast<double>(index) / 1024.0));
        }

        const auto beforeAudio = coordinator.telemetry();
        coordinator.captureAudioBlock(signal.data(), signal.data(), signal.size(), 48'000.0, 2);
        expect(waitForTelemetry(coordinator, [beforeAudio](const AnalysisTelemetry& telemetry) {
            return telemetry.jobsCompleted > beforeAudio.jobsCompleted
                && telemetry.spectrogramColumnsMapped >= beforeAudio.spectrogramColumnsMapped + 3;
        }));

        const auto columns = drainSpectrogramColumns(coordinator);
        expect(columns.size() == 3);
        constexpr std::array<std::uint64_t, 3> expectedEndpoints { 1024, 1424, 1824 };
        for (std::size_t index = 0; index < columns.size(); ++index) {
            expect(!columns[index].resetMarker);
            expect(!columns[index].mappingSeed);
            expect(columns[index].capturedFrameEnd == expectedEndpoints[index]);
            expect(columns[index].rawTransformSequence != 0);
            expect(columns[index].rowCount == expectedUsableBinCount(1024, 48'000.0));
            if (index > 0) {
                expect(columns[index].sequence == columns[index - 1].sequence + 1);
                expect(columns[index].rawTransformSequence
                    == columns[index - 1].rawTransformSequence + 1);
            }
        }

        auto afterAudio = coordinator.telemetry();
        expect(afterAudio.lastJobSpectrumTransforms == 3);
        expect(afterAudio.spectrogramTransformsOffered
            == beforeAudio.spectrogramTransformsOffered + 3);
        expect(
            afterAudio.spectrogramColumnsPublished == beforeAudio.spectrogramColumnsPublished + 3);
        expect(afterAudio.spectrogramQueueReadyColumns == 0);

        VisualizationFrame latestFrame;
        auto foundLatestSpectrum = false;
        const auto frameDeadline = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < frameDeadline) {
            if (coordinator.copyLatestVisualizationFrame(latestFrame) && latestFrame.spectrumValid
                && latestFrame.spectrumCapturedFrameEnd == expectedEndpoints.back()) {
                foundLatestSpectrum = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        expect(foundLatestSpectrum);

        constexpr std::array<float, 400> nextHop { };
        const auto beforePreservedColumn = coordinator.telemetry();
        coordinator.captureAudioBlock(nextHop.data(), nextHop.data(), nextHop.size(), 48'000.0, 2);
        expect(
            waitForTelemetry(coordinator, [beforePreservedColumn](const AnalysisTelemetry& value) {
                return value.spectrogramColumnsMapped
                    > beforePreservedColumn.spectrogramColumnsMapped;
            }));

        const auto beforeTemporal = coordinator.telemetry();
        expect(beforeTemporal.spectrogramQueueReadyColumns == 1);
        const auto jobsBeforeTemporal = beforeTemporal.jobsCompleted;
        coordinator.setSpectrumTemporalConfiguration({ 0.0, 0.0, SpectrumPeakHoldMode::off, 2.0 });
        expect(waitForTelemetry(coordinator, [jobsBeforeTemporal](const AnalysisTelemetry& value) {
            return value.jobsCompleted > jobsBeforeTemporal;
        }));
        const auto afterTemporal = coordinator.telemetry();
        expect(afterTemporal.spectrogramQueueReadyColumns == 1);
        expect(afterTemporal.spectrogramColumnsDiscarded
            == beforeTemporal.spectrogramColumnsDiscarded);
        expect(afterTemporal.spectrogramTransformsOffered
            == beforeTemporal.spectrogramTransformsOffered);

        const auto jobsBeforeClear = afterTemporal.jobsCompleted;
        coordinator.resetSpectrum();
        expect(waitForTelemetry(coordinator, [jobsBeforeClear](const AnalysisTelemetry& value) {
            return value.jobsCompleted > jobsBeforeClear;
        }));
        const auto afterClear = coordinator.telemetry();
        expect(afterClear.spectrogramQueueReadyColumns == 1);
        expect(
            afterClear.spectrogramColumnsDiscarded == beforeTemporal.spectrogramColumnsDiscarded);
        expect(
            afterClear.spectrogramTransformsOffered == beforeTemporal.spectrogramTransformsOffered);

        SpectrogramColumn preservedColumn;
        expect(coordinator.copyNextSpectrogramColumn(preservedColumn));
        expect(!preservedColumn.resetMarker);
        expect(!preservedColumn.mappingSeed);
        expect(preservedColumn.capturedFrameEnd == 2224);
        expect(preservedColumn.rawTransformSequence == columns.back().rawTransformSequence + 1);
        expect(!coordinator.copyNextSpectrogramColumn(preservedColumn));

        const auto beforeRemap = coordinator.telemetry();
        const auto previousColumn = preservedColumn;
        coordinator.setSpectrogramFrequencySpacing(0.5);
        SpectrogramColumn remapped;
        expect(coordinator.copyNextSpectrogramColumn(remapped));
        expect(!remapped.resetMarker);
        expect(remapped.mappingSeed);
        expect(remapped.rawTransformSequence == previousColumn.rawTransformSequence);
        expect(remapped.capturedFrameEnd == previousColumn.capturedFrameEnd);
        expect(remapped.captureGeneration == previousColumn.captureGeneration);
        expect(remapped.fftGeneration == previousColumn.fftGeneration);
        expect(remapped.mappingGeneration > previousColumn.mappingGeneration);

        const auto afterRemap = coordinator.telemetry();
        expect(afterRemap.spectrumTransforms == beforeRemap.spectrumTransforms);
        expect(afterRemap.fftGeneration == beforeRemap.fftGeneration);
        expect(afterRemap.spectrogramMappingChanges == beforeRemap.spectrogramMappingChanges + 1);
        expect(afterRemap.spectrogramTransformsOffered
            == beforeRemap.spectrogramTransformsOffered + 1);
        expect(!coordinator.copyNextSpectrogramColumn(remapped));

        coordinator.setSpectrogramFrequencySpacing(0.5);
        expect(coordinator.telemetry().spectrogramMappingChanges
            == afterRemap.spectrogramMappingChanges);
        expect(!coordinator.copyNextSpectrogramColumn(remapped));
    }

    void testControlMarkersWithoutAudio()
    {
        beginTest("Reset and mapping markers clear history even when no FFT can seed it");
        AnalysisCoordinator coordinator;
        coordinator.setVisualizationActive(true);

        SpectrogramColumn activation;
        expect(coordinator.copyNextSpectrogramColumn(activation));
        expect(activation.resetMarker);
        const auto firstCaptureGeneration = activation.captureGeneration;
        const auto firstResetEpoch = activation.resetEpoch;

        coordinator.setSpectrogramFrequencySpacing(0.25);
        SpectrogramColumn remapMarker;
        expect(coordinator.copyNextSpectrogramColumn(remapMarker));
        expect(remapMarker.resetMarker);
        expect(remapMarker.captureGeneration == firstCaptureGeneration);
        expect(remapMarker.resetEpoch == firstResetEpoch);
        expect(remapMarker.mappingGeneration > activation.mappingGeneration);

        const auto jobsBeforeClear = coordinator.telemetry().jobsCompleted;
        coordinator.resetSpectrum();
        expect(waitForTelemetry(coordinator, [jobsBeforeClear](const AnalysisTelemetry& value) {
            return value.jobsCompleted > jobsBeforeClear;
        }));
        expect(drainSpectrogramColumns(coordinator).empty(),
            "Spectrum Clear incorrectly reset Spectrogram history");

        coordinator.setSpectrumAnalysisConfiguration({ 2048, FftWindow::periodicHann, 60 });
        SpectrogramColumn fftMarker;
        expect(coordinator.copyNextSpectrogramColumn(fftMarker));
        expect(fftMarker.resetMarker);
        expect(fftMarker.fftGeneration > remapMarker.fftGeneration);
        expect(fftMarker.resetEpoch > remapMarker.resetEpoch);

        coordinator.setVisualizationActive(false);
        SpectrogramColumn deactivationOutput;
        expect(!coordinator.copyNextSpectrogramColumn(deactivationOutput));
        expect(coordinator.telemetry().spectrogramQueueReadyColumns == 0);

        coordinator.setSpectrumAnalysisConfiguration(
            { 4096, FftWindow::fourTermBlackmanHarris, 30 });
        coordinator.setSpectrogramFrequencySpacing(0.75);
        expect(!coordinator.copyNextSpectrogramColumn(deactivationOutput));
        expect(coordinator.telemetry().spectrogramQueueReadyColumns == 0,
            "Inactive settings retained a pending Spectrogram control record");

        coordinator.setVisualizationActive(true);
        SpectrogramColumn reactivationMarker;
        expect(coordinator.copyNextSpectrogramColumn(reactivationMarker));
        expect(reactivationMarker.resetMarker);
        expect(reactivationMarker.captureGeneration > firstCaptureGeneration);
        expect(reactivationMarker.resetEpoch > fftMarker.resetEpoch);
        expect(reactivationMarker.fftGeneration > fftMarker.fftGeneration);
        expect(reactivationMarker.mappingGeneration > remapMarker.mappingGeneration);
        expect(!coordinator.copyNextSpectrogramColumn(reactivationMarker));
    }

    void testDiscontinuityClearsQueuedHistory()
    {
        beginTest("A raw-capture discontinuity discards incompatible queued columns");
        AnalysisCoordinator coordinator;
        coordinator.setSpectrumAnalysisConfiguration({ 1024, FftWindow::rectangular, 120 });
        coordinator.setVisualizationActive(true);
        expectAndDrainActivationMarker(coordinator);

        std::array<float, 2048> block { };
        block.fill(0.25F);
        const auto beforeWarmup = coordinator.telemetry();
        coordinator.captureAudioBlock(block.data(), block.data(), block.size(), 48'000.0, 2);
        expect(waitForTelemetry(coordinator, [beforeWarmup](const AnalysisTelemetry& value) {
            return value.spectrogramColumnsMapped >= beforeWarmup.spectrogramColumnsMapped + 3;
        }));

        SpectrogramColumn firstOldColumn;
        expect(coordinator.copyNextSpectrogramColumn(firstOldColumn));
        expect(!firstOldColumn.resetMarker);
        const auto oldResetEpoch = firstOldColumn.resetEpoch;
        const auto beforeGap = coordinator.telemetry();
        expect(beforeGap.spectrogramQueueReadyColumns == 2);

        for (std::size_t index = 0;
            index < StereoSampleCapture::bufferedFrameCapacity / block.size() + 2; ++index) {
            coordinator.captureAudioBlock(block.data(), block.data(), block.size(), 48'000.0, 2);
        }

        auto observedBoundary = false;
        auto observedBoundaryMarker = false;
        auto analyzedPostGapAudio = false;
        VisualizationFrame boundaryFrame;
        SpectrogramColumn boundaryMarker;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            coordinator.requestAnalysis();
            if (coordinator.copyLatestVisualizationFrame(boundaryFrame)
                && boundaryFrame.generation > firstOldColumn.captureGeneration
                && boundaryFrame.captureBoundary && !boundaryFrame.spectrumValid
                && !boundaryFrame.meterValid && !boundaryFrame.stereoFieldValid
                && !boundaryFrame.loudnessMomentaryValid && !boundaryFrame.loudnessShortTermValid
                && !boundaryFrame.loudnessIntegratedValid) {
                observedBoundary = true;
            }

            while (observedBoundary && !observedBoundaryMarker
                && coordinator.copyNextSpectrogramColumn(boundaryMarker)) {
                observedBoundaryMarker = boundaryMarker.captureBoundary
                    && boundaryMarker.resetMarker
                    && boundaryMarker.captureGeneration == boundaryFrame.generation;
            }

            const auto telemetry = coordinator.telemetry();
            if (observedBoundary && observedBoundaryMarker
                && telemetry.jobsCompleted > beforeGap.jobsCompleted
                && telemetry.spectrogramColumnsMapped > beforeGap.spectrogramColumnsMapped) {
                analyzedPostGapAudio = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        expect(observedBoundary);
        expect(observedBoundaryMarker);
        expect(analyzedPostGapAudio);

        const auto afterGapColumns = drainSpectrogramColumns(coordinator);
        expect(afterGapColumns.size() == 1);
        if (afterGapColumns.size() == 1) {
            expect(boundaryMarker.resetEpoch > oldResetEpoch);
            expect(boundaryMarker.captureGeneration > firstOldColumn.captureGeneration);
            expect(!afterGapColumns.front().resetMarker);
            expect(afterGapColumns.front().resetEpoch == boundaryMarker.resetEpoch);
            expect(afterGapColumns.front().captureGeneration == boundaryMarker.captureGeneration);
        }

        const auto afterGap = coordinator.telemetry();
        expect(afterGap.capture.reclaimedReadyChunks > 0);
        expect(afterGap.spectrogramColumnsDiscarded >= beforeGap.spectrogramColumnsDiscarded + 2);
    }
};

SpectrumTransformSinkTests spectrumTransformSinkTests;
SpectrogramColumnMapperTests spectrogramColumnMapperTests;
SpectrogramColumnQueueTests spectrogramColumnQueueTests;
SpectrogramCoordinatorTests spectrogramCoordinatorTests;
} // namespace
} // namespace audio_insight
