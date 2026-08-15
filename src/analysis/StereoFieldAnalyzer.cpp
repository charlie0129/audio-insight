// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StereoFieldAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audio_insight {
bool StereoFieldAnalyzer::process(const CapturedStereoChunkView& chunk) noexcept
{
    if (!isChunkValid(chunk)) {
        invalidChunks_.fetch_add(1, std::memory_order_relaxed);
        reset();
        return false;
    }

    const auto chunkFrameStart = chunk.capturedFrameEnd - chunk.frameCount;
    const auto needsReset = initialized_
        && (chunk.followsDiscontinuity || !isContinuousWithPrevious(chunk, chunkFrameStart));

    if (needsReset)
        reset();

    if (!initialized_)
        configureForChunk(chunk, chunkFrameStart);

    auto selectedPointCount = std::uint64_t { 0 };
    for (std::size_t frame = 0; frame < chunk.frameCount; ++frame) {
        const auto capturedFrame = chunkFrameStart + frame;
        if (capturedFrame < nextPointFrame_)
            continue;

        const auto left
            = chunk.left != nullptr && std::isfinite(chunk.left[frame]) ? chunk.left[frame] : 0.0F;
        const auto right
            = chunk.channelCount == 2 && chunk.right != nullptr && std::isfinite(chunk.right[frame])
            ? chunk.right[frame]
            : left;
        appendPoint(left, right, capturedFrame);
        ++selectedPointCount;

        if (nextPointFrame_ > std::numeric_limits<std::uint64_t>::max() - pointStrideFrames_)
            nextPointFrame_ = std::numeric_limits<std::uint64_t>::max();
        else
            nextPointFrame_ += pointStrideFrames_;
    }

    pruneExpiredPoints(chunk.capturedFrameEnd);
    previousSequence_ = chunk.sequence;
    previousCapturedFrameEnd_ = chunk.capturedFrameEnd;

    processedChunks_.fetch_add(1, std::memory_order_relaxed);
    processedFrames_.fetch_add(chunk.frameCount, std::memory_order_relaxed);
    selectedPoints_.fetch_add(selectedPointCount, std::memory_order_relaxed);
    telemetryCapturedFrameEnd_.store(chunk.capturedFrameEnd, std::memory_order_relaxed);
    telemetryPointCount_.store(static_cast<std::uint32_t>(pointCount_), std::memory_order_relaxed);
    telemetryPointStrideFrames_.store(pointStrideFrames_, std::memory_order_relaxed);
    return true;
}

bool StereoFieldAnalyzer::writeFrame(VisualizationFrame& destination) const noexcept
{
    if (!initialized_) {
        clearFrame(destination);
        return false;
    }

    destination.stereoFieldPointCount = static_cast<std::uint32_t>(pointCount_);
    destination.stereoPointStrideFrames = pointStrideFrames_;
    destination.stereoCapturedFrameEnd = previousCapturedFrameEnd_;
    destination.channelCount = channelCount_;
    destination.sampleRate = sampleRate_;
    destination.stereoFieldValid = true;
    destination.stereoMono = channelCount_ == 1;

    for (std::size_t index = 0; index < pointCount_; ++index) {
        const auto& source = points_[(oldestPoint_ + index) % points_.size()];
        const auto newestRepresentedFrame
            = previousCapturedFrameEnd_ == 0 ? 0 : previousCapturedFrameEnd_ - 1;
        const auto ageFrames = newestRepresentedFrame >= source.capturedFrame
            ? newestRepresentedFrame - source.capturedFrame
            : 0;
        const auto normalizedAge = windowFrames_ == 0
            ? 0.0
            : static_cast<double>(ageFrames) / static_cast<double>(windowFrames_);
        destination.stereoFieldPoints[index] = { source.horizontal, source.vertical,
            static_cast<float>(std::clamp(normalizedAge, 0.0, 1.0)) };
    }

    return true;
}

void StereoFieldAnalyzer::reset(VisualizationFrame* const destination) noexcept
{
    clearState();
    historyResets_.fetch_add(1, std::memory_order_relaxed);
    telemetryCapturedFrameEnd_.store(0, std::memory_order_relaxed);
    telemetryPointCount_.store(0, std::memory_order_relaxed);
    telemetryPointStrideFrames_.store(0, std::memory_order_relaxed);

    if (destination != nullptr)
        clearFrame(*destination);
}

StereoFieldAnalyzer::Statistics StereoFieldAnalyzer::statistics() const noexcept
{
    return {
        processedChunks_.load(std::memory_order_relaxed),
        processedFrames_.load(std::memory_order_relaxed),
        selectedPoints_.load(std::memory_order_relaxed),
        historyResets_.load(std::memory_order_relaxed),
        invalidChunks_.load(std::memory_order_relaxed),
        telemetryCapturedFrameEnd_.load(std::memory_order_relaxed),
        telemetryPointCount_.load(std::memory_order_relaxed),
        telemetryPointStrideFrames_.load(std::memory_order_relaxed),
    };
}

