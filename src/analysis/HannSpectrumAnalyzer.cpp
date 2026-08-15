// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HannSpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace audio_insight
{
namespace
{
[[nodiscard]] bool sampleRatesDiffer(const double left, const double right) noexcept
{
    const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}
} // namespace

HannSpectrumAnalyzer::HannSpectrumAnalyzer(const double targetUpdatesPerSecond)
    : targetUpdatesPerSecond_(std::isfinite(targetUpdatesPerSecond) && targetUpdatesPerSecond > 0.0
                                  ? targetUpdatesPerSecond
                                  : 60.0)
{
    double windowSum = 0.0;
    for (std::size_t index = 0; index < hannWindow_.size(); ++index)
    {
        const auto phase = (2.0 * std::numbers::pi * static_cast<double>(index)) /
                           static_cast<double>(fftSize - 1);
        const auto coefficient = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
        hannWindow_[index] = coefficient;
        windowSum += coefficient;
    }

    edgeBinScale_ = static_cast<float>(1.0 / windowSum);
    interiorBinScale_ = 2.0F * edgeBinScale_;
}

bool HannSpectrumAnalyzer::process(const CapturedStereoChunkView& chunk,
                                   VisualizationFrame& destination) noexcept
{
    ++statistics_.inputChunks;

    if (chunk.frameCount == 0 || chunk.left == nullptr || chunk.right == nullptr ||
        !std::isfinite(chunk.sampleRate) || chunk.sampleRate <= 0.0)
    {
        return false;
    }

    if (chunk.capturedFrameEnd < chunk.frameCount)
    {
        resetTemporalState(ResetReason::capturedFrameGap, &destination);
        hasPreviousChunk_ = false;
        previousGeneration_ = 0;
        previousChunkSequence_ = 0;
        previousCapturedFrameEnd_ = 0;
        return false;
    }

    ResetReason resetReason = ResetReason::explicitReset;
    bool needsReset = false;

    if (hasPreviousChunk_)
    {
        if (chunk.generation != previousGeneration_)
        {
            resetReason = ResetReason::generationChange;
            needsReset = true;
        }
        else if (chunk.sequence != previousChunkSequence_ + 1 || chunk.followsDiscontinuity)
        {
            resetReason = ResetReason::sequenceGap;
            needsReset = true;
        }
        else
        {
            const auto hasValidFrameRange = chunk.capturedFrameEnd >= chunk.frameCount;
            const auto capturedFrameStart =
                hasValidFrameRange ? chunk.capturedFrameEnd - chunk.frameCount : 0;
            if (!hasValidFrameRange || capturedFrameStart != previousCapturedFrameEnd_)
            {
                resetReason = ResetReason::capturedFrameGap;
                needsReset = true;
            }
            else if (sampleRatesDiffer(sampleRate_, chunk.sampleRate))
            {
                resetReason = ResetReason::sampleRateChange;
                needsReset = true;
            }
        }
    }

    if (needsReset)
        resetTemporalState(resetReason, &destination);

    if (sampleRatesDiffer(sampleRate_, chunk.sampleRate))
        configureSampleRate(chunk.sampleRate);

    bool produced = false;
    const auto chunkFrameStart = chunk.capturedFrameEnd - chunk.frameCount;

    for (std::size_t frame = 0; frame < chunk.frameCount; ++frame)
    {
        leftRing_[writeIndex_] = std::isfinite(chunk.left[frame]) ? chunk.left[frame] : 0.0F;
        rightRing_[writeIndex_] = std::isfinite(chunk.right[frame]) ? chunk.right[frame] : 0.0F;
        writeIndex_ = (writeIndex_ + 1) % fftSize;
        validSampleCount_ = std::min(validSampleCount_ + 1, fftSize);
        ++samplesSinceTransform_;

        const bool firstWindowReady = !hasProducedSinceReset_ && validSampleCount_ == fftSize;
        const bool nextHopReady = hasProducedSinceReset_ && samplesSinceTransform_ >= hopSize_;
        if (firstWindowReady || nextHopReady)
        {
            runTransform(chunk.generation, chunkFrameStart + frame + 1, destination);
            produced = true;
        }
    }

    hasPreviousChunk_ = true;
    previousGeneration_ = chunk.generation;
    previousChunkSequence_ = chunk.sequence;
    previousCapturedFrameEnd_ = chunk.capturedFrameEnd;
    return produced;
}

void HannSpectrumAnalyzer::reset(VisualizationFrame* const destinationToInvalidate) noexcept
{
    resetTemporalState(ResetReason::explicitReset, destinationToInvalidate);
    hasPreviousChunk_ = false;
    previousGeneration_ = 0;
    previousChunkSequence_ = 0;
    previousCapturedFrameEnd_ = 0;
}

void HannSpectrumAnalyzer::resetTemporalState(const ResetReason reason,
                                              VisualizationFrame* const destination) noexcept
{
    std::fill(leftRing_.begin(), leftRing_.end(), 0.0F);
    std::fill(rightRing_.begin(), rightRing_.end(), 0.0F);
    writeIndex_ = 0;
    validSampleCount_ = 0;
    samplesSinceTransform_ = 0;
    hasProducedSinceReset_ = false;
    ++statistics_.temporalResets;

    if (reason == ResetReason::sequenceGap || reason == ResetReason::capturedFrameGap)
        ++statistics_.sequenceGapResets;

    if (destination != nullptr)
    {
        destination->spectrumDecibels.fill(minimumDisplayDecibels);
        destination->spectrumValid = false;
    }
}

void HannSpectrumAnalyzer::configureSampleRate(const double sampleRate) noexcept
{
    sampleRate_ = sampleRate;
    hopSize_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::llround(sampleRate / targetUpdatesPerSecond_)));
}

