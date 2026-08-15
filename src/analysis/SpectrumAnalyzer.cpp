// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SpectrumAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace audio_insight {
namespace {
[[nodiscard]] bool sampleRatesDiffer(const double left, const double right) noexcept
{
    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}

[[nodiscard]] double windowCoefficient(const FftWindow window, const double phase) noexcept
{
    switch (window) {
    case FftWindow::rectangular:
        return 1.0;
    case FftWindow::periodicHann:
        return 0.5 - (0.5 * std::cos(phase));
    case FftWindow::fourTermBlackmanHarris:
        return 0.35875 - (0.48829 * std::cos(phase)) + (0.14128 * std::cos(2.0 * phase))
            - (0.01168 * std::cos(3.0 * phase));
    case FftWindow::fiveTermFlatTop:
        return 0.21557895 - (0.41663158 * std::cos(phase)) + (0.277263158 * std::cos(2.0 * phase))
            - (0.083578947 * std::cos(3.0 * phase)) + (0.006947368 * std::cos(4.0 * phase));
    }

    return 1.0;
}
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer()
{
    buildWindow();
}

bool SpectrumAnalyzer::reconfigure(const SpectrumAnalysisConfiguration& configuration,
    const std::uint64_t fftGeneration, VisualizationFrame* const destinationToInvalidate) noexcept
{
    if (!isSupportedConfiguration(configuration) || fftGeneration == 0)
        return false;

    if (configuration == configuration_ && fftGeneration == fftGeneration_)
        return true;

    configuration_ = configuration;
    fftGeneration_ = fftGeneration;
    configuredBinCount_ = (configuration_.fftSize / 2) + 1;
    buildWindow();
    resetTemporalState(ResetReason::configurationChange, destinationToInvalidate);
    hasPreviousChunk_ = false;
    previousGeneration_ = 0;
    previousChunkSequence_ = 0;
    previousCapturedFrameEnd_ = 0;
    previousChannelCount_ = 0;

    if (sampleRate_ > 0.0)
        configureSampleRate(sampleRate_);

    ++statistics_.configurationChanges;
    return true;
}

bool SpectrumAnalyzer::reconfigureTemporal(const SpectrumTemporalConfiguration& configuration,
    VisualizationFrame* const destinationToInvalidate) noexcept
{
    if (!isSupportedTemporalConfiguration(configuration))
        return false;

    if (configuration == temporalConfiguration_)
        return true;

    temporalConfiguration_ = configuration;
    resetSpectrumTemporalState(destinationToInvalidate);
    ++statistics_.temporalConfigurationChanges;
    return true;
}

bool SpectrumAnalyzer::process(const CapturedStereoChunkView& chunk,
    VisualizationFrame& destination, SpectrumTransformSink* const rawTransformSink) noexcept
{
    ++statistics_.inputChunks;

    const auto hasSupportedChannels = chunk.channelCount == 1 || chunk.channelCount == 2;
    if (chunk.frameCount == 0 || chunk.left == nullptr || !hasSupportedChannels
        || (chunk.channelCount == 2 && chunk.right == nullptr) || !std::isfinite(chunk.sampleRate)
        || chunk.sampleRate <= 0.0) {
        return false;
    }

    if (chunk.capturedFrameEnd < chunk.frameCount) {
        resetTemporalState(ResetReason::capturedFrameGap, &destination);
        hasPreviousChunk_ = false;
        previousGeneration_ = 0;
        previousChunkSequence_ = 0;
        previousCapturedFrameEnd_ = 0;
        previousChannelCount_ = 0;
        return false;
    }

    auto resetReason = ResetReason::explicitReset;
    auto needsReset = false;

    if (hasPreviousChunk_) {
        if (chunk.generation != previousGeneration_) {
            resetReason = ResetReason::generationChange;
            needsReset = true;
        } else if (chunk.sequence != previousChunkSequence_ + 1 || chunk.followsDiscontinuity) {
            resetReason = ResetReason::sequenceGap;
            needsReset = true;
        } else {
            const auto capturedFrameStart = chunk.capturedFrameEnd - chunk.frameCount;
            if (capturedFrameStart != previousCapturedFrameEnd_) {
                resetReason = ResetReason::capturedFrameGap;
                needsReset = true;
            } else if (sampleRatesDiffer(sampleRate_, chunk.sampleRate)
                || chunk.channelCount != previousChannelCount_) {
                resetReason = ResetReason::sampleRateChange;
                needsReset = true;
            }
        }
    }

    if (needsReset)
        resetTemporalState(resetReason, &destination);

    if (sampleRatesDiffer(sampleRate_, chunk.sampleRate))
        configureSampleRate(chunk.sampleRate);

    auto produced = false;
    const auto chunkFrameStart = chunk.capturedFrameEnd - chunk.frameCount;

    for (std::size_t frame = 0; frame < chunk.frameCount; ++frame) {
        leftRing_[writeIndex_] = std::isfinite(chunk.left[frame]) ? chunk.left[frame] : 0.0F;
        rightRing_[writeIndex_] = chunk.channelCount == 2 && std::isfinite(chunk.right[frame])
            ? chunk.right[frame]
            : 0.0F;
        writeIndex_ = (writeIndex_ + 1) % configuration_.fftSize;
        validSampleCount_ = std::min(validSampleCount_ + 1, configuration_.fftSize);
        ++samplesSinceTransform_;

        const auto firstWindowReady
            = !hasProducedSinceReset_ && validSampleCount_ == configuration_.fftSize;
        const auto nextHopReady = hasProducedSinceReset_ && samplesSinceTransform_ >= hopSize_;
        if (firstWindowReady || nextHopReady) {
            runTransform(chunk.generation, chunkFrameStart + frame + 1, chunk.channelCount,
                destination, rawTransformSink);
            produced = true;
        }
    }

    hasPreviousChunk_ = true;
    previousGeneration_ = chunk.generation;
    previousChunkSequence_ = chunk.sequence;
    previousCapturedFrameEnd_ = chunk.capturedFrameEnd;
    previousChannelCount_ = chunk.channelCount;
    return produced;
}

bool SpectrumAnalyzer::emitLatestRawTransform(SpectrumTransformSink& sink) const noexcept
{
    if (!hasLatestRawTransform_)
        return false;

    sink.consumeSpectrumTransform(latestRawTransformView());
    return true;
}

void SpectrumAnalyzer::reset(VisualizationFrame* const destinationToInvalidate) noexcept
{
    resetTemporalState(ResetReason::explicitReset, destinationToInvalidate);
    hasPreviousChunk_ = false;
    previousGeneration_ = 0;
    previousChunkSequence_ = 0;
    previousCapturedFrameEnd_ = 0;
    previousChannelCount_ = 0;
}

void SpectrumAnalyzer::clearTemporalState(
    VisualizationFrame* const destinationToInvalidate) noexcept
{
    resetSpectrumTemporalState(destinationToInvalidate);
    ++statistics_.userClears;
}

bool SpectrumAnalyzer::isSupportedConfiguration(
    const SpectrumAnalysisConfiguration& configuration) noexcept
{
    constexpr std::array supportedFftSizes {
        std::size_t { 1024 },
        std::size_t { 2048 },
        std::size_t { 4096 },
        std::size_t { 8192 },
        std::size_t { 16384 },
    };
    constexpr std::array supportedSliceRates { 15, 30, 60, 120 };
    const auto sizeIsSupported
        = std::find(supportedFftSizes.begin(), supportedFftSizes.end(), configuration.fftSize)
        != supportedFftSizes.end();
    const auto rateIsSupported = std::find(supportedSliceRates.begin(), supportedSliceRates.end(),
                                     configuration.requestedSliceRateHz)
        != supportedSliceRates.end();

    switch (configuration.window) {
    case FftWindow::rectangular:
    case FftWindow::periodicHann:
    case FftWindow::fourTermBlackmanHarris:
    case FftWindow::fiveTermFlatTop:
        return sizeIsSupported && rateIsSupported;
    }

    return false;
}

bool SpectrumAnalyzer::isSupportedTemporalConfiguration(
    const SpectrumTemporalConfiguration& configuration) noexcept
{
    constexpr auto minimumAveragingMilliseconds = 25.0;
    constexpr auto maximumAveragingMilliseconds = 2'000.0;
    constexpr auto minimumPeakHoldSeconds = 0.25;
    constexpr auto maximumPeakHoldSeconds = 10.0;

    if (!std::isfinite(configuration.averagingMilliseconds)
        || configuration.averagingMilliseconds < minimumAveragingMilliseconds
        || configuration.averagingMilliseconds > maximumAveragingMilliseconds
        || !std::isfinite(configuration.finitePeakHoldSeconds)
        || configuration.finitePeakHoldSeconds < minimumPeakHoldSeconds
        || configuration.finitePeakHoldSeconds > maximumPeakHoldSeconds) {
        return false;
    }

    switch (configuration.peakHoldMode) {
    case SpectrumPeakHoldMode::off:
    case SpectrumPeakHoldMode::finite:
    case SpectrumPeakHoldMode::infinite:
        return true;
    }

    return false;
}

void SpectrumAnalyzer::resetTemporalState(
    const ResetReason reason, VisualizationFrame* const destination) noexcept
{
    std::fill(leftRing_.begin(), leftRing_.end(), 0.0F);
    std::fill(rightRing_.begin(), rightRing_.end(), 0.0F);
    writeIndex_ = 0;
    validSampleCount_ = 0;
    samplesSinceTransform_ = 0;
    hasProducedSinceReset_ = false;
    latestPower_.fill(0.0F);
    hasLatestRawTransform_ = false;
    latestRawTransformSequence_ = 0;
    latestRawCaptureGeneration_ = 0;
    latestRawCapturedFrameEnd_ = 0;
    latestRawChannelCount_ = 0;
    ++resetEpoch_;
    if (resetEpoch_ == 0)
        ++resetEpoch_;
    resetSpectrumTemporalState(destination);
    ++statistics_.temporalResets;

    if (reason == ResetReason::sequenceGap || reason == ResetReason::capturedFrameGap)
        ++statistics_.sequenceGapResets;
}

void SpectrumAnalyzer::resetSpectrumTemporalState(VisualizationFrame* const destination) noexcept
{
    averagedPower_.fill(0.0F);
    heldPower_.fill(0.0F);
    finiteHoldRemainingSeconds_.fill(0.0);
    previousTransformCapturedFrameEnd_ = 0;
    hasSpectrumTemporalState_ = false;

    if (destination != nullptr)
        invalidateSpectrum(*destination);
}

void SpectrumAnalyzer::invalidateSpectrum(VisualizationFrame& destination) const noexcept
{
    destination.spectrumDecibels.fill(minimumSpectrumDecibels);
    destination.spectrumPeakHoldDecibels.fill(minimumSpectrumDecibels);
    destination.fftGeneration = fftGeneration_;
    destination.spectrumFftSize = static_cast<std::uint32_t>(configuration_.fftSize);
    destination.spectrumBinCount = static_cast<std::uint32_t>(configuredBinCount_);
    destination.spectrumCapturedFrameEnd = 0;
    destination.spectrumValid = false;
    destination.spectrumPeakHoldValid = false;
}

void SpectrumAnalyzer::configureSampleRate(const double sampleRate) noexcept
{
    sampleRate_ = sampleRate;
    hopSize_ = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::llround(sampleRate / configuration_.requestedSliceRateHz)));
}

