// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SpectrogramColumnMapper.h"

#include "core/SpectrumAnalysisConfiguration.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace audio_insight {
namespace {
[[nodiscard]] bool sampleRatesDiffer(const double left, const double right) noexcept
{
    const auto scale = std::max({ 1.0, std::abs(left), std::abs(right) });
    return std::abs(left - right) > std::numeric_limits<double>::epsilon() * scale * 4.0;
}

[[nodiscard]] bool hasIdenticalBits(const double left, const double right) noexcept
{
    return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
}

[[nodiscard]] float sanitisePower(const float power) noexcept
{
    if (std::isnan(power) || power <= 0.0F)
        return 0.0F;

    return std::isfinite(power) ? power : std::numeric_limits<float>::max();
}
} // namespace

bool SpectrogramColumnMapper::setFrequencySpacing(
    const double spacing, const std::uint64_t mappingGeneration) noexcept
{
    if (!std::isfinite(spacing) || spacing < 0.0 || spacing > 1.0 || mappingGeneration == 0)
        return false;

    if (hasIdenticalBits(spacing, frequencySpacing_) && mappingGeneration == mappingGeneration_) {
        return true;
    }

    frequencySpacing_ = spacing;
    mappingGeneration_ = mappingGeneration;
    mappingValid_ = false;
    rowCount_ = 0;
    return true;
}

bool SpectrogramColumnMapper::map(const SpectrumTransformView& transform,
    const std::uint64_t columnSequence, const bool mappingSeed,
    SpectrogramColumn& destination) noexcept
{
    const auto supportedFftSize = transform.fftSize >= (std::uint32_t { 1 } << minimumFftOrder)
        && transform.fftSize <= maximumFftSize
        && (transform.fftSize & (transform.fftSize - 1U)) == 0;
    const auto expectedBinCount = (transform.fftSize / 2U) + 1U;
    if (transform.powerBins == nullptr || transform.captureGeneration == 0
        || transform.fftGeneration == 0 || transform.rawTransformSequence == 0
        || transform.resetEpoch == 0 || transform.capturedFrameEnd == 0
        || !std::isfinite(transform.sampleRate) || transform.sampleRate <= 0.0 || !supportedFftSize
        || transform.binCount != expectedBinCount || transform.binCount > maximumSpectrumBinCount
        || transform.hopSizeFrames == 0 || transform.requestedSliceRateHz == 0
        || columnSequence == 0) {
        return false;
    }

    if (!mappingValid_ || mappedFftSize_ != transform.fftSize
        || mappedBinCount_ != transform.binCount
        || sampleRatesDiffer(mappedSampleRate_, transform.sampleRate)) {
        if (!rebuildMapping(transform))
            return false;
    }

    destination.decibels.fill(minimumSpectrumDecibels);
    for (std::size_t row = 0; row < rowCount_; ++row) {
        const auto& mapping = rows_[row];
        auto rowPower = 0.0F;

        if (mapping.firstBin < mapping.onePastLastBin) {
            for (auto bin = mapping.firstBin; bin < mapping.onePastLastBin; ++bin)
                rowPower = std::max(rowPower, sanitisePower(transform.powerBins[bin]));
        } else {
            const auto lower = sanitisePower(transform.powerBins[mapping.interpolationLowerBin]);
            const auto upper = sanitisePower(transform.powerBins[mapping.interpolationUpperBin]);
            rowPower = lower + (mapping.interpolationWeight * (upper - lower));
        }

        destination.decibels[row] = powerToDecibels(rowPower);
    }

    destination.sequence = columnSequence;
    destination.rawTransformSequence = transform.rawTransformSequence;
    destination.captureGeneration = transform.captureGeneration;
    destination.fftGeneration = transform.fftGeneration;
    destination.mappingGeneration = mappingGeneration_;
    destination.resetEpoch = transform.resetEpoch;
    destination.capturedFrameEnd = transform.capturedFrameEnd;
    destination.sampleRate = transform.sampleRate;
    destination.fftSize = transform.fftSize;
    destination.binCount = static_cast<std::uint32_t>(transform.binCount);
    destination.rowCount = static_cast<std::uint32_t>(rowCount_);
    destination.hopSizeFrames = transform.hopSizeFrames;
    destination.requestedSliceRateHz = transform.requestedSliceRateHz;
    destination.resetMarker = false;
    destination.mappingSeed = mappingSeed;
    return true;
}

