// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MetalVisualization.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include <os/lock.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace audio_insight::detail {
class MetalRenderBackend;
}

@interface AIAudioInsightMetalView : MTKView <CAMetalDisplayLinkDelegate> {
@private
    audio_insight::detail::MetalRenderBackend* renderBackend;
    NSWindow* observedWindow;
    CAMetalDisplayLink* metalDisplayLink;
}

- (void)attachRenderBackend:(audio_insight::detail::MetalRenderBackend*)backend;
- (void)detachRenderBackend;
- (void)setDisplayLinkPaused:(BOOL)shouldBePaused;
- (void)setDisplayLinkMaximumFramesPerSecond:(NSInteger)maximumFramesPerSecond;
- (BOOL)hasDisplayLink;
- (BOOL)performPeakRmsClearAccessibilityAction;

@end

namespace audio_insight::detail {
float mapFrequencyToUnit(const FrequencyAxisMapping& mapping, const float frequencyHz) noexcept
{
    if (!std::isfinite(mapping.minimumFrequencyHz) || !std::isfinite(mapping.maximumFrequencyHz)
        || mapping.minimumFrequencyHz <= 0.0F
        || mapping.maximumFrequencyHz <= mapping.minimumFrequencyHz) {
        return 0.0F;
    }

    const auto frequency
        = std::clamp(std::isfinite(frequencyHz) ? frequencyHz : mapping.minimumFrequencyHz,
            mapping.minimumFrequencyHz, mapping.maximumFrequencyHz);
    const auto spacing
        = std::clamp(std::isfinite(mapping.spacing) ? mapping.spacing : 1.0F, 0.0F, 1.0F);
    const auto linear = (frequency - mapping.minimumFrequencyHz)
        / (mapping.maximumFrequencyHz - mapping.minimumFrequencyHz);
    const auto logarithmic = std::log(frequency / mapping.minimumFrequencyHz)
        / std::log(mapping.maximumFrequencyHz / mapping.minimumFrequencyHz);
    return std::clamp(((1.0F - spacing) * linear) + (spacing * logarithmic), 0.0F, 1.0F);
}

std::size_t formatFrequencyAxisLabel(
    const float frequencyHz, std::array<char, frequencyAxisLabelStorage>& destination) noexcept
{
    destination.fill('\0');

    if (!std::isfinite(frequencyHz) || frequencyHz < 0.0F
        || frequencyHz > frequencyAxisTickCandidates.back()) {
        return 0;
    }

    const auto roundedFrequency = static_cast<int>(
        std::lround(std::clamp(frequencyHz, 0.0F, frequencyAxisTickCandidates.back())));
    auto* cursor = destination.data();
    auto* const end = destination.data() + maximumFrequencyAxisLabelGlyphs;
    const auto appendCharacter = [&](const char character) noexcept {
        if (cursor >= end)
            return false;

        *cursor++ = character;
        return true;
    };
    const auto appendLiteral = [&](const std::string_view literal) noexcept {
        for (const auto character : literal) {
            if (!appendCharacter(character))
                return false;
        }

        return true;
    };

    if (roundedFrequency < 1'000) {
        const auto conversion = std::to_chars(cursor, end, roundedFrequency);
        if (conversion.ec != std::errc { })
            return 0;

        cursor = conversion.ptr;
        if (!appendLiteral(" Hz"))
            return 0;
    } else {
        const auto kilohertz = roundedFrequency / 1'000;
        auto remainder = roundedFrequency % 1'000;
        const auto conversion = std::to_chars(cursor, end, kilohertz);
        if (conversion.ec != std::errc { })
            return 0;

        cursor = conversion.ptr;
        if (remainder != 0) {
            if (!appendCharacter('.'))
                return 0;

            const std::array<char, 3> fractionalDigits {
                static_cast<char>('0' + (remainder / 100)),
                static_cast<char>('0' + ((remainder / 10) % 10)),
                static_cast<char>('0' + (remainder % 10)),
            };
            auto fractionalCount = fractionalDigits.size();
            while (fractionalCount > 0 && fractionalDigits[fractionalCount - 1] == '0')
                --fractionalCount;

            for (std::size_t index = 0; index < fractionalCount; ++index) {
                if (!appendCharacter(fractionalDigits[index]))
                    return 0;
            }
        }

        if (!appendLiteral(" kHz"))
            return 0;
    }

    *cursor = '\0';
    return static_cast<std::size_t>(cursor - destination.data());
}

FrequencyAxisTickSelection selectFrequencyAxisTicks(const FrequencyAxisMapping& mapping,
    const float axisLength, const std::array<float, frequencyAxisTickCandidateCount>& labelExtents,
    const float upperEndpointLabelExtent, const float minimumGap) noexcept
{
    FrequencyAxisTickSelection result;

    if (!std::isfinite(axisLength) || axisLength <= 0.0F
        || !std::isfinite(mapping.minimumFrequencyHz) || !std::isfinite(mapping.maximumFrequencyHz)
        || mapping.maximumFrequencyHz <= mapping.minimumFrequencyHz) {
        return result;
    }

    const auto safeGap = std::max(0.0F, std::isfinite(minimumGap) ? minimumGap : 0.0F);
    struct Candidate final {
        float frequencyHz = 0.0F;
        float intervalStart = 0.0F;
        float intervalEnd = 0.0F;
        std::size_t candidateIndex = frequencyAxisTickCandidateCount;
        bool usesUpperEndpointLabel = false;
        bool selected = false;
    };
    std::array<Candidate, maximumFrequencyAxisTickCount> candidates { };
    std::size_t candidateCount = 0;

    const auto appendCandidate
        = [&](const float frequency, const float extent, const std::size_t candidateIndex,
              const bool usesUpperEndpointLabel) noexcept {
              if (candidateCount >= candidates.size() || !std::isfinite(extent) || extent <= 0.0F
                  || extent > axisLength) {
                  return;
              }

              const auto centre = mapFrequencyToUnit(mapping, frequency) * axisLength;
              const auto start = std::clamp(centre - (extent * 0.5F), 0.0F, axisLength - extent);
              candidates[candidateCount++] = {
                  frequency,
                  start,
                  start + extent,
                  candidateIndex,
                  usesUpperEndpointLabel,
                  false,
              };
          };

    for (std::size_t index = 0; index < frequencyAxisTickCandidates.size(); ++index) {
        const auto frequency = frequencyAxisTickCandidates[index];
        if (frequency < mapping.minimumFrequencyHz || frequency > mapping.maximumFrequencyHz)
            continue;

        appendCandidate(frequency, labelExtents[index], index, false);
    }

    const auto lastCandidateMatchesEndpoint = candidateCount != 0
        && std::abs(candidates[candidateCount - 1].frequencyHz - mapping.maximumFrequencyHz)
            <= std::max(0.01F, mapping.maximumFrequencyHz * 0.000001F);
    if (!lastCandidateMatchesEndpoint) {
        appendCandidate(mapping.maximumFrequencyHz, upperEndpointLabelExtent,
            frequencyAxisTickCandidateCount, true);
    }

    const auto trySelect = [&](const std::size_t candidate) noexcept {
        if (candidate >= candidateCount || candidates[candidate].selected)
            return;

        for (std::size_t selected = 0; selected < candidateCount; ++selected) {
            if (!candidates[selected].selected)
                continue;

            const auto separated
                = candidates[candidate].intervalEnd + safeGap <= candidates[selected].intervalStart
                || candidates[selected].intervalEnd + safeGap
                    <= candidates[candidate].intervalStart;
            if (!separated)
                return;
        }

        candidates[candidate].selected = true;
    };

    if (candidateCount == 0)
        return result;

    trySelect(0);
    trySelect(candidateCount - 1);

    // Prefer decade labels before the remaining 2x and 5x multiples. Endpoints
    // above were already reserved, so narrow panels retain useful anchors at
    // both ends rather than allowing a low-frequency greedy pass to crowd out
    // the upper scale.
    constexpr std::array<std::size_t, frequencyAxisTickCandidateCount> priority {
        2,
        5,
        8,
        3,
        6,
        9,
        0,
        1,
        4,
        7,
    };
    for (const auto candidateIndex : priority) {
        for (std::size_t candidate = 0; candidate < candidateCount; ++candidate) {
            if (candidates[candidate].candidateIndex == candidateIndex)
                trySelect(candidate);
        }
    }

    for (std::size_t candidate = 0; candidate < candidateCount; ++candidate) {
        if (!candidates[candidate].selected)
            continue;

        result.ticks[result.count++] = { candidates[candidate].frequencyHz,
            candidates[candidate].candidateIndex, candidates[candidate].usesUpperEndpointLabel };
    }

    return result;
}

int chooseSpectrumDecibelTickStep(
    const float floorDecibels, const float ceilingDecibels, const float axisLength) noexcept
{
    if (!std::isfinite(floorDecibels) || !std::isfinite(ceilingDecibels)
        || ceilingDecibels <= floorDecibels || !std::isfinite(axisLength) || axisLength <= 0.0F) {
        return spectrumDecibelTickSteps.back();
    }

    const auto span = ceilingDecibels - floorDecibels;
    for (const auto step : spectrumDecibelTickSteps) {
        if ((axisLength * static_cast<float>(step) / span) >= minimumSpectrumDecibelLabelSpacing) {
            return step;
        }
    }

    return spectrumDecibelTickSteps.back();
}

SpectrumDecibelTicks makeSpectrumDecibelTicks(
    const float floorDecibels, const float ceilingDecibels, const float axisLength) noexcept
{
    SpectrumDecibelTicks result;

    if (!std::isfinite(floorDecibels) || !std::isfinite(ceilingDecibels)
        || !std::isfinite(axisLength) || axisLength <= 0.0F) {
        return result;
    }

    const auto floor = std::clamp(floorDecibels, static_cast<float>(minimumCachedDecibelTick),
        static_cast<float>(maximumCachedDecibelTick));
    const auto ceiling = std::clamp(ceilingDecibels, static_cast<float>(minimumCachedDecibelTick),
        static_cast<float>(maximumCachedDecibelTick));

    if (ceiling <= floor)
        return result;

    result.candidateStep = chooseSpectrumDecibelTickStep(floor, ceiling, axisLength);
    const auto rawSpacing
        = axisLength * static_cast<float>(result.candidateStep) / (ceiling - floor);
    const auto labelStride = rawSpacing > 0.0F
        ? std::max(1, static_cast<int>(std::ceil(minimumSpectrumDecibelLabelSpacing / rawSpacing)))
        : 1;
    result.displayedStep = result.candidateStep * labelStride;
    auto decibels = static_cast<int>(std::ceil(floor / static_cast<float>(result.displayedStep)))
        * result.displayedStep;

    while (decibels <= ceiling && result.count < result.values.size()) {
        result.values[result.count++] = decibels;
        decibels += result.displayedStep;
    }

    return result;
}

float mapPeakRmsDecibelsToUnit(const float decibels) noexcept
{
    const auto value = std::isfinite(decibels) ? decibels : peakRmsMinimumDecibels;
    return (std::clamp(value, peakRmsMinimumDecibels, peakRmsMaximumDecibels)
               - peakRmsMinimumDecibels)
        / (peakRmsMaximumDecibels - peakRmsMinimumDecibels);
}

PeakRmsReadout classifyPeakRmsReadout(const float decibels) noexcept
{
    PeakRmsReadout result;

    if (!std::isfinite(decibels) || decibels <= minimumDisplayDecibels)
        return result;

    result.kind = PeakRmsReadout::Kind::decibelTenths;
    const auto bounded = std::clamp(
        decibels, -119.9F, static_cast<float>(maximumFiniteFloatPeakRmsReadoutTenths) * 0.1F);
    result.decibelTenths = std::clamp(static_cast<int>(std::lround(bounded * 10.0F)), -1'199,
        maximumFiniteFloatPeakRmsReadoutTenths);

    if (decibels >= 0.0F)
        result.levelRange = PeakRmsLevelRange::red;
    else if (decibels >= -6.0F)
        result.levelRange = PeakRmsLevelRange::amber;

    return result;
}

float fitPeakRmsTextScale(
    const float availableWidth, const float unscaledTextWidth, const float preferredScale) noexcept
{
    if (!std::isfinite(availableWidth) || !std::isfinite(unscaledTextWidth)
        || !std::isfinite(preferredScale) || availableWidth <= 0.0F || unscaledTextWidth <= 0.0F
        || preferredScale <= 0.0F) {
        return 0.0F;
    }

    return std::min(preferredScale, availableWidth / unscaledTextWidth);
}

PeakRmsTickLabelSelection selectPeakRmsTickLabels(
    const float axisLength, const float labelHeight, const float minimumGap) noexcept
{
    PeakRmsTickLabelSelection result;

    if (!std::isfinite(axisLength) || axisLength <= 0.0F || !std::isfinite(labelHeight)
        || labelHeight <= 0.0F || labelHeight > axisLength) {
        return result;
    }

    const auto gap = std::max(0.0F, std::isfinite(minimumGap) ? minimumGap : 0.0F);
    std::array<float, peakRmsMajorDecibelTicks.size()> starts { };
    std::array<float, peakRmsMajorDecibelTicks.size()> ends { };
    constexpr std::array<std::size_t, peakRmsMajorDecibelTicks.size()> priority {
        0,
        7,
        6,
        5,
        4,
        3,
        2,
        1,
    };

    for (const auto candidate : priority) {
        const auto centre
            = mapPeakRmsDecibelsToUnit(static_cast<float>(peakRmsMajorDecibelTicks[candidate]))
            * axisLength;
        const auto start
            = std::clamp(centre - (labelHeight * 0.5F), 0.0F, axisLength - labelHeight);
        const auto end = start + labelHeight;
        auto overlaps = false;

        for (std::size_t index = 0; index < result.visible.size(); ++index) {
            if (!result.visible[index])
                continue;

            if (start < ends[index] + gap && end + gap > starts[index]) {
                overlaps = true;
                break;
            }
        }

        if (!overlaps) {
            result.visible[candidate] = true;
            starts[candidate] = start;
            ends[candidate] = end;
        }
    }

    return result;
}

PeakRmsPanelLayout calculatePeakRmsPanelLayout(const float panelWidth, const float panelHeight,
    const float requestedHeaderHeight, const std::uint32_t requestedChannelCount,
    const float requestedTextHeight, const float requestedMaximumTickLabelWidth,
    const float requestedMaximumReadoutWidth) noexcept
{
    PeakRmsPanelLayout result;

    if (!std::isfinite(panelWidth) || !std::isfinite(panelHeight) || panelWidth <= 0.0F
        || panelHeight <= 0.0F) {
        return result;
    }

    const auto headerHeight = std::clamp(
        std::isfinite(requestedHeaderHeight) ? requestedHeaderHeight : 0.0F, 0.0F, panelHeight);
    const auto textHeight
        = std::clamp(std::isfinite(requestedTextHeight) ? requestedTextHeight : 10.0F, 6.0F, 24.0F);
    const auto maximumTickLabelWidth = std::max(0.0F,
        std::isfinite(requestedMaximumTickLabelWidth) ? requestedMaximumTickLabelWidth : 0.0F);
    const auto maximumReadoutWidth = std::max(
        0.0F, std::isfinite(requestedMaximumReadoutWidth) ? requestedMaximumReadoutWidth : 0.0F);
    result.channelCount
        = requestedChannelCount == 1 || requestedChannelCount == 2 ? requestedChannelCount : 0;

    if (panelWidth >= 68.0F && headerHeight >= 14.0F) {
        constexpr auto visualWidth = 45.0F;
        const auto visualBottom = panelHeight - headerHeight + 3.0F;
        result.clearVisualBounds = { std::max(4.0F, panelWidth - visualWidth - 5.0F), visualBottom,
            panelWidth - 5.0F, panelHeight - 3.0F };
        const auto hitHeight = std::min(panelHeight, std::max(24.0F, headerHeight));
        result.clearHitBounds = {
            std::max(0.0F, result.clearVisualBounds.left - 7.0F),
            panelHeight - hitHeight,
            panelWidth,
            panelHeight,
        };
    }

    constexpr auto contentInset = 6.0F;
    const auto contentLeft = std::min(contentInset, panelWidth * 0.5F);
    const auto contentRight = std::max(contentLeft, panelWidth - contentInset);
    const auto contentBottom = std::min(contentInset, panelHeight);
    const auto contentTop = std::max(contentBottom, panelHeight - headerHeight - contentInset);
    const auto contentWidth = contentRight - contentLeft;
    const auto contentHeight = contentTop - contentBottom;

    if (contentWidth <= 0.0F || contentHeight <= 0.0F)
        return result;

    const auto desiredScaleLane = maximumTickLabelWidth + 7.0F;
    result.showTickLabels = desiredScaleLane > 7.0F && contentWidth >= desiredScaleLane + 28.0F;
    const auto scaleLane = result.showTickLabels ? desiredScaleLane : 4.0F;
    const auto availableLeft = std::min(contentRight, contentLeft + scaleLane);
    const auto availableWidth = std::max(0.0F, contentRight - availableLeft);
    const auto groupMaximum = result.channelCount == 1 ? 54.0F : 98.0F;
    const auto groupWidth = std::min(availableWidth, groupMaximum);
    const auto groupLeft = availableLeft + ((availableWidth - groupWidth) * 0.5F);
    const auto groupRight = groupLeft + groupWidth;
    const auto layoutChannelCount = std::max<std::size_t>(1, result.channelCount);
    const auto columnWidth = groupWidth / static_cast<float>(layoutChannelCount);

    result.channelLabelBottom = contentBottom;
    result.overBottom = std::max(contentBottom, contentTop - textHeight);
    const auto scaleBottom = contentBottom + textHeight + 3.0F;
    const auto scaleTopWithoutReadout = result.overBottom - 3.0F;
    const auto scaleTopWithReadout = scaleTopWithoutReadout - textHeight - 2.0F;
    result.showReadouts = result.channelCount != 0 && columnWidth >= maximumReadoutWidth + 2.0F
        && scaleTopWithReadout - scaleBottom >= 42.0F;
    result.scaleBottom = std::min(scaleBottom, contentTop);
    result.scaleTop = std::max(
        result.scaleBottom, result.showReadouts ? scaleTopWithReadout : scaleTopWithoutReadout);
    result.readoutBottom = result.scaleTop + 2.0F;
    result.tickLabelRight = groupLeft - 5.0F;
    result.tickLineLeft = std::max(contentLeft, groupLeft - 4.0F);
    result.tickLineRight = groupRight;

    if (result.channelCount == 0 || groupWidth <= 0.0F || result.scaleTop <= result.scaleBottom)
        return result;

    for (std::size_t channel = 0; channel < result.channelCount; ++channel) {
        const auto columnLeft = groupLeft + (static_cast<float>(channel) * columnWidth);
        const auto columnRight = columnLeft + columnWidth;
        result.channelColumns[channel]
            = { columnLeft, result.scaleBottom, columnRight, result.scaleTop };
        const auto trackWidth = std::min(
            columnWidth, std::clamp(columnWidth * 0.56F, std::min(10.0F, columnWidth), 32.0F));
        const auto trackLeft = columnLeft + ((columnWidth - trackWidth) * 0.5F);
        result.channelTracks[channel]
            = { trackLeft, result.scaleBottom, trackLeft + trackWidth, result.scaleTop };
    }

    return result;
}

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t renderBufferCount = 3;
constexpr std::size_t maximumVertexCount = MetalVisualizationGeometryLimits::vertexCapacity;
constexpr std::size_t printableAsciiFirst = 32;
constexpr std::size_t printableAsciiLast = 126;
constexpr std::size_t printableAsciiCount = printableAsciiLast - printableAsciiFirst + 1;
constexpr std::size_t infinityGlyphAtlasIndex = printableAsciiCount;
constexpr std::size_t glyphAtlasGlyphCount = printableAsciiCount + 1;
constexpr std::size_t glyphAtlasColumns = 16;
constexpr std::size_t glyphAtlasRows
    = (glyphAtlasGlyphCount + glyphAtlasColumns - 1) / glyphAtlasColumns;
constexpr std::size_t maximumCachedTextGlyphs = 24;
constexpr std::size_t cachedFixedTextRunCount = 11;

constexpr std::array<std::string_view, cachedFixedTextRunCount> cachedFixedTextStrings { "Spectrum",
    "Peak / RMS", "Spectrogram", "Stereo / Correlation", "Loudness", "Not yet implemented", "CLEAR",
    "L", "R", "M", "OVER" };

constexpr std::array<std::string_view, frequencyAxisTickCandidateCount>
    cachedFrequencyAxisTextStrings { "20 Hz", "50 Hz", "100 Hz", "200 Hz", "500 Hz", "1 kHz",
        "2 kHz", "5 kHz", "10 kHz", "20 kHz" };

constexpr std::array<std::string_view, peakRmsMajorDecibelTicks.size()>
    cachedPeakRmsTickTextStrings { "-60", "-48", "-36", "-24", "-12", "-6", "0", "+3" };

constexpr std::array<std::size_t, dashboardPanelCount> panelTitleTextRunIndices { 0, 1, 2, 3, 4 };
constexpr std::size_t placeholderTextRunIndex = 5;
constexpr std::size_t clearTextRunIndex = 6;
constexpr std::size_t leftChannelTextRunIndex = 7;
constexpr std::size_t rightChannelTextRunIndex = 8;
constexpr std::size_t monoChannelTextRunIndex = 9;
constexpr std::size_t overTextRunIndex = 10;
constexpr int minimumPeakRmsReadoutTenths = -1'199;
constexpr int maximumPeakRmsReadoutTenths = maximumFiniteFloatPeakRmsReadoutTenths;
constexpr std::size_t cachedPeakRmsReadoutCount
    = static_cast<std::size_t>(maximumPeakRmsReadoutTenths - minimumPeakRmsReadoutTenths + 1);

// The fixed capacity covers five filled/bordered/header-divided tiles, both
// numeric frequency axes, every possible decibel tick, the maximum FFT trace,
// both live meter channels, and every cached text run. Keep this proof beside
// the allocation so a future builder cannot silently truncate a frame.
constexpr std::size_t fixedTextGlyphCount = [] {
    std::size_t glyphCount = 0;

    for (const auto titleIndex : panelTitleTextRunIndices)
        glyphCount += cachedFixedTextStrings[titleIndex].size();

    constexpr auto placeholderPanelCount = dashboardPanelCount - 2;
    return glyphCount
        + (placeholderPanelCount * cachedFixedTextStrings[placeholderTextRunIndex].size());
}();
constexpr std::size_t peakRmsTextGlyphCount = [] {
    std::size_t glyphCount = cachedFixedTextStrings[clearTextRunIndex].size();
    glyphCount += 2;
    glyphCount += 2 * cachedFixedTextStrings[overTextRunIndex].size();
    glyphCount += 2 * maximumPeakRmsReadoutGlyphs;

    for (const auto text : cachedPeakRmsTickTextStrings)
        glyphCount += text.size();

    return glyphCount;
}();
static_assert(fixedTextGlyphCount == MetalVisualizationGeometryLimits::maximumFixedTextGlyphs);
static_assert(peakRmsTextGlyphCount == MetalVisualizationGeometryLimits::maximumPeakRmsTextGlyphs);
static_assert(MetalVisualizationGeometryLimits::maximumGeneratedVertices <= maximumVertexCount);
static_assert([] {
    for (const auto text : cachedFixedTextStrings) {
        if (text.size() > maximumCachedTextGlyphs)
            return false;
    }

    for (const auto text : cachedFrequencyAxisTextStrings) {
        if (text.size() > maximumCachedTextGlyphs)
            return false;
    }

    for (const auto text : cachedPeakRmsTickTextStrings) {
        if (text.size() > maximumCachedTextGlyphs)
            return false;
    }

    return true;
}());

constexpr float minimumSpectrumFrequency = 20.0F;
constexpr float maximumSpectrumFrequency = 20'000.0F;
constexpr float minimumAllowedSpectrumFloor = -180.0F;
constexpr float maximumAllowedSpectrumFloor = -36.0F;
constexpr float minimumAllowedSpectrumCeiling = -24.0F;
constexpr float maximumAllowedSpectrumCeiling = 12.0F;
constexpr float minimumSpectrumRange = 24.0F;

struct MetalVertex {
    simd_float2 position;
    simd_float4 colour;
    simd_float2 textureCoordinate;
};

static_assert(offsetof(MetalVertex, position) == 0);
static_assert(offsetof(MetalVertex, colour) == 16);
static_assert(offsetof(MetalVertex, textureCoordinate) == 32);
static_assert(sizeof(MetalVertex) == 48);

struct GlyphAtlasEntry {
    float leftTextureCoordinate = 0.0F;
    float bottomTextureCoordinate = 0.0F;
    float rightTextureCoordinate = 0.0F;
    float topTextureCoordinate = 0.0F;
};

struct CachedTextGlyph {
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    GlyphAtlasEntry atlas;
};

struct CachedTextRun {
    std::array<CachedTextGlyph, maximumCachedTextGlyphs> glyphs { };
    std::size_t glyphCount = 0;
    float width = 0.0F;
    float height = 0.0F;
};

struct CachedMonospacedTextRun {
    std::array<std::uint8_t, maximumFrequencyAxisLabelGlyphs> atlasIndices { };
    std::uint8_t glyphCount = 0;
};

std::uint8_t atlasIndexForAscii(const char character) noexcept
{
    const auto value = static_cast<unsigned char>(character);
    return static_cast<std::uint8_t>(value >= printableAsciiFirst && value <= printableAsciiLast
            ? value - printableAsciiFirst
            : static_cast<unsigned char>('?' - printableAsciiFirst));
}

const std::array<CachedMonospacedTextRun, 20'001>& cachedFrequencyEndpointTextRuns() noexcept
{
    // This density-independent table is built once during the first glyph-atlas
    // initialization, never lazily from a display callback. It makes every
    // integer-Hz upper endpoint through 20 kHz available without per-instance
    // strings or callback-time formatting.
    static const auto runs = [] {
        std::array<CachedMonospacedTextRun, 20'001> result { };

        for (std::size_t frequency = 0; frequency < result.size(); ++frequency) {
            std::array<char, frequencyAxisLabelStorage> label { };
            const auto labelLength = formatFrequencyAxisLabel(static_cast<float>(frequency), label);
            auto& run = result[frequency];
            run.glyphCount = static_cast<std::uint8_t>(labelLength);

            for (std::size_t index = 0; index < labelLength; ++index)
                run.atlasIndices[index] = atlasIndexForAscii(label[index]);
        }

        return result;
    }();

    return runs;
}

const std::array<CachedMonospacedTextRun, cachedPeakRmsReadoutCount>&
cachedPeakRmsReadoutTextRuns() noexcept
{
    // Every finite one-decimal readout is prepared before rendering begins.
    // The display callback only classifies, rounds, and indexes this table.
    static const auto runs = [] {
        std::array<CachedMonospacedTextRun, cachedPeakRmsReadoutCount> result { };

        for (std::size_t index = 0; index < result.size(); ++index) {
            const auto tenths = minimumPeakRmsReadoutTenths + static_cast<int>(index);
            const auto absoluteTenths = std::abs(tenths);
            std::array<char, maximumPeakRmsReadoutGlyphs> text { };
            auto* cursor = text.data();
            auto* const end = text.data() + text.size();

            if (tenths < 0)
                *cursor++ = '-';
            else if (tenths > 0)
                *cursor++ = '+';

            const auto conversion = std::to_chars(cursor, end, absoluteTenths / 10);
            jassert(conversion.ec == std::errc { });

            if (conversion.ec != std::errc { })
                continue;

            cursor = conversion.ptr;
            jassert(end - cursor >= 2);

            if (end - cursor < 2)
                continue;

            *cursor++ = '.';
            *cursor++ = static_cast<char>('0' + (absoluteTenths % 10));
            auto& run = result[index];
            run.glyphCount = static_cast<std::uint8_t>(cursor - text.data());

            for (std::size_t glyph = 0; glyph < run.glyphCount; ++glyph)
                run.atlasIndices[glyph] = atlasIndexForAscii(text[glyph]);
        }

        return result;
    }();

    return runs;
}

const CachedMonospacedTextRun& cachedMinusInfinityTextRun() noexcept
{
    static const auto run = [] {
        CachedMonospacedTextRun result;
        result.glyphCount = 2;
        result.atlasIndices[0] = atlasIndexForAscii('-');
        result.atlasIndices[1] = static_cast<std::uint8_t>(infinityGlyphAtlasIndex);
        return result;
    }();

    return run;
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

struct AtomicRenderTelemetry {
    std::uint64_t epoch = 1;
    std::atomic<std::uint64_t> displayLinkCallbacks { 0 };
    std::atomic<std::uint64_t> submittedFrames { 0 };
    std::atomic<std::uint64_t> completedFrames { 0 };
    std::atomic<std::uint64_t> gpuTimingSamples { 0 };
    std::atomic<std::uint64_t> gpuTimingUnavailableSamples { 0 };
    std::atomic<std::uint64_t> commandBufferFailures { 0 };
    std::atomic<std::uint64_t> presentationCallbacks { 0 };
    std::atomic<std::uint64_t> presentedFrames { 0 };
    std::atomic<std::uint64_t> presentationLatenessSamples { 0 };
    std::atomic<std::uint64_t> presentationLatenessUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> presentationHistoryDiscardedTimestamps { 0 };
    std::atomic<std::uint64_t> frameLatencySamples { 0 };
    std::atomic<std::uint64_t> frameLatencyTotalTimingSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyTotalTimingUnavailableSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyComponentTimingSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyComponentTimingUnavailableSamples { 0 };
    std::atomic<std::uint64_t> frameLatencyHistoryDiscardedSamples { 0 };
    std::atomic<std::uint64_t> presentationsAfterTarget { 0 };
    std::atomic<std::uint64_t> skippedPresentations { 0 };
    std::atomic<std::uint64_t> gpuBackpressureDrops { 0 };
    std::atomic<std::uint64_t> drawableUnavailableDrops { 0 };
    std::atomic<std::uint64_t> callbackHostDelaySamples { 0 };
    std::atomic<std::uint64_t> callbackHostDelayUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> callbackAlreadyLateHostDelays { 0 };
    std::atomic<std::uint64_t> cpuCommitLatenessSamples { 0 };
    std::atomic<std::uint64_t> cpuCommitLatenessUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> cpuCommitDeadlineMisses { 0 };
    std::atomic<std::uint64_t> gpuCompletionLatenessSamples { 0 };
    std::atomic<std::uint64_t> gpuCompletionLatenessUnclassifiableSamples { 0 };
    std::atomic<std::uint64_t> gpuCompletionDeadlineMisses { 0 };
    std::atomic<std::uint64_t> analysisRequestCalls { 0 };
    std::atomic<std::uint64_t> snapshotReads { 0 };
    std::atomic<std::uint64_t> framesWithNewSnapshot { 0 };
    std::atomic<std::uint64_t> lastSpectrumSequence { 0 };

    std::atomic<std::uint64_t> lastCpuEncodeNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumCpuEncodeNanoseconds { 0 };
    std::atomic<std::uint64_t> lastGpuExecutionNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumGpuExecutionNanoseconds { 0 };
    std::atomic<std::uint64_t> lastDisplayCallbackIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetPresentationIntervalNanoseconds { 0 };
    std::atomic<std::uint64_t> lastCallbackHostDelayNanoseconds { 0 };
    std::atomic<std::uint64_t> lastCpuCommitLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastGpuCompletionLatenessNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastTargetPresentationTimestampNanoseconds { 0 };
    std::atomic<std::uint64_t> lastProvidedDrawableAccessNanoseconds { 0 };
    std::atomic<std::uint64_t> maximumProvidedDrawableAccessNanoseconds { 0 };

    // Command-buffer completion handlers may overlap. This lock makes their
    // related timing values and validity counters one coherent UI snapshot.
    mutable os_unfair_lock gpuTelemetryLock = OS_UNFAIR_LOCK_INIT;

    // Drawable presentation handlers are not audio callbacks and may run
    // concurrently or arrive out of timestamp order. A tiny unfair lock keeps
    // the fixed history exact without imposing any work on the audio thread.
    mutable os_unfair_lock presentedFrameHistoryLock = OS_UNFAIR_LOCK_INIT;
    PresentedFrameHistory presentedFrameHistory;
    FrameLatencyHistory frameLatencyHistory;
    std::uint64_t lastPresentationLatenessNanoseconds = 0;
    std::uint64_t lastPresentationLatenessTimestampNanoseconds = 0;
    std::uint64_t maximumPresentationLatenessNanoseconds = 0;

    std::atomic<std::uint32_t> drawableWidthPixels { 0 };
    std::atomic<std::uint32_t> drawableHeightPixels { 0 };
    std::atomic<std::uint32_t> configuredMaximumFramesPerSecond { 0 };
    std::atomic<double> backingScale { 1.0 };

    std::atomic<bool> metalAvailable { false };
    std::atomic<bool> renderingRequested { false };
    std::atomic<bool> effectivelyRendering { false };
};

template <typename Integer>
void updateMaximum(std::atomic<Integer>& destination, Integer candidate) noexcept
{
    auto previous = destination.load(std::memory_order_relaxed);

    while (candidate > previous
        && !destination.compare_exchange_weak(
            previous, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) { }
}

std::uint64_t nanosecondsBetween(Clock::time_point start, Clock::time_point end) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

std::uint64_t hostTimeNanoseconds(CFTimeInterval hostTime) noexcept
{
    if (!std::isfinite(hostTime) || hostTime <= 0.0)
        return 0;

    const auto nanoseconds = hostTime * 1'000'000'000.0;
    const auto bounded = std::min<double>(
        nanoseconds, static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
    return static_cast<std::uint64_t>(std::llround(bounded));
}

std::uint64_t positiveHostTimeDifference(CFTimeInterval later, CFTimeInterval earlier) noexcept
{
    if (!std::isfinite(later) || !std::isfinite(earlier) || later <= 0.0 || earlier <= 0.0
        || later <= earlier) {
        return 0;
    }

    return hostTimeNanoseconds(later - earlier);
}

std::uint32_t roundedPixelDimension(CGFloat value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0;

    const auto bounded = std::min<double>(value, std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(std::llround(bounded));
}

float sanitiseDecibels(float value, float minimum, float maximum) noexcept
{
    if (!std::isfinite(value))
        return minimum;

    return std::clamp(value, minimum, maximum);
}

float smoothingCoefficient(double elapsedSeconds, double timeConstantSeconds) noexcept
{
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0)
        return 1.0F;

    return static_cast<float>(1.0 - std::exp(-elapsedSeconds / timeConstantSeconds));
}

struct RenderBufferSlot {
    // Even values are free. The following odd value identifies a particular
    // in-flight admission, allowing late callbacks to release only their own.
    std::atomic<std::uint64_t> admissionState { 0 };
    id<MTLBuffer> vertexBuffer = nil;

    ~RenderBufferSlot()
    {
        [vertexBuffer release];
    }
};

struct SharedRenderState {
    std::array<RenderBufferSlot, renderBufferCount> slots;
};

struct VertexRange {
    std::size_t start = 0;
    std::size_t count = 0;
};

struct VertexBatches {
    VertexRange shell;
    VertexRange spectrumGrid;
    VertexRange spectrum;
    VertexRange spectrogramAxis;
    VertexRange peakRms;
    std::array<VertexRange, dashboardPanelCount> text;
};

struct RenderRect {
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;

    [[nodiscard]] float width() const noexcept
    {
        return std::max(0.0F, right - left);
    }

    [[nodiscard]] float height() const noexcept
    {
        return std::max(0.0F, top - bottom);
    }
};

struct RenderBufferAdmission {
    std::size_t index = renderBufferCount;
    std::uint64_t busyState = 0;
};

SpectrumRenderSettings sanitiseSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    if (!std::isfinite(settings.floorDecibels))
        settings.floorDecibels = SpectrumRenderSettings { }.floorDecibels;

    if (!std::isfinite(settings.ceilingDecibels))
        settings.ceilingDecibels = SpectrumRenderSettings { }.ceilingDecibels;

    if (!std::isfinite(settings.smoothing))
        settings.smoothing = SpectrumRenderSettings { }.smoothing;

    if (!std::isfinite(settings.frequencySpacing))
        settings.frequencySpacing = SpectrumRenderSettings { }.frequencySpacing;

    settings.floorDecibels = std::clamp(
        settings.floorDecibels, minimumAllowedSpectrumFloor, maximumAllowedSpectrumFloor);
    settings.ceilingDecibels = std::clamp(
        settings.ceilingDecibels, minimumAllowedSpectrumCeiling, maximumAllowedSpectrumCeiling);
    if (settings.ceilingDecibels - settings.floorDecibels < minimumSpectrumRange)
        settings.floorDecibels = settings.ceilingDecibels - minimumSpectrumRange;
    settings.smoothing = std::clamp(settings.smoothing, 0.0F, 1.0F);
    settings.frequencySpacing = std::clamp(settings.frequencySpacing, 0.0F, 1.0F);
    return settings;
}

std::uint64_t packSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    settings = sanitiseSpectrumSettings(settings);
    const auto floorValue = static_cast<std::uint16_t>(
        std::lround((settings.floorDecibels - minimumAllowedSpectrumFloor) * 100.0F));
    const auto ceilingValue = static_cast<std::uint16_t>(
        std::lround((settings.ceilingDecibels - minimumAllowedSpectrumFloor) * 100.0F));
    const auto smoothingValue
        = static_cast<std::uint16_t>(std::lround(settings.smoothing * 65'535.0F));
    const auto frequencySpacingValue
        = static_cast<std::uint16_t>(std::lround(settings.frequencySpacing * 65'535.0F));

    return static_cast<std::uint64_t>(floorValue)
        | (static_cast<std::uint64_t>(ceilingValue) << 16U)
        | (static_cast<std::uint64_t>(smoothingValue) << 32U)
        | (static_cast<std::uint64_t>(frequencySpacingValue) << 48U);
}

SpectrumRenderSettings unpackSpectrumSettings(std::uint64_t packed) noexcept
{
    const auto floorValue = static_cast<std::uint16_t>(packed & 0xffffU);
    const auto ceilingValue = static_cast<std::uint16_t>((packed >> 16U) & 0xffffU);
    const auto smoothingValue = static_cast<std::uint16_t>((packed >> 32U) & 0xffffU);
    const auto frequencySpacingValue = static_cast<std::uint16_t>((packed >> 48U) & 0xffffU);

    return { minimumAllowedSpectrumFloor + (static_cast<float>(floorValue) * 0.01F),
        minimumAllowedSpectrumFloor + (static_cast<float>(ceilingValue) * 0.01F),
        static_cast<float>(smoothingValue) / 65'535.0F,
        static_cast<float>(frequencySpacingValue) / 65'535.0F };
}

std::uint32_t packDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
{
    splits = DashboardLayout::validOrDefault(splits);
    return static_cast<std::uint32_t>(splits.horizontal)
        | (static_cast<std::uint32_t>(splits.upper) << 8U)
        | (static_cast<std::uint32_t>(splits.lowerLeft) << 16U)
        | (static_cast<std::uint32_t>(splits.lowerRight) << 24U);
}

DashboardLayoutSplits unpackDashboardLayoutSplits(const std::uint32_t packed) noexcept
{
    const DashboardLayoutSplits splits { static_cast<int>(packed & 0xffU),
        static_cast<int>((packed >> 8U) & 0xffU), static_cast<int>((packed >> 16U) & 0xffU),
        static_cast<int>((packed >> 24U) & 0xffU) };
    return DashboardLayout::validOrDefault(splits);
}

RenderRect toRenderRect(const DashboardLogicalBounds& bounds, const float logicalHeight) noexcept
{
    return { static_cast<float>(bounds.x), logicalHeight - static_cast<float>(bounds.bottom()),
        static_cast<float>(bounds.right()), logicalHeight - static_cast<float>(bounds.y) };
}

RenderRect insetRenderRect(const RenderRect& bounds, const float left, const float bottom,
    const float right, const float top) noexcept
{
    const auto insetLeft = std::min(std::max(0.0F, left), bounds.width());
    const auto insetRight
        = std::min(std::max(0.0F, right), std::max(0.0F, bounds.width() - insetLeft));
    const auto insetBottom = std::min(std::max(0.0F, bottom), bounds.height());
    const auto insetTop
        = std::min(std::max(0.0F, top), std::max(0.0F, bounds.height() - insetBottom));
    return { bounds.left + insetLeft, bounds.bottom + insetBottom, bounds.right - insetRight,
        bounds.top - insetTop };
}

MTLScissorRect makeScissorRect(const DashboardLogicalBounds& bounds, const CGSize logicalSize,
    const NSUInteger drawableWidth, const NSUInteger drawableHeight) noexcept
{
    if (logicalSize.width <= 0.0 || logicalSize.height <= 0.0 || drawableWidth == 0
        || drawableHeight == 0) {
        return { 0, 0, 0, 0 };
    }

    const auto xScale = static_cast<double>(drawableWidth) / logicalSize.width;
    const auto yScale = static_cast<double>(drawableHeight) / logicalSize.height;
    const auto clampHorizontal = [drawableWidth](const double value) noexcept {
        return std::clamp(value, 0.0, static_cast<double>(drawableWidth));
    };
    const auto clampVertical = [drawableHeight](const double value) noexcept {
        return std::clamp(value, 0.0, static_cast<double>(drawableHeight));
    };
    const auto left = clampHorizontal(std::floor(bounds.x * xScale));
    const auto top = clampVertical(std::floor(bounds.y * yScale));
    const auto right = clampHorizontal(std::ceil(bounds.right() * xScale));
    const auto bottom = clampVertical(std::ceil(bounds.bottom() * yScale));

    return { static_cast<NSUInteger>(left), static_cast<NSUInteger>(top),
        static_cast<NSUInteger>(std::max(0.0, right - left)),
        static_cast<NSUInteger>(std::max(0.0, bottom - top)) };
}

void releaseRenderBuffer(
    const std::shared_ptr<SharedRenderState>& state, RenderBufferAdmission admission) noexcept
{
    if (admission.index >= state->slots.size())
        return;

    auto expected = admission.busyState;
    state->slots[admission.index].admissionState.compare_exchange_strong(
        expected, admission.busyState + 1, std::memory_order_release, std::memory_order_relaxed);
}

void recordSkippedPresentation(const std::shared_ptr<FrameLatencySubmission>& submission,
    const std::shared_ptr<AtomicRenderTelemetry>& telemetry) noexcept
{
    os_unfair_lock_lock(&telemetry->presentedFrameHistoryLock);

    if (submission->classifySkipped() == PresentationOutcomeTransition::newlySkipped)
        telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);

    os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
}

void recordFrameLatencySample(const std::shared_ptr<AtomicRenderTelemetry>& telemetry,
    const FrameLatencySample& sample) noexcept
{
    os_unfair_lock_lock(&telemetry->presentedFrameHistoryLock);
    telemetry->frameLatencySamples.fetch_add(1, std::memory_order_relaxed);

    if (sample.totalValid) {
        telemetry->frameLatencyTotalTimingSamples.fetch_add(1, std::memory_order_relaxed);
    } else {
        telemetry->frameLatencyTotalTimingUnavailableSamples.fetch_add(
            1, std::memory_order_relaxed);
    }

    if (sample.componentsValid) {
        telemetry->frameLatencyComponentTimingSamples.fetch_add(1, std::memory_order_relaxed);
    } else {
        telemetry->frameLatencyComponentTimingUnavailableSamples.fetch_add(
            1, std::memory_order_relaxed);
    }

    if (!telemetry->frameLatencyHistory.record(sample))
        telemetry->frameLatencyHistoryDiscardedSamples.fetch_add(1, std::memory_order_relaxed);

    os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
}

void copyGpuTelemetry(
    const AtomicRenderTelemetry& source, MetalRenderTelemetry& destination) noexcept
{
    os_unfair_lock_lock(&source.gpuTelemetryLock);
    destination.completedFrames = source.completedFrames.load(std::memory_order_relaxed);
    destination.gpuTimingSamples = source.gpuTimingSamples.load(std::memory_order_relaxed);
    destination.gpuTimingUnavailableSamples
        = source.gpuTimingUnavailableSamples.load(std::memory_order_relaxed);
    destination.gpuCompletionLatenessSamples
        = source.gpuCompletionLatenessSamples.load(std::memory_order_relaxed);
    destination.gpuCompletionLatenessUnclassifiableSamples
        = source.gpuCompletionLatenessUnclassifiableSamples.load(std::memory_order_relaxed);
    destination.gpuCompletionDeadlineMisses
        = source.gpuCompletionDeadlineMisses.load(std::memory_order_relaxed);
    destination.lastGpuExecutionNanoseconds
        = source.lastGpuExecutionNanoseconds.load(std::memory_order_relaxed);
    destination.maximumGpuExecutionNanoseconds
        = source.maximumGpuExecutionNanoseconds.load(std::memory_order_relaxed);
    destination.lastGpuCompletionLatenessNanoseconds
        = source.lastGpuCompletionLatenessNanoseconds.load(std::memory_order_relaxed);
    os_unfair_lock_unlock(&source.gpuTelemetryLock);
}

void copyPresentedFrameTelemetry(
    const AtomicRenderTelemetry& source, MetalRenderTelemetry& destination) noexcept
{
    os_unfair_lock_lock(&source.presentedFrameHistoryLock);
    destination.presentedFrameIntervalHistoryCount
        = source.presentedFrameHistory.snapshotIntervals(destination.presentedFrameIntervalHistory);
    destination.lastPresentedHostTimestampNanoseconds
        = source.presentedFrameHistory.latestTimestampNanoseconds();
    destination.lastPresentedFrameIntervalNanoseconds
        = source.presentedFrameHistory.latestIntervalNanoseconds();
    destination.lastPresentationLatenessNanoseconds = source.lastPresentationLatenessNanoseconds;
    destination.maximumPresentationLatenessNanoseconds
        = source.maximumPresentationLatenessNanoseconds;
    destination.presentedFrames = source.presentedFrames.load(std::memory_order_relaxed);
    destination.presentationLatenessSamples
        = source.presentationLatenessSamples.load(std::memory_order_relaxed);
    destination.presentationLatenessUnclassifiableSamples
        = source.presentationLatenessUnclassifiableSamples.load(std::memory_order_relaxed);
    destination.presentationHistoryDiscardedTimestamps
        = source.presentationHistoryDiscardedTimestamps.load(std::memory_order_relaxed);
    destination.frameLatencySamples = source.frameLatencySamples.load(std::memory_order_relaxed);
    destination.frameLatencyTotalTimingSamples
        = source.frameLatencyTotalTimingSamples.load(std::memory_order_relaxed);
    destination.frameLatencyTotalTimingUnavailableSamples
        = source.frameLatencyTotalTimingUnavailableSamples.load(std::memory_order_relaxed);
    destination.frameLatencyComponentTimingSamples
        = source.frameLatencyComponentTimingSamples.load(std::memory_order_relaxed);
    destination.frameLatencyComponentTimingUnavailableSamples
        = source.frameLatencyComponentTimingUnavailableSamples.load(std::memory_order_relaxed);
    destination.frameLatencyHistoryDiscardedSamples
        = source.frameLatencyHistoryDiscardedSamples.load(std::memory_order_relaxed);
    destination.frameLatencyHistoryCount
        = source.frameLatencyHistory.snapshot(destination.frameLatencyHistory);
    destination.presentationsAfterTarget
        = source.presentationsAfterTarget.load(std::memory_order_relaxed);
    os_unfair_lock_unlock(&source.presentedFrameHistoryLock);
}

void recordPresentedDrawable(const std::shared_ptr<FrameLatencySubmission>& submission,
    const std::shared_ptr<AtomicRenderTelemetry>& telemetry, id<MTLDrawable> drawable,
    CFTimeInterval targetPresentationTimestamp, const std::uint64_t presentationSequence) noexcept
{
    telemetry->presentationCallbacks.fetch_add(1, std::memory_order_relaxed);

    const auto presentedTime = drawable.presentedTime;
    const auto presentedNanoseconds = hostTimeNanoseconds(presentedTime);
    FrameLatencySample latencySample;

    if (submission->recordPresentation(presentedNanoseconds, latencySample))
        recordFrameLatencySample(telemetry, latencySample);

    if (!std::isfinite(presentedTime) || presentedTime <= 0.0) {
        recordSkippedPresentation(submission, telemetry);
        return;
    }

    const auto hasTargetPresentationTimestamp
        = std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0;
    const auto presentationLateness = hasTargetPresentationTimestamp
        ? positiveHostTimeDifference(presentedTime, targetPresentationTimestamp)
        : 0;
    os_unfair_lock_lock(&telemetry->presentedFrameHistoryLock);
    const auto outcomeTransition = submission->classifyPresented();

    if (outcomeTransition == PresentationOutcomeTransition::none) {
        os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
        return;
    }

    if (outcomeTransition == PresentationOutcomeTransition::upgradedSkippedToPresented)
        telemetry->skippedPresentations.fetch_sub(1, std::memory_order_relaxed);

    const auto historyAccepted = telemetry->presentedFrameHistory.recordPresentation(
        presentationSequence, presentedNanoseconds);

    if (hasTargetPresentationTimestamp) {
        telemetry->maximumPresentationLatenessNanoseconds
            = std::max(telemetry->maximumPresentationLatenessNanoseconds, presentationLateness);
        telemetry->presentationLatenessSamples.fetch_add(1, std::memory_order_relaxed);

        if (presentedNanoseconds > telemetry->lastPresentationLatenessTimestampNanoseconds) {
            telemetry->lastPresentationLatenessTimestampNanoseconds = presentedNanoseconds;
            telemetry->lastPresentationLatenessNanoseconds = presentationLateness;
        }
    } else {
        telemetry->presentationLatenessUnclassifiableSamples.fetch_add(
            1, std::memory_order_relaxed);
    }

    if (!historyAccepted)
        telemetry->presentationHistoryDiscardedTimestamps.fetch_add(1, std::memory_order_relaxed);

    if (hasTargetPresentationTimestamp && presentationLateness != 0)
        telemetry->presentationsAfterTarget.fetch_add(1, std::memory_order_relaxed);

    telemetry->presentedFrames.fetch_add(1, std::memory_order_relaxed);
    os_unfair_lock_unlock(&telemetry->presentedFrameHistoryLock);
}

const char* metalShaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Vertex
{
    float2 position;
    float4 colour;
    float2 textureCoordinate;
};

struct RasterVertex
{
    float4 position [[position]];
    float4 colour;
    float2 textureCoordinate;
};

vertex RasterVertex audioInsightVertex(const device Vertex* vertices [[buffer(0)]],
                                       uint vertexId [[vertex_id]])
{
    RasterVertex output;
    output.position = float4(vertices[vertexId].position, 0.0, 1.0);
    output.colour = vertices[vertexId].colour;
    output.textureCoordinate = vertices[vertexId].textureCoordinate;
    return output;
}

fragment half4 audioInsightFragment(RasterVertex input [[stage_in]])
{
    return half4(input.colour);
}

fragment half4 audioInsightTextFragment(RasterVertex input [[stage_in]],
                                        texture2d<float> glyphAtlas [[texture(0)]],
                                        sampler glyphSampler [[sampler(0)]])
{
    const float coverage = glyphAtlas.sample(glyphSampler, input.textureCoordinate).r;
    return half4(half3(input.colour.rgb), half(input.colour.a * coverage));
}
)metal";
} // namespace

class MetalRenderBackend {
public:
    MetalRenderBackend(VisualizationDataSource& sourceToUse, AIAudioInsightMetalView* viewToUse)
        : source(sourceToUse), view(viewToUse), sharedState(std::make_shared<SharedRenderState>())
    {
        callbackTelemetry = std::make_shared<AtomicRenderTelemetry>();
        std::atomic_store_explicit(
            &publishedTelemetry, callbackTelemetry, std::memory_order_release);
        setSpectrumSettings({ });
        setDashboardLayoutSplits(DashboardLayout::defaultSplits);
        initialiseMetal();
        callbackTelemetry->metalAvailable.store(metalReady, std::memory_order_relaxed);
    }

    ~MetalRenderBackend()
    {
        shutdown();
        [glyphAtlasTexture release];
        [glyphSamplerState release];
        [textPipelineState release];
        [pipelineState release];
        [commandQueue release];
    }

    void attachNativeView()
    {
        assertMessageThread();

        if (view == nil)
            return;

        view.delegate = nil;
        view.paused = YES;
        view.enableSetNeedsDisplay = NO;
        view.autoResizeDrawable = YES;
        view.framebufferOnly = YES;
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        view.clearColor = MTLClearColorMake(0.018, 0.024, 0.035, 1.0);

        if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
            auto* layer = static_cast<CAMetalLayer*>(view.layer);
            layer.maximumDrawableCount = renderBufferCount;
            layer.allowsNextDrawableTimeout = YES;
            layer.displaySyncEnabled = YES;
            layer.presentsWithTransaction = NO;
        }

        [view attachRenderBackend:this];

        if (![view hasDisplayLink]) {
            metalReady = false;
            initializationError = "CAMetalDisplayLink could not attach to the Metal layer.";
            callbackTelemetry->metalAvailable.store(false, std::memory_order_relaxed);
        }

        nativeViewStateChanged();
    }

    void shutdown() noexcept
    {
        assertMessageThread();

        if (hasShutDown.exchange(true, std::memory_order_acq_rel))
            return;

        requestedActive.store(false, std::memory_order_relaxed);
        loadPublishedTelemetry()->renderingRequested.store(false, std::memory_order_relaxed);
        setEffectiveActive(false);

        if (view != nil) {
            view.paused = YES;
            view.delegate = nil;
            [view detachRenderBackend];
        }

        view = nil;
    }

    void setRequestedActive(bool shouldBeActive)
    {
        assertMessageThread();

        if (hasShutDown.load(std::memory_order_acquire))
            return;

        requestedActive.store(shouldBeActive, std::memory_order_relaxed);
        loadPublishedTelemetry()->renderingRequested.store(
            shouldBeActive, std::memory_order_relaxed);
        refreshEffectiveActivity();
    }

    void setJuceShowing(bool shouldBeShowing)
    {
        assertMessageThread();
        juceShowing.store(shouldBeShowing, std::memory_order_relaxed);
        refreshEffectiveActivity();
    }

    [[nodiscard]] bool isRenderingRequested() const noexcept
    {
        return requestedActive.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isEffectivelyRendering() const noexcept
    {
        return effectiveActive.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isMetalAvailable() const noexcept
    {
        return metalReady;
    }

    [[nodiscard]] juce::String getInitializationError() const
    {
        return initializationError;
    }

    void setEffectiveActivityCallback(MetalVisualization::EffectiveActivityCallback callback)
    {
        assertMessageThread();
        effectiveActivityCallback = std::move(callback);
    }

    void setSpectrumSettings(SpectrumRenderSettings settings) noexcept
    {
        packedSpectrumSettings.store(packSpectrumSettings(settings), std::memory_order_release);
    }

    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept
    {
        return unpackSpectrumSettings(packedSpectrumSettings.load(std::memory_order_acquire));
    }

    void setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
    {
        packedDashboardLayoutSplits.store(
            packDashboardLayoutSplits(splits), std::memory_order_release);
    }

    [[nodiscard]] DashboardLayoutSplits getDashboardLayoutSplits() const noexcept
    {
        return unpackDashboardLayoutSplits(
            packedDashboardLayoutSplits.load(std::memory_order_acquire));
    }

    [[nodiscard]] MetalRenderTelemetry getTelemetry() const noexcept
    {
        const auto telemetry = loadPublishedTelemetry();
        MetalRenderTelemetry result;

        result.epoch = telemetry->epoch;
        result.displayLinkCallbacks
            = telemetry->displayLinkCallbacks.load(std::memory_order_relaxed);
        result.submittedFrames = telemetry->submittedFrames.load(std::memory_order_relaxed);
        result.commandBufferFailures
            = telemetry->commandBufferFailures.load(std::memory_order_relaxed);
        result.presentationCallbacks
            = telemetry->presentationCallbacks.load(std::memory_order_relaxed);
        result.skippedPresentations
            = telemetry->skippedPresentations.load(std::memory_order_relaxed);
        result.gpuBackpressureDrops
            = telemetry->gpuBackpressureDrops.load(std::memory_order_relaxed);
        result.drawableUnavailableDrops
            = telemetry->drawableUnavailableDrops.load(std::memory_order_relaxed);
        result.callbackHostDelaySamples
            = telemetry->callbackHostDelaySamples.load(std::memory_order_relaxed);
        result.callbackHostDelayUnclassifiableSamples
            = telemetry->callbackHostDelayUnclassifiableSamples.load(std::memory_order_relaxed);
        result.callbackAlreadyLateHostDelays
            = telemetry->callbackAlreadyLateHostDelays.load(std::memory_order_relaxed);
        result.cpuCommitLatenessSamples
            = telemetry->cpuCommitLatenessSamples.load(std::memory_order_relaxed);
        result.cpuCommitLatenessUnclassifiableSamples
            = telemetry->cpuCommitLatenessUnclassifiableSamples.load(std::memory_order_relaxed);
        result.cpuCommitDeadlineMisses
            = telemetry->cpuCommitDeadlineMisses.load(std::memory_order_relaxed);
        result.analysisRequestCalls
            = telemetry->analysisRequestCalls.load(std::memory_order_relaxed);
        result.snapshotReads = telemetry->snapshotReads.load(std::memory_order_relaxed);
        result.framesWithNewSnapshot
            = telemetry->framesWithNewSnapshot.load(std::memory_order_relaxed);
        result.lastSpectrumSequence
            = telemetry->lastSpectrumSequence.load(std::memory_order_relaxed);
        result.lastCpuEncodeNanoseconds
            = telemetry->lastCpuEncodeNanoseconds.load(std::memory_order_relaxed);
        result.maximumCpuEncodeNanoseconds
            = telemetry->maximumCpuEncodeNanoseconds.load(std::memory_order_relaxed);
        result.lastDisplayCallbackIntervalNanoseconds
            = telemetry->lastDisplayCallbackIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetIntervalNanoseconds
            = telemetry->lastTargetIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetPresentationIntervalNanoseconds
            = telemetry->lastTargetPresentationIntervalNanoseconds.load(std::memory_order_relaxed);
        result.lastCallbackHostDelayNanoseconds
            = telemetry->lastCallbackHostDelayNanoseconds.load(std::memory_order_relaxed);
        result.lastCpuCommitLatenessNanoseconds
            = telemetry->lastCpuCommitLatenessNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetTimestampNanoseconds
            = telemetry->lastTargetTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastTargetPresentationTimestampNanoseconds
            = telemetry->lastTargetPresentationTimestampNanoseconds.load(std::memory_order_relaxed);
        result.lastProvidedDrawableAccessNanoseconds
            = telemetry->lastProvidedDrawableAccessNanoseconds.load(std::memory_order_relaxed);
        result.maximumProvidedDrawableAccessNanoseconds
            = telemetry->maximumProvidedDrawableAccessNanoseconds.load(std::memory_order_relaxed);
        copyGpuTelemetry(*telemetry, result);
        copyPresentedFrameTelemetry(*telemetry, result);
        result.drawableWidthPixels = telemetry->drawableWidthPixels.load(std::memory_order_relaxed);
        result.drawableHeightPixels
            = telemetry->drawableHeightPixels.load(std::memory_order_relaxed);
        result.configuredMaximumFramesPerSecond
            = telemetry->configuredMaximumFramesPerSecond.load(std::memory_order_relaxed);
        result.backingScale = telemetry->backingScale.load(std::memory_order_relaxed);
        result.metalAvailable = telemetry->metalAvailable.load(std::memory_order_relaxed);
        result.renderingRequested = telemetry->renderingRequested.load(std::memory_order_relaxed);
        result.effectivelyRendering
            = telemetry->effectivelyRendering.load(std::memory_order_relaxed);
        result.resetPending
            = requestedTelemetryEpoch.load(std::memory_order_acquire) != result.epoch;
        return result;
    }

    void resetTelemetry()
    {
        assertMessageThread();
        queueTelemetryReset();

        // An active display link installs the replacement at the next callback
        // boundary. Without callbacks there is no reason to leave the reset
        // pending: the message thread is already a safe boundary and in-flight
        // completion/presentation handlers retain the old telemetry object.
        if (!effectiveActive.load(std::memory_order_acquire) || !metalReady || view == nil)
            applyPendingTelemetryAtSafeBoundary();
    }

    void nativeViewStateChanged()
    {
        assertMessageThread();

        if (hasShutDown.load(std::memory_order_acquire) || view == nil)
            return;

        updateScreenProperties();
        refreshEffectiveActivity();
    }

    bool tryClearPeakRmsAt(const NSPoint point) noexcept
    {
        assertMessageThread();

        if (view == nil || !std::isfinite(point.x) || !std::isfinite(point.y))
            return false;

        const auto boundsSize = view.bounds.size;
        const auto dashboardLayout = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, static_cast<double>(boundsSize.width),
                static_cast<double>(boundsSize.height) },
            getDashboardLayoutSplits());
        const auto panel = toRenderRect(
            dashboardLayout[DashboardPanel::peakRms], static_cast<float>(boundsSize.height));
        const auto panelHeaderHeight
            = std::min(panel.height(), std::clamp(panel.height() * 0.13F, 18.0F, 26.0F));
        constexpr auto meterTextScale = 0.78F;
        const auto readoutWidth
            = (((static_cast<float>(maximumPeakRmsReadoutGlyphs) - 1.0F) * cachedGlyphAdvance)
                  + cachedGlyphCellWidth)
            * meterTextScale;
        const auto layout = calculatePeakRmsPanelLayout(panel.width(), panel.height(),
            panelHeaderHeight, targetFrame.channelCount, cachedGlyphCellHeight * meterTextScale,
            maximumCachedPeakRmsTickTextWidth * meterTextScale, readoutWidth);
        const auto localX = static_cast<float>(point.x) - panel.left;
        const auto localY = static_cast<float>(point.y) - panel.bottom;

        if (!layout.clearHitBounds.contains(localX, localY))
            return false;

        source.resetPeakRms();
        return true;
    }

    bool performPeakRmsClearAction() noexcept
    {
        assertMessageThread();
        source.resetPeakRms();
        return true;
    }

    void displayLinkUpdate(CAMetalDisplayLinkUpdate* update)
    {
        assertMessageThread();

        if (!effectiveActive.load(std::memory_order_relaxed) || view == nil || update == nil
            || !metalReady) {
            return;
        }

        const auto callbackTime = Clock::now();
        const auto callbackHostTime = CACurrentMediaTime();
        applyPendingTelemetryAtSafeBoundary();
        const auto telemetry = callbackTelemetry;
        const auto targetTimestamp = update.targetTimestamp;
        const auto targetPresentationTimestamp = update.targetPresentationTimestamp;
        const auto presentationSequence = recordDisplayLinkCallback(
            *telemetry, callbackHostTime, targetTimestamp, targetPresentationTimestamp);

        const auto admission = acquireRenderBuffer();

        if (admission.index >= renderBufferCount) {
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::shared_ptr<FrameLatencySubmission> submission;

        try {
            // Presentation may trail GPU completion by several display periods,
            // so this small state must outlive reusable vertex-buffer admission.
            submission = std::make_shared<FrameLatencySubmission>(
                presentationSequence, hostTimeNanoseconds(callbackHostTime));
        } catch (...) {
            releaseRenderBuffer(sharedState, admission);
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto& slot = sharedState->slots[admission.index];
        const auto drawableAccessStart = Clock::now();
        id<CAMetalDrawable> drawable = update.drawable;
        const auto drawableAccessNanoseconds
            = nanosecondsBetween(drawableAccessStart, Clock::now());
        telemetry->lastProvidedDrawableAccessNanoseconds.store(
            drawableAccessNanoseconds, std::memory_order_relaxed);
        updateMaximum(
            telemetry->maximumProvidedDrawableAccessNanoseconds, drawableAccessNanoseconds);

        if (drawable == nil || drawable.texture == nil) {
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->drawableUnavailableDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        telemetry->drawableWidthPixels.store(
            roundedPixelDimension(static_cast<CGFloat>(drawable.texture.width)),
            std::memory_order_relaxed);
        telemetry->drawableHeightPixels.store(
            roundedPixelDimension(static_cast<CGFloat>(drawable.texture.height)),
            std::memory_order_relaxed);
        // Keep telemetry current here, but leave atlas rebuilding to AppKit's
        // existing backing-property and window-lifecycle notifications.
        updateBackingScale(false);

        source.requestAnalysis();
        telemetry->analysisRequestCalls.fetch_add(1, std::memory_order_relaxed);

        VisualizationFrame incomingFrame;

        if (source.copyLatestVisualizationFrame(incomingFrame)) {
            telemetry->snapshotReads.fetch_add(1, std::memory_order_relaxed);
            acceptSnapshot(incomingFrame, *telemetry);
        }

        const auto spectrumSettings = getSpectrumSettings();
        updateSmoothedDisplayValues(callbackTime, spectrumSettings);

        const auto boundsSize = view.bounds.size;
        const auto dashboardLayout = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, static_cast<double>(boundsSize.width),
                static_cast<double>(boundsSize.height) },
            getDashboardLayoutSplits());

        auto* vertices = static_cast<MetalVertex*>(slot.vertexBuffer.contents);
        const auto batches
            = populateVertices(vertices, boundsSize, dashboardLayout, spectrumSettings);

        auto* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* colourAttachment = descriptor.colorAttachments[0];
        colourAttachment.texture = drawable.texture;
        colourAttachment.loadAction = MTLLoadActionClear;
        colourAttachment.storeAction = MTLStoreActionStore;
        colourAttachment.clearColor = MTLClearColorMake(0.018, 0.024, 0.035, 1.0);

        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];

        if (commandBuffer == nil) {
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:descriptor];

        if (encoder == nil) {
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->drawableUnavailableDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        encoder.label = @"Audio Insight visualization";
        [encoder setRenderPipelineState:pipelineState];
        [encoder setVertexBuffer:slot.vertexBuffer offset:0 atIndex:0];

        if (batches.shell.count != 0)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.shell.start
                        vertexCount:batches.shell.count];

        const auto spectrumScissor = makeScissorRect(dashboardLayout[DashboardPanel::spectrum],
            boundsSize, drawable.texture.width, drawable.texture.height);

        if (spectrumScissor.width != 0 && spectrumScissor.height != 0) {
            [encoder setScissorRect:spectrumScissor];

            if (batches.spectrumGrid.count != 0)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:batches.spectrumGrid.start
                            vertexCount:batches.spectrumGrid.count];

            if (batches.spectrum.count >= 2)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:batches.spectrum.start
                            vertexCount:batches.spectrum.count];
        }

        const auto spectrogramScissor
            = makeScissorRect(dashboardLayout[DashboardPanel::spectrogram], boundsSize,
                drawable.texture.width, drawable.texture.height);

        if (spectrogramScissor.width != 0 && spectrogramScissor.height != 0
            && batches.spectrogramAxis.count != 0) {
            [encoder setScissorRect:spectrogramScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.spectrogramAxis.start
                        vertexCount:batches.spectrogramAxis.count];
        }

        const auto meterScissor = makeScissorRect(dashboardLayout[DashboardPanel::peakRms],
            boundsSize, drawable.texture.width, drawable.texture.height);

        if (meterScissor.width != 0 && meterScissor.height != 0 && batches.peakRms.count != 0) {
            [encoder setScissorRect:meterScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.peakRms.start
                        vertexCount:batches.peakRms.count];
        }

        if (glyphAtlasTexture != nil && textPipelineState != nil && glyphSamplerState != nil) {
            [encoder setRenderPipelineState:textPipelineState];
            [encoder setFragmentTexture:glyphAtlasTexture atIndex:0];
            [encoder setFragmentSamplerState:glyphSamplerState atIndex:0];

            for (std::size_t index = 0; index < dashboardPanelCount; ++index) {
                if (batches.text[index].count == 0)
                    continue;

                const auto panel = static_cast<DashboardPanel>(index);
                const auto textScissor = makeScissorRect(dashboardLayout[panel], boundsSize,
                    drawable.texture.width, drawable.texture.height);

                if (textScissor.width == 0 || textScissor.height == 0)
                    continue;

                [encoder setScissorRect:textScissor];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:batches.text[index].start
                            vertexCount:batches.text[index].count];
            }
        }

        [encoder endEncoding];

        auto presentationTelemetry = telemetry;
        [drawable addPresentedHandler:^(id<MTLDrawable> presentedDrawable) {
            recordPresentedDrawable(submission, presentationTelemetry, presentedDrawable,
                targetPresentationTimestamp, presentationSequence);
        }];

        auto completionState = sharedState;
        auto completionTelemetry = telemetry;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            // The GPU has finished reading this submission's shared vertex
            // buffer. Presentation may be delayed by several refresh periods,
            // so it must not retain renderer admission.
            releaseRenderBuffer(completionState, admission);

            const auto gpuStart = completedBuffer.GPUStartTime;
            const auto gpuEnd = completedBuffer.GPUEndTime;
            const auto commandSucceeded = completedBuffer.status != MTLCommandBufferStatusError;
            const auto hasGpuTiming = commandSucceeded && std::isfinite(gpuStart)
                && std::isfinite(gpuEnd) && gpuEnd >= gpuStart && gpuStart > 0.0;
            FrameLatencySample latencySample;

            if (submission->recordGpuCompletion(hasGpuTiming ? hostTimeNanoseconds(gpuStart) : 0,
                    hasGpuTiming ? hostTimeNanoseconds(gpuEnd) : 0, latencySample)) {
                recordFrameLatencySample(completionTelemetry, latencySample);
            }

            if (!commandSucceeded) {
                completionTelemetry->commandBufferFailures.fetch_add(1, std::memory_order_relaxed);
                recordSkippedPresentation(submission, completionTelemetry);
                return;
            }

            const auto hasTargetPresentationTimestamp
                = std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0;

            os_unfair_lock_lock(&completionTelemetry->gpuTelemetryLock);
            if (hasGpuTiming) {
                const auto gpuNanoseconds = positiveHostTimeDifference(gpuEnd, gpuStart);
                completionTelemetry->lastGpuExecutionNanoseconds.store(
                    gpuNanoseconds, std::memory_order_relaxed);
                updateMaximum(completionTelemetry->maximumGpuExecutionNanoseconds, gpuNanoseconds);
                completionTelemetry->gpuTimingSamples.fetch_add(1, std::memory_order_relaxed);

                if (hasTargetPresentationTimestamp) {
                    const auto completionLateness
                        = positiveHostTimeDifference(gpuEnd, targetPresentationTimestamp);
                    completionTelemetry->lastGpuCompletionLatenessNanoseconds.store(
                        completionLateness, std::memory_order_relaxed);
                    completionTelemetry->gpuCompletionLatenessSamples.fetch_add(
                        1, std::memory_order_relaxed);

                    if (completionLateness != 0)
                        completionTelemetry->gpuCompletionDeadlineMisses.fetch_add(
                            1, std::memory_order_relaxed);
                } else {
                    completionTelemetry->gpuCompletionLatenessUnclassifiableSamples.fetch_add(
                        1, std::memory_order_relaxed);
                }
            } else {
                completionTelemetry->gpuTimingUnavailableSamples.fetch_add(
                    1, std::memory_order_relaxed);
                completionTelemetry->gpuCompletionLatenessUnclassifiableSamples.fetch_add(
                    1, std::memory_order_relaxed);
            }

            completionTelemetry->completedFrames.fetch_add(1, std::memory_order_relaxed);
            os_unfair_lock_unlock(&completionTelemetry->gpuTelemetryLock);
        }];

        submission->setCpuReadyTimestamp(hostTimeNanoseconds(CACurrentMediaTime()));
        [commandBuffer commit];

        // CAMetalDisplayLink owns the drawable's presentation timing. Timed presentation APIs
        // assert for drawables supplied by its update callback.
        [drawable present];

        const auto commitHostTime = CACurrentMediaTime();
        const auto hasCpuCommitLateness = std::isfinite(commitHostTime) && commitHostTime > 0.0
            && std::isfinite(targetPresentationTimestamp) && targetPresentationTimestamp > 0.0;

        if (hasCpuCommitLateness) {
            const auto commitLateness
                = positiveHostTimeDifference(commitHostTime, targetPresentationTimestamp);
            telemetry->lastCpuCommitLatenessNanoseconds.store(
                commitLateness, std::memory_order_relaxed);
            telemetry->cpuCommitLatenessSamples.fetch_add(1, std::memory_order_relaxed);

            if (commitLateness != 0)
                telemetry->cpuCommitDeadlineMisses.fetch_add(1, std::memory_order_relaxed);
        } else {
            telemetry->cpuCommitLatenessUnclassifiableSamples.fetch_add(
                1, std::memory_order_relaxed);
        }

        const auto cpuNanoseconds = nanosecondsBetween(callbackTime, Clock::now());
        telemetry->lastCpuEncodeNanoseconds.store(cpuNanoseconds, std::memory_order_relaxed);
        updateMaximum(telemetry->maximumCpuEncodeNanoseconds, cpuNanoseconds);
        telemetry->submittedFrames.fetch_add(1, std::memory_order_relaxed);
    }