void SpectrumAnalyzer::buildWindow() noexcept
{
    window_.fill(0.0F);
    auto windowSum = 0.0;
    for (std::size_t index = 0; index < configuration_.fftSize; ++index) {
        const auto phase = (2.0 * std::numbers::pi * static_cast<double>(index))
            / static_cast<double>(configuration_.fftSize);
        const auto coefficient
            = static_cast<float>(windowCoefficient(configuration_.window, phase));
        window_[index] = coefficient;
        windowSum += coefficient;
    }

    edgeBinScale_ = static_cast<float>(1.0 / windowSum);
    interiorBinScale_ = 2.0F * edgeBinScale_;
}

void SpectrumAnalyzer::runTransform(const std::uint64_t generation,
    const std::uint64_t capturedFrameEnd, const std::uint32_t channelCount,
    VisualizationFrame& destination, SpectrumTransformSink* const rawTransformSink) noexcept
{
    prepareChannelTransform(leftRing_, leftWorkspace_);
    selectedFft().performFrequencyOnlyForwardTransform(leftWorkspace_.data(), true);

    if (channelCount == 2) {
        prepareChannelTransform(rightRing_, rightWorkspace_);
        selectedFft().performFrequencyOnlyForwardTransform(rightWorkspace_.data(), true);
    }

    const auto elapsedSeconds = hasSpectrumTemporalState_
            && capturedFrameEnd > previousTransformCapturedFrameEnd_ && sampleRate_ > 0.0
        ? static_cast<double>(capturedFrameEnd - previousTransformCapturedFrameEnd_) / sampleRate_
        : 0.0;
    const auto averagingCoefficient = temporalConfiguration_.averagingEnabled
            && hasSpectrumTemporalState_ && elapsedSeconds > 0.0
        ? std::exp(-elapsedSeconds / (temporalConfiguration_.averagingMilliseconds * 0.001))
        : 0.0;
    constexpr auto holdDecayDecibelsPerSecond = 12.0;

    for (std::size_t bin = 0; bin < configuredBinCount_; ++bin) {
        const auto scale
            = (bin == 0 || bin == configuration_.fftSize / 2) ? edgeBinScale_ : interiorBinScale_;
        const auto magnitude
            = (channelCount == 2 ? std::max(leftWorkspace_[bin], rightWorkspace_[bin])
                                 : leftWorkspace_[bin])
            * scale;
        const auto powerAsDouble = static_cast<double>(magnitude) * magnitude;
        const auto currentPower = std::isnan(powerAsDouble) || powerAsDouble <= 0.0
            ? 0.0F
            : static_cast<float>(
                  std::min(powerAsDouble, static_cast<double>(std::numeric_limits<float>::max())));
        latestPower_[bin] = currentPower;

        if (!temporalConfiguration_.averagingEnabled || !hasSpectrumTemporalState_) {
            averagedPower_[bin] = currentPower;
        } else {
            const auto averaged = (averagingCoefficient * averagedPower_[bin])
                + ((1.0 - averagingCoefficient) * currentPower);
            averagedPower_[bin] = static_cast<float>(
                std::clamp(averaged, 0.0, static_cast<double>(std::numeric_limits<float>::max())));
        }

        destination.spectrumDecibels[bin] = powerToDecibels(averagedPower_[bin]);

        switch (temporalConfiguration_.peakHoldMode) {
        case SpectrumPeakHoldMode::off:
            heldPower_[bin] = 0.0F;
            finiteHoldRemainingSeconds_[bin] = 0.0;
            destination.spectrumPeakHoldDecibels[bin] = minimumSpectrumDecibels;
            break;
        case SpectrumPeakHoldMode::infinite:
            heldPower_[bin] = hasSpectrumTemporalState_ ? std::max(heldPower_[bin], currentPower)
                                                        : currentPower;
            finiteHoldRemainingSeconds_[bin] = 0.0;
            destination.spectrumPeakHoldDecibels[bin] = powerToDecibels(heldPower_[bin]);
            break;
        case SpectrumPeakHoldMode::finite: {
            if (!hasSpectrumTemporalState_ || currentPower >= heldPower_[bin]) {
                heldPower_[bin] = currentPower;
                finiteHoldRemainingSeconds_[bin] = temporalConfiguration_.finitePeakHoldSeconds;
            } else {
                const auto heldBefore = finiteHoldRemainingSeconds_[bin];
                finiteHoldRemainingSeconds_[bin] = std::max(0.0, heldBefore - elapsedSeconds);
                const auto decaySeconds = std::max(0.0, elapsedSeconds - heldBefore);
                if (decaySeconds > 0.0) {
                    const auto powerDecay
                        = std::pow(10.0, -(holdDecayDecibelsPerSecond * decaySeconds) / 10.0);
                    heldPower_[bin] = static_cast<float>(heldPower_[bin] * powerDecay);
                    if (heldPower_[bin] < currentPower)
                        heldPower_[bin] = currentPower;
                }
            }

            destination.spectrumPeakHoldDecibels[bin] = powerToDecibels(heldPower_[bin]);
            break;
        }
        }
    }

    destination.generation = generation;
    destination.fftGeneration = fftGeneration_;
    destination.capturedFrameEnd = capturedFrameEnd;
    destination.spectrumCapturedFrameEnd = capturedFrameEnd;
    destination.spectrumFftSize = static_cast<std::uint32_t>(configuration_.fftSize);
    destination.spectrumBinCount = static_cast<std::uint32_t>(configuredBinCount_);
    destination.channelCount = channelCount;
    destination.sampleRate = sampleRate_;
    destination.spectrumValid = true;
    destination.spectrumPeakHoldValid
        = temporalConfiguration_.peakHoldMode != SpectrumPeakHoldMode::off;

    latestRawTransformSequence_ = nextRawTransformSequence_++;
    if (latestRawTransformSequence_ == 0)
        latestRawTransformSequence_ = nextRawTransformSequence_++;
    latestRawCaptureGeneration_ = generation;
    latestRawCapturedFrameEnd_ = capturedFrameEnd;
    latestRawChannelCount_ = channelCount;
    hasLatestRawTransform_ = true;

    if (rawTransformSink != nullptr)
        rawTransformSink->consumeSpectrumTransform(latestRawTransformView());

    samplesSinceTransform_ = 0;
    hasProducedSinceReset_ = true;
    hasSpectrumTemporalState_ = true;
    previousTransformCapturedFrameEnd_ = capturedFrameEnd;
    ++statistics_.transforms;
}

