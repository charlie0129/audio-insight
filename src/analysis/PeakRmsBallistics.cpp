// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PeakRmsBallistics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audio_insight {
namespace {
[[nodiscard]] bool sampleRatesDiffer(const double left, const double right) noexcept
{
    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}
} // namespace

PeakRmsBallisticsFrame PeakRmsBallistics::processBlock(const float* const left,
    const float* const right, const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation, const std::uint32_t channelCount,
    const bool followsDiscontinuity) noexcept
{
    if (!isFormatValid(frameCount, sampleRate, generation, channelCount)) {
        reset();
        return output_;
    }

    const auto formatChanged = !initialized_ || generation != generation_
        || channelCount != channelCount_ || sampleRatesDiffer(sampleRate, sampleRate_);

    if (formatChanged) {
        resetState();
        configureFormat(sampleRate, generation, channelCount);
    } else if (followsDiscontinuity) {
        resetState();
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        const auto leftSample = left != nullptr && std::isfinite(left[frame])
            ? static_cast<double>(left[frame])
            : 0.0;
        const auto rightSample = right != nullptr && std::isfinite(right[frame])
            ? static_cast<double>(right[frame])
            : 0.0;
        processSample(channels_[0], leftSample);
        if (channelCount == 2) {
            processSample(channels_[1], rightSample);
            crossMeanProduct_ = (rmsDecayPerSample_ * crossMeanProduct_)
                + (rmsInputPerSample_ * leftSample * rightSample);
        }
    }

    if (channelCount == 1)
        channels_[1] = { };

    publish();
    return output_;
}

void PeakRmsBallistics::reset() noexcept
{
    resetState();
    output_ = { };
    generation_ = 0;
    channelCount_ = 0;
    sampleRate_ = 0.0;
    rmsDecayPerSample_ = 0.0;
    rmsInputPerSample_ = 1.0;
    peakReleasePerSample_ = 0.0;
    holdDecayPerSample_ = 0.0;
    holdSamples_ = 0.0;
    crossMeanProduct_ = 0.0;
    initialized_ = false;
}

void PeakRmsBallistics::userReset() noexcept
{
    for (auto& channel : channels_) {
        channel.heldSamplePeak = 0.0;
        channel.holdSamplesRemaining = 0.0;
        channel.over = false;
    }

    if (initialized_)
        publish();
}

void PeakRmsBallistics::clearLiveMeasurements() noexcept
{
    for (auto& channel : channels_) {
        channel.rmsMeanSquare = 0.0;
        channel.liveSamplePeak = 0.0;
    }
    crossMeanProduct_ = 0.0;

    if (initialized_)
        publish();
}

bool PeakRmsBallistics::isFormatValid(const std::size_t frameCount, const double sampleRate,
    const std::uint64_t generation, const std::uint32_t channelCount) noexcept
{
    return frameCount > 0 && (channelCount == 1 || channelCount == 2) && generation != 0
        && std::isfinite(sampleRate) && sampleRate > 0.0
        && sampleRate <= std::numeric_limits<double>::max() / peakHoldSeconds;
}

float PeakRmsBallistics::linearToDecibels(const double value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0)
        return minimumDisplayDecibels;

    return std::max(minimumDisplayDecibels, static_cast<float>(20.0 * std::log10(value)));
}

void PeakRmsBallistics::configureFormat(const double sampleRate, const std::uint64_t generation,
    const std::uint32_t channelCount) noexcept
{
    generation_ = generation;
    channelCount_ = channelCount;
    sampleRate_ = sampleRate;

    rmsInputPerSample_ = -std::expm1(-1.0 / (sampleRate_ * rmsTimeConstantSeconds));
    rmsDecayPerSample_ = 1.0 - rmsInputPerSample_;

    constexpr auto naturalLogarithmOfTen = 2.30258509299404568402;
    peakReleasePerSample_
        = std::exp(-naturalLogarithmOfTen * (peakReleaseDecibelsPerSecond / 20.0) / sampleRate_);
    holdDecayPerSample_
        = std::exp(-naturalLogarithmOfTen * (holdDecayDecibelsPerSecond / 20.0) / sampleRate_);
    holdSamples_ = peakHoldSeconds * sampleRate_;
    initialized_ = true;
}

void PeakRmsBallistics::processSample(ChannelState& state, const double sample) const noexcept
{
    const auto magnitude = std::abs(sample);
    const auto power = magnitude * magnitude;

    state.rmsMeanSquare = (rmsDecayPerSample_ * state.rmsMeanSquare) + (rmsInputPerSample_ * power);
    state.liveSamplePeak = std::max(magnitude, state.liveSamplePeak * peakReleasePerSample_);
    state.over = state.over || magnitude >= 1.0;

    auto decaySamples = 0.0;
    if (state.holdSamplesRemaining >= 1.0) {
        state.holdSamplesRemaining -= 1.0;
    } else if (state.holdSamplesRemaining > 0.0) {
        decaySamples = 1.0 - state.holdSamplesRemaining;
        state.holdSamplesRemaining = 0.0;
    } else {
        decaySamples = 1.0;
    }

    if (decaySamples == 1.0) {
        state.heldSamplePeak *= holdDecayPerSample_;
    } else if (decaySamples > 0.0) {
        state.heldSamplePeak *= std::pow(holdDecayPerSample_, decaySamples);
    }

    if (magnitude >= state.heldSamplePeak) {
        state.heldSamplePeak = magnitude;
        state.holdSamplesRemaining = holdSamples_;
    }
}

void PeakRmsBallistics::resetState() noexcept
{
    channels_ = { };
    crossMeanProduct_ = 0.0;
}

void PeakRmsBallistics::publish() noexcept
{
    output_ = { };
    output_.generation = generation_;
    output_.channelCount = channelCount_;
    output_.sampleRate = sampleRate_;
    output_.valid = initialized_;

    for (std::size_t channel = 0; channel < channelCount_; ++channel) {
        const auto livePeak = channels_[channel].liveSamplePeak;
        const auto rms = std::sqrt(std::max(0.0, channels_[channel].rmsMeanSquare));
        const auto heldPeak = channels_[channel].heldSamplePeak;

        output_.liveSamplePeakLinear[channel] = static_cast<float>(livePeak);
        output_.rmsLinear[channel] = static_cast<float>(rms);
        output_.heldSamplePeakLinear[channel] = static_cast<float>(heldPeak);
        output_.rmsMeanSquare[channel] = channels_[channel].rmsMeanSquare;
        output_.liveSamplePeakDecibels[channel] = linearToDecibels(livePeak);
        output_.rmsDecibels[channel] = linearToDecibels(rms);
        output_.heldSamplePeakDecibels[channel] = linearToDecibels(heldPeak);
        output_.over[channel] = channels_[channel].over;
        output_.channelValid[channel] = true;
    }

    output_.crossMeanProduct = channelCount_ == 2 ? crossMeanProduct_ : 0.0;
    if (channelCount_ != 2)
        return;

    const auto leftPower = channels_[0].rmsMeanSquare;
    const auto rightPower = channels_[1].rmsMeanSquare;
    if (leftPower < correlationSilenceThresholdMeanSquare
        || rightPower < correlationSilenceThresholdMeanSquare) {
        return;
    }

    const auto denominator = std::sqrt(leftPower * rightPower);
    if (!std::isfinite(denominator) || denominator <= 0.0 || !std::isfinite(crossMeanProduct_)) {
        return;
    }

    output_.correlation
        = static_cast<float>(std::clamp(crossMeanProduct_ / denominator, -1.0, 1.0));
    output_.correlationValid = true;
}
} // namespace audio_insight