private:
    VisualizationDataSource& source;
    AIAudioInsightMetalView* view = nil;
    std::shared_ptr<SharedRenderState> sharedState;
    std::shared_ptr<AtomicRenderTelemetry> callbackTelemetry;
    std::shared_ptr<AtomicRenderTelemetry> publishedTelemetry;
    std::shared_ptr<AtomicRenderTelemetry> pendingTelemetry;

    id<MTLCommandQueue> commandQueue = nil;
    id<MTLRenderPipelineState> pipelineState = nil;
    id<MTLRenderPipelineState> textPipelineState = nil;
    id<MTLSamplerState> glyphSamplerState = nil;
    id<MTLTexture> glyphAtlasTexture = nil;
    std::array<CachedTextRun, cachedFixedTextRunCount> cachedFixedTextRuns { };
    std::array<CachedTextRun, frequencyAxisTickCandidateCount> cachedFrequencyAxisTextRuns { };
    std::array<CachedTextRun, cachedDecibelTickCount> cachedDecibelTextRuns { };
    std::array<CachedTextRun, peakRmsMajorDecibelTicks.size()> cachedPeakRmsTickTextRuns { };
    std::array<GlyphAtlasEntry, glyphAtlasGlyphCount> cachedGlyphAtlasEntries { };
    float cachedGlyphAdvance = 0.0F;
    float cachedGlyphCellWidth = 0.0F;
    float cachedGlyphCellHeight = 0.0F;
    float maximumCachedDecibelTextWidth = 0.0F;
    float maximumCachedPeakRmsTickTextWidth = 0.0F;
    double glyphAtlasBackingScale = 0.0;
    juce::String initializationError;
    bool metalReady = false;
    std::atomic<std::uint64_t> packedSpectrumSettings { 0 };
    std::atomic<std::uint32_t> packedDashboardLayoutSplits { 0 };
    std::atomic<std::uint64_t> requestedTelemetryEpoch { 1 };

    std::atomic<bool> requestedActive { false };
    std::atomic<bool> juceShowing { false };
    std::atomic<bool> effectiveActive { false };
    std::atomic<bool> hasShutDown { false };
    MetalVisualization::EffectiveActivityCallback effectiveActivityCallback;

    VisualizationFrame targetFrame;
    std::array<float, spectrumBinCount> displayedSpectrum { };
    std::uint64_t lastSpectrumSequence = 0;
    std::uint64_t lastMeterSequence = 0;
    std::uint64_t lastGeneration = 0;
    bool hasDisplayFrame = false;

    Clock::time_point previousSmoothingTime;
    CFTimeInterval previousDisplayCallbackHostTime = 0.0;
    CFTimeInterval previousTargetTimestamp = 0.0;
    CFTimeInterval previousTargetPresentationTimestamp = 0.0;

    void initialiseMetal()
    {
        if (view == nil || view.device == nil) {
            initializationError = "Metal is not available on this system.";
            return;
        }

        commandQueue = [view.device newCommandQueue];

        if (commandQueue == nil) {
            initializationError = "Metal could not create a command queue.";
            return;
        }

        NSError* libraryError = nil;
        auto* sourceString = [NSString stringWithUTF8String:metalShaderSource];
        id<MTLLibrary> library = [view.device newLibraryWithSource:sourceString
                                                           options:nil
                                                             error:&libraryError];

        if (library == nil) {
            initializationError = libraryError != nil
                ? juce::String::fromUTF8(libraryError.localizedDescription.UTF8String)
                : juce::String("Metal could not compile the visualization shader.");
            return;
        }

        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"audioInsightVertex"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"audioInsightFragment"];
        id<MTLFunction> textFragmentFunction =
            [library newFunctionWithName:@"audioInsightTextFragment"];

        if (vertexFunction == nil || fragmentFunction == nil || textFragmentFunction == nil) {
            initializationError = "Metal could not load the visualization shader functions.";
            [vertexFunction release];
            [fragmentFunction release];
            [textFragmentFunction release];
            [library release];
            return;
        }

        auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.label = @"Audio Insight visualization pipeline";
        descriptor.vertexFunction = vertexFunction;
        descriptor.fragmentFunction = fragmentFunction;
        auto* colourAttachment = descriptor.colorAttachments[0];
        colourAttachment.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        colourAttachment.blendingEnabled = YES;
        colourAttachment.rgbBlendOperation = MTLBlendOperationAdd;
        colourAttachment.alphaBlendOperation = MTLBlendOperationAdd;
        colourAttachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        colourAttachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        colourAttachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        colourAttachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        NSError* pipelineError = nil;
        pipelineState = [view.device newRenderPipelineStateWithDescriptor:descriptor
                                                                    error:&pipelineError];

        NSError* textPipelineError = nil;
        descriptor.label = @"Audio Insight glyph-atlas text pipeline";
        descriptor.fragmentFunction = textFragmentFunction;
        textPipelineState = [view.device newRenderPipelineStateWithDescriptor:descriptor
                                                                        error:&textPipelineError];

        [descriptor release];
        [vertexFunction release];
        [fragmentFunction release];
        [textFragmentFunction release];
        [library release];

        if (pipelineState == nil) {
            initializationError = pipelineError != nil
                ? juce::String::fromUTF8(pipelineError.localizedDescription.UTF8String)
                : juce::String("Metal could not create the visualization pipeline.");
            return;
        }

        if (textPipelineState == nil) {
            initializationError = textPipelineError != nil
                ? juce::String::fromUTF8(textPipelineError.localizedDescription.UTF8String)
                : juce::String("Metal could not create the glyph-atlas text pipeline.");
            return;
        }

        auto* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
        samplerDescriptor.label = @"Audio Insight glyph-atlas sampler";
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        glyphSamplerState = [view.device newSamplerStateWithDescriptor:samplerDescriptor];
        [samplerDescriptor release];

        if (glyphSamplerState == nil) {
            initializationError = "Metal could not create the glyph-atlas sampler.";
            return;
        }

        for (auto& slot : sharedState->slots) {
            slot.vertexBuffer =
                [view.device newBufferWithLength:maximumVertexCount * sizeof(MetalVertex)
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];

            if (slot.vertexBuffer == nil) {
                initializationError = "Metal could not allocate the visualization buffers.";
                return;
            }
        }

        if (!rebuildGlyphAtlas(1.0)) {
            initializationError = "Metal could not create the initial glyph atlas.";
            return;
        }

        metalReady = true;
    }

    // Font selection, measurement, rasterization, texture allocation, and fixed
    // run caching happen only at initialization or a native backing-scale seam.
    // The display callback later performs bounded arithmetic over these arrays.
    bool rebuildGlyphAtlas(const double backingScale)
    {
        assertMessageThread();

        if (view == nil || view.device == nil || !std::isfinite(backingScale)
            || backingScale <= 0.0) {
            return false;
        }

        constexpr auto logicalFontSize = 11.0;
        const auto pixelFontSize = static_cast<CGFloat>(logicalFontSize * backingScale);
        auto* font = [NSFont monospacedSystemFontOfSize:pixelFontSize weight:NSFontWeightRegular];

        if (font == nil)
            return false;

        auto* attributes = @ {
            NSFontAttributeName : font,
            NSForegroundColorAttributeName : [NSColor whiteColor],
        };
        const auto advanceMeasurement = [@"M" sizeWithAttributes:attributes];
        const auto lineMeasurement = [@"Mg" sizeWithAttributes:attributes];
        const auto advancePixels = std::max<CGFloat>(1.0, advanceMeasurement.width);
        const auto lineHeightPixels = std::max<CGFloat>(1.0, lineMeasurement.height);
        const auto paddingPixels
            = std::max<NSInteger>(1, static_cast<NSInteger>(std::ceil(backingScale)));
        const auto cellWidth
            = static_cast<NSInteger>(std::ceil(advancePixels)) + (2 * paddingPixels);
        const auto cellHeight
            = static_cast<NSInteger>(std::ceil(lineHeightPixels)) + (2 * paddingPixels);
        const auto atlasWidth = static_cast<NSInteger>(glyphAtlasColumns) * cellWidth;
        const auto atlasHeight = static_cast<NSInteger>(glyphAtlasRows) * cellHeight;

        if (cellWidth <= 0 || cellHeight <= 0 || atlasWidth <= 0 || atlasHeight <= 0
            || atlasWidth > 8'192 || atlasHeight > 8'192) {
            return false;
        }

        auto* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                               pixelsWide:atlasWidth
                                                               pixelsHigh:atlasHeight
                                                            bitsPerSample:8
                                                          samplesPerPixel:1
                                                                 hasAlpha:NO
                                                                 isPlanar:NO
                                                           colorSpaceName:NSDeviceWhiteColorSpace
                                                              bytesPerRow:atlasWidth
                                                             bitsPerPixel:8];

        auto* bitmapData = bitmap.bitmapData;

        if (bitmap == nil || bitmapData == nullptr) {
            [bitmap release];
            return false;
        }

        std::fill_n(bitmapData, static_cast<std::size_t>(bitmap.bytesPerRow * bitmap.pixelsHigh),
            static_cast<unsigned char>(0));

        auto* graphicsContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];

        if (graphicsContext == nil) {
            [bitmap release];
            return false;
        }

        std::array<GlyphAtlasEntry, glyphAtlasGlyphCount> atlasEntries { };
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:graphicsContext];
        graphicsContext.shouldAntialias = YES;
        graphicsContext.imageInterpolation = NSImageInterpolationHigh;
        [[NSColor blackColor] setFill];
        NSRectFill(NSMakeRect(0.0, 0.0, atlasWidth, atlasHeight));

        for (std::size_t index = 0; index < glyphAtlasGlyphCount; ++index) {
            const auto column = index % glyphAtlasColumns;
            const auto row = index / glyphAtlasColumns;
            const auto cellLeft
                = static_cast<CGFloat>(column * static_cast<std::size_t>(cellWidth));
            const auto cellBottom
                = static_cast<CGFloat>(row * static_cast<std::size_t>(cellHeight));
            const auto cellRight = cellLeft + static_cast<CGFloat>(cellWidth);
            const auto cellTop = cellBottom + static_cast<CGFloat>(cellHeight);
            const unichar characterValue = index == infinityGlyphAtlasIndex
                ? static_cast<unichar>(0x221e)
                : static_cast<unichar>(index + printableAsciiFirst);
            auto* character = [NSString stringWithCharacters:&characterValue length:1];
            [character drawAtPoint:NSMakePoint(cellLeft + static_cast<CGFloat>(paddingPixels),
                                       cellBottom + static_cast<CGFloat>(paddingPixels))
                    withAttributes:attributes];

            atlasEntries[index] = {
                static_cast<float>(cellLeft / static_cast<CGFloat>(atlasWidth)),
                static_cast<float>(1.0 - (cellBottom / static_cast<CGFloat>(atlasHeight))),
                static_cast<float>(cellRight / static_cast<CGFloat>(atlasWidth)),
                static_cast<float>(1.0 - (cellTop / static_cast<CGFloat>(atlasHeight))),
            };
        }

        [NSGraphicsContext restoreGraphicsState];

        auto* textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                         width:static_cast<NSUInteger>(atlasWidth)
                                        height:static_cast<NSUInteger>(atlasHeight)
                                     mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> newTexture = [view.device newTextureWithDescriptor:textureDescriptor];

        if (newTexture == nil) {
            [bitmap release];
            return false;
        }

        newTexture.label = @"Audio Insight scale-aware glyph atlas";
        [newTexture replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(atlasWidth),
                                      static_cast<NSUInteger>(atlasHeight))
                      mipmapLevel:0
                        withBytes:bitmapData
                      bytesPerRow:static_cast<NSUInteger>(bitmap.bytesPerRow)];
        [bitmap release];

        const auto advancePoints = static_cast<float>(advancePixels / backingScale);
        const auto cellWidthPoints = static_cast<float>(cellWidth / backingScale);
        const auto cellHeightPoints = static_cast<float>(cellHeight / backingScale);
        const auto makeCachedTextRun = [&](const std::string_view text) noexcept {
            CachedTextRun run;
            run.glyphCount = text.size();
            run.height = cellHeightPoints;

            for (std::size_t glyphIndex = 0; glyphIndex < text.size(); ++glyphIndex) {
                const auto character = static_cast<unsigned char>(text[glyphIndex]);
                const auto atlasIndex
                    = character >= printableAsciiFirst && character <= printableAsciiLast
                    ? static_cast<std::size_t>(character - printableAsciiFirst)
                    : static_cast<std::size_t>('?' - printableAsciiFirst);
                const auto left = static_cast<float>(glyphIndex) * advancePoints;
                run.glyphs[glyphIndex] = { left, 0.0F, left + cellWidthPoints, cellHeightPoints,
                    atlasEntries[atlasIndex] };
            }

            if (!text.empty())
                run.width = (static_cast<float>(text.size() - 1) * advancePoints) + cellWidthPoints;

            return run;
        };
        std::array<CachedTextRun, cachedFixedTextRunCount> newCachedFixedTextRuns { };
        for (std::size_t index = 0; index < cachedFixedTextStrings.size(); ++index)
            newCachedFixedTextRuns[index] = makeCachedTextRun(cachedFixedTextStrings[index]);

        std::array<CachedTextRun, frequencyAxisTickCandidateCount>
            newCachedFrequencyAxisTextRuns { };
        for (std::size_t index = 0; index < cachedFrequencyAxisTextStrings.size(); ++index) {
            newCachedFrequencyAxisTextRuns[index]
                = makeCachedTextRun(cachedFrequencyAxisTextStrings[index]);
        }

        std::array<CachedTextRun, cachedDecibelTickCount> newCachedDecibelTextRuns { };
        auto newMaximumCachedDecibelTextWidth = 0.0F;
        for (std::size_t index = 0; index < newCachedDecibelTextRuns.size(); ++index) {
            const auto decibels
                = minimumCachedDecibelTick + (static_cast<int>(index) * cachedDecibelTickInterval);
            std::array<char, maximumCachedTextGlyphs> text { };
            auto* cursor = text.data();
            auto* const end = text.data() + text.size();
            const auto conversion = std::to_chars(cursor, end, decibels);

            if (conversion.ec != std::errc { }
                || static_cast<std::size_t>(end - conversion.ptr) < 3) {
                [newTexture release];
                return false;
            }

            cursor = conversion.ptr;
            *cursor++ = ' ';
            *cursor++ = 'd';
            *cursor++ = 'B';
            auto& run = newCachedDecibelTextRuns[index];
            run = makeCachedTextRun(
                std::string_view(text.data(), static_cast<std::size_t>(cursor - text.data())));
            newMaximumCachedDecibelTextWidth
                = std::max(newMaximumCachedDecibelTextWidth, run.width);
        }

        std::array<CachedTextRun, peakRmsMajorDecibelTicks.size()> newCachedPeakRmsTickTextRuns { };
        auto newMaximumCachedPeakRmsTickTextWidth = 0.0F;
        for (std::size_t index = 0; index < newCachedPeakRmsTickTextRuns.size(); ++index) {
            auto& run = newCachedPeakRmsTickTextRuns[index];
            run = makeCachedTextRun(cachedPeakRmsTickTextStrings[index]);
            newMaximumCachedPeakRmsTickTextWidth
                = std::max(newMaximumCachedPeakRmsTickTextWidth, run.width);
        }

        // Force construction of the immutable, density-independent endpoint
        // and meter-readout tables here. Later display callbacks only index
        // already-formatted runs.
        juce::ignoreUnused(cachedFrequencyEndpointTextRuns());
        juce::ignoreUnused(cachedPeakRmsReadoutTextRuns(), cachedMinusInfinityTextRun());

        [glyphAtlasTexture release];
        glyphAtlasTexture = newTexture;
        cachedFixedTextRuns = newCachedFixedTextRuns;
        cachedFrequencyAxisTextRuns = newCachedFrequencyAxisTextRuns;
        cachedDecibelTextRuns = newCachedDecibelTextRuns;
        cachedPeakRmsTickTextRuns = newCachedPeakRmsTickTextRuns;
        cachedGlyphAtlasEntries = atlasEntries;
        cachedGlyphAdvance = advancePoints;
        cachedGlyphCellWidth = cellWidthPoints;
        cachedGlyphCellHeight = cellHeightPoints;
        maximumCachedDecibelTextWidth = newMaximumCachedDecibelTextWidth;
        maximumCachedPeakRmsTickTextWidth = newMaximumCachedPeakRmsTickTextWidth;
        glyphAtlasBackingScale = backingScale;
        return true;
    }

    static void assertMessageThread() noexcept
    {
        auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        jassert(messageManager != nullptr && messageManager->isThisTheMessageThread());
        juce::ignoreUnused(messageManager);
    }

    [[nodiscard]] std::shared_ptr<AtomicRenderTelemetry> loadPublishedTelemetry() const noexcept
    {
        return std::atomic_load_explicit(&publishedTelemetry, std::memory_order_acquire);
    }

    void queueTelemetryReset()
    {
        auto replacement = std::make_shared<AtomicRenderTelemetry>();
        const auto epoch = requestedTelemetryEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        replacement->epoch = epoch;

        auto pending = std::atomic_load_explicit(&pendingTelemetry, std::memory_order_acquire);

        for (;;) {
            if (requestedTelemetryEpoch.load(std::memory_order_acquire) != epoch)
                return;

            if (pending != nullptr && pending->epoch >= epoch)
                return;

            if (std::atomic_compare_exchange_weak_explicit(&pendingTelemetry, &pending, replacement,
                    std::memory_order_release, std::memory_order_acquire)) {
                return;
            }
        }
    }

    static void copyPersistentTelemetry(
        const AtomicRenderTelemetry& sourceTelemetry, AtomicRenderTelemetry& destination) noexcept
    {
        destination.drawableWidthPixels.store(
            sourceTelemetry.drawableWidthPixels.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.drawableHeightPixels.store(
            sourceTelemetry.drawableHeightPixels.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.configuredMaximumFramesPerSecond.store(
            sourceTelemetry.configuredMaximumFramesPerSecond.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.backingScale.store(sourceTelemetry.backingScale.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.metalAvailable.store(
            sourceTelemetry.metalAvailable.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.renderingRequested.store(
            sourceTelemetry.renderingRequested.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.effectivelyRendering.store(
            sourceTelemetry.effectivelyRendering.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    void resetTelemetryTimingAtCallbackBoundary() noexcept
    {
        previousDisplayCallbackHostTime = 0.0;
        previousTargetTimestamp = 0.0;
        previousTargetPresentationTimestamp = 0.0;
    }

    void resetRendererStateWhileDisplayLinkIsPaused() noexcept
    {
        targetFrame = { };
        displayedSpectrum.fill(minimumDisplayDecibels);
        lastGeneration = 0;
        lastSpectrumSequence = 0;
        lastMeterSequence = 0;
        hasDisplayFrame = false;
        previousSmoothingTime = { };
        resetTelemetryTimingAtCallbackBoundary();
    }

    void applyPendingTelemetryAtSafeBoundary()
    {
        if (auto replacement = std::atomic_exchange_explicit(&pendingTelemetry,
                std::shared_ptr<AtomicRenderTelemetry> { }, std::memory_order_acq_rel);
            replacement != nullptr && replacement->epoch > callbackTelemetry->epoch) {
            copyPersistentTelemetry(*loadPublishedTelemetry(), *replacement);
            callbackTelemetry = std::move(replacement);
            std::atomic_store_explicit(
                &publishedTelemetry, callbackTelemetry, std::memory_order_release);
            resetTelemetryTimingAtCallbackBoundary();
        }
    }

    void setEffectiveActive(bool shouldBeActive)
    {
        assertMessageThread();

        if (effectiveActive.load(std::memory_order_acquire) == shouldBeActive)
            return;

        if (shouldBeActive) {
            // Keep the link paused until the coordinator has advanced its
            // generation and renderer state has been reset.
            source.setVisualizationActive(true);
            resetRendererStateWhileDisplayLinkIsPaused();
            queueTelemetryReset();
            applyPendingTelemetryAtSafeBoundary();
            effectiveActive.store(true, std::memory_order_release);
            loadPublishedTelemetry()->effectivelyRendering.store(true, std::memory_order_relaxed);

            if (view != nil)
                [view setDisplayLinkPaused:NO];

            notifyEffectiveActivityChanged(true);
            return;
        }

        if (view != nil)
            [view setDisplayLinkPaused:YES];

        effectiveActive.store(false, std::memory_order_release);
        loadPublishedTelemetry()->effectivelyRendering.store(false, std::memory_order_relaxed);
        applyPendingTelemetryAtSafeBoundary();
        source.setVisualizationActive(false);
        notifyEffectiveActivityChanged(false);
    }

    void refreshEffectiveActivity()
    {
        if (hasShutDown.load(std::memory_order_acquire) || view == nil) {
            setEffectiveActive(false);
            return;
        }

        auto* window = view.window;
        const auto windowIsVisible = window != nil && window.visible && !window.miniaturized
            && (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
        const auto nativeViewIsVisible = !view.hiddenOrHasHiddenAncestor;
        const auto shouldRender = requestedActive.load(std::memory_order_relaxed)
            && juceShowing.load(std::memory_order_relaxed) && metalReady && windowIsVisible
            && nativeViewIsVisible;

        setEffectiveActive(shouldRender);
    }

    void notifyEffectiveActivityChanged(const bool isActive) noexcept
    {
        if (!effectiveActivityCallback)
            return;

        try {
            effectiveActivityCallback(isActive);
        } catch (...) {
            // A diagnostics/lifecycle observer must never destabilize its host.
        }
    }

    void updateScreenProperties()
    {
        if (view == nil)
            return;

        auto* screen = view.window.screen;
        const auto maximumFps
            = screen != nil ? std::max<NSInteger>(1, screen.maximumFramesPerSecond) : 60;
        [view setDisplayLinkMaximumFramesPerSecond:maximumFps];
        loadPublishedTelemetry()->configuredMaximumFramesPerSecond.store(
            static_cast<std::uint32_t>(maximumFps), std::memory_order_relaxed);
        updateBackingScale(true);
    }

    void updateBackingScale(const bool mayRebuildDensityDependentResources)
    {
        if (view == nil)
            return;

        const auto backingSize = [view convertSizeToBacking:NSMakeSize(1.0, 1.0)];
        auto scale = static_cast<double>(backingSize.width);

        if ((!std::isfinite(scale) || scale <= 0.0) && view.window != nil)
            scale = static_cast<double>(view.window.backingScaleFactor);

        if (!std::isfinite(scale) || scale <= 0.0)
            scale = 1.0;

        loadPublishedTelemetry()->backingScale.store(scale, std::memory_order_relaxed);

        if (mayRebuildDensityDependentResources
            && std::abs(scale - glyphAtlasBackingScale) > 0.001) {
            rebuildGlyphAtlas(scale);
        }
    }

    RenderBufferAdmission acquireRenderBuffer() noexcept
    {
        for (std::size_t index = 0; index < sharedState->slots.size(); ++index) {
            auto expected
                = sharedState->slots[index].admissionState.load(std::memory_order_relaxed);

            if ((expected & 1U) == 0U
                && sharedState->slots[index].admissionState.compare_exchange_strong(
                    expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                return { index, expected + 1 };
            }
        }

        return { };
    }

    std::uint64_t recordDisplayLinkCallback(AtomicRenderTelemetry& telemetry,
        CFTimeInterval callbackHostTime, CFTimeInterval targetTimestamp,
        CFTimeInterval targetPresentationTimestamp) noexcept
    {
        const auto presentationSequence
            = telemetry.displayLinkCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        telemetry.lastTargetTimestampNanoseconds.store(
            hostTimeNanoseconds(targetTimestamp), std::memory_order_relaxed);
        telemetry.lastTargetPresentationTimestampNanoseconds.store(
            hostTimeNanoseconds(targetPresentationTimestamp), std::memory_order_relaxed);

        if (previousDisplayCallbackHostTime > 0.0)
            telemetry.lastDisplayCallbackIntervalNanoseconds.store(
                positiveHostTimeDifference(callbackHostTime, previousDisplayCallbackHostTime),
                std::memory_order_relaxed);

        if (previousTargetTimestamp > 0.0)
            telemetry.lastTargetIntervalNanoseconds.store(
                positiveHostTimeDifference(targetTimestamp, previousTargetTimestamp),
                std::memory_order_relaxed);

        if (previousTargetPresentationTimestamp > 0.0)
            telemetry.lastTargetPresentationIntervalNanoseconds.store(
                positiveHostTimeDifference(
                    targetPresentationTimestamp, previousTargetPresentationTimestamp),
                std::memory_order_relaxed);

        const auto hasCallbackHostDelay = std::isfinite(callbackHostTime) && callbackHostTime > 0.0
            && std::isfinite(targetTimestamp) && targetTimestamp > 0.0;

        if (hasCallbackHostDelay) {
            const auto callbackHostDelay
                = positiveHostTimeDifference(callbackHostTime, targetTimestamp);
            telemetry.lastCallbackHostDelayNanoseconds.store(
                callbackHostDelay, std::memory_order_relaxed);
            telemetry.callbackHostDelaySamples.fetch_add(1, std::memory_order_relaxed);

            if (callbackHostDelay != 0)
                telemetry.callbackAlreadyLateHostDelays.fetch_add(1, std::memory_order_relaxed);
        } else {
            telemetry.callbackHostDelayUnclassifiableSamples.fetch_add(
                1, std::memory_order_relaxed);
        }

        previousDisplayCallbackHostTime = callbackHostTime;
        previousTargetTimestamp = targetTimestamp;
        previousTargetPresentationTimestamp = targetPresentationTimestamp;
        return presentationSequence;
    }

    void acceptSnapshot(const VisualizationFrame& incoming, AtomicRenderTelemetry& telemetry)
    {
        const auto generationChanged = incoming.generation != lastGeneration;
        const auto isNew = !hasDisplayFrame || generationChanged
            || incoming.spectrumSequence != lastSpectrumSequence
            || incoming.meterSequence != lastMeterSequence;

        if (!isNew)
            return;

        targetFrame = incoming;
        lastGeneration = incoming.generation;
        lastSpectrumSequence = incoming.spectrumSequence;
        lastMeterSequence = incoming.meterSequence;
        telemetry.framesWithNewSnapshot.fetch_add(1, std::memory_order_relaxed);
        telemetry.lastSpectrumSequence.store(lastSpectrumSequence, std::memory_order_relaxed);

        if (!hasDisplayFrame || generationChanged) {
            const auto settings = getSpectrumSettings();

            for (std::size_t index = 0; index < displayedSpectrum.size(); ++index)
                displayedSpectrum[index] = sanitiseDecibels(targetFrame.spectrumDecibels[index],
                    settings.floorDecibels, settings.ceilingDecibels);

            hasDisplayFrame = true;
        }
    }

    void updateSmoothedDisplayValues(Clock::time_point now, SpectrumRenderSettings settings)
    {
        if (!hasDisplayFrame)
            return;

        const auto elapsed = previousSmoothingTime != Clock::time_point { }
            ? std::chrono::duration<double>(now - previousSmoothingTime).count()
            : 1.0 / 60.0;
        previousSmoothingTime = now;

        const auto smoothingShape = settings.smoothing * settings.smoothing;
        const auto spectrumRise = settings.smoothing <= 0.0F
            ? 1.0F
            : smoothingCoefficient(elapsed, 0.004 + (0.096 * smoothingShape));
        const auto spectrumFall = settings.smoothing <= 0.0F
            ? 1.0F
            : smoothingCoefficient(elapsed, 0.015 + (0.435 * smoothingShape));
        for (std::size_t index = 0; index < displayedSpectrum.size(); ++index) {
            const auto target = sanitiseDecibels(targetFrame.spectrumDecibels[index],
                settings.floorDecibels, settings.ceilingDecibels);
            const auto coefficient
                = target >= displayedSpectrum[index] ? spectrumRise : spectrumFall;
            displayedSpectrum[index] += coefficient * (target - displayedSpectrum[index]);
        }
    }

    VertexBatches populateVertices(MetalVertex* vertices, CGSize logicalSize,
        const DashboardTileLayout& dashboardLayout, SpectrumRenderSettings settings) const noexcept
    {
        VertexBatches batches;

        if (vertices == nullptr || logicalSize.width < 2.0 || logicalSize.height < 2.0)
            return batches;

        std::size_t cursor = 0;
        const auto width = static_cast<float>(logicalSize.width);
        const auto height = static_cast<float>(logicalSize.height);

        const auto pointToClip = [width, height](float x, float y) noexcept {
            return simd_make_float2((2.0F * x / width) - 1.0F, (2.0F * y / height) - 1.0F);
        };

        const auto appendVertex = [&](float x, float y, simd_float4 colour) noexcept {
            jassert(cursor < maximumVertexCount);

            if (cursor >= maximumVertexCount)
                return;

            vertices[cursor++] = { pointToClip(x, y), colour, simd_make_float2(0.0F, 0.0F) };
        };

        const auto appendQuad
            = [&](float left, float bottom, float right, float top, simd_float4 colour) noexcept {
                  if (right <= left || top <= bottom)
                      return;

                  const auto bottomLeft = pointToClip(left, bottom);
                  const auto bottomRight = pointToClip(right, bottom);
                  const auto topLeft = pointToClip(left, top);
                  const auto topRight = pointToClip(right, top);

                  jassert(cursor + 6 <= maximumVertexCount);

                  if (cursor + 6 > maximumVertexCount)
                      return;

                  constexpr auto noTextureCoordinate = simd_float2 { 0.0F, 0.0F };
                  vertices[cursor++] = { bottomLeft, colour, noTextureCoordinate };
                  vertices[cursor++] = { bottomRight, colour, noTextureCoordinate };
                  vertices[cursor++] = { topLeft, colour, noTextureCoordinate };
                  vertices[cursor++] = { topLeft, colour, noTextureCoordinate };
                  vertices[cursor++] = { bottomRight, colour, noTextureCoordinate };
                  vertices[cursor++] = { topRight, colour, noTextureCoordinate };
              };

        const auto density = static_cast<float>(
            std::isfinite(glyphAtlasBackingScale) && glyphAtlasBackingScale > 0.0
                ? glyphAtlasBackingScale
                : 1.0);
        const auto snapToPixel = [density](const float value) noexcept {
            return std::round(value * density) / density;
        };
        const auto appendTextRun
            = [&](const CachedTextRun& run, const float originX, const float originY,
                  const float textScale, const simd_float4 colour) noexcept {
                  if (run.glyphCount == 0 || textScale <= 0.0F)
                      return;

                  jassert(cursor + (run.glyphCount * 6) <= maximumVertexCount);

                  if (cursor + (run.glyphCount * 6) > maximumVertexCount)
                      return;

                  for (std::size_t index = 0; index < run.glyphCount; ++index) {
                      const auto& glyph = run.glyphs[index];
                      const auto left = snapToPixel(originX + (glyph.left * textScale));
                      const auto bottom = snapToPixel(originY + (glyph.bottom * textScale));
                      const auto right = snapToPixel(originX + (glyph.right * textScale));
                      const auto top = snapToPixel(originY + (glyph.top * textScale));
                      const auto bottomLeft = pointToClip(left, bottom);
                      const auto bottomRight = pointToClip(right, bottom);
                      const auto topLeft = pointToClip(left, top);
                      const auto topRight = pointToClip(right, top);
                      const auto bottomLeftTexture = simd_make_float2(
                          glyph.atlas.leftTextureCoordinate, glyph.atlas.bottomTextureCoordinate);
                      const auto bottomRightTexture = simd_make_float2(
                          glyph.atlas.rightTextureCoordinate, glyph.atlas.bottomTextureCoordinate);
                      const auto topLeftTexture = simd_make_float2(
                          glyph.atlas.leftTextureCoordinate, glyph.atlas.topTextureCoordinate);
                      const auto topRightTexture = simd_make_float2(
                          glyph.atlas.rightTextureCoordinate, glyph.atlas.topTextureCoordinate);

                      vertices[cursor++] = { bottomLeft, colour, bottomLeftTexture };
                      vertices[cursor++] = { bottomRight, colour, bottomRightTexture };
                      vertices[cursor++] = { topLeft, colour, topLeftTexture };
                      vertices[cursor++] = { topLeft, colour, topLeftTexture };
                      vertices[cursor++] = { bottomRight, colour, bottomRightTexture };
                      vertices[cursor++] = { topRight, colour, topRightTexture };
                  }
              };
        const auto monospacedTextRunWidth = [&](const CachedMonospacedTextRun& run) noexcept {
            return run.glyphCount == 0
                ? 0.0F
                : (static_cast<float>(run.glyphCount - 1) * cachedGlyphAdvance)
                    + cachedGlyphCellWidth;
        };
        const auto appendMonospacedTextRun = [&](const CachedMonospacedTextRun& run,
                                                 const float originX, const float originY,
                                                 const float textScale,
                                                 const simd_float4 colour) noexcept {
            if (run.glyphCount == 0 || textScale <= 0.0F)
                return;

            jassert(cursor + (static_cast<std::size_t>(run.glyphCount) * 6) <= maximumVertexCount);

            if (cursor + (static_cast<std::size_t>(run.glyphCount) * 6) > maximumVertexCount) {
                return;
            }

            for (std::size_t index = 0; index < run.glyphCount; ++index) {
                const auto left = snapToPixel(
                    originX + (static_cast<float>(index) * cachedGlyphAdvance * textScale));
                const auto bottom = snapToPixel(originY);
                const auto right = snapToPixel(left + (cachedGlyphCellWidth * textScale));
                const auto top = snapToPixel(originY + (cachedGlyphCellHeight * textScale));
                const auto bottomLeft = pointToClip(left, bottom);
                const auto bottomRight = pointToClip(right, bottom);
                const auto topLeft = pointToClip(left, top);
                const auto topRight = pointToClip(right, top);
                const auto& atlas = cachedGlyphAtlasEntries[run.atlasIndices[index]];
                const auto bottomLeftTexture
                    = simd_make_float2(atlas.leftTextureCoordinate, atlas.bottomTextureCoordinate);
                const auto bottomRightTexture
                    = simd_make_float2(atlas.rightTextureCoordinate, atlas.bottomTextureCoordinate);
                const auto topLeftTexture
                    = simd_make_float2(atlas.leftTextureCoordinate, atlas.topTextureCoordinate);
                const auto topRightTexture
                    = simd_make_float2(atlas.rightTextureCoordinate, atlas.topTextureCoordinate);

                vertices[cursor++] = { bottomLeft, colour, bottomLeftTexture };
                vertices[cursor++] = { bottomRight, colour, bottomRightTexture };
                vertices[cursor++] = { topLeft, colour, topLeftTexture };
                vertices[cursor++] = { topLeft, colour, topLeftTexture };
                vertices[cursor++] = { bottomRight, colour, bottomRightTexture };
                vertices[cursor++] = { topRight, colour, topRightTexture };
            }
        };

        const auto appendBorder = [&](const RenderRect& bounds, float thickness,
                                      simd_float4 colour) noexcept {
            if (bounds.width() <= 0.0F || bounds.height() <= 0.0F)
                return;

            thickness
                = std::min(thickness, std::min(bounds.width() * 0.5F, bounds.height() * 0.5F));
            appendQuad(bounds.left, bounds.bottom, bounds.right, bounds.bottom + thickness, colour);
            appendQuad(bounds.left, bounds.top - thickness, bounds.right, bounds.top, colour);
            appendQuad(bounds.left, bounds.bottom + thickness, bounds.left + thickness,
                bounds.top - thickness, colour);
            appendQuad(bounds.right - thickness, bounds.bottom + thickness, bounds.right,
                bounds.top - thickness, colour);
        };

        const auto headerHeight = [](const RenderRect& bounds) noexcept {
            return std::min(bounds.height(), std::clamp(bounds.height() * 0.13F, 18.0F, 26.0F));
        };

        constexpr auto livePanelColour = simd_float4 { 0.025F, 0.035F, 0.052F, 1.0F };
        constexpr auto placeholderPanelColour = simd_float4 { 0.014F, 0.020F, 0.030F, 1.0F };
        constexpr auto spectrogramPanelColour = simd_float4 { 0.0F, 0.0F, 0.0F, 1.0F };
        constexpr auto panelBorderColour = simd_float4 { 0.13F, 0.17F, 0.23F, 0.90F };
        constexpr auto headerDividerColour = simd_float4 { 0.12F, 0.17F, 0.24F, 0.72F };

        batches.shell.start = cursor;

        for (std::size_t index = 0; index < dashboardPanelCount; ++index) {
            const auto panel = static_cast<DashboardPanel>(index);
            const auto bounds = toRenderRect(dashboardLayout[panel], height);
            auto fillColour = placeholderPanelColour;

            if (panel == DashboardPanel::spectrum || panel == DashboardPanel::peakRms)
                fillColour = livePanelColour;
            else if (panel == DashboardPanel::spectrogram)
                fillColour = spectrogramPanelColour;

            appendQuad(bounds.left, bounds.bottom, bounds.right, bounds.top, fillColour);
            appendBorder(bounds, 1.0F, panelBorderColour);

            const auto dividerY = bounds.top - headerHeight(bounds);
            appendQuad(bounds.left + 1.0F, dividerY - 0.5F, bounds.right - 1.0F, dividerY + 0.5F,
                headerDividerColour);
        }

        batches.shell.count = cursor - batches.shell.start;

        constexpr auto axisTextScale = 0.80F;
        const auto axisTextHeight = cachedGlyphCellHeight * axisTextScale;
        const auto nyquist = targetFrame.sampleRate > 0.0
            ? static_cast<float>(targetFrame.sampleRate * 0.5)
            : maximumSpectrumFrequency;
        const auto maximumFrequency
            = std::clamp(nyquist, minimumSpectrumFrequency + 1.0F, maximumSpectrumFrequency);
        const FrequencyAxisMapping frequencyMapping {
            minimumSpectrumFrequency,
            maximumFrequency,
            settings.frequencySpacing,
        };
        const auto upperEndpointFrequency = static_cast<std::size_t>(std::lround(maximumFrequency));
        const auto& upperEndpointTextRun = cachedFrequencyEndpointTextRuns()[std::min<std::size_t>(
            upperEndpointFrequency, cachedFrequencyEndpointTextRuns().size() - 1)];
        const auto upperEndpointTextWidth
            = monospacedTextRunWidth(upperEndpointTextRun) * axisTextScale;
        std::array<float, frequencyAxisTickCandidateCount> frequencyLabelWidths { };
        std::array<float, frequencyAxisTickCandidateCount> frequencyLabelHeights { };
        auto maximumFrequencyLabelWidth = upperEndpointTextWidth;
        for (std::size_t index = 0; index < cachedFrequencyAxisTextRuns.size(); ++index) {
            frequencyLabelWidths[index] = cachedFrequencyAxisTextRuns[index].width * axisTextScale;
            frequencyLabelHeights[index]
                = cachedFrequencyAxisTextRuns[index].height * axisTextScale;
            maximumFrequencyLabelWidth
                = std::max(maximumFrequencyLabelWidth, frequencyLabelWidths[index]);
        }

        const auto spectrumPanel = toRenderRect(dashboardLayout[DashboardPanel::spectrum], height);
        const auto spectrumPlot
            = insetRenderRect(spectrumPanel, (maximumCachedDecibelTextWidth * axisTextScale) + 8.0F,
                axisTextHeight + 7.0F, 8.0F, headerHeight(spectrumPanel) + 7.0F);
        const auto plotLeft = spectrumPlot.left;
        const auto plotRight = spectrumPlot.right;
        const auto plotBottom = spectrumPlot.bottom;
        const auto plotTop = spectrumPlot.top;
        const auto frequencyToX = [&](const float frequency) noexcept {
            return plotLeft
                + (mapFrequencyToUnit(frequencyMapping, frequency) * spectrumPlot.width());
        };

        const auto decibelsToY = [&](float decibels, float minimum, float maximum) noexcept {
            const auto normalised
                = (sanitiseDecibels(decibels, minimum, maximum) - minimum) / (maximum - minimum);
            return plotBottom + normalised * (plotTop - plotBottom);
        };

        constexpr auto gridColour = simd_float4 { 0.16F, 0.20F, 0.27F, 0.62F };
        const auto spectrumFrequencyTicks = selectFrequencyAxisTicks(
            frequencyMapping, spectrumPlot.width(), frequencyLabelWidths, upperEndpointTextWidth);
        const auto spectrumDecibelTicks = makeSpectrumDecibelTicks(
            settings.floorDecibels, settings.ceilingDecibels, spectrumPlot.height());

        batches.spectrumGrid.start = cursor;

        if (spectrumPlot.width() > 0.0F && spectrumPlot.height() > 0.0F) {
            for (std::size_t index = 0; index < spectrumFrequencyTicks.count; ++index) {
                const auto x = frequencyToX(spectrumFrequencyTicks.ticks[index].frequencyHz);
                appendQuad(x - 0.5F, plotBottom, x + 0.5F, plotTop, gridColour);
            }

            for (std::size_t index = 0; index < spectrumDecibelTicks.count; ++index) {
                const auto y = decibelsToY(static_cast<float>(spectrumDecibelTicks.values[index]),
                    settings.floorDecibels, settings.ceilingDecibels);
                appendQuad(plotLeft, y - 0.5F, plotRight, y + 0.5F, gridColour);
            }
        }

        batches.spectrumGrid.count = cursor - batches.spectrumGrid.start;
        batches.spectrum.start = cursor;

        if (spectrumPlot.width() > 0.0F && spectrumPlot.height() > 0.0F && hasDisplayFrame
            && targetFrame.spectrumValid && targetFrame.sampleRate > 0.0) {
            const auto binFrequency
                = static_cast<float>(targetFrame.sampleRate / static_cast<double>(fftSize));
            const auto firstBin = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::ceil(minimumSpectrumFrequency / binFrequency)));
            const auto finalBin = std::min<std::size_t>(spectrumBinCount - 1,
                static_cast<std::size_t>(std::floor(maximumFrequency / binFrequency)));
            constexpr auto spectrumColour = simd_float4 { 0.18F, 0.90F, 0.85F, 1.0F };
            constexpr auto spectrumHalfWidthPoints = 0.75F;
            std::array<simd_float2, spectrumBinCount> spectrumPoints;
            std::size_t pointCount = 0;

            for (auto bin = firstBin; bin <= finalBin && pointCount < spectrumPoints.size();
                ++bin) {
                const auto frequency
                    = std::max(minimumSpectrumFrequency, static_cast<float>(bin) * binFrequency);
                spectrumPoints[pointCount++] = simd_make_float2(frequencyToX(frequency),
                    decibelsToY(
                        displayedSpectrum[bin], settings.floorDecibels, settings.ceilingDecibels));
            }

            for (std::size_t index = 0; index < pointCount; ++index) {
                const auto previous = spectrumPoints[index == 0 ? index : index - 1];
                const auto next = spectrumPoints[index + 1 < pointCount ? index + 1 : index];
                const auto deltaX = next.x - previous.x;
                const auto deltaY = next.y - previous.y;
                const auto length = std::sqrt((deltaX * deltaX) + (deltaY * deltaY));
                const auto inverseLength = length > 0.0001F ? 1.0F / length : 0.0F;
                const auto normalX = -deltaY * inverseLength * spectrumHalfWidthPoints;
                const auto normalY = deltaX * inverseLength * spectrumHalfWidthPoints;

                appendVertex(spectrumPoints[index].x + normalX, spectrumPoints[index].y + normalY,
                    spectrumColour);
                appendVertex(spectrumPoints[index].x - normalX, spectrumPoints[index].y - normalY,
                    spectrumColour);
            }
        }

        batches.spectrum.count = cursor - batches.spectrum.start;
        const auto spectrogramPanel
            = toRenderRect(dashboardLayout[DashboardPanel::spectrogram], height);
        const auto spectrogramPlot = insetRenderRect(spectrogramPanel,
            maximumFrequencyLabelWidth + 8.0F, 8.0F, 8.0F, headerHeight(spectrogramPanel) + 7.0F);
        const auto spectrogramFrequencyTicks = selectFrequencyAxisTicks(
            frequencyMapping, spectrogramPlot.height(), frequencyLabelHeights, axisTextHeight);
        constexpr auto spectrogramTickColour = simd_float4 { 0.19F, 0.25F, 0.33F, 0.78F };
        batches.spectrogramAxis.start = cursor;

        if (spectrogramPlot.width() > 0.0F && spectrogramPlot.height() > 0.0F) {
            for (std::size_t index = 0; index < spectrogramFrequencyTicks.count; ++index) {
                const auto unit = mapFrequencyToUnit(
                    frequencyMapping, spectrogramFrequencyTicks.ticks[index].frequencyHz);
                const auto y = spectrogramPlot.bottom + (unit * spectrogramPlot.height());
                appendQuad(spectrogramPlot.left - 4.0F, y - 0.5F, spectrogramPlot.left + 2.0F,
                    y + 0.5F, spectrogramTickColour);
            }
        }

        batches.spectrogramAxis.count = cursor - batches.spectrogramAxis.start;
        batches.peakRms.start = cursor;

        const auto meterPanel = toRenderRect(dashboardLayout[DashboardPanel::peakRms], height);
        constexpr auto meterTextScale = 0.78F;
        const auto meterTextHeight = cachedGlyphCellHeight * meterTextScale;
        const auto maximumMeterReadoutWidth
            = (((static_cast<float>(maximumPeakRmsReadoutGlyphs) - 1.0F) * cachedGlyphAdvance)
                  + cachedGlyphCellWidth)
            * meterTextScale;
        const auto meterChannelCount = hasDisplayFrame && targetFrame.meterValid
                && (targetFrame.channelCount == 1 || targetFrame.channelCount == 2)
            ? targetFrame.channelCount
            : 0;
        const auto meterLayout = calculatePeakRmsPanelLayout(meterPanel.width(),
            meterPanel.height(), headerHeight(meterPanel), meterChannelCount, meterTextHeight,
            maximumCachedPeakRmsTickTextWidth * meterTextScale, maximumMeterReadoutWidth);
        const auto toMeterPanelRect = [&](const PeakRmsLogicalRect& bounds) noexcept {
            return RenderRect { meterPanel.left + bounds.left, meterPanel.bottom + bounds.bottom,
                meterPanel.left + bounds.right, meterPanel.bottom + bounds.top };
        };
        constexpr auto meterTrackColour = simd_float4 { 0.08F, 0.11F, 0.15F, 1.0F };
        constexpr auto meterGridColour = simd_float4 { 0.17F, 0.22F, 0.29F, 0.58F };
        constexpr auto meterZeroGridColour = simd_float4 { 0.45F, 0.21F, 0.16F, 0.82F };
        constexpr auto clearButtonColour = simd_float4 { 0.055F, 0.075F, 0.105F, 1.0F };
        constexpr auto clearButtonBorderColour = simd_float4 { 0.20F, 0.27F, 0.36F, 0.90F };
        const auto peakColour = [](const PeakRmsLevelRange range) noexcept {
            switch (range) {
            case PeakRmsLevelRange::amber:
                return simd_float4 { 1.0F, 0.72F, 0.20F, 1.0F };
            case PeakRmsLevelRange::red:
                return simd_float4 { 1.0F, 0.20F, 0.12F, 1.0F };
            case PeakRmsLevelRange::cyan:
                return simd_float4 { 0.18F, 0.90F, 0.85F, 1.0F };
            }

            return simd_float4 { 0.18F, 0.90F, 0.85F, 1.0F };
        };
        const auto rmsColour = [](const PeakRmsLevelRange range) noexcept {
            switch (range) {
            case PeakRmsLevelRange::amber:
                return simd_float4 { 0.55F, 0.36F, 0.09F, 0.82F };
            case PeakRmsLevelRange::red:
                return simd_float4 { 0.56F, 0.11F, 0.08F, 0.82F };
            case PeakRmsLevelRange::cyan:
                return simd_float4 { 0.10F, 0.48F, 0.62F, 0.82F };
            }

            return simd_float4 { 0.10F, 0.48F, 0.62F, 0.82F };
        };

        if (meterLayout.clearVisualBounds.width() > 0.0F) {
            const auto clearBounds = toMeterPanelRect(meterLayout.clearVisualBounds);
            appendQuad(clearBounds.left, clearBounds.bottom, clearBounds.right, clearBounds.top,
                clearButtonColour);
            appendBorder(clearBounds, 1.0F, clearButtonBorderColour);
        }

        const auto meterScaleHeight = meterLayout.scaleTop - meterLayout.scaleBottom;
        if (meterScaleHeight > 0.0F && meterLayout.tickLineRight > meterLayout.tickLineLeft) {
            for (const auto decibels : peakRmsMajorDecibelTicks) {
                const auto y = meterPanel.bottom + meterLayout.scaleBottom
                    + (mapPeakRmsDecibelsToUnit(static_cast<float>(decibels)) * meterScaleHeight);
                const auto colour = decibels == 0 ? meterZeroGridColour : meterGridColour;
                appendQuad(meterPanel.left + meterLayout.tickLineLeft, y - 0.5F,
                    meterPanel.left + meterLayout.tickLineRight, y + 0.5F, colour);
            }
        }

        for (std::size_t channel = 0; channel < meterLayout.channelCount; ++channel) {
            const auto track = toMeterPanelRect(meterLayout.channelTracks[channel]);
            appendQuad(track.left, track.bottom, track.right, track.top, meterTrackColour);

            const auto rmsReadout = classifyPeakRmsReadout(targetFrame.rmsDecibels[channel]);
            if (rmsReadout.kind == PeakRmsReadout::Kind::decibelTenths
                && targetFrame.rmsDecibels[channel] > peakRmsMinimumDecibels) {
                const auto rmsTop = track.bottom
                    + (mapPeakRmsDecibelsToUnit(targetFrame.rmsDecibels[channel]) * track.height());
                appendQuad(track.left + 1.0F, track.bottom + 1.0F, track.right - 1.0F, rmsTop,
                    rmsColour(rmsReadout.levelRange));
            }

            const auto peakReadout = classifyPeakRmsReadout(targetFrame.peakDecibels[channel]);
            if (peakReadout.kind == PeakRmsReadout::Kind::decibelTenths && track.height() >= 2.0F) {
                const auto peakY = std::clamp(track.bottom
                        + (mapPeakRmsDecibelsToUnit(targetFrame.peakDecibels[channel])
                            * track.height()),
                    track.bottom + 1.0F, track.top - 1.0F);
                const auto peakWidth = track.width() * 0.48F;
                const auto peakLeft = track.left + ((track.width() - peakWidth) * 0.5F);
                appendQuad(peakLeft, peakY - 1.0F, peakLeft + peakWidth, peakY + 1.0F,
                    peakColour(peakReadout.levelRange));
            }

            const auto heldReadout = classifyPeakRmsReadout(targetFrame.heldPeakDecibels[channel]);
            if (heldReadout.kind == PeakRmsReadout::Kind::decibelTenths && track.height() >= 1.0F) {
                const auto holdY = std::clamp(track.bottom
                        + (mapPeakRmsDecibelsToUnit(targetFrame.heldPeakDecibels[channel])
                            * track.height()),
                    track.bottom + 0.5F, track.top - 0.5F);
                appendQuad(track.left - 2.0F, holdY - 0.5F, track.right + 2.0F, holdY + 0.5F,
                    peakColour(heldReadout.levelRange));
            }
        }

        batches.peakRms.count = cursor - batches.peakRms.start;

        constexpr auto titleColour = simd_float4 { 0.72F, 0.79F, 0.88F, 0.94F };
        constexpr auto axisTextColour = simd_float4 { 0.54F, 0.62F, 0.72F, 0.90F };
        constexpr auto placeholderTextColour = simd_float4 { 0.39F, 0.46F, 0.55F, 0.72F };

        for (std::size_t index = 0; index < dashboardPanelCount; ++index) {
            const auto panel = static_cast<DashboardPanel>(index);
            const auto bounds = toRenderRect(dashboardLayout[panel], height);
            const auto panelHeaderHeight = headerHeight(bounds);
            const auto& titleRun = cachedFixedTextRuns[panelTitleTextRunIndices[index]];
            const auto titleAvailableWidth
                = panel == DashboardPanel::peakRms && meterLayout.clearVisualBounds.width() > 0.0F
                ? std::max(0.0F, meterLayout.clearVisualBounds.left - 12.0F)
                : std::max(0.0F, bounds.width() - 14.0F);
            const auto titleScale = titleRun.width > 0.0F
                ? std::min(1.0F, titleAvailableWidth / titleRun.width)
                : 1.0F;
            const auto titleX = bounds.left + 7.0F;
            const auto titleY = bounds.top - panelHeaderHeight
                + std::max(0.0F, (panelHeaderHeight - (titleRun.height * titleScale)) * 0.5F);

            batches.text[index].start = cursor;
            appendTextRun(titleRun, titleX, titleY, titleScale, titleColour);

            if (panel == DashboardPanel::peakRms) {
                constexpr auto meterAxisTextColour = simd_float4 { 0.54F, 0.62F, 0.72F, 0.90F };
                constexpr auto meterChannelTextColour = simd_float4 { 0.67F, 0.75F, 0.84F, 0.94F };
                constexpr auto clearTextColour = simd_float4 { 0.58F, 0.69F, 0.80F, 0.96F };
                constexpr auto overTextColour = simd_float4 { 1.0F, 0.28F, 0.18F, 1.0F };

                if (meterLayout.clearVisualBounds.width() > 0.0F) {
                    const auto clearBounds = toMeterPanelRect(meterLayout.clearVisualBounds);
                    const auto& clearRun = cachedFixedTextRuns[clearTextRunIndex];
                    const auto clearScale = std::min(0.78F,
                        std::min(clearBounds.width() / clearRun.width,
                            clearBounds.height() / clearRun.height));
                    appendTextRun(clearRun,
                        clearBounds.left
                            + ((clearBounds.width() - (clearRun.width * clearScale)) * 0.5F),
                        clearBounds.bottom
                            + ((clearBounds.height() - (clearRun.height * clearScale)) * 0.5F),
                        clearScale, clearTextColour);
                }

                if (meterLayout.showTickLabels && meterScaleHeight > 0.0F) {
                    const auto visibleLabels
                        = selectPeakRmsTickLabels(meterScaleHeight, meterTextHeight);

                    for (std::size_t tick = 0; tick < peakRmsMajorDecibelTicks.size(); ++tick) {
                        if (!visibleLabels.visible[tick])
                            continue;

                        const auto& run = cachedPeakRmsTickTextRuns[tick];
                        const auto labelWidth = run.width * meterTextScale;
                        const auto labelHeight = run.height * meterTextScale;
                        const auto centreY = meterPanel.bottom + meterLayout.scaleBottom
                            + (mapPeakRmsDecibelsToUnit(
                                   static_cast<float>(peakRmsMajorDecibelTicks[tick]))
                                * meterScaleHeight);
                        const auto labelY = std::clamp(centreY - (labelHeight * 0.5F),
                            meterPanel.bottom + meterLayout.scaleBottom,
                            meterPanel.bottom + meterLayout.scaleTop - labelHeight);
                        appendTextRun(run,
                            meterPanel.left + meterLayout.tickLabelRight - labelWidth, labelY,
                            meterTextScale, meterAxisTextColour);
                    }
                }

                for (std::size_t channel = 0; channel < meterLayout.channelCount; ++channel) {
                    const auto column = toMeterPanelRect(meterLayout.channelColumns[channel]);
                    const auto channelRunIndex = meterLayout.channelCount == 1
                        ? monoChannelTextRunIndex
                        : (channel == 0 ? leftChannelTextRunIndex : rightChannelTextRunIndex);
                    const auto& channelRun = cachedFixedTextRuns[channelRunIndex];
                    const auto channelLabelWidth = channelRun.width * meterTextScale;
                    appendTextRun(channelRun,
                        column.left + ((column.width() - channelLabelWidth) * 0.5F),
                        meterPanel.bottom + meterLayout.channelLabelBottom, meterTextScale,
                        meterChannelTextColour);

                    if (meterLayout.showReadouts) {
                        const auto readout
                            = classifyPeakRmsReadout(targetFrame.peakDecibels[channel]);
                        const auto& readoutRun = readout.kind == PeakRmsReadout::Kind::minusInfinity
                            ? cachedMinusInfinityTextRun()
                            : cachedPeakRmsReadoutTextRuns()[static_cast<std::size_t>(
                                  readout.decibelTenths - minimumPeakRmsReadoutTenths)];
                        const auto readoutWidth
                            = monospacedTextRunWidth(readoutRun) * meterTextScale;
                        appendMonospacedTextRun(readoutRun,
                            column.left + ((column.width() - readoutWidth) * 0.5F),
                            meterPanel.bottom + meterLayout.readoutBottom, meterTextScale,
                            peakColour(readout.levelRange));
                    }

                    if (targetFrame.over[channel]) {
                        const auto& overRun = cachedFixedTextRuns[overTextRunIndex];
                        const auto overScale = fitPeakRmsTextScale(
                            std::max(0.0F, column.width() - 2.0F), overRun.width, meterTextScale);
                        const auto overWidth = overRun.width * overScale;
                        const auto overHeight = overRun.height * overScale;
                        appendTextRun(overRun, column.left + ((column.width() - overWidth) * 0.5F),
                            meterPanel.bottom + meterLayout.overBottom
                                + std::max(0.0F, (meterTextHeight - overHeight) * 0.5F),
                            overScale, overTextColour);
                    }
                }
            } else if (panel == DashboardPanel::spectrum) {
                for (std::size_t tickIndex = 0; tickIndex < spectrumFrequencyTicks.count;
                    ++tickIndex) {
                    const auto& tick = spectrumFrequencyTicks.ticks[tickIndex];
                    const auto centreX = frequencyToX(tick.frequencyHz);

                    if (tick.usesUpperEndpointLabel) {
                        const auto labelX = std::clamp(centreX - (upperEndpointTextWidth * 0.5F),
                            spectrumPlot.left,
                            std::max(
                                spectrumPlot.left, spectrumPlot.right - upperEndpointTextWidth));
                        appendMonospacedTextRun(upperEndpointTextRun, labelX,
                            spectrumPlot.bottom - axisTextHeight - 4.0F, axisTextScale,
                            axisTextColour);
                    } else if (tick.candidateIndex < cachedFrequencyAxisTextRuns.size()) {
                        const auto& run = cachedFrequencyAxisTextRuns[tick.candidateIndex];
                        const auto labelWidth = run.width * axisTextScale;
                        const auto labelX
                            = std::clamp(centreX - (labelWidth * 0.5F), spectrumPlot.left,
                                std::max(spectrumPlot.left, spectrumPlot.right - labelWidth));
                        appendTextRun(run, labelX, spectrumPlot.bottom - axisTextHeight - 4.0F,
                            axisTextScale, axisTextColour);
                    }
                }

                for (std::size_t tickIndex = 0; tickIndex < spectrumDecibelTicks.count;
                    ++tickIndex) {
                    const auto decibels = spectrumDecibelTicks.values[tickIndex];
                    const auto runIndex = static_cast<std::size_t>(
                        (decibels - minimumCachedDecibelTick) / cachedDecibelTickInterval);

                    if (runIndex >= cachedDecibelTextRuns.size())
                        continue;

                    const auto& run = cachedDecibelTextRuns[runIndex];
                    const auto labelWidth = run.width * axisTextScale;
                    const auto labelHeight = run.height * axisTextScale;
                    const auto centreY = decibelsToY(static_cast<float>(decibels),
                        settings.floorDecibels, settings.ceilingDecibels);
                    const auto labelY
                        = std::clamp(centreY - (labelHeight * 0.5F), spectrumPlot.bottom,
                            std::max(spectrumPlot.bottom, spectrumPlot.top - labelHeight));
                    appendTextRun(run, spectrumPlot.left - labelWidth - 5.0F, labelY, axisTextScale,
                        axisTextColour);
                }
            } else if (panel == DashboardPanel::spectrogram) {
                for (std::size_t tickIndex = 0; tickIndex < spectrogramFrequencyTicks.count;
                    ++tickIndex) {
                    const auto& tick = spectrogramFrequencyTicks.ticks[tickIndex];
                    const auto unit = mapFrequencyToUnit(frequencyMapping, tick.frequencyHz);
                    const auto centreY = spectrogramPlot.bottom + (unit * spectrogramPlot.height());

                    if (tick.usesUpperEndpointLabel) {
                        const auto labelY = std::clamp(centreY - (axisTextHeight * 0.5F),
                            spectrogramPlot.bottom,
                            std::max(spectrogramPlot.bottom, spectrogramPlot.top - axisTextHeight));
                        appendMonospacedTextRun(upperEndpointTextRun,
                            spectrogramPlot.left - upperEndpointTextWidth - 5.0F, labelY,
                            axisTextScale, axisTextColour);
                    } else if (tick.candidateIndex < cachedFrequencyAxisTextRuns.size()) {
                        const auto& run = cachedFrequencyAxisTextRuns[tick.candidateIndex];
                        const auto labelWidth = run.width * axisTextScale;
                        const auto labelHeight = run.height * axisTextScale;
                        const auto labelY = std::clamp(centreY - (labelHeight * 0.5F),
                            spectrogramPlot.bottom,
                            std::max(spectrogramPlot.bottom, spectrogramPlot.top - labelHeight));
                        appendTextRun(run, spectrogramPlot.left - labelWidth - 5.0F, labelY,
                            axisTextScale, axisTextColour);
                    }
                }
            }

            if (panel != DashboardPanel::spectrum && panel != DashboardPanel::peakRms) {
                const auto& placeholderRun = cachedFixedTextRuns[placeholderTextRunIndex];
                const auto placeholderBounds = panel == DashboardPanel::spectrogram
                    ? spectrogramPlot
                    : RenderRect { bounds.left, bounds.bottom, bounds.right,
                          bounds.top - panelHeaderHeight };
                const auto availableWidth = std::max(0.0F, placeholderBounds.width() - 12.0F);
                const auto availableHeight = std::max(0.0F, placeholderBounds.height() - 12.0F);
                const auto horizontalScale
                    = placeholderRun.width > 0.0F ? availableWidth / placeholderRun.width : 1.0F;
                const auto verticalScale
                    = placeholderRun.height > 0.0F ? availableHeight / placeholderRun.height : 1.0F;
                const auto placeholderScale
                    = std::max(0.0F, std::min({ 1.0F, horizontalScale, verticalScale }));
                const auto placeholderX = placeholderBounds.left
                    + (placeholderBounds.width() - (placeholderRun.width * placeholderScale))
                        * 0.5F;
                const auto placeholderY = placeholderBounds.bottom
                    + (placeholderBounds.height() - (placeholderRun.height * placeholderScale))
                        * 0.5F;
                appendTextRun(placeholderRun, placeholderX, placeholderY, placeholderScale,
                    placeholderTextColour);
            }

            batches.text[index].count = cursor - batches.text[index].start;
        }

        return batches;
    }
};
} // namespace audio_insight::detail

@implementation AIAudioInsightMetalView

- (void)attachRenderBackend:(audio_insight::detail::MetalRenderBackend*)backend
{
    renderBackend = backend;
    self.delegate = nil;
    self.paused = YES;
    self.accessibilityElement = YES;
    self.accessibilityRole = NSAccessibilityGroupRole;
    self.accessibilityLabel = @"Audio Insight analyzer dashboard";
    self.accessibilityHelp
        = @"Contains Spectrum and Peak/RMS visualizations plus unfinished analyzer panels.";
    auto* clearAction = [[NSAccessibilityCustomAction alloc]
        initWithName:@"Clear Peak/RMS holds and OVER"
              target:self
            selector:@selector(performPeakRmsClearAccessibilityAction)];
    self.accessibilityCustomActions = @[ clearAction ];
    [clearAction release];

    if ([self.layer isKindOfClass:[CAMetalLayer class]]) {
        metalDisplayLink =
            [[CAMetalDisplayLink alloc] initWithMetalLayer:static_cast<CAMetalLayer*>(self.layer)];
        metalDisplayLink.delegate = self;
        metalDisplayLink.preferredFrameLatency = 1.0F;
        metalDisplayLink.paused = YES;
        [metalDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
}

- (void)detachRenderBackend
{
    self.paused = YES;
    self.delegate = nil;

    if (metalDisplayLink != nil) {
        metalDisplayLink.paused = YES;
        metalDisplayLink.delegate = nil;
        [metalDisplayLink invalidate];
        [metalDisplayLink release];
        metalDisplayLink = nil;
    }

    [[NSNotificationCenter defaultCenter] removeObserver:self];
    self.accessibilityCustomActions = @[];
    observedWindow = nil;
    renderBackend = nullptr;
}

- (void)mouseDown:(NSEvent*)event
{
    if (renderBackend != nullptr) {
        const auto point = [self convertPoint:event.locationInWindow fromView:nil];

        if (renderBackend->tryClearPeakRmsAt(point))
            return;
    }

    [super mouseDown:event];
}

- (BOOL)performPeakRmsClearAccessibilityAction
{
    return renderBackend != nullptr && renderBackend->performPeakRmsClearAction();
}

- (void)setDisplayLinkPaused:(BOOL)shouldBePaused
{
    self.paused = YES;

    if (metalDisplayLink != nil)
        metalDisplayLink.paused = shouldBePaused;
}

- (void)setDisplayLinkMaximumFramesPerSecond:(NSInteger)maximumFramesPerSecond
{
    if (metalDisplayLink == nil)
        return;

    const auto maximum = static_cast<float>(std::max<NSInteger>(1, maximumFramesPerSecond));
    const auto minimum = std::min(60.0F, maximum);
    metalDisplayLink.preferredFrameRateRange = CAFrameRateRangeMake(minimum, maximum, maximum);
}

- (BOOL)hasDisplayLink
{
    return metalDisplayLink != nil;
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow
{
    auto* notificationCenter = [NSNotificationCenter defaultCenter];

    if (observedWindow != nil)
        [notificationCenter removeObserver:self name:nil object:observedWindow];

    [super viewWillMoveToWindow:newWindow];
    observedWindow = newWindow;

    if (observedWindow != nil) {
        const std::array<NSNotificationName, 6> notificationNames {
            NSWindowDidChangeOcclusionStateNotification, NSWindowDidChangeScreenNotification,
            NSWindowDidChangeBackingPropertiesNotification, NSWindowDidMiniaturizeNotification,
            NSWindowDidDeminiaturizeNotification, NSWindowDidResizeNotification
        };

        for (const auto notificationName : notificationNames)
            [notificationCenter addObserver:self
                                   selector:@selector(nativeVisibilityChanged:)
                                       name:notificationName
                                     object:observedWindow];
    }
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self notifyRenderBackend];
}

- (void)viewDidMoveToSuperview
{
    [super viewDidMoveToSuperview];
    [self notifyRenderBackend];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self notifyRenderBackend];
}

- (void)viewDidHide
{
    [super viewDidHide];
    [self notifyRenderBackend];
}

- (void)viewDidUnhide
{
    [super viewDidUnhide];
    [self notifyRenderBackend];
}

- (void)nativeVisibilityChanged:(NSNotification*)notification
{
    juce::ignoreUnused(notification);
    [self notifyRenderBackend];
}

- (void)notifyRenderBackend
{
    if (renderBackend != nullptr)
        renderBackend->nativeViewStateChanged();
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)displayLink
             needsUpdate:(CAMetalDisplayLinkUpdate*)update
{
    juce::ignoreUnused(displayLink);

    if (renderBackend != nullptr)
        renderBackend->displayLinkUpdate(update);
}

@end

namespace audio_insight {
struct MetalVisualization::Impl {
    Impl(MetalVisualization& componentToUse, VisualizationDataSource& dataSource)
        : component(componentToUse)
    {
        auto device = MTLCreateSystemDefaultDevice();
        nativeView = [[AIAudioInsightMetalView alloc] initWithFrame:NSZeroRect device:device];
        [device release];
        backend = std::make_unique<detail::MetalRenderBackend>(dataSource, nativeView);
        backend->attachNativeView();
        component.setView(nativeView);
        [nativeView release];
        backend->setJuceShowing(component.isShowing());
    }

    ~Impl()
    {
        backend->shutdown();
        component.setView(nullptr);
    }

    MetalVisualization& component;
    AIAudioInsightMetalView* nativeView = nil;
    std::unique_ptr<detail::MetalRenderBackend> backend;
};

MetalVisualization::MetalVisualization(VisualizationDataSource& dataSource)
    : impl(std::make_unique<Impl>(*this, dataSource))
{
    setOpaque(true);
}

MetalVisualization::~MetalVisualization() = default;

void MetalVisualization::setRenderingActive(bool shouldRender)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    impl->backend->setJuceShowing(isShowing());
    impl->backend->setRequestedActive(shouldRender);
}

bool MetalVisualization::isRenderingRequested() const noexcept
{
    return impl->backend->isRenderingRequested();
}

bool MetalVisualization::isEffectivelyRendering() const noexcept
{
    return impl->backend->isEffectivelyRendering();
}

bool MetalVisualization::isMetalAvailable() const noexcept
{
    return impl->backend->isMetalAvailable();
}

juce::String MetalVisualization::getInitializationError() const
{
    return impl->backend->getInitializationError();
}

void MetalVisualization::setEffectiveActivityCallback(EffectiveActivityCallback callback)
{
    impl->backend->setEffectiveActivityCallback(std::move(callback));
}

void MetalVisualization::setSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    impl->backend->setSpectrumSettings(settings);
}

SpectrumRenderSettings MetalVisualization::getSpectrumSettings() const noexcept
{
    return impl->backend->getSpectrumSettings();
}

void MetalVisualization::setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
{
    impl->backend->setDashboardLayoutSplits(splits);
}

DashboardLayoutSplits MetalVisualization::getDashboardLayoutSplits() const noexcept
{
    return impl->backend->getDashboardLayoutSplits();
}

MetalRenderTelemetry MetalVisualization::getRenderTelemetry() const noexcept
{
    return impl->backend->getTelemetry();
}

void MetalVisualization::resetRenderTelemetry()
{
    impl->backend->resetTelemetry();
}

void MetalVisualization::visibilityChanged()
{
    if (impl != nullptr)
        impl->backend->setJuceShowing(isShowing());
}

void MetalVisualization::parentHierarchyChanged()
{
    if (impl != nullptr)
        impl->backend->setJuceShowing(isShowing());
}
} // namespace audio_insight