SpectrumTransformView SpectrumAnalyzer::latestRawTransformView() const noexcept
{
    return { latestPower_.data(), configuredBinCount_, latestRawCaptureGeneration_, fftGeneration_,
        latestRawTransformSequence_, resetEpoch_, latestRawCapturedFrameEnd_, sampleRate_,
        static_cast<std::uint32_t>(configuration_.fftSize), static_cast<std::uint32_t>(hopSize_),
        static_cast<std::uint32_t>(configuration_.requestedSliceRateHz), latestRawChannelCount_ };
}

void SpectrumAnalyzer::prepareChannelTransform(const std::array<float, maximumFftSize>& ring,
    std::array<float, maximumTransformWorkspaceSize>& workspace) noexcept
{
    for (std::size_t index = 0; index < configuration_.fftSize; ++index) {
        workspace[index] = ring[(writeIndex_ + index) % configuration_.fftSize] * window_[index];
    }

    std::fill(workspace.begin() + static_cast<std::ptrdiff_t>(configuration_.fftSize),
        workspace.begin() + static_cast<std::ptrdiff_t>(configuration_.fftSize * 2), 0.0F);
}

juce::dsp::FFT& SpectrumAnalyzer::selectedFft() noexcept
{
    switch (configuration_.fftSize) {
    case 1024:
        return fft1024_;
    case 2048:
        return fft2048_;
    case 8192:
        return fft8192_;
    case 16384:
        return fft16384_;
    case 4096:
    default:
        return fft4096_;
    }
}

float SpectrumAnalyzer::powerToDecibels(const float power) noexcept
{
    constexpr auto floorPower = 1.0e-18F;
    if (!std::isfinite(power) || power <= floorPower)
        return minimumSpectrumDecibels;

    return std::max(minimumSpectrumDecibels, 10.0F * std::log10(power));
}
} // namespace audio_insight