bool SpectrogramColumnMapper::rebuildMapping(const SpectrumTransformView& transform) noexcept
{
    mappingValid_ = false;
    rowCount_ = 0;

    const auto fftSizeAsDouble = static_cast<double>(transform.fftSize);
    const auto binWidth = transform.sampleRate / fftSizeAsDouble;
    const auto maximumFrequency = std::min(20'000.0, transform.sampleRate * 0.5);
    if (!std::isfinite(binWidth) || binWidth <= 0.0 || maximumFrequency <= 20.0)
        return false;

    const auto maximumBin = transform.binCount - 1;
    const auto firstUsableBin
        = std::min<std::size_t>(maximumBin, static_cast<std::size_t>(std::ceil(20.0 / binWidth)));
    const auto lastUsableBin = std::min<std::size_t>(
        maximumBin, static_cast<std::size_t>(std::floor(maximumFrequency / binWidth)));
    if (lastUsableBin < firstUsableBin)
        return false;

    const auto usableBinCount = (lastUsableBin - firstUsableBin) + 1;
    rowCount_ = std::min(maximumSpectrogramRowCount, usableBinCount);
    if (rowCount_ == 0)
        return false;

    minimumFrequencyHz_ = 20.0;
    maximumFrequencyHz_ = maximumFrequency;
    rows_.fill(RowMapping { });
    for (std::size_t bin = firstUsableBin; bin <= lastUsableBin; ++bin) {
        const auto frequency = static_cast<double>(bin) * binWidth;
        const auto unit = std::clamp(mapFrequencyToUnit(frequency), 0.0, 1.0);
        const auto row = std::min(rowCount_ - 1,
            static_cast<std::size_t>(std::floor(unit * static_cast<double>(rowCount_))));
        auto& mapping = rows_[row];
        if (mapping.firstBin == mapping.onePastLastBin) {
            mapping.firstBin = static_cast<std::uint32_t>(bin);
            mapping.onePastLastBin = static_cast<std::uint32_t>(bin + 1);
        } else {
            mapping.onePastLastBin = static_cast<std::uint32_t>(bin + 1);
        }
    }

    for (std::size_t row = 0; row < rowCount_; ++row) {
        auto& mapping = rows_[row];
        if (mapping.firstBin < mapping.onePastLastBin)
            continue;

        const auto centreUnit = (static_cast<double>(row) + 0.5) / static_cast<double>(rowCount_);
        const auto centreFrequency = inverseMapUnitToFrequency(centreUnit);
        const auto fractionalBin
            = std::clamp(centreFrequency / binWidth, 0.0, static_cast<double>(maximumBin));
        const auto lowerBin = static_cast<std::size_t>(std::floor(fractionalBin));
        const auto upperBin = static_cast<std::size_t>(std::ceil(fractionalBin));
        mapping.interpolationLowerBin = static_cast<std::uint32_t>(lowerBin);
        mapping.interpolationUpperBin = static_cast<std::uint32_t>(upperBin);
        mapping.interpolationWeight = static_cast<float>(fractionalBin - lowerBin);
    }

    mappedSampleRate_ = transform.sampleRate;
    mappedFftSize_ = transform.fftSize;
    mappedBinCount_ = transform.binCount;
    mappingValid_ = true;
    return true;
}

double SpectrogramColumnMapper::mapFrequencyToUnit(const double frequencyHz) const noexcept
{
    const auto bounded = std::clamp(frequencyHz, minimumFrequencyHz_, maximumFrequencyHz_);
    const auto linear
        = (bounded - minimumFrequencyHz_) / (maximumFrequencyHz_ - minimumFrequencyHz_);
    const auto logarithmic = std::log(bounded / minimumFrequencyHz_)
        / std::log(maximumFrequencyHz_ / minimumFrequencyHz_);
    return ((1.0 - frequencySpacing_) * linear) + (frequencySpacing_ * logarithmic);
}

double SpectrogramColumnMapper::inverseMapUnitToFrequency(const double unit) const noexcept
{
    const auto boundedUnit = std::clamp(unit, 0.0, 1.0);
    if (frequencySpacing_ <= 0.0)
        return minimumFrequencyHz_ + (boundedUnit * (maximumFrequencyHz_ - minimumFrequencyHz_));

    if (frequencySpacing_ >= 1.0) {
        return minimumFrequencyHz_
            * std::exp(boundedUnit * std::log(maximumFrequencyHz_ / minimumFrequencyHz_));
    }

    auto lower = minimumFrequencyHz_;
    auto upper = maximumFrequencyHz_;
    for (std::size_t iteration = 0; iteration < 40; ++iteration) {
        const auto midpoint = (lower + upper) * 0.5;
        if (mapFrequencyToUnit(midpoint) < boundedUnit)
            lower = midpoint;
        else
            upper = midpoint;
    }
    return (lower + upper) * 0.5;
}

float SpectrogramColumnMapper::powerToDecibels(const float power) noexcept
{
    constexpr auto floorPower = 1.0e-18F;
    const auto finitePower = sanitisePower(power);
    if (finitePower <= floorPower)
        return minimumSpectrumDecibels;

    return std::max(minimumSpectrumDecibels, 10.0F * std::log10(finitePower));
}
} // namespace audio_insight
