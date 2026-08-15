// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace audio_insight {
/** A callback-lifetime-only view of one calibrated, unsmoothed FFT result. */
struct SpectrumTransformView final {
    const float* powerBins = nullptr;
    std::size_t binCount = 0;

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

/**
    Synchronous non-owning sink for every completed raw-power transform.

    consumeSpectrumTransform() must not retain powerBins. The analyzer invokes
    it only from its non-audio worker after every bin has been written.
*/
class SpectrumTransformSink {
public:
    virtual ~SpectrumTransformSink() = default;
    virtual void consumeSpectrumTransform(const SpectrumTransformView& transform) noexcept = 0;
};
} // namespace audio_insight