void HannSpectrumAnalyzer::runTransform(const std::uint64_t generation,
                                        const std::uint64_t capturedFrameEnd,
                                        VisualizationFrame& destination) noexcept
{
    prepareChannelTransform(leftRing_, leftWorkspace_);
    prepareChannelTransform(rightRing_, rightWorkspace_);

    fft_.performFrequencyOnlyForwardTransform(leftWorkspace_.data(), true);
    fft_.performFrequencyOnlyForwardTransform(rightWorkspace_.data(), true);

    for (std::size_t bin = 0; bin < spectrumBinCount; ++bin)
    {
        const auto scale = (bin == 0 || bin == fftSize / 2) ? edgeBinScale_ : interiorBinScale_;
        const auto magnitude = std::max(leftWorkspace_[bin], rightWorkspace_[bin]) * scale;
        destination.spectrumDecibels[bin] = magnitudeToDecibels(magnitude);
    }

    destination.generation = generation;
    destination.spectrumSequence = nextSpectrumSequence_++;
    destination.capturedFrameEnd = capturedFrameEnd;
    destination.sampleRate = sampleRate_;
    destination.spectrumValid = true;

    samplesSinceTransform_ = 0;
    hasProducedSinceReset_ = true;
    ++statistics_.transforms;
}

void HannSpectrumAnalyzer::prepareChannelTransform(
    const std::array<float, fftSize>& ring,
    std::array<float, transformWorkspaceSize>& workspace) noexcept
{
    // Once the ring is full, writeIndex_ identifies its oldest sample.
    for (std::size_t index = 0; index < fftSize; ++index)
        workspace[index] = ring[(writeIndex_ + index) % fftSize] * hannWindow_[index];

    std::fill(workspace.begin() + static_cast<std::ptrdiff_t>(fftSize), workspace.end(), 0.0F);
}

float HannSpectrumAnalyzer::magnitudeToDecibels(const float magnitude) noexcept
{
    constexpr auto floorLinear = 1.0e-6F; // -120 dBFS
    if (!std::isfinite(magnitude) || magnitude <= floorLinear)
        return minimumDisplayDecibels;

    return std::max(minimumDisplayDecibels, 20.0F * std::log10(magnitude));
}
} // namespace audio_insight