bool StereoFieldAnalyzer::isChunkValid(const CapturedStereoChunkView& chunk) noexcept
{
    constexpr auto maximumWindowFrames
        = static_cast<long double>(std::numeric_limits<std::uint32_t>::max())
        * static_cast<long double>(maximumStereoFieldPointCount);
    const auto requestedWindowFrames = static_cast<long double>(chunk.sampleRate)
        * static_cast<long double>(stereoFieldHistorySeconds);

    return chunk.left != nullptr && (chunk.channelCount == 1 || chunk.right != nullptr)
        && chunk.frameCount > 0 && chunk.capturedFrameEnd >= chunk.frameCount
        && chunk.generation != 0 && chunk.sequence != 0
        && (chunk.channelCount == 1 || chunk.channelCount == 2) && std::isfinite(chunk.sampleRate)
        && chunk.sampleRate > 0.0 && requestedWindowFrames <= maximumWindowFrames;
}

bool StereoFieldAnalyzer::sampleRatesDiffer(const double left, const double right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return true;

    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}

std::uint64_t StereoFieldAnalyzer::calculateWindowFrames(const double sampleRate) noexcept
{
    const auto frames = std::ceil(
        static_cast<long double>(sampleRate) * static_cast<long double>(stereoFieldHistorySeconds));
    if (!std::isfinite(frames) || frames <= 1.0L)
        return 1;

    const auto maximum = static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    if (frames >= maximum)
        return std::numeric_limits<std::uint64_t>::max();

    return static_cast<std::uint64_t>(frames);
}

std::uint32_t StereoFieldAnalyzer::calculatePointStride(const std::uint64_t windowFrames) noexcept
{
    constexpr auto capacity = static_cast<std::uint64_t>(maximumStereoFieldPointCount);
    const auto quotient = windowFrames / capacity;
    const auto remainder = windowFrames % capacity;
    const auto stride = quotient + static_cast<std::uint64_t>(remainder != 0);
    return static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(stride, 1, std::numeric_limits<std::uint32_t>::max()));
}

bool StereoFieldAnalyzer::isContinuousWithPrevious(
    const CapturedStereoChunkView& chunk, const std::uint64_t chunkFrameStart) const noexcept
{
    return chunk.generation == generation_
        && previousSequence_ != std::numeric_limits<std::uint64_t>::max()
        && chunk.sequence == previousSequence_ + 1 && chunkFrameStart == previousCapturedFrameEnd_
        && chunk.channelCount == channelCount_ && !sampleRatesDiffer(chunk.sampleRate, sampleRate_);
}

void StereoFieldAnalyzer::configureForChunk(
    const CapturedStereoChunkView& chunk, const std::uint64_t chunkFrameStart) noexcept
{
    generation_ = chunk.generation;
    previousSequence_ = chunk.sequence - 1;
    previousCapturedFrameEnd_ = chunkFrameStart;
    sampleRate_ = chunk.sampleRate;
    channelCount_ = chunk.channelCount;
    windowFrames_ = calculateWindowFrames(sampleRate_);
    pointStrideFrames_ = calculatePointStride(windowFrames_);
    nextPointFrame_ = chunkFrameStart;
    initialized_ = true;
}

void StereoFieldAnalyzer::appendPoint(
    const float left, const float right, const std::uint64_t capturedFrame) noexcept
{
    const auto leftValue = static_cast<double>(left);
    const auto rightValue = static_cast<double>(right);
    const auto horizontal = channelCount_ == 1 ? 0.0 : (rightValue - leftValue) * 0.5;
    const auto vertical = channelCount_ == 1 ? leftValue : (leftValue + rightValue) * 0.5;

    auto destinationIndex = (oldestPoint_ + pointCount_) % points_.size();
    if (pointCount_ == points_.size()) {
        destinationIndex = oldestPoint_;
        oldestPoint_ = (oldestPoint_ + 1) % points_.size();
    } else {
        ++pointCount_;
    }

    points_[destinationIndex]
        = { static_cast<float>(horizontal), static_cast<float>(vertical), capturedFrame };
}

void StereoFieldAnalyzer::pruneExpiredPoints(const std::uint64_t capturedFrameEnd) noexcept
{
    const auto oldestRetainedFrame
        = capturedFrameEnd > windowFrames_ ? capturedFrameEnd - windowFrames_ : 0;

    while (pointCount_ != 0 && points_[oldestPoint_].capturedFrame < oldestRetainedFrame) {
        oldestPoint_ = (oldestPoint_ + 1) % points_.size();
        --pointCount_;
    }
}

void StereoFieldAnalyzer::clearState() noexcept
{
    oldestPoint_ = 0;
    pointCount_ = 0;
    generation_ = 0;
    previousSequence_ = 0;
    previousCapturedFrameEnd_ = 0;
    nextPointFrame_ = 0;
    windowFrames_ = 0;
    pointStrideFrames_ = 0;
    channelCount_ = 0;
    sampleRate_ = 0.0;
    initialized_ = false;
}

void StereoFieldAnalyzer::clearFrame(VisualizationFrame& destination) noexcept
{
    destination.stereoFieldPointCount = 0;
    destination.stereoPointStrideFrames = 0;
    destination.stereoCapturedFrameEnd = 0;
    destination.stereoFieldValid = false;
    destination.stereoMono = false;
}
} // namespace audio_insight
