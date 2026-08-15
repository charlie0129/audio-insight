// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "StereoSampleCapture.h"
#include "core/VisualizationFrame.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_insight {
/**
    Worker-owned, fixed-capacity history for the rotated stereo vectorscope.

    Every accepted raw chunk is observed before the coordinator performs any
    FFT-specific backlog trimming. Only the display points are decimated; stereo
    correlation is producer-owned by PeakRmsBallistics and is deliberately not
    calculated from this history.
*/
class StereoFieldAnalyzer final {
public:
    struct Statistics final {
        std::uint64_t processedChunks = 0;
        std::uint64_t processedFrames = 0;
        std::uint64_t selectedPoints = 0;
        std::uint64_t historyResets = 0;
        std::uint64_t invalidChunks = 0;
        std::uint64_t capturedFrameEnd = 0;
        std::uint32_t pointCount = 0;
        std::uint32_t pointStrideFrames = 0;
    };

    /** Updates the history from one source-order capture chunk. */
    [[nodiscard]] bool process(const CapturedStereoChunkView& chunk) noexcept;

    /** Copies the current chronological point history into one immutable frame. */
    [[nodiscard]] bool writeFrame(VisualizationFrame& destination) const noexcept;

    /** Clears point history. The optional frame is invalidated in the same operation. */
    void reset(VisualizationFrame* destination = nullptr) noexcept;

    [[nodiscard]] Statistics statistics() const noexcept;

private:
    struct StoredPoint final {
        float horizontal = 0.0F;
        float vertical = 0.0F;
        std::uint64_t capturedFrame = 0;
    };

    [[nodiscard]] static bool isChunkValid(const CapturedStereoChunkView& chunk) noexcept;
    [[nodiscard]] static bool sampleRatesDiffer(double left, double right) noexcept;
    [[nodiscard]] static std::uint64_t calculateWindowFrames(double sampleRate) noexcept;
    [[nodiscard]] static std::uint32_t calculatePointStride(std::uint64_t windowFrames) noexcept;
    [[nodiscard]] bool isContinuousWithPrevious(
        const CapturedStereoChunkView& chunk, std::uint64_t chunkFrameStart) const noexcept;
    void configureForChunk(
        const CapturedStereoChunkView& chunk, std::uint64_t chunkFrameStart) noexcept;
    void appendPoint(float left, float right, std::uint64_t capturedFrame) noexcept;
    void pruneExpiredPoints(std::uint64_t capturedFrameEnd) noexcept;
    void clearState() noexcept;
    static void clearFrame(VisualizationFrame& destination) noexcept;

    std::array<StoredPoint, maximumStereoFieldPointCount> points_ { };
    std::size_t oldestPoint_ = 0;
    std::size_t pointCount_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t previousSequence_ = 0;
    std::uint64_t previousCapturedFrameEnd_ = 0;
    std::uint64_t nextPointFrame_ = 0;
    std::uint64_t windowFrames_ = 0;
    std::uint32_t pointStrideFrames_ = 0;
    std::uint32_t channelCount_ = 0;
    double sampleRate_ = 0.0;
    bool initialized_ = false;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::atomic<std::uint64_t> processedChunks_ { 0 };
    std::atomic<std::uint64_t> processedFrames_ { 0 };
    std::atomic<std::uint64_t> selectedPoints_ { 0 };
    std::atomic<std::uint64_t> historyResets_ { 0 };
    std::atomic<std::uint64_t> invalidChunks_ { 0 };
    std::atomic<std::uint64_t> telemetryCapturedFrameEnd_ { 0 };
    std::atomic<std::uint32_t> telemetryPointCount_ { 0 };
    std::atomic<std::uint32_t> telemetryPointStrideFrames_ { 0 };
};
} // namespace audio_insight
