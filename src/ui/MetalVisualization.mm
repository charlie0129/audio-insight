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
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace audio_insight::detail {
class MetalRenderBackend;
}

@class AIAudioInsightMetalView;

@interface AIAudioInsightDashboardSplitterAccessibilityElement : NSAccessibilityElement {
@private
    AIAudioInsightMetalView* ownerView;
    NSUInteger splitterIndex;
}

- (instancetype)initWithOwnerView:(AIAudioInsightMetalView*)view splitterIndex:(NSUInteger)index;
- (void)detachOwnerView;

@end

@interface AIAudioInsightMetalView : MTKView <CAMetalDisplayLinkDelegate> {
@private
    audio_insight::detail::MetalRenderBackend* renderBackend;
    NSWindow* observedWindow;
    CAMetalDisplayLink* metalDisplayLink;
    NSArray* dashboardSplitterAccessibilityElements;
}

- (void)attachRenderBackend:(audio_insight::detail::MetalRenderBackend*)backend;
- (void)detachRenderBackend;
- (void)setDisplayLinkPaused:(BOOL)shouldBePaused;
- (void)setDisplayLinkMaximumFramesPerSecond:(NSInteger)maximumFramesPerSecond;
- (BOOL)hasDisplayLink;
- (BOOL)performSpectrumClearAccessibilityAction;
- (BOOL)performPeakRmsClearAccessibilityAction;
- (BOOL)performLoudnessResetAccessibilityAction;
- (void)dashboardLayoutEditingStateChanged;
- (void)dashboardLayoutGeometryChanged;
- (void)dashboardSplitterFocusChangedFromIndex:(NSUInteger)previousIndex
                                       toIndex:(NSUInteger)nextIndex;
- (void)dashboardSplitterValueChangedAtIndex:(NSUInteger)index;
- (BOOL)isDashboardLayoutEditing;
- (BOOL)isDashboardSplitterFocusedAtIndex:(NSUInteger)index;
- (BOOL)focusDashboardSplitterAtIndex:(NSUInteger)index;
- (BOOL)incrementDashboardSplitterAtIndex:(NSUInteger)index;
- (BOOL)decrementDashboardSplitterAtIndex:(NSUInteger)index;
- (NSRect)dashboardSplitterAccessibilityFrameAtIndex:(NSUInteger)index;
- (NSString*)dashboardSplitterAccessibilityLabelAtIndex:(NSUInteger)index;
- (NSString*)dashboardSplitterAccessibilityValueDescriptionAtIndex:(NSUInteger)index;
- (NSNumber*)dashboardSplitterAccessibilityValueAtIndex:(NSUInteger)index;
- (NSNumber*)dashboardSplitterAccessibilityMinimumAtIndex:(NSUInteger)index;
- (NSNumber*)dashboardSplitterAccessibilityMaximumAtIndex:(NSUInteger)index;
- (NSAccessibilityOrientation)dashboardSplitterAccessibilityOrientationAtIndex:(NSUInteger)index;
- (NSRect)dashboardSplitterLocalBoundsAtIndex:(NSUInteger)index;
- (AIAudioInsightDashboardSplitterAccessibilityElement*)
    dashboardSplitterAccessibilityElementAtIndex:(NSUInteger)index;

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

float mapLoudnessLufsToUnit(const float lufs) noexcept
{
    const auto value = std::isfinite(lufs) ? lufs : loudnessMinimumLufs;
    return (std::clamp(value, loudnessMinimumLufs, loudnessMaximumLufs) - loudnessMinimumLufs)
        / (loudnessMaximumLufs - loudnessMinimumLufs);
}

LoudnessReadout classifyLoudnessReadout(const double lufs, const bool valid) noexcept
{
    LoudnessReadout result;
    if (!valid || std::isnan(lufs) || (std::isinf(lufs) && !std::signbit(lufs)))
        return result;

    if (std::isinf(lufs) && std::signbit(lufs)) {
        result.kind = LoudnessReadout::Kind::minusInfinity;
        return result;
    }

    if (!std::isfinite(lufs))
        return result;

    const auto bounded
        = std::clamp(lufs, static_cast<double>(minimumCachedLoudnessReadoutTenths) * 0.1,
            static_cast<double>(maximumCachedLoudnessReadoutTenths) * 0.1);
    result.kind = LoudnessReadout::Kind::lufsTenths;
    result.lufsTenths = std::clamp(static_cast<int>(std::lround(bounded * 10.0)),
        minimumCachedLoudnessReadoutTenths, maximumCachedLoudnessReadoutTenths);
    return result;
}

juce::String formatLoudnessAccessibilityReading(const double lufs, const bool valid)
{
    const auto readout = classifyLoudnessReadout(lufs, valid);
    switch (readout.kind) {
    case LoudnessReadout::Kind::emDash:
        return "not ready";
    case LoudnessReadout::Kind::minusInfinity:
        return "minus infinity";
    case LoudnessReadout::Kind::lufsTenths: {
        const auto absoluteTenths = std::abs(readout.lufsTenths);
        auto result = readout.lufsTenths < 0 ? juce::String("-")
            : readout.lufsTenths > 0         ? juce::String("+")
                                             : juce::String();
        result += juce::String(absoluteTenths / 10) + "." + juce::String(absoluteTenths % 10)
            + " LUFS";
        return result;
    }
    }

    return "not ready";
}

LoudnessPanelLayout calculateLoudnessPanelLayout(const float panelWidth, const float panelHeight,
    const float requestedHeaderHeight, const float requestedTextHeight,
    const float requestedMaximumReadoutWidth) noexcept
{
    LoudnessPanelLayout result;
    if (!std::isfinite(panelWidth) || !std::isfinite(panelHeight) || panelWidth <= 0.0F
        || panelHeight <= 0.0F) {
        return result;
    }

    const auto headerHeight = std::clamp(
        std::isfinite(requestedHeaderHeight) ? requestedHeaderHeight : 0.0F, 0.0F, panelHeight);
    const auto textHeight
        = std::clamp(std::isfinite(requestedTextHeight) ? requestedTextHeight : 10.0F, 6.0F, 24.0F);
    const auto maximumReadoutWidth = std::max(
        0.0F, std::isfinite(requestedMaximumReadoutWidth) ? requestedMaximumReadoutWidth : 0.0F);

    if (panelWidth >= 68.0F && headerHeight >= 14.0F) {
        constexpr auto visualWidth = 45.0F;
        const auto visualBottom = panelHeight - headerHeight + 3.0F;
        result.resetVisualBounds = { std::max(4.0F, panelWidth - visualWidth - 5.0F), visualBottom,
            panelWidth - 5.0F, panelHeight - 3.0F };
        const auto hitHeight = std::min(panelHeight, std::max(24.0F, headerHeight));
        result.resetHitBounds = { std::max(0.0F, result.resetVisualBounds.left - 7.0F),
            panelHeight - hitHeight, panelWidth, panelHeight };
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

    constexpr auto minimumTrackHeight = 24.0F;
    constexpr auto rowGap = 2.0F;
    constexpr auto trackGap = 4.0F;
    const auto textFitsHorizontally = contentWidth >= maximumReadoutWidth + 10.0F;
    result.showMomentaryText
        = textFitsHorizontally && contentHeight >= minimumTrackHeight + textHeight + trackGap;
    result.showSecondaryText = result.showMomentaryText
        && contentHeight
            >= minimumTrackHeight + (3.0F * textHeight) + (2.0F * rowGap) + (2.0F * trackGap);

    auto trackBottom = contentBottom;
    auto trackTop = contentTop;
    if (result.showMomentaryText) {
        result.momentaryTextBounds
            = { contentLeft, contentTop - textHeight, contentRight, contentTop };
        trackTop = result.momentaryTextBounds.bottom - trackGap;
    }

    if (result.showSecondaryText) {
        result.integratedTextBounds
            = { contentLeft, contentBottom, contentRight, contentBottom + textHeight };
        result.shortTermTextBounds = { contentLeft, result.integratedTextBounds.top + rowGap,
            contentRight, result.integratedTextBounds.top + rowGap + textHeight };
        trackBottom = result.shortTermTextBounds.top + trackGap;
    }

    if (trackTop <= trackBottom) {
        result.showMomentaryText = false;
        result.showSecondaryText = false;
        result.momentaryTextBounds = { };
        result.shortTermTextBounds = { };
        result.integratedTextBounds = { };
        trackBottom = contentBottom;
        trackTop = contentTop;
    }

    const auto desiredTrackWidth = std::clamp(contentWidth * 0.14F, 8.0F, 16.0F);
    const auto trackWidth = std::min(contentWidth, desiredTrackWidth);
    const auto trackLeft = contentLeft + ((contentWidth - trackWidth) * 0.5F);
    result.trackBounds = { trackLeft, trackBottom, trackLeft + trackWidth, trackTop };
    return result;
}

StereoFieldPanelLayout calculateStereoFieldPanelLayout(const float panelWidth,
    const float panelHeight, const float requestedHeaderHeight,
    const float requestedTextHeight) noexcept
{
    StereoFieldPanelLayout result;
    if (!std::isfinite(panelWidth) || !std::isfinite(panelHeight) || panelWidth <= 0.0F
        || panelHeight <= 0.0F) {
        return result;
    }

    const auto headerHeight = std::clamp(
        std::isfinite(requestedHeaderHeight) ? requestedHeaderHeight : 0.0F, 0.0F, panelHeight);
    const auto textHeight
        = std::clamp(std::isfinite(requestedTextHeight) ? requestedTextHeight : 10.0F, 6.0F, 24.0F);
    constexpr auto contentInset = 7.0F;
    constexpr auto correlationTrackHeight = 5.0F;
    constexpr auto correlationTextGap = 3.0F;
    constexpr auto scopeGap = 6.0F;
    const auto contentLeft = std::min(contentInset, panelWidth * 0.5F);
    const auto contentRight = std::max(contentLeft, panelWidth - contentInset);
    const auto contentBottom = std::min(contentInset, panelHeight);
    const auto contentTop = std::max(contentBottom, panelHeight - headerHeight - contentInset);
    const auto contentWidth = contentRight - contentLeft;
    const auto contentHeight = contentTop - contentBottom;
    if (contentWidth <= 0.0F || contentHeight < correlationTrackHeight)
        return result;

    result.correlationTrackBounds
        = { contentLeft, contentBottom, contentRight, contentBottom + correlationTrackHeight };
    result.showCorrelationReadout = contentWidth >= 44.0F
        && contentHeight
            >= correlationTrackHeight + correlationTextGap + textHeight + scopeGap + 20.0F;
    result.correlationReadoutBottom = result.correlationTrackBounds.top + correlationTextGap;
    const auto scopeAreaBottom = result.showCorrelationReadout
        ? result.correlationReadoutBottom + textHeight + scopeGap
        : result.correlationTrackBounds.top + scopeGap;
    const auto scopeAreaHeight = std::max(0.0F, contentTop - scopeAreaBottom);
    const auto scopeSide = std::min(contentWidth, scopeAreaHeight);
    if (scopeSide <= 0.0F)
        return result;

    const auto scopeLeft = contentLeft + ((contentWidth - scopeSide) * 0.5F);
    const auto scopeBottom = scopeAreaBottom + ((scopeAreaHeight - scopeSide) * 0.5F);
    result.scopeBounds = { scopeLeft, scopeBottom, scopeLeft + scopeSide, scopeBottom + scopeSide };
    return result;
}

float mapStereoFieldCoordinate(
    const float coordinate, const float minimumPosition, const float maximumPosition) noexcept
{
    if (!std::isfinite(minimumPosition) || !std::isfinite(maximumPosition)
        || maximumPosition <= minimumPosition) {
        return 0.0F;
    }

    const auto value = std::isfinite(coordinate) ? coordinate : 0.0F;
    return minimumPosition + ((value + 1.0F) * 0.5F * (maximumPosition - minimumPosition));
}

float mapStereoCorrelationToUnit(const float correlation) noexcept
{
    const auto value = std::isfinite(correlation) ? correlation : 0.0F;
    return (std::clamp(value, -1.0F, 1.0F) + 1.0F) * 0.5F;
}

StereoCorrelationReadout classifyStereoCorrelationReadout(
    const float correlation, const bool valid) noexcept
{
    StereoCorrelationReadout result;
    if (!valid || !std::isfinite(correlation))
        return result;

    const auto bounded = std::clamp(correlation, -1.0F, 1.0F);
    result.hundredths = std::clamp(static_cast<int>(std::lround(bounded * 100.0F)), -100, 100);
    result.available = true;
    if (bounded > stereoCorrelationNeutralThreshold)
        result.colourRange = StereoCorrelationColourRange::cyan;
    else if (bounded < -stereoCorrelationNeutralThreshold)
        result.colourRange = StereoCorrelationColourRange::amber;
    return result;
}

float stereoFieldPointAgeOpacity(
    const float normalizedAge, const double elapsedSinceSnapshotSeconds) noexcept
{
    if (!std::isfinite(normalizedAge))
        return 0.0F;

    const auto elapsed = std::isfinite(elapsedSinceSnapshotSeconds)
        ? std::max(0.0, elapsedSinceSnapshotSeconds)
        : 0.0;
    const auto age = std::clamp(normalizedAge, 0.0F, 1.0F)
        + static_cast<float>(elapsed / static_cast<double>(stereoFieldHistorySeconds));
    return std::clamp(1.0F - age, 0.0F, 1.0F);
}

SpectrumClearLayout calculateSpectrumClearLayout(
    const float panelWidth, const float panelHeight, const float requestedHeaderHeight) noexcept
{
    SpectrumClearLayout result;
    if (!std::isfinite(panelWidth) || !std::isfinite(panelHeight) || panelWidth < 96.0F
        || panelHeight <= 0.0F) {
        return result;
    }

    const auto headerHeight = std::clamp(
        std::isfinite(requestedHeaderHeight) ? requestedHeaderHeight : 0.0F, 0.0F, panelHeight);
    if (headerHeight < 14.0F)
        return result;

    constexpr auto visualWidth = 45.0F;
    const auto visualBottom = panelHeight - headerHeight + 3.0F;
    result.visualBounds
        = { panelWidth - visualWidth - 5.0F, visualBottom, panelWidth - 5.0F, panelHeight - 3.0F };
    const auto hitHeight = std::min(panelHeight, std::max(24.0F, headerHeight));
    result.hitBounds = { std::max(0.0F, result.visualBounds.left - 7.0F), panelHeight - hitHeight,
        panelWidth, panelHeight };
    return result;
}

float spectrumSlopeCompensationDecibels(
    const float frequencyHz, const float slopeDecibelsPerOctave) noexcept
{
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0F
        || !std::isfinite(slopeDecibelsPerOctave)) {
        return 0.0F;
    }

    return slopeDecibelsPerOctave * std::log2(frequencyHz / 1'000.0F);
}

float sanitiseSpectrumAnalysisDecibels(const float decibels) noexcept
{
    return std::isfinite(decibels) ? std::max(decibels, minimumSpectrumDecibels)
                                   : minimumSpectrumDecibels;
}

float srgbComponentToLinear(const float component) noexcept
{
    const auto bounded = std::clamp(std::isfinite(component) ? component : 0.0F, 0.0F, 1.0F);
    return bounded <= 0.04045F ? bounded / 12.92F : std::pow((bounded + 0.055F) / 1.055F, 2.4F);
}

namespace {
struct PaletteStop final {
    float coordinate;
    SpectrogramPaletteColour colour;
};

template <std::size_t Size>
SpectrogramPaletteColour interpolatePalette(
    const std::array<PaletteStop, Size>& stops, const float coordinate) noexcept
{
    const auto bounded = std::clamp(std::isfinite(coordinate) ? coordinate : 0.0F, 0.0F, 1.0F);
    for (std::size_t index = 1; index < stops.size(); ++index) {
        if (bounded > stops[index].coordinate)
            continue;

        const auto& lower = stops[index - 1];
        const auto& upper = stops[index];
        const auto span = upper.coordinate - lower.coordinate;
        const auto amount = span > 0.0F ? (bounded - lower.coordinate) / span : 0.0F;
        return { lower.colour.red + (amount * (upper.colour.red - lower.colour.red)),
            lower.colour.green + (amount * (upper.colour.green - lower.colour.green)),
            lower.colour.blue + (amount * (upper.colour.blue - lower.colour.blue)) };
    }

    return stops.back().colour;
}
} // namespace

void SpectrogramHistoryRing::configure(const std::uint32_t columnCount) noexcept
{
    const auto bounded = std::min<std::uint32_t>(
        columnCount, static_cast<std::uint32_t>(maximumSpectrogramHistoryColumnCount));
    if (bounded == columnCount_)
        return;

    columnCount_ = bounded;
    clear();
}

void SpectrogramHistoryRing::clear() noexcept
{
    validity_.fill(0);
    nextWriteColumn_ = 0;
    timelineSpan_ = 0;
    lastTimelineSlot_ = 0;
    lastSequence_ = 0;
    hasTimeline_ = false;
}

void SpectrogramHistoryRing::appendGap() noexcept
{
    if (columnCount_ == 0)
        return;

    validity_[nextWriteColumn_] = 0;
    nextWriteColumn_ = (nextWriteColumn_ + 1) % columnCount_;
    timelineSpan_ = std::min(columnCount_, timelineSpan_ + 1);
}

SpectrogramRingAdvance SpectrogramHistoryRing::append(
    const std::uint64_t timelineSlot, const std::uint64_t sequence) noexcept
{
    SpectrogramRingAdvance result;
    if (columnCount_ == 0 || timelineSlot == 0 || sequence == 0)
        return result;

    if (hasTimeline_ && (timelineSlot < lastTimelineSlot_ || sequence <= lastSequence_))
        return result;

    if (hasTimeline_ && timelineSlot == lastTimelineSlot_) {
        result.writeColumn = (nextWriteColumn_ + columnCount_ - 1) % columnCount_;
        result.accepted = true;
        lastSequence_ = sequence;
        return result;
    }

    auto gapCount = std::uint64_t { 0 };
    if (hasTimeline_) {
        const auto timestampGap = timelineSlot - lastTimelineSlot_ - 1;
        const auto sequenceGap = sequence - lastSequence_ - 1;
        gapCount = std::max(timestampGap, sequenceGap);

        if (gapCount >= columnCount_) {
            validity_.fill(0);
            nextWriteColumn_ = static_cast<std::uint32_t>(
                (nextWriteColumn_ + (gapCount % columnCount_)) % columnCount_);
            timelineSpan_ = columnCount_ - 1;
            result.discardedPreviousSpan = true;
        } else {
            for (auto missing = std::uint64_t { 0 }; missing < gapCount; ++missing)
                appendGap();
        }
    }

    result.writeColumn = nextWriteColumn_;
    result.gapColumnCount = gapCount;
    result.accepted = true;
    validity_[nextWriteColumn_] = 1;
    nextWriteColumn_ = (nextWriteColumn_ + 1) % columnCount_;
    timelineSpan_ = std::min(columnCount_, timelineSpan_ + 1);
    lastTimelineSlot_ = timelineSlot;
    lastSequence_ = sequence;
    hasTimeline_ = true;
    return result;
}

std::optional<std::uint32_t> SpectrogramHistoryRing::physicalColumnForScreenColumn(
    const std::uint32_t screenColumn, const SpectrogramRenderHistoryMode mode) const noexcept
{
    if (columnCount_ == 0 || screenColumn >= columnCount_)
        return std::nullopt;

    if (mode == SpectrogramRenderHistoryMode::overwrite)
        return screenColumn;

    const auto emptyColumns = columnCount_ - timelineSpan_;
    if (screenColumn < emptyColumns)
        return std::nullopt;

    const auto oldest = (nextWriteColumn_ + columnCount_ - timelineSpan_) % columnCount_;
    return (oldest + (screenColumn - emptyColumns)) % columnCount_;
}

bool SpectrogramHistoryRing::isColumnValid(const std::uint32_t physicalColumn) const noexcept
{
    return physicalColumn < columnCount_ && validity_[physicalColumn] != 0;
}

std::uint32_t calculateSpectrogramHistoryColumnCount(
    const int historyDurationSeconds, const int requestedSliceRateHz) noexcept
{
    constexpr std::array supportedDurations { 2, 5, 10, 20, 30, 60 };
    constexpr std::array supportedRates { 15, 30, 60, 120 };
    if (std::find(supportedDurations.begin(), supportedDurations.end(), historyDurationSeconds)
            == supportedDurations.end()
        || std::find(supportedRates.begin(), supportedRates.end(), requestedSliceRateHz)
            == supportedRates.end()) {
        return 0;
    }

    const auto requested = static_cast<std::uint64_t>(historyDurationSeconds)
        * static_cast<std::uint64_t>(requestedSliceRateHz);
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(requested, maximumSpectrogramHistoryColumnCount));
}

std::uint64_t calculateSpectrogramTimelineSlot(const std::uint64_t capturedFrameEnd,
    const double sampleRate, const std::uint32_t requestedSliceRateHz) noexcept
{
    if (capturedFrameEnd == 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || requestedSliceRateHz == 0) {
        return 0;
    }

    const auto slot = (static_cast<long double>(capturedFrameEnd) * requestedSliceRateHz)
        / static_cast<long double>(sampleRate);
    if (!std::isfinite(slot) || slot <= 0.0L)
        return 0;

    const auto bounded = std::min<long double>(
        slot, static_cast<long double>(std::numeric_limits<std::uint64_t>::max()));
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(bounded)));
}

SpectrogramHistoryTransition spectrogramHistoryTransition(
    const std::optional<SpectrogramHistorySignature>& previous,
    const SpectrogramHistorySignature& next) noexcept
{
    const auto nextIsValid = next.captureGeneration != 0 && next.fftGeneration != 0
        && next.mappingGeneration != 0 && next.resetEpoch != 0 && next.fftSize != 0
        && next.rowCount != 0 && next.rowCount <= maximumSpectrogramRowCount
        && next.columnCount != 0 && next.columnCount <= maximumSpectrogramHistoryColumnCount
        && next.requestedSliceRateHz != 0 && std::isfinite(next.sampleRate)
        && next.sampleRate > 0.0;
    if (!nextIsValid)
        return { };

    if (!previous.has_value())
        return { true, true };

    const auto dimensionsChanged
        = previous->rowCount != next.rowCount || previous->columnCount != next.columnCount;
    const auto sampleRateScale
        = std::max({ 1.0, std::abs(previous->sampleRate), std::abs(next.sampleRate) });
    const auto sampleRateChanged = std::abs(previous->sampleRate - next.sampleRate)
        > std::numeric_limits<double>::epsilon() * sampleRateScale * 4.0;
    const auto historyIsIncompatible = previous->captureGeneration != next.captureGeneration
        || previous->fftGeneration != next.fftGeneration
        || previous->mappingGeneration != next.mappingGeneration
        || previous->resetEpoch != next.resetEpoch || previous->fftSize != next.fftSize
        || previous->requestedSliceRateHz != next.requestedSliceRateHz || sampleRateChanged
        || dimensionsChanged;
    return { historyIsIncompatible, dimensionsChanged };
}

float spectrogramPaletteCoordinate(const float decibels, const float floorDecibels,
    const float ceilingDecibels, const float response) noexcept
{
    if (!std::isfinite(decibels) || !std::isfinite(floorDecibels) || !std::isfinite(ceilingDecibels)
        || ceilingDecibels <= floorDecibels) {
        return 0.0F;
    }

    const auto value
        = std::clamp((decibels - floorDecibels) / (ceilingDecibels - floorDecibels), 0.0F, 1.0F);
    const auto boundedResponse = std::clamp(std::isfinite(response) ? response : 0.0F, -2.0F, 2.0F);
    return std::pow(value, std::exp2(boundedResponse));
}

SpectrogramPaletteColour spectrogramPaletteColour(
    const SpectrogramRenderPalette palette, const float coordinate) noexcept
{
    constexpr std::array blueFire {
        PaletteStop { 0.0F, { 0.0F, 0.0F, 0.0F } },
        PaletteStop { 0.10F, { 0.008F, 0.016F, 0.070F } },
        PaletteStop { 0.28F, { 0.016F, 0.080F, 0.310F } },
        PaletteStop { 0.48F, { 0.020F, 0.330F, 0.760F } },
        PaletteStop { 0.58F, { 0.650F, 0.360F, 0.760F } },
        PaletteStop { 0.68F, { 0.980F, 0.500F, 0.080F } },
        PaletteStop { 0.84F, { 1.000F, 0.690F, 0.170F } },
        PaletteStop { 1.0F, { 1.000F, 0.985F, 0.920F } },
    };
    constexpr std::array inferno {
        PaletteStop { 0.0F, { 0.0F, 0.0F, 0.0F } },
        PaletteStop { 0.13F, { 0.106F, 0.047F, 0.255F } },
        PaletteStop { 0.31F, { 0.365F, 0.071F, 0.431F } },
        PaletteStop { 0.49F, { 0.665F, 0.139F, 0.365F } },
        PaletteStop { 0.67F, { 0.902F, 0.318F, 0.169F } },
        PaletteStop { 0.84F, { 0.988F, 0.645F, 0.039F } },
        PaletteStop { 1.0F, { 0.988F, 1.000F, 0.644F } },
    };
    constexpr std::array viridis {
        PaletteStop { 0.0F, { 0.0F, 0.0F, 0.0F } },
        PaletteStop { 0.13F, { 0.267F, 0.005F, 0.329F } },
        PaletteStop { 0.31F, { 0.231F, 0.322F, 0.545F } },
        PaletteStop { 0.49F, { 0.129F, 0.569F, 0.549F } },
        PaletteStop { 0.67F, { 0.369F, 0.789F, 0.383F } },
        PaletteStop { 0.84F, { 0.678F, 0.864F, 0.190F } },
        PaletteStop { 1.0F, { 0.993F, 0.906F, 0.144F } },
    };
    constexpr std::array grayscale {
        PaletteStop { 0.0F, { 0.0F, 0.0F, 0.0F } },
        PaletteStop { 1.0F, { 1.0F, 1.0F, 1.0F } },
    };

    switch (palette) {
    case SpectrogramRenderPalette::inferno:
        return interpolatePalette(inferno, coordinate);
    case SpectrogramRenderPalette::viridis:
        return interpolatePalette(viridis, coordinate);
    case SpectrogramRenderPalette::grayscale:
        return interpolatePalette(grayscale, coordinate);
    case SpectrogramRenderPalette::blueFire:
        return interpolatePalette(blueFire, coordinate);
    }

    return { };
}

float spectrogramPerceivedLuminance(const SpectrogramPaletteColour& colour) noexcept
{
    return (0.2126F * srgbComponentToLinear(colour.red))
        + (0.7152F * srgbComponentToLinear(colour.green))
        + (0.0722F * srgbComponentToLinear(colour.blue));
}

float spectrogramFrequencyCoordinate(const float topOriginCoordinate) noexcept
{
    return 1.0F
        - std::clamp(std::isfinite(topOriginCoordinate) ? topOriginCoordinate : 1.0F, 0.0F, 1.0F);
}

float spectrogramLogicalPixelWidth(const float backingScale) noexcept
{
    return 1.0F / (std::isfinite(backingScale) && backingScale > 0.0F ? backingScale : 1.0F);
}

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t renderBufferCount = 3;
constexpr std::size_t maximumVertexCount = MetalVisualizationGeometryLimits::vertexCapacity;
constexpr std::size_t metalTextureBufferAlignment = 256;
constexpr std::size_t spectrogramHalfRowBytes = maximumSpectrogramRowCount * sizeof(_Float16);
constexpr std::size_t spectrogramStagingRowStride
    = ((spectrogramHalfRowBytes + metalTextureBufferAlignment - 1) / metalTextureBufferAlignment)
    * metalTextureBufferAlignment;
constexpr std::size_t spectrogramStagingBufferBytes
    = maximumSpectrogramColumnsDrainedPerFrame * spectrogramStagingRowStride;
constexpr std::size_t spectrogramUniformBufferBytes = 256;
constexpr std::size_t stereoPointUniformBufferBytes = 256;
static_assert(sizeof(_Float16) == 2);
static_assert(spectrogramStagingRowStride % metalTextureBufferAlignment == 0);
constexpr std::size_t printableAsciiFirst = 32;
constexpr std::size_t printableAsciiLast = 126;
constexpr std::size_t printableAsciiCount = printableAsciiLast - printableAsciiFirst + 1;
constexpr std::size_t infinityGlyphAtlasIndex = printableAsciiCount;
constexpr std::size_t emDashGlyphAtlasIndex = infinityGlyphAtlasIndex + 1;
constexpr std::size_t glyphAtlasGlyphCount = printableAsciiCount + 2;
constexpr std::size_t glyphAtlasColumns = 16;
constexpr std::size_t glyphAtlasRows
    = (glyphAtlasGlyphCount + glyphAtlasColumns - 1) / glyphAtlasColumns;
constexpr std::size_t maximumCachedTextGlyphs = 24;
constexpr std::size_t cachedFixedTextRunCount = 14;

constexpr std::array<std::string_view, cachedFixedTextRunCount> cachedFixedTextStrings { "Spectrum",
    "Peak / RMS", "Spectrogram", "Stereo / Correlation", "Loudness", "CLEAR", "L", "R", "M", "OVER",
    "MONO", "RESET", "S", "I" };

constexpr std::array<std::string_view, frequencyAxisTickCandidateCount>
    cachedFrequencyAxisTextStrings { "20 Hz", "50 Hz", "100 Hz", "200 Hz", "500 Hz", "1 kHz",
        "2 kHz", "5 kHz", "10 kHz", "20 kHz" };

constexpr std::array<std::string_view, peakRmsMajorDecibelTicks.size()>
    cachedPeakRmsTickTextStrings { "-60", "-48", "-36", "-24", "-12", "-6", "0", "+3" };

constexpr std::array<std::size_t, dashboardPanelCount> panelTitleTextRunIndices { 0, 1, 2, 3, 4 };
constexpr std::size_t clearTextRunIndex = 5;
constexpr std::size_t leftChannelTextRunIndex = 6;
constexpr std::size_t rightChannelTextRunIndex = 7;
constexpr std::size_t monoChannelTextRunIndex = 8;
constexpr std::size_t overTextRunIndex = 9;
constexpr std::size_t monoStateTextRunIndex = 10;
constexpr std::size_t loudnessResetTextRunIndex = 11;
constexpr std::size_t shortTermTextRunIndex = 12;
constexpr std::size_t integratedTextRunIndex = 13;
constexpr int minimumPeakRmsReadoutTenths = -1'199;
constexpr int maximumPeakRmsReadoutTenths = maximumFiniteFloatPeakRmsReadoutTenths;
constexpr std::size_t cachedPeakRmsReadoutCount
    = static_cast<std::size_t>(maximumPeakRmsReadoutTenths - minimumPeakRmsReadoutTenths + 1);
constexpr std::size_t cachedLoudnessReadoutCount = static_cast<std::size_t>(
    maximumCachedLoudnessReadoutTenths - minimumCachedLoudnessReadoutTenths + 1);

// The fixed capacity covers five filled/bordered/header-divided tiles, both
// numeric frequency axes, every possible decibel tick, all maximum-size FFT
// traces and fill, both live meter channels, and every cached text run. Keep this proof beside
// the allocation so a future builder cannot silently truncate a frame.
constexpr std::size_t fixedTextGlyphCount = [] {
    std::size_t glyphCount = 0;

    for (const auto titleIndex : panelTitleTextRunIndices)
        glyphCount += cachedFixedTextStrings[titleIndex].size();

    return glyphCount + cachedFixedTextStrings[monoStateTextRunIndex].size();
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
constexpr std::size_t loudnessTextGlyphCount
    = cachedFixedTextStrings[loudnessResetTextRunIndex].size()
    + cachedFixedTextStrings[monoChannelTextRunIndex].size()
    + cachedFixedTextStrings[shortTermTextRunIndex].size()
    + cachedFixedTextStrings[integratedTextRunIndex].size() + (3 * maximumLoudnessReadoutGlyphs);
static_assert(fixedTextGlyphCount == MetalVisualizationGeometryLimits::maximumFixedTextGlyphs);
static_assert(peakRmsTextGlyphCount == MetalVisualizationGeometryLimits::maximumPeakRmsTextGlyphs);
static_assert(
    loudnessTextGlyphCount == MetalVisualizationGeometryLimits::maximumLoudnessTextGlyphs);
static_assert(cachedFixedTextStrings[clearTextRunIndex].size()
    == MetalVisualizationGeometryLimits::maximumSpectrumControlTextGlyphs);
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
constexpr float minimumLoudnessReferenceLufs = -36.0F;
constexpr float maximumLoudnessReferenceLufs = -9.0F;
constexpr float loudnessReferenceStepLufs = 0.5F;
constexpr std::size_t noDashboardSplitterIndex = dashboardSplitterCount;

DashboardSplitter dashboardSplitterAtIndex(const std::size_t index) noexcept
{
    return dashboardSplitterTabOrder[std::min(index, dashboardSplitterTabOrder.size() - 1)];
}

std::size_t dashboardSplitterIndex(const DashboardSplitter splitter) noexcept
{
    const auto found
        = std::find(dashboardSplitterTabOrder.begin(), dashboardSplitterTabOrder.end(), splitter);
    return found != dashboardSplitterTabOrder.end()
        ? static_cast<std::size_t>(std::distance(dashboardSplitterTabOrder.begin(), found))
        : noDashboardSplitterIndex;
}

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

const std::array<CachedMonospacedTextRun, cachedLoudnessReadoutCount>&
cachedLoudnessReadoutTextRuns() noexcept
{
    // Loudness values preserve finite readings beyond the visual -60..0 LUFS
    // scale. This bounded table avoids callback-time formatting while covering
    // analyzer values through its defensive +800 LUFS integration cap.
    static const auto runs = [] {
        std::array<CachedMonospacedTextRun, cachedLoudnessReadoutCount> result { };

        for (std::size_t index = 0; index < result.size(); ++index) {
            const auto tenths = minimumCachedLoudnessReadoutTenths + static_cast<int>(index);
            const auto absoluteTenths = std::abs(tenths);
            std::array<char, maximumLoudnessReadoutGlyphs> text { };
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

const CachedMonospacedTextRun& cachedEmDashTextRun() noexcept
{
    static const auto run = [] {
        CachedMonospacedTextRun result;
        result.glyphCount = 1;
        result.atlasIndices[0] = static_cast<std::uint8_t>(emDashGlyphAtlasIndex);
        return result;
    }();

    return run;
}

const std::array<CachedMonospacedTextRun, 201>& cachedStereoCorrelationTextRuns() noexcept
{
    static const auto runs = [] {
        std::array<CachedMonospacedTextRun, 201> result { };
        for (auto hundredths = -100; hundredths <= 100; ++hundredths) {
            std::array<char, maximumStereoCorrelationReadoutGlyphs> text { };
            auto* cursor = text.data();
            if (hundredths < 0)
                *cursor++ = '-';
            else if (hundredths > 0)
                *cursor++ = '+';

            const auto absolute = std::abs(hundredths);
            *cursor++ = static_cast<char>('0' + (absolute / 100));
            *cursor++ = '.';
            *cursor++ = static_cast<char>('0' + ((absolute / 10) % 10));
            *cursor++ = static_cast<char>('0' + (absolute % 10));

            auto& run = result[static_cast<std::size_t>(hundredths + 100)];
            run.glyphCount = static_cast<std::uint8_t>(cursor - text.data());
            for (std::size_t glyph = 0; glyph < run.glyphCount; ++glyph)
                run.atlasIndices[glyph] = atlasIndexForAscii(text[glyph]);
        }
        return result;
    }();

    return runs;
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
    std::atomic<std::uint64_t> spectrogramColumnsRead { 0 };
    std::atomic<std::uint64_t> spectrogramColumnsUploaded { 0 };
    std::atomic<std::uint64_t> spectrogramColumnsRejected { 0 };
    std::atomic<std::uint64_t> spectrogramGapColumns { 0 };
    std::atomic<std::uint64_t> spectrogramHistoryClears { 0 };
    std::atomic<std::uint64_t> spectrogramTextureReallocations { 0 };
    std::atomic<std::uint64_t> spectrogramTextureAllocationFailures { 0 };
    std::atomic<std::uint64_t> spectrogramUploadBackpressureDrops { 0 };
    std::atomic<std::uint64_t> spectrogramUploadCommands { 0 };
    std::atomic<std::uint64_t> spectrogramUploadBytes { 0 };
    std::atomic<std::uint64_t> spectrogramLastColumnSequence { 0 };
    std::atomic<std::uint64_t> lastStereoSequence { 0 };
    std::atomic<std::uint64_t> stereoPointInstancesPrepared { 0 };
    std::atomic<std::uint64_t> stereoPointDrawCalls { 0 };
    std::atomic<std::uint64_t> lastLoudnessSequence { 0 };
    std::atomic<std::uint64_t> loudnessMeasurementCapturedFrameEnd { 0 };
    std::atomic<std::uint64_t> loudnessIntegratedCapturedFrameEnd { 0 };

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
    std::atomic<std::uint32_t> spectrogramTextureRows { 0 };
    std::atomic<std::uint32_t> spectrogramTextureColumns { 0 };
    std::atomic<std::uint64_t> spectrogramTextureBytes { 0 };
    std::atomic<std::uint32_t> stereoLastPointCount { 0 };
    std::atomic<double> backingScale { 1.0 };
    std::atomic<double> stereoCorrelation { 0.0 };
    std::atomic<double> loudnessMomentaryLufs { 0.0 };
    std::atomic<double> loudnessShortTermLufs { 0.0 };
    std::atomic<double> loudnessIntegratedLufs { 0.0 };
    std::atomic<double> loudnessReferenceLufs { -23.0 };

    std::atomic<bool> metalAvailable { false };
    std::atomic<bool> renderingRequested { false };
    std::atomic<bool> effectivelyRendering { false };
    std::atomic<bool> stereoCorrelationValid { false };
    std::atomic<bool> stereoMono { false };
    std::atomic<bool> loudnessMomentaryValid { false };
    std::atomic<bool> loudnessShortTermValid { false };
    std::atomic<bool> loudnessIntegratedValid { false };
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
    id<MTLBuffer> spectrogramStagingBuffer = nil;
    id<MTLBuffer> spectrogramValidityBuffer = nil;
    id<MTLBuffer> spectrogramUniformBuffer = nil;
    id<MTLBuffer> stereoPointInstanceBuffer = nil;
    id<MTLBuffer> stereoPointUniformBuffer = nil;

    ~RenderBufferSlot()
    {
        [stereoPointUniformBuffer release];
        [stereoPointInstanceBuffer release];
        [spectrogramUniformBuffer release];
        [spectrogramValidityBuffer release];
        [spectrogramStagingBuffer release];
        [vertexBuffer release];
    }
};

struct SharedRenderState {
    std::array<RenderBufferSlot, renderBufferCount> slots;
    SpectrogramUploadGate spectrogramUploadGate;
};

struct VertexRange {
    std::size_t start = 0;
    std::size_t count = 0;
};

struct VertexBatches {
    VertexRange shell;
    VertexRange spectrumFill;
    VertexRange spectrumGrid;
    VertexRange spectrumHeld;
    VertexRange spectrum;
    VertexRange spectrumControls;
    VertexRange spectrogramHistory;
    VertexRange spectrogramAxis;
    VertexRange peakRms;
    VertexRange stereoGuides;
    VertexRange loudness;
    VertexRange dashboardSplitters;
    std::array<VertexRange, dashboardPanelCount> text;
};

struct SpectrogramShaderUniforms final {
    std::uint32_t historyColumnCount = 0;
    std::uint32_t frequencyRowCount = 0;
    std::uint32_t nextWriteColumn = 0;
    std::uint32_t timelineSpan = 0;
    std::uint32_t historyMode = 0;
    std::uint32_t palette = 0;
    float colorFloorDecibels = -120.0F;
    float colorCeilingDecibels = 0.0F;
    float colorResponse = 0.0F;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(SpectrogramShaderUniforms) <= spectrogramUniformBufferBytes);

struct StereoPointInstance final {
    simd_float2 clipPosition { };
    float opacity = 0.0F;
    float reserved = 0.0F;
};

struct StereoPointShaderUniforms final {
    simd_float2 clipHalfSize { };
    simd_float2 reserved { };
    simd_float4 colour { };
};

static_assert(sizeof(StereoPointInstance) == 16);
static_assert(sizeof(StereoPointShaderUniforms) <= stereoPointUniformBufferBytes);

struct SpectrogramUpload final {
    NSUInteger sourceOffset = 0;
    NSUInteger destinationRow = 0;
    NSUInteger rowCount = 0;
};

struct PreparedSpectrogramUploads final {
    std::array<SpectrogramUpload, maximumSpectrogramColumnsDrainedPerFrame> uploads { };
    std::size_t count = 0;
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

struct PreparedStereoPointInstances final {
    RenderRect scopeBounds;
    std::size_t count = 0;
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

    if (!std::isfinite(settings.slopeDecibelsPerOctave))
        settings.slopeDecibelsPerOctave = SpectrumRenderSettings { }.slopeDecibelsPerOctave;

    if (!std::isfinite(settings.frequencySpacing))
        settings.frequencySpacing = SpectrumRenderSettings { }.frequencySpacing;

    if (!std::isfinite(settings.fillOpacity))
        settings.fillOpacity = SpectrumRenderSettings { }.fillOpacity;

    settings.floorDecibels = std::clamp(
        settings.floorDecibels, minimumAllowedSpectrumFloor, maximumAllowedSpectrumFloor);
    settings.ceilingDecibels = std::clamp(
        settings.ceilingDecibels, minimumAllowedSpectrumCeiling, maximumAllowedSpectrumCeiling);
    if (settings.ceilingDecibels - settings.floorDecibels < minimumSpectrumRange)
        settings.floorDecibels = settings.ceilingDecibels - minimumSpectrumRange;
    constexpr std::array supportedSlopes { 0.0F, 3.0F, 4.5F, 6.0F };
    settings.slopeDecibelsPerOctave = *std::min_element(
        supportedSlopes.begin(), supportedSlopes.end(), [&](const auto left, const auto right) {
            return std::abs(left - settings.slopeDecibelsPerOctave)
                < std::abs(right - settings.slopeDecibelsPerOctave);
        });
    settings.frequencySpacing = std::clamp(settings.frequencySpacing, 0.0F, 1.0F);
    settings.fillOpacity = std::clamp(settings.fillOpacity, 0.0F, 0.5F);
    settings.traceColourRgb &= 0x00ffffffU;
    return settings;
}

std::uint64_t packSpectrumSettings(SpectrumRenderSettings settings) noexcept
{
    settings = sanitiseSpectrumSettings(settings);
    constexpr std::array supportedSlopes { 0.0F, 3.0F, 4.5F, 6.0F };
    const auto floorValue = static_cast<std::uint64_t>(
        std::lround(settings.floorDecibels - minimumAllowedSpectrumFloor));
    const auto ceilingValue = static_cast<std::uint64_t>(
        std::lround(settings.ceilingDecibels - minimumAllowedSpectrumCeiling));
    const auto slopeValue = static_cast<std::uint64_t>(std::distance(supportedSlopes.begin(),
        std::find(
            supportedSlopes.begin(), supportedSlopes.end(), settings.slopeDecibelsPerOctave)));
    const auto frequencySpacingValue
        = static_cast<std::uint64_t>(std::lround(settings.frequencySpacing * 32'767.0F));
    const auto fillOpacityValue
        = static_cast<std::uint64_t>(std::lround(settings.fillOpacity * 1'000.0F));

    return floorValue | (ceilingValue << 8U) | (slopeValue << 14U) | (frequencySpacingValue << 16U)
        | (fillOpacityValue << 31U) | (static_cast<std::uint64_t>(settings.traceColourRgb) << 40U);
}

SpectrumRenderSettings unpackSpectrumSettings(std::uint64_t packed) noexcept
{
    constexpr std::array supportedSlopes { 0.0F, 3.0F, 4.5F, 6.0F };
    const auto floorValue = static_cast<std::uint8_t>(packed & 0xffU);
    const auto ceilingValue = static_cast<std::uint8_t>((packed >> 8U) & 0x3fU);
    const auto slopeValue = static_cast<std::uint8_t>((packed >> 14U) & 0x03U);
    const auto frequencySpacingValue = static_cast<std::uint16_t>((packed >> 16U) & 0x7fffU);
    const auto fillOpacityValue = static_cast<std::uint16_t>((packed >> 31U) & 0x01ffU);
    const auto traceColourRgb = static_cast<std::uint32_t>((packed >> 40U) & 0x00ffffffU);

    return { minimumAllowedSpectrumFloor + static_cast<float>(floorValue),
        minimumAllowedSpectrumCeiling + static_cast<float>(ceilingValue),
        supportedSlopes[slopeValue], static_cast<float>(frequencySpacingValue) / 32'767.0F,
        static_cast<float>(fillOpacityValue) / 1'000.0F, traceColourRgb };
}

SpectrogramRenderSettings sanitiseSpectrogramSettings(SpectrogramRenderSettings settings) noexcept
{
    if (!std::isfinite(settings.colorResponse))
        settings.colorResponse = SpectrogramRenderSettings { }.colorResponse;
    if (!std::isfinite(settings.colorFloorDecibels))
        settings.colorFloorDecibels = SpectrogramRenderSettings { }.colorFloorDecibels;
    if (!std::isfinite(settings.colorCeilingDecibels))
        settings.colorCeilingDecibels = SpectrogramRenderSettings { }.colorCeilingDecibels;

    settings.colorResponse = std::clamp(settings.colorResponse, -2.0F, 2.0F);
    settings.colorFloorDecibels = std::clamp(settings.colorFloorDecibels, -180.0F, -36.0F);
    settings.colorCeilingDecibels = std::clamp(settings.colorCeilingDecibels, -24.0F, 12.0F);
    if (settings.colorCeilingDecibels - settings.colorFloorDecibels < 24.0F)
        settings.colorFloorDecibels = settings.colorCeilingDecibels - 24.0F;

    constexpr std::array supportedDurations { 2, 5, 10, 20, 30, 60 };
    settings.historyDurationSeconds = *std::min_element(supportedDurations.begin(),
        supportedDurations.end(), [&](const auto left, const auto right) {
            return std::abs(left - settings.historyDurationSeconds)
                < std::abs(right - settings.historyDurationSeconds);
        });
    constexpr std::array supportedRates { 15, 30, 60, 120 };
    settings.requestedSliceRateHz = *std::min_element(
        supportedRates.begin(), supportedRates.end(), [&](const auto left, const auto right) {
            return std::abs(left - settings.requestedSliceRateHz)
                < std::abs(right - settings.requestedSliceRateHz);
        });

    switch (settings.palette) {
    case SpectrogramRenderPalette::blueFire:
    case SpectrogramRenderPalette::inferno:
    case SpectrogramRenderPalette::viridis:
    case SpectrogramRenderPalette::grayscale:
        break;
    default:
        settings.palette = SpectrogramRenderPalette::blueFire;
        break;
    }

    switch (settings.historyMode) {
    case SpectrogramRenderHistoryMode::scroll:
    case SpectrogramRenderHistoryMode::overwrite:
        break;
    default:
        settings.historyMode = SpectrogramRenderHistoryMode::scroll;
        break;
    }

    return settings;
}

std::uint32_t packSpectrogramSettings(SpectrogramRenderSettings settings) noexcept
{
    settings = sanitiseSpectrogramSettings(settings);
    constexpr std::array supportedDurations { 2, 5, 10, 20, 30, 60 };
    constexpr std::array supportedRates { 15, 30, 60, 120 };
    const auto palette = static_cast<std::uint32_t>(settings.palette);
    const auto mode = static_cast<std::uint32_t>(settings.historyMode);
    const auto duration = static_cast<std::uint32_t>(std::distance(supportedDurations.begin(),
        std::find(supportedDurations.begin(), supportedDurations.end(),
            settings.historyDurationSeconds)));
    const auto rate = static_cast<std::uint32_t>(std::distance(supportedRates.begin(),
        std::find(supportedRates.begin(), supportedRates.end(), settings.requestedSliceRateHz)));
    const auto floor
        = static_cast<std::uint32_t>(std::lround(settings.colorFloorDecibels + 180.0F));
    const auto ceiling
        = static_cast<std::uint32_t>(std::lround(settings.colorCeilingDecibels + 24.0F));
    const auto response
        = static_cast<std::uint32_t>(std::lround((settings.colorResponse + 2.0F) * 100.0F));
    return palette | (mode << 2U) | (duration << 3U) | (rate << 6U) | (floor << 8U)
        | (ceiling << 16U) | (response << 22U);
}

SpectrogramRenderSettings unpackSpectrogramSettings(const std::uint32_t packed) noexcept
{
    constexpr std::array supportedDurations { 2, 5, 10, 20, 30, 60 };
    constexpr std::array supportedRates { 15, 30, 60, 120 };
    const auto palette = static_cast<SpectrogramRenderPalette>(packed & 0x03U);
    const auto mode = static_cast<SpectrogramRenderHistoryMode>((packed >> 2U) & 0x01U);
    const auto duration = static_cast<std::size_t>((packed >> 3U) & 0x07U);
    const auto rate = static_cast<std::size_t>((packed >> 6U) & 0x03U);
    const auto floor = static_cast<float>((packed >> 8U) & 0xffU) - 180.0F;
    const auto ceiling = static_cast<float>((packed >> 16U) & 0x3fU) - 24.0F;
    const auto response = (static_cast<float>((packed >> 22U) & 0x01ffU) / 100.0F) - 2.0F;
    return sanitiseSpectrogramSettings({ palette, response, floor, ceiling,
        supportedDurations[std::min(duration, supportedDurations.size() - 1)], mode,
        supportedRates[std::min(rate, supportedRates.size() - 1)] });
}

LoudnessRenderSettings sanitiseLoudnessSettings(LoudnessRenderSettings settings) noexcept
{
    if (!std::isfinite(settings.referenceLufs))
        settings.referenceLufs = LoudnessRenderSettings { }.referenceLufs;

    settings.referenceLufs = std::clamp(
        settings.referenceLufs, minimumLoudnessReferenceLufs, maximumLoudnessReferenceLufs);
    const auto step = std::lround(
        (settings.referenceLufs - minimumLoudnessReferenceLufs) / loudnessReferenceStepLufs);
    settings.referenceLufs
        = minimumLoudnessReferenceLufs + (static_cast<float>(step) * loudnessReferenceStepLufs);
    return settings;
}

std::uint32_t packLoudnessSettings(const LoudnessRenderSettings settings) noexcept
{
    const auto sanitized = sanitiseLoudnessSettings(settings);
    return static_cast<std::uint32_t>(std::lround(
        (sanitized.referenceLufs - minimumLoudnessReferenceLufs) / loudnessReferenceStepLufs));
}

LoudnessRenderSettings unpackLoudnessSettings(const std::uint32_t packed) noexcept
{
    constexpr auto maximumStep = static_cast<std::uint32_t>(
        (maximumLoudnessReferenceLufs - minimumLoudnessReferenceLufs) / loudnessReferenceStepLufs);
    return { minimumLoudnessReferenceLufs
        + (static_cast<float>(std::min(packed, maximumStep)) * loudnessReferenceStepLufs) };
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

MTLScissorRect makeScissorRect(const RenderRect& bounds, const CGSize logicalSize,
    const NSUInteger drawableWidth, const NSUInteger drawableHeight) noexcept
{
    if (logicalSize.height <= 0.0)
        return { 0, 0, 0, 0 };

    return makeScissorRect(
        DashboardLogicalBounds { static_cast<double>(bounds.left),
            static_cast<double>(logicalSize.height) - static_cast<double>(bounds.top),
            static_cast<double>(bounds.width()), static_cast<double>(bounds.height()) },
        logicalSize, drawableWidth, drawableHeight);
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

struct SpectrogramUniforms
{
    uint historyColumnCount;
    uint frequencyRowCount;
    uint nextWriteColumn;
    uint timelineSpan;
    uint historyMode;
    uint palette;
    float colorFloorDecibels;
    float colorCeilingDecibels;
    float colorResponse;
    uint reserved;
};

struct StereoPointInstance
{
    float2 clipPosition;
    float opacity;
    float reserved;
};

struct StereoPointUniforms
{
    float2 clipHalfSize;
    float2 reserved;
    float4 colour;
};

struct StereoPointRasterVertex
{
    float4 position [[position]];
    float4 colour;
    float2 pointCoordinate;
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

vertex StereoPointRasterVertex audioInsightStereoPointVertex(
    const device StereoPointInstance* instances [[buffer(0)]],
    constant StereoPointUniforms& uniforms [[buffer(1)]],
    uint vertexId [[vertex_id]], uint instanceId [[instance_id]])
{
    constexpr float2 corners[] = {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f),
    };
    const float2 corner = corners[min(vertexId, 3u)];
    const StereoPointInstance point = instances[instanceId];
    StereoPointRasterVertex output;
    output.position = float4(point.clipPosition + corner * uniforms.clipHalfSize, 0.0f, 1.0f);
    output.colour = float4(uniforms.colour.rgb, uniforms.colour.a * point.opacity);
    output.pointCoordinate = corner;
    return output;
}

fragment half4 audioInsightStereoPointFragment(StereoPointRasterVertex input [[stage_in]])
{
    const float radius = length(input.pointCoordinate);
    const float coverage = 1.0f - smoothstep(0.70f, 1.0f, radius);
    return half4(half3(input.colour.rgb), half(input.colour.a * coverage));
}

fragment half4 audioInsightTextFragment(RasterVertex input [[stage_in]],
                                        texture2d<float> glyphAtlas [[texture(0)]],
                                        sampler glyphSampler [[sampler(0)]])
{
    const float coverage = glyphAtlas.sample(glyphSampler, input.textureCoordinate).r;
    return half4(half3(input.colour.rgb), half(input.colour.a * coverage));
}

fragment half4 audioInsightSpectrogramFragment(
    RasterVertex input [[stage_in]],
    constant SpectrogramUniforms& uniforms [[buffer(0)]],
    const device uchar* validColumns [[buffer(1)]],
    texture2d<float, access::read> history [[texture(0)]],
    texture2d<float> palettes [[texture(1)]],
    sampler paletteSampler [[sampler(0)]])
{
    if (uniforms.historyColumnCount == 0 || uniforms.frequencyRowCount == 0)
        return half4(0.0h, 0.0h, 0.0h, 1.0h);

    const float boundedX = clamp(input.textureCoordinate.x, 0.0f, 1.0f);
    const uint screenColumn = min(uint(boundedX * float(uniforms.historyColumnCount)),
                                  uniforms.historyColumnCount - 1);
    uint physicalColumn = screenColumn;

    if (uniforms.historyMode == 0) {
        const uint span = min(uniforms.timelineSpan, uniforms.historyColumnCount);
        const uint emptyColumns = uniforms.historyColumnCount - span;
        if (screenColumn < emptyColumns)
            return half4(0.0h, 0.0h, 0.0h, 1.0h);

        const uint oldest = (uniforms.nextWriteColumn + uniforms.historyColumnCount - span)
            % uniforms.historyColumnCount;
        physicalColumn = (oldest + screenColumn - emptyColumns) % uniforms.historyColumnCount;
    }

    if (validColumns[physicalColumn] == 0)
        return half4(0.0h, 0.0h, 0.0h, 1.0h);

    // Quad texture coordinates use the conventional top-origin direction.
    // Stored rows increase from low to high frequency, so reverse that axis.
    const float frequencyCoordinate
        = 1.0f - clamp(input.textureCoordinate.y, 0.0f, 1.0f);
    const float rowPosition
        = frequencyCoordinate * float(max(uniforms.frequencyRowCount, 1u) - 1u);
    const uint lowerRow = min(uint(floor(rowPosition)), uniforms.frequencyRowCount - 1);
    const uint upperRow = min(lowerRow + 1, uniforms.frequencyRowCount - 1);
    const float rowFraction = rowPosition - float(lowerRow);
    const float lowerDecibels = history.read(uint2(lowerRow, physicalColumn)).r;
    const float upperDecibels = history.read(uint2(upperRow, physicalColumn)).r;
    const float decibels = mix(lowerDecibels, upperDecibels, rowFraction);
    const float range = max(0.0001f,
        uniforms.colorCeilingDecibels - uniforms.colorFloorDecibels);
    const float value = clamp((decibels - uniforms.colorFloorDecibels) / range, 0.0f, 1.0f);
    const float paletteCoordinate = pow(value, exp2(uniforms.colorResponse));
    const float paletteX = (paletteCoordinate * 255.0f + 0.5f) / 256.0f;
    const float paletteY = (float(min(uniforms.palette, 3u)) + 0.5f) / 4.0f;
    const float3 colour = palettes.sample(paletteSampler, float2(paletteX, paletteY)).rgb;
    return half4(half3(colour), 1.0h);
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
        setSpectrogramSettings({ });
        setLoudnessSettings({ });
        setDashboardLayoutSplits(DashboardLayout::defaultSplits);
        initialiseMetal();
        callbackTelemetry->metalAvailable.store(metalReady, std::memory_order_relaxed);
    }

    ~MetalRenderBackend()
    {
        shutdown();
        [stereoPointPipelineState release];
        [spectrogramHistoryTexture release];
        [spectrogramPaletteTexture release];
        [spectrogramPaletteSamplerState release];
        [spectrogramPipelineState release];
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
        dashboardLayoutEditing.store(false, std::memory_order_release);
        draggedDashboardSplitter.reset();
        focusedDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(noDashboardSplitterIndex), std::memory_order_release);
        activeDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(noDashboardSplitterIndex), std::memory_order_release);

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
        // The ordered analysis stream owns Spectrogram mapping invalidation.
        // A renderer-side clear here can arrive after the new-generation seed
        // and erase it without another column to restore the history.
        packedSpectrumSettings.store(packSpectrumSettings(settings), std::memory_order_release);
    }

    [[nodiscard]] SpectrumRenderSettings getSpectrumSettings() const noexcept
    {
        return unpackSpectrumSettings(packedSpectrumSettings.load(std::memory_order_acquire));
    }

    void setSpectrogramSettings(SpectrogramRenderSettings settings) noexcept
    {
        const auto packed = packSpectrogramSettings(settings);
        const auto previous = packedSpectrogramSettings.exchange(packed, std::memory_order_acq_rel);
        if (previous == packed)
            return;

        const auto before = unpackSpectrogramSettings(previous);
        const auto after = unpackSpectrogramSettings(packed);
        // FFT slice-rate invalidation is ordered with its reset/seed records in
        // the analysis stream. History duration is renderer-owned and is the
        // only setting that needs an independent deferred clear here.
        if (before.historyDurationSeconds != after.historyDurationSeconds) {
            source.discardPendingSpectrogramColumns();
            spectrogramConfigurationClearPending.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] SpectrogramRenderSettings getSpectrogramSettings() const noexcept
    {
        return unpackSpectrogramSettings(packedSpectrogramSettings.load(std::memory_order_acquire));
    }

    void setLoudnessSettings(const LoudnessRenderSettings settings) noexcept
    {
        const auto sanitized = sanitiseLoudnessSettings(settings);
        packedLoudnessSettings.store(packLoudnessSettings(sanitized), std::memory_order_release);
        loadPublishedTelemetry()->loudnessReferenceLufs.store(
            sanitized.referenceLufs, std::memory_order_relaxed);
    }

    [[nodiscard]] LoudnessRenderSettings getLoudnessSettings() const noexcept
    {
        return unpackLoudnessSettings(packedLoudnessSettings.load(std::memory_order_acquire));
    }

    void setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
    {
        const auto packed = packDashboardLayoutSplits(splits);
        if (packedDashboardLayoutSplits.exchange(packed, std::memory_order_acq_rel) == packed)
            return;

        auto* const messageManager = juce::MessageManager::getInstanceWithoutCreating();
        if (messageManager != nullptr && messageManager->isThisTheMessageThread() && view != nil)
            [view dashboardLayoutGeometryChanged];
    }

    [[nodiscard]] DashboardLayoutSplits getDashboardLayoutSplits() const noexcept
    {
        return unpackDashboardLayoutSplits(
            packedDashboardLayoutSplits.load(std::memory_order_acquire));
    }

    void setDashboardLayoutEditing(const bool shouldEdit)
    {
        assertMessageThread();
        if (dashboardLayoutEditing.exchange(shouldEdit, std::memory_order_acq_rel) == shouldEdit)
            return;

        draggedDashboardSplitter.reset();
        activeDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(noDashboardSplitterIndex), std::memory_order_release);
        focusedDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(noDashboardSplitterIndex), std::memory_order_release);

        if (view != nil) {
            [view dashboardLayoutEditingStateChanged];

            if (shouldEdit)
                static_cast<void>(focusDashboardSplitter(0));
        }
    }

    [[nodiscard]] bool isDashboardLayoutEditing() const noexcept
    {
        return dashboardLayoutEditing.load(std::memory_order_acquire);
    }

    void setDashboardLayoutEditCancelCallback(
        MetalVisualization::DashboardLayoutEditCancelCallback callback)
    {
        assertMessageThread();
        dashboardLayoutEditCancelCallback = std::move(callback);
    }

    [[nodiscard]] DashboardSplitterLayout dashboardSplitterLayout() const noexcept
    {
        return DashboardLayout::calculateSplitterLayout(
            dashboardLogicalBounds(), getDashboardLayoutSplits());
    }

    [[nodiscard]] DashboardSplitterAccessibilityValue dashboardSplitterAccessibilityValue(
        const std::size_t index) const noexcept
    {
        return DashboardLayout::accessibilityValue(
            getDashboardLayoutSplits(), dashboardSplitterAtIndex(index));
    }

    [[nodiscard]] DashboardSplitterRange dashboardSplitterAccessibilityRange(
        const std::size_t index) const noexcept
    {
        return DashboardLayout::legalRange(
            getDashboardLayoutSplits(), dashboardSplitterAtIndex(index));
    }

    [[nodiscard]] int dashboardSplitterAccessibilityPosition(const std::size_t index) const noexcept
    {
        return DashboardLayout::splitterPosition(
            getDashboardLayoutSplits(), dashboardSplitterAtIndex(index));
    }

    [[nodiscard]] DashboardSplitterAxis dashboardSplitterAxis(
        const std::size_t index) const noexcept
    {
        if (index >= dashboardSplitterCount)
            return DashboardSplitterAxis::horizontal;

        return dashboardSplitterLayout().splitters[index].axis;
    }

    [[nodiscard]] DashboardLogicalBounds dashboardSplitterPointerHitBounds(
        const std::size_t index) const noexcept
    {
        if (index >= dashboardSplitterCount)
            return { };

        return dashboardSplitterLayout().splitters[index].pointerHitBounds;
    }

    [[nodiscard]] bool isDashboardSplitterFocused(const std::size_t index) const noexcept
    {
        return isDashboardLayoutEditing()
            && focusedDashboardSplitterIndex.load(std::memory_order_acquire) == index && view != nil
            && view.window != nil && view.window.firstResponder == view;
    }

    [[nodiscard]] bool focusDashboardSplitter(const std::size_t index)
    {
        assertMessageThread();
        if (!isDashboardLayoutEditing() || index >= dashboardSplitterCount || view == nil
            || view.window == nil || ![view.window makeFirstResponder:view]) {
            return false;
        }

        setFocusedDashboardSplitterIndex(index);
        return true;
    }

    [[nodiscard]] bool beginDashboardSplitterDrag(const NSPoint point)
    {
        assertMessageThread();
        if (!isDashboardLayoutEditing())
            return false;

        draggedDashboardSplitter.reset();
        activeDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(noDashboardSplitterIndex), std::memory_order_release);

        const auto hit = DashboardLayout::hitTestSplitter(
            dashboardSplitterLayout(), dashboardPointFromNativePoint(point));
        if (!hit.has_value())
            return false;

        draggedDashboardSplitter = *hit;
        const auto index = dashboardSplitterIndex(*hit);
        activeDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(index), std::memory_order_release);
        static_cast<void>(focusDashboardSplitter(index));
        return true;
    }

    [[nodiscard]] bool dragDashboardSplitter(const NSPoint point)
    {
        assertMessageThread();
        if (!isDashboardLayoutEditing() || !draggedDashboardSplitter.has_value())
            return false;

        const auto splitter = *draggedDashboardSplitter;
        const auto moved = DashboardLayout::moveSplitterToPointer(getDashboardLayoutSplits(),
            splitter, dashboardLogicalBounds(), dashboardPointFromNativePoint(point));
        publishDashboardLayoutEdit(moved, dashboardSplitterIndex(splitter));
        return true;
    }

    [[nodiscard]] bool endDashboardSplitterDrag(const NSPoint point)
    {
        assertMessageThread();
        if (!draggedDashboardSplitter.has_value())
            return false;

        static_cast<void>(dragDashboardSplitter(point));
        draggedDashboardSplitter.reset();
        activeDashboardSplitterIndex.store(
            static_cast<std::uint32_t>(noDashboardSplitterIndex), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool handleDashboardLayoutKeyDown(NSEvent* const event)
    {
        assertMessageThread();
        if (!isDashboardLayoutEditing() || event == nil)
            return false;

        constexpr unsigned short tabKeyCode = 48;
        constexpr unsigned short escapeKeyCode = 53;
        constexpr unsigned short homeKeyCode = 115;
        constexpr unsigned short endKeyCode = 119;
        constexpr unsigned short leftArrowKeyCode = 123;
        constexpr unsigned short rightArrowKeyCode = 124;
        constexpr unsigned short downArrowKeyCode = 125;
        constexpr unsigned short upArrowKeyCode = 126;

        const auto keyCode = event.keyCode;
        if (keyCode == escapeKeyCode) {
            invokeDashboardLayoutEditCancelCallback();
            return true;
        }

        auto focusedIndex = static_cast<std::size_t>(
            focusedDashboardSplitterIndex.load(std::memory_order_acquire));
        if (focusedIndex >= dashboardSplitterCount)
            focusedIndex = 0;

        if (keyCode == tabKeyCode) {
            const auto backwards = (event.modifierFlags & NSEventModifierFlagShift) != 0;
            const auto nextIndex = backwards
                ? (focusedIndex + dashboardSplitterCount - 1) % dashboardSplitterCount
                : (focusedIndex + 1) % dashboardSplitterCount;
            setFocusedDashboardSplitterIndex(nextIndex);
            return true;
        }

        const auto splitter = dashboardSplitterAtIndex(focusedIndex);
        const auto geometry = dashboardSplitterLayout().splitters[focusedIndex];
        auto requestedPosition
            = DashboardLayout::splitterPosition(getDashboardLayoutSplits(), splitter);
        auto handled = true;

        if (keyCode == homeKeyCode) {
            requestedPosition
                = DashboardLayout::legalRange(getDashboardLayoutSplits(), splitter).minimum;
        } else if (keyCode == endKeyCode) {
            requestedPosition
                = DashboardLayout::legalRange(getDashboardLayoutSplits(), splitter).maximum;
        } else if (geometry.axis == DashboardSplitterAxis::horizontal
            && (keyCode == upArrowKeyCode || keyCode == downArrowKeyCode)) {
            requestedPosition += keyCode == upArrowKeyCode ? -1 : 1;
        } else if (geometry.axis == DashboardSplitterAxis::vertical
            && (keyCode == leftArrowKeyCode || keyCode == rightArrowKeyCode)) {
            requestedPosition += keyCode == leftArrowKeyCode ? -1 : 1;
        } else {
            handled = false;
        }

        if (!handled)
            return false;

        publishDashboardLayoutEdit(
            DashboardLayout::moveSplitter(getDashboardLayoutSplits(), splitter, requestedPosition),
            focusedIndex);
        return true;
    }

    [[nodiscard]] bool adjustDashboardSplitter(const std::size_t index, const int deltaTracks)
    {
        assertMessageThread();
        if (!isDashboardLayoutEditing() || index >= dashboardSplitterCount || deltaTracks == 0)
            return false;

        if (!focusDashboardSplitter(index))
            return false;

        const auto current = getDashboardLayoutSplits();
        const auto splitter = dashboardSplitterAtIndex(index);
        const auto requested = DashboardLayout::splitterPosition(current, splitter) + deltaTracks;
        const auto moved = DashboardLayout::moveSplitter(current, splitter, requested);
        if (moved == current)
            return false;

        publishDashboardLayoutEdit(moved, index);
        return true;
    }

    [[nodiscard]] juce::String loudnessAccessibilityValue() const
    {
        const auto telemetry = loadPublishedTelemetry();
        juce::String result { "Momentary: " };
        result += formatLoudnessAccessibilityReading(
            telemetry->loudnessMomentaryLufs.load(std::memory_order_relaxed),
            telemetry->loudnessMomentaryValid.load(std::memory_order_relaxed));
        result += "; Short-term: ";
        result += formatLoudnessAccessibilityReading(
            telemetry->loudnessShortTermLufs.load(std::memory_order_relaxed),
            telemetry->loudnessShortTermValid.load(std::memory_order_relaxed));
        result += "; Integrated: ";
        result += formatLoudnessAccessibilityReading(
            telemetry->loudnessIntegratedLufs.load(std::memory_order_relaxed),
            telemetry->loudnessIntegratedValid.load(std::memory_order_relaxed));
        return result;
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
        result.spectrogramColumnsRead
            = telemetry->spectrogramColumnsRead.load(std::memory_order_relaxed);
        result.spectrogramColumnsUploaded
            = telemetry->spectrogramColumnsUploaded.load(std::memory_order_relaxed);
        result.spectrogramColumnsRejected
            = telemetry->spectrogramColumnsRejected.load(std::memory_order_relaxed);
        result.spectrogramGapColumns
            = telemetry->spectrogramGapColumns.load(std::memory_order_relaxed);
        result.spectrogramHistoryClears
            = telemetry->spectrogramHistoryClears.load(std::memory_order_relaxed);
        result.spectrogramTextureReallocations
            = telemetry->spectrogramTextureReallocations.load(std::memory_order_relaxed);
        result.spectrogramTextureAllocationFailures
            = telemetry->spectrogramTextureAllocationFailures.load(std::memory_order_relaxed);
        result.spectrogramUploadBackpressureDrops
            = telemetry->spectrogramUploadBackpressureDrops.load(std::memory_order_relaxed);
        result.spectrogramUploadCommands
            = telemetry->spectrogramUploadCommands.load(std::memory_order_relaxed);
        result.spectrogramUploadBytes
            = telemetry->spectrogramUploadBytes.load(std::memory_order_relaxed);
        result.spectrogramLastColumnSequence
            = telemetry->spectrogramLastColumnSequence.load(std::memory_order_relaxed);
        result.lastStereoSequence = telemetry->lastStereoSequence.load(std::memory_order_relaxed);
        result.stereoPointInstancesPrepared
            = telemetry->stereoPointInstancesPrepared.load(std::memory_order_relaxed);
        result.stereoPointDrawCalls
            = telemetry->stereoPointDrawCalls.load(std::memory_order_relaxed);
        result.lastLoudnessSequence
            = telemetry->lastLoudnessSequence.load(std::memory_order_relaxed);
        result.loudnessMeasurementCapturedFrameEnd
            = telemetry->loudnessMeasurementCapturedFrameEnd.load(std::memory_order_relaxed);
        result.loudnessIntegratedCapturedFrameEnd
            = telemetry->loudnessIntegratedCapturedFrameEnd.load(std::memory_order_relaxed);
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
        result.spectrogramTextureRows
            = telemetry->spectrogramTextureRows.load(std::memory_order_relaxed);
        result.spectrogramTextureColumns
            = telemetry->spectrogramTextureColumns.load(std::memory_order_relaxed);
        result.spectrogramTextureBytes
            = telemetry->spectrogramTextureBytes.load(std::memory_order_relaxed);
        result.stereoLastPointCount
            = telemetry->stereoLastPointCount.load(std::memory_order_relaxed);
        result.backingScale = telemetry->backingScale.load(std::memory_order_relaxed);
        result.stereoCorrelation = telemetry->stereoCorrelation.load(std::memory_order_relaxed);
        result.loudnessMomentaryLufs
            = telemetry->loudnessMomentaryLufs.load(std::memory_order_relaxed);
        result.loudnessShortTermLufs
            = telemetry->loudnessShortTermLufs.load(std::memory_order_relaxed);
        result.loudnessIntegratedLufs
            = telemetry->loudnessIntegratedLufs.load(std::memory_order_relaxed);
        result.loudnessReferenceLufs
            = telemetry->loudnessReferenceLufs.load(std::memory_order_relaxed);
        result.metalAvailable = telemetry->metalAvailable.load(std::memory_order_relaxed);
        result.renderingRequested = telemetry->renderingRequested.load(std::memory_order_relaxed);
        result.effectivelyRendering
            = telemetry->effectivelyRendering.load(std::memory_order_relaxed);
        result.stereoCorrelationValid
            = telemetry->stereoCorrelationValid.load(std::memory_order_relaxed);
        result.stereoMono = telemetry->stereoMono.load(std::memory_order_relaxed);
        result.loudnessMomentaryValid
            = telemetry->loudnessMomentaryValid.load(std::memory_order_relaxed);
        result.loudnessShortTermValid
            = telemetry->loudnessShortTermValid.load(std::memory_order_relaxed);
        result.loudnessIntegratedValid
            = telemetry->loudnessIntegratedValid.load(std::memory_order_relaxed);
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

    bool tryClearSpectrumAt(const NSPoint point) noexcept
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
            dashboardLayout[DashboardPanel::spectrum], static_cast<float>(boundsSize.height));
        const auto panelHeaderHeight
            = std::min(panel.height(), std::clamp(panel.height() * 0.13F, 18.0F, 26.0F));
        const auto layout
            = calculateSpectrumClearLayout(panel.width(), panel.height(), panelHeaderHeight);
        const auto localX = static_cast<float>(point.x) - panel.left;
        const auto localY = static_cast<float>(point.y) - panel.bottom;

        if (!layout.hitBounds.contains(localX, localY))
            return false;

        source.resetSpectrum();
        return true;
    }

    bool performSpectrumClearAction() noexcept
    {
        assertMessageThread();

        if (isDashboardLayoutEditing())
            return false;

        source.resetSpectrum();
        return true;
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

        if (isDashboardLayoutEditing())
            return false;

        source.resetPeakRms();
        return true;
    }

    bool tryResetLoudnessAt(const NSPoint point) noexcept
    {
        assertMessageThread();

        if (view == nil || isDashboardLayoutEditing() || !std::isfinite(point.x)
            || !std::isfinite(point.y)) {
            return false;
        }

        const auto boundsSize = view.bounds.size;
        const auto dashboardLayout = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, static_cast<double>(boundsSize.width),
                static_cast<double>(boundsSize.height) },
            getDashboardLayoutSplits());
        const auto panel = toRenderRect(
            dashboardLayout[DashboardPanel::loudness], static_cast<float>(boundsSize.height));
        const auto panelHeaderHeight
            = std::min(panel.height(), std::clamp(panel.height() * 0.13F, 18.0F, 26.0F));
        constexpr auto textScale = 0.78F;
        const auto readoutWidth
            = (((static_cast<float>(maximumLoudnessReadoutGlyphs) - 1.0F) * cachedGlyphAdvance)
                  + cachedGlyphCellWidth)
            * textScale;
        const auto layout = calculateLoudnessPanelLayout(panel.width(), panel.height(),
            panelHeaderHeight, cachedGlyphCellHeight * textScale, readoutWidth);
        const auto localX = static_cast<float>(point.x) - panel.left;
        const auto localY = static_cast<float>(point.y) - panel.bottom;
        if (!layout.resetHitBounds.contains(localX, localY))
            return false;

        invalidateIntegratedLoudnessForReset();
        source.resetLoudness();
        return true;
    }

    bool performLoudnessResetAction() noexcept
    {
        assertMessageThread();

        if (isDashboardLayoutEditing())
            return false;

        invalidateIntegratedLoudnessForReset();
        source.resetLoudness();
        return true;
    }

    PreparedStereoPointInstances prepareStereoPointInstances(RenderBufferSlot& slot,
        const CGSize logicalSize, const DashboardTileLayout& dashboardLayout,
        const Clock::time_point now, AtomicRenderTelemetry& telemetry) const noexcept
    {
        PreparedStereoPointInstances prepared;
        if (logicalSize.width <= 0.0 || logicalSize.height <= 0.0
            || slot.stereoPointInstanceBuffer == nil || slot.stereoPointUniformBuffer == nil) {
            return prepared;
        }

        const auto logicalHeight = static_cast<float>(logicalSize.height);
        const auto panel
            = toRenderRect(dashboardLayout[DashboardPanel::stereoField], logicalHeight);
        const auto panelHeaderHeight
            = std::min(panel.height(), std::clamp(panel.height() * 0.13F, 18.0F, 26.0F));
        constexpr auto stereoTextScale = 0.78F;
        const auto layout = calculateStereoFieldPanelLayout(panel.width(), panel.height(),
            panelHeaderHeight, cachedGlyphCellHeight * stereoTextScale);
        prepared.scopeBounds
            = { panel.left + layout.scopeBounds.left, panel.bottom + layout.scopeBounds.bottom,
                  panel.left + layout.scopeBounds.right, panel.bottom + layout.scopeBounds.top };

        const auto fieldValid = hasDisplayFrame && targetFrame.stereoFieldValid;
        const auto isMono = hasDisplayFrame && targetFrame.stereoMono;
        const auto correlationValid = hasDisplayFrame && !isMono
            && targetFrame.stereoCorrelationValid && std::isfinite(targetFrame.stereoCorrelation);
        telemetry.stereoCorrelation.store(correlationValid
                ? static_cast<double>(std::clamp(targetFrame.stereoCorrelation, -1.0F, 1.0F))
                : 0.0,
            std::memory_order_relaxed);
        telemetry.stereoCorrelationValid.store(correlationValid, std::memory_order_relaxed);
        telemetry.stereoMono.store(isMono, std::memory_order_relaxed);

        auto* const uniforms
            = static_cast<StereoPointShaderUniforms*>(slot.stereoPointUniformBuffer.contents);
        const auto radius = std::min(1.25F,
            std::max(0.0F,
                std::min(prepared.scopeBounds.width(), prepared.scopeBounds.height()) * 0.025F));
        *uniforms = { simd_make_float2(2.0F * radius / static_cast<float>(logicalSize.width),
                          2.0F * radius / static_cast<float>(logicalSize.height)),
            simd_make_float2(0.0F, 0.0F), simd_make_float4(0.10F, 0.55F, 0.70F, 0.28F) };

        if (!fieldValid || prepared.scopeBounds.width() <= 0.0F
            || prepared.scopeBounds.height() <= 0.0F) {
            telemetry.stereoLastPointCount.store(0, std::memory_order_relaxed);
            return prepared;
        }

        const auto elapsed = stereoSnapshotAcceptedTime != Clock::time_point { }
            ? std::chrono::duration<double>(now - stereoSnapshotAcceptedTime).count()
            : 0.0;
        const auto sourceCount = std::min<std::size_t>(
            targetFrame.stereoFieldPointCount, maximumStereoFieldPointCount);
        auto* const instances
            = static_cast<StereoPointInstance*>(slot.stereoPointInstanceBuffer.contents);
        const auto pointToClip = [logicalSize](const float x, const float y) noexcept {
            return simd_make_float2((2.0F * x / static_cast<float>(logicalSize.width)) - 1.0F,
                (2.0F * y / static_cast<float>(logicalSize.height)) - 1.0F);
        };

        for (std::size_t index = 0; index < sourceCount; ++index) {
            const auto& point = targetFrame.stereoFieldPoints[index];
            if (!std::isfinite(point.horizontal) || !std::isfinite(point.vertical))
                continue;

            const auto opacity = stereoFieldPointAgeOpacity(point.normalizedAge, elapsed);
            if (opacity <= 0.0F)
                continue;

            const auto x = mapStereoFieldCoordinate(
                point.horizontal, prepared.scopeBounds.left, prepared.scopeBounds.right);
            const auto y = mapStereoFieldCoordinate(
                point.vertical, prepared.scopeBounds.bottom, prepared.scopeBounds.top);
            instances[prepared.count++] = { pointToClip(x, y), opacity, 0.0F };
        }

        telemetry.stereoLastPointCount.store(
            static_cast<std::uint32_t>(prepared.count), std::memory_order_relaxed);
        telemetry.stereoPointInstancesPrepared.fetch_add(prepared.count, std::memory_order_relaxed);
        return prepared;
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

        // CPU ring validity becomes speculative as soon as a column is staged.
        // Do not drain, mutate, or render that state from another callback until
        // the upload-bearing command buffer has completed successfully or
        // published its failure for the next callback to consume.
        if (sharedState->spectrogramUploadGate.isUploadInFlight()) {
            telemetry->spectrogramUploadBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            telemetry->skippedPresentations.fetch_add(1, std::memory_order_relaxed);
            return;
        }

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
            acceptSnapshot(incomingFrame, callbackTime, *telemetry);
        }

        const auto spectrumSettings = getSpectrumSettings();
        const auto spectrogramSettings = getSpectrogramSettings();
        const auto loudnessSettings = getLoudnessSettings();
        telemetry->loudnessReferenceLufs.store(
            loudnessSettings.referenceLufs, std::memory_order_relaxed);
        const auto preparedSpectrogramUploads
            = prepareSpectrogramUploads(slot, spectrogramSettings, *telemetry);
        updateInterpolatedDisplayValues(callbackTime);

        const auto boundsSize = view.bounds.size;
        const auto dashboardLayout = DashboardLayout::calculateTileLayout(
            { 0.0, 0.0, static_cast<double>(boundsSize.width),
                static_cast<double>(boundsSize.height) },
            getDashboardLayoutSplits());
        const auto preparedStereoPoints = prepareStereoPointInstances(
            slot, boundsSize, dashboardLayout, callbackTime, *telemetry);

        auto* vertices = static_cast<MetalVertex*>(slot.vertexBuffer.contents);
        const auto batches = populateVertices(vertices, boundsSize, dashboardLayout,
            spectrumSettings, spectrogramSettings, loudnessSettings);

        auto* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* colourAttachment = descriptor.colorAttachments[0];
        colourAttachment.texture = drawable.texture;
        colourAttachment.loadAction = MTLLoadActionClear;
        colourAttachment.storeAction = MTLStoreActionStore;
        colourAttachment.clearColor = MTLClearColorMake(0.018, 0.024, 0.035, 1.0);

        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];

        if (commandBuffer == nil) {
            if (preparedSpectrogramUploads.count != 0)
                clearSpectrogramHistory(telemetry.get());
            recordSkippedPresentation(submission, telemetry);
            releaseRenderBuffer(sharedState, admission);
            telemetry->gpuBackpressureDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const auto spectrogramUploadsEncoded
            = encodeSpectrogramUploads(commandBuffer, slot, preparedSpectrogramUploads, *telemetry);
        if (!spectrogramUploadsEncoded) {
            clearSpectrogramHistory(telemetry.get());
            writeSpectrogramGpuState(slot, spectrogramSettings);
        }

        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:descriptor];

        if (encoder == nil) {
            if (preparedSpectrogramUploads.count != 0)
                clearSpectrogramHistory(telemetry.get());
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

            if (batches.spectrumFill.count >= 4)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:batches.spectrumFill.start
                            vertexCount:batches.spectrumFill.count];

            if (batches.spectrumGrid.count != 0)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:batches.spectrumGrid.start
                            vertexCount:batches.spectrumGrid.count];

            if (batches.spectrumHeld.count >= 2)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:batches.spectrumHeld.start
                            vertexCount:batches.spectrumHeld.count];

            if (batches.spectrum.count >= 2)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:batches.spectrum.start
                            vertexCount:batches.spectrum.count];

            if (batches.spectrumControls.count != 0)
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:batches.spectrumControls.start
                            vertexCount:batches.spectrumControls.count];
        }

        const auto spectrogramScissor
            = makeScissorRect(dashboardLayout[DashboardPanel::spectrogram], boundsSize,
                drawable.texture.width, drawable.texture.height);

        if (spectrogramScissor.width != 0 && spectrogramScissor.height != 0) {
            [encoder setScissorRect:spectrogramScissor];

            if (batches.spectrogramHistory.count >= 4 && spectrogramPipelineState != nil
                && spectrogramHistoryTexture != nil && spectrogramPaletteTexture != nil
                && spectrogramPaletteSamplerState != nil) {
                [encoder setRenderPipelineState:spectrogramPipelineState];
                [encoder setFragmentBuffer:slot.spectrogramUniformBuffer offset:0 atIndex:0];
                [encoder setFragmentBuffer:slot.spectrogramValidityBuffer offset:0 atIndex:1];
                [encoder setFragmentTexture:spectrogramHistoryTexture atIndex:0];
                [encoder setFragmentTexture:spectrogramPaletteTexture atIndex:1];
                [encoder setFragmentSamplerState:spectrogramPaletteSamplerState atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:batches.spectrogramHistory.start
                            vertexCount:batches.spectrogramHistory.count];
                [encoder setRenderPipelineState:pipelineState];
            }

            if (batches.spectrogramAxis.count != 0) {
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:batches.spectrogramAxis.start
                            vertexCount:batches.spectrogramAxis.count];
            }
        }

        const auto meterScissor = makeScissorRect(dashboardLayout[DashboardPanel::peakRms],
            boundsSize, drawable.texture.width, drawable.texture.height);

        if (meterScissor.width != 0 && meterScissor.height != 0 && batches.peakRms.count != 0) {
            [encoder setScissorRect:meterScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.peakRms.start
                        vertexCount:batches.peakRms.count];
        }

        const auto stereoPanelScissor
            = makeScissorRect(dashboardLayout[DashboardPanel::stereoField], boundsSize,
                drawable.texture.width, drawable.texture.height);
        if (stereoPanelScissor.width != 0 && stereoPanelScissor.height != 0
            && batches.stereoGuides.count != 0) {
            [encoder setScissorRect:stereoPanelScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.stereoGuides.start
                        vertexCount:batches.stereoGuides.count];
        }

        if (preparedStereoPoints.count != 0 && stereoPointPipelineState != nil) {
            const auto stereoScopeScissor = makeScissorRect(preparedStereoPoints.scopeBounds,
                boundsSize, drawable.texture.width, drawable.texture.height);
            if (stereoScopeScissor.width != 0 && stereoScopeScissor.height != 0) {
                [encoder setScissorRect:stereoScopeScissor];
                [encoder setRenderPipelineState:stereoPointPipelineState];
                [encoder setVertexBuffer:slot.stereoPointInstanceBuffer offset:0 atIndex:0];
                [encoder setVertexBuffer:slot.stereoPointUniformBuffer offset:0 atIndex:1];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                            vertexStart:0
                            vertexCount:4
                          instanceCount:preparedStereoPoints.count];
                telemetry->stereoPointDrawCalls.fetch_add(1, std::memory_order_relaxed);
                [encoder setRenderPipelineState:pipelineState];
                [encoder setVertexBuffer:slot.vertexBuffer offset:0 atIndex:0];
            }
        }

        const auto loudnessScissor = makeScissorRect(dashboardLayout[DashboardPanel::loudness],
            boundsSize, drawable.texture.width, drawable.texture.height);
        if (loudnessScissor.width != 0 && loudnessScissor.height != 0
            && batches.loudness.count != 0) {
            [encoder setScissorRect:loudnessScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.loudness.start
                        vertexCount:batches.loudness.count];
        }

        if (batches.dashboardSplitters.count != 0) {
            const MTLScissorRect fullScissor { 0, 0, drawable.texture.width,
                drawable.texture.height };
            [encoder setScissorRect:fullScissor];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:batches.dashboardSplitters.start
                        vertexCount:batches.dashboardSplitters.count];
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
        const auto commandContainsSpectrogramUploads
            = spectrogramUploadsEncoded && preparedSpectrogramUploads.count != 0;
        const auto commandSpectrogramUploadCount = preparedSpectrogramUploads.count;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
            const auto commandSucceeded = completedBuffer.status != MTLCommandBufferStatusError;
            if (commandContainsSpectrogramUploads)
                completionState->spectrogramUploadGate.completeUpload(commandSucceeded);

            // The GPU has finished reading this submission's shared vertex
            // buffer. Presentation may be delayed by several refresh periods,
            // so it must not retain renderer admission.
            releaseRenderBuffer(completionState, admission);

            const auto gpuStart = completedBuffer.GPUStartTime;
            const auto gpuEnd = completedBuffer.GPUEndTime;
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

            if (commandContainsSpectrogramUploads) {
                completionTelemetry->spectrogramColumnsUploaded.fetch_add(
                    commandSpectrogramUploadCount, std::memory_order_relaxed);
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
        if (commandContainsSpectrogramUploads)
            sharedState->spectrogramUploadGate.beginUpload();
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
    void invalidateIntegratedLoudnessForReset() noexcept
    {
        assertMessageThread();
        targetFrame.loudnessIntegratedValid = false;
        loadPublishedTelemetry()->loudnessIntegratedValid.store(false, std::memory_order_relaxed);

        if (view != nil)
            NSAccessibilityPostNotification(view, NSAccessibilityValueChangedNotification);
    }

    VisualizationDataSource& source;
    AIAudioInsightMetalView* view = nil;
    std::shared_ptr<SharedRenderState> sharedState;
    std::shared_ptr<AtomicRenderTelemetry> callbackTelemetry;
    std::shared_ptr<AtomicRenderTelemetry> publishedTelemetry;
    std::shared_ptr<AtomicRenderTelemetry> pendingTelemetry;

    id<MTLCommandQueue> commandQueue = nil;
    id<MTLRenderPipelineState> pipelineState = nil;
    id<MTLRenderPipelineState> textPipelineState = nil;
    id<MTLRenderPipelineState> spectrogramPipelineState = nil;
    id<MTLRenderPipelineState> stereoPointPipelineState = nil;
    id<MTLSamplerState> glyphSamplerState = nil;
    id<MTLSamplerState> spectrogramPaletteSamplerState = nil;
    id<MTLTexture> glyphAtlasTexture = nil;
    id<MTLTexture> spectrogramPaletteTexture = nil;
    id<MTLTexture> spectrogramHistoryTexture = nil;
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
    std::atomic<std::uint32_t> packedSpectrogramSettings { 0 };
    std::atomic<std::uint32_t> packedLoudnessSettings { 0 };
    std::atomic<bool> spectrogramConfigurationClearPending { false };
    std::atomic<std::uint32_t> packedDashboardLayoutSplits { 0 };
    std::atomic<bool> dashboardLayoutEditing { false };
    std::atomic<std::uint32_t> focusedDashboardSplitterIndex { static_cast<std::uint32_t>(
        noDashboardSplitterIndex) };
    std::atomic<std::uint32_t> activeDashboardSplitterIndex { static_cast<std::uint32_t>(
        noDashboardSplitterIndex) };
    std::atomic<std::uint64_t> requestedTelemetryEpoch { 1 };

    std::atomic<bool> requestedActive { false };
    std::atomic<bool> juceShowing { false };
    std::atomic<bool> effectiveActive { false };
    std::atomic<bool> hasShutDown { false };
    MetalVisualization::EffectiveActivityCallback effectiveActivityCallback;
    MetalVisualization::DashboardLayoutEditCancelCallback dashboardLayoutEditCancelCallback;
    std::optional<DashboardSplitter> draggedDashboardSplitter;

    VisualizationFrame targetFrame;
    std::array<float, maximumSpectrumBinCount> displayedSpectrum { };
    std::array<float, maximumSpectrumBinCount> displayedSpectrumPeakHold { };
    std::uint64_t lastSpectrumSequence = 0;
    std::uint64_t lastMeterSequence = 0;
    std::uint64_t lastStereoSequence = 0;
    std::uint64_t lastLoudnessSequence = 0;
    std::uint64_t lastGeneration = 0;
    std::uint64_t lastFftGeneration = 0;
    bool hasDisplayFrame = false;

    SpectrogramHistoryRing spectrogramHistoryRing;
    std::optional<SpectrogramHistorySignature> spectrogramHistorySignature;
    std::uint32_t spectrogramTextureRowCount = 0;
    std::uint32_t spectrogramTextureColumnCount = 0;

    Clock::time_point previousInterpolationTime;
    Clock::time_point stereoSnapshotAcceptedTime;
    CFTimeInterval previousDisplayCallbackHostTime = 0.0;
    CFTimeInterval previousTargetTimestamp = 0.0;
    CFTimeInterval previousTargetPresentationTimestamp = 0.0;

    [[nodiscard]] DashboardLogicalBounds dashboardLogicalBounds() const noexcept
    {
        if (view == nil)
            return { };

        const auto size = view.bounds.size;
        return { 0.0, 0.0, std::max(0.0, static_cast<double>(size.width)),
            std::max(0.0, static_cast<double>(size.height)) };
    }

    [[nodiscard]] DashboardLogicalPoint dashboardPointFromNativePoint(
        const NSPoint point) const noexcept
    {
        if (view == nil)
            return { };

        const auto bounds = view.bounds;
        const auto height = std::max(0.0, static_cast<double>(bounds.size.height));
        const auto x = static_cast<double>(point.x - bounds.origin.x);
        const auto y = static_cast<double>(point.y - bounds.origin.y);
        return { x, view.isFlipped ? y : height - y };
    }

    void setFocusedDashboardSplitterIndex(const std::size_t nextIndex)
    {
        const auto boundedNext
            = nextIndex < dashboardSplitterCount ? nextIndex : noDashboardSplitterIndex;
        const auto previous = static_cast<std::size_t>(focusedDashboardSplitterIndex.exchange(
            static_cast<std::uint32_t>(boundedNext), std::memory_order_acq_rel));
        if (view != nil && previous != boundedNext)
            [view dashboardSplitterFocusChangedFromIndex:previous toIndex:boundedNext];
    }

    void publishDashboardLayoutEdit(
        const DashboardLayoutSplits& splits, const std::size_t splitterIndex)
    {
        const auto packed = packDashboardLayoutSplits(splits);
        if (packedDashboardLayoutSplits.exchange(packed, std::memory_order_acq_rel) == packed)
            return;

        if (view != nil) {
            [view dashboardLayoutGeometryChanged];
            if (splitterIndex < dashboardSplitterCount)
                [view dashboardSplitterValueChangedAtIndex:splitterIndex];
        }
    }

    void invokeDashboardLayoutEditCancelCallback() noexcept
    {
        try {
            const auto callback = dashboardLayoutEditCancelCallback;
            if (callback)
                callback();
        } catch (...) {
            // An editor interaction callback must not destabilize its host.
        }
    }

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
        id<MTLFunction> spectrogramFragmentFunction =
            [library newFunctionWithName:@"audioInsightSpectrogramFragment"];
        id<MTLFunction> stereoPointVertexFunction =
            [library newFunctionWithName:@"audioInsightStereoPointVertex"];
        id<MTLFunction> stereoPointFragmentFunction =
            [library newFunctionWithName:@"audioInsightStereoPointFragment"];

        if (vertexFunction == nil || fragmentFunction == nil || textFragmentFunction == nil
            || spectrogramFragmentFunction == nil || stereoPointVertexFunction == nil
            || stereoPointFragmentFunction == nil) {
            initializationError = "Metal could not load the visualization shader functions.";
            [vertexFunction release];
            [fragmentFunction release];
            [textFragmentFunction release];
            [spectrogramFragmentFunction release];
            [stereoPointVertexFunction release];
            [stereoPointFragmentFunction release];
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

        NSError* spectrogramPipelineError = nil;
        descriptor.label = @"Audio Insight Spectrogram pipeline";
        descriptor.fragmentFunction = spectrogramFragmentFunction;
        spectrogramPipelineState =
            [view.device newRenderPipelineStateWithDescriptor:descriptor
                                                        error:&spectrogramPipelineError];

        NSError* stereoPointPipelineError = nil;
        descriptor.label = @"Audio Insight Stereo field point pipeline";
        descriptor.vertexFunction = stereoPointVertexFunction;
        descriptor.fragmentFunction = stereoPointFragmentFunction;
        // Add each soft sprite's source-weighted colour to the field. Repeated
        // points therefore brighten into a bounded density impression instead
        // of repeatedly replacing the same translucent cyan.
        colourAttachment.destinationRGBBlendFactor = MTLBlendFactorOne;
        stereoPointPipelineState =
            [view.device newRenderPipelineStateWithDescriptor:descriptor
                                                        error:&stereoPointPipelineError];

        [descriptor release];
        [vertexFunction release];
        [fragmentFunction release];
        [textFragmentFunction release];
        [spectrogramFragmentFunction release];
        [stereoPointVertexFunction release];
        [stereoPointFragmentFunction release];
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

        if (spectrogramPipelineState == nil) {
            initializationError = spectrogramPipelineError != nil
                ? juce::String::fromUTF8(spectrogramPipelineError.localizedDescription.UTF8String)
                : juce::String("Metal could not create the Spectrogram pipeline.");
            return;
        }

        if (stereoPointPipelineState == nil) {
            initializationError = stereoPointPipelineError != nil
                ? juce::String::fromUTF8(stereoPointPipelineError.localizedDescription.UTF8String)
                : juce::String("Metal could not create the Stereo field point pipeline.");
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

        auto* spectrogramSamplerDescriptor = [[MTLSamplerDescriptor alloc] init];
        spectrogramSamplerDescriptor.label = @"Audio Insight Spectrogram palette sampler";
        spectrogramSamplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        spectrogramSamplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        spectrogramSamplerDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
        spectrogramSamplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        spectrogramSamplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        spectrogramPaletteSamplerState =
            [view.device newSamplerStateWithDescriptor:spectrogramSamplerDescriptor];
        [spectrogramSamplerDescriptor release];

        if (spectrogramPaletteSamplerState == nil) {
            initializationError = "Metal could not create the Spectrogram palette sampler.";
            return;
        }

        for (auto& slot : sharedState->slots) {
            slot.vertexBuffer =
                [view.device newBufferWithLength:maximumVertexCount * sizeof(MetalVertex)
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];

            slot.spectrogramStagingBuffer =
                [view.device newBufferWithLength:spectrogramStagingBufferBytes
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];
            slot.spectrogramValidityBuffer =
                [view.device newBufferWithLength:maximumSpectrogramHistoryColumnCount
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];
            slot.spectrogramUniformBuffer =
                [view.device newBufferWithLength:spectrogramUniformBufferBytes
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];
            slot.stereoPointInstanceBuffer = [view.device
                newBufferWithLength:maximumStereoFieldPointCount * sizeof(StereoPointInstance)
                            options:MTLResourceStorageModeShared |
                MTLResourceCPUCacheModeWriteCombined];
            slot.stereoPointUniformBuffer =
                [view.device newBufferWithLength:stereoPointUniformBufferBytes
                                         options:MTLResourceStorageModeShared |
                    MTLResourceCPUCacheModeWriteCombined];

            if (slot.vertexBuffer == nil || slot.spectrogramStagingBuffer == nil
                || slot.spectrogramValidityBuffer == nil || slot.spectrogramUniformBuffer == nil
                || slot.stereoPointInstanceBuffer == nil || slot.stereoPointUniformBuffer == nil) {
                initializationError = "Metal could not allocate the visualization buffers.";
                return;
            }
        }

        if (!buildSpectrogramPaletteTexture()) {
            initializationError = "Metal could not create the Spectrogram palettes.";
            return;
        }

        if (!rebuildGlyphAtlas(1.0)) {
            initializationError = "Metal could not create the initial glyph atlas.";
            return;
        }

        metalReady = true;
    }

    bool buildSpectrogramPaletteTexture()
    {
        assertMessageThread();
        if (view == nil || view.device == nil)
            return false;

        constexpr auto paletteWidth = std::size_t { 256 };
        constexpr auto paletteCount = std::size_t { 4 };
        constexpr auto componentCount = std::size_t { 4 };
        std::array<std::uint8_t, paletteWidth * paletteCount * componentCount> pixels { };

        for (std::size_t palette = 0; palette < paletteCount; ++palette) {
            for (std::size_t coordinate = 0; coordinate < paletteWidth; ++coordinate) {
                const auto colour
                    = spectrogramPaletteColour(static_cast<SpectrogramRenderPalette>(palette),
                        static_cast<float>(coordinate) / static_cast<float>(paletteWidth - 1));
                const auto offset = ((palette * paletteWidth) + coordinate) * componentCount;
                const auto toByte = [](const float component) noexcept {
                    return static_cast<std::uint8_t>(std::lround(
                        std::clamp(std::isfinite(component) ? component : 0.0F, 0.0F, 1.0F)
                        * 255.0F));
                };
                pixels[offset] = toByte(colour.red);
                pixels[offset + 1] = toByte(colour.green);
                pixels[offset + 2] = toByte(colour.blue);
                pixels[offset + 3] = 255;
            }
        }

        auto* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                               width:paletteWidth
                                                              height:paletteCount
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [view.device newTextureWithDescriptor:descriptor];
        if (texture == nil)
            return false;

        texture.label = @"Audio Insight Spectrogram palettes";
        [texture replaceRegion:MTLRegionMake2D(0, 0, paletteWidth, paletteCount)
                   mipmapLevel:0
                     withBytes:pixels.data()
                   bytesPerRow:paletteWidth * componentCount];
        [spectrogramPaletteTexture release];
        spectrogramPaletteTexture = texture;
        return true;
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
                : index == emDashGlyphAtlasIndex
                ? static_cast<unichar>(0x2014)
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
        juce::ignoreUnused(cachedPeakRmsReadoutTextRuns(), cachedLoudnessReadoutTextRuns(),
            cachedMinusInfinityTextRun(), cachedStereoCorrelationTextRuns(), cachedEmDashTextRun());

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
        destination.spectrogramTextureRows.store(
            sourceTelemetry.spectrogramTextureRows.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.spectrogramTextureColumns.store(
            sourceTelemetry.spectrogramTextureColumns.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.spectrogramTextureBytes.store(
            sourceTelemetry.spectrogramTextureBytes.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.stereoLastPointCount.store(
            sourceTelemetry.stereoLastPointCount.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.backingScale.store(sourceTelemetry.backingScale.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.stereoCorrelation.store(
            sourceTelemetry.stereoCorrelation.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessMeasurementCapturedFrameEnd.store(
            sourceTelemetry.loudnessMeasurementCapturedFrameEnd.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessIntegratedCapturedFrameEnd.store(
            sourceTelemetry.loudnessIntegratedCapturedFrameEnd.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessMomentaryLufs.store(
            sourceTelemetry.loudnessMomentaryLufs.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessShortTermLufs.store(
            sourceTelemetry.loudnessShortTermLufs.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessIntegratedLufs.store(
            sourceTelemetry.loudnessIntegratedLufs.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessReferenceLufs.store(
            sourceTelemetry.loudnessReferenceLufs.load(std::memory_order_relaxed),
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
        destination.stereoCorrelationValid.store(
            sourceTelemetry.stereoCorrelationValid.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.stereoMono.store(
            sourceTelemetry.stereoMono.load(std::memory_order_relaxed), std::memory_order_relaxed);
        destination.loudnessMomentaryValid.store(
            sourceTelemetry.loudnessMomentaryValid.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessShortTermValid.store(
            sourceTelemetry.loudnessShortTermValid.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        destination.loudnessIntegratedValid.store(
            sourceTelemetry.loudnessIntegratedValid.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    void resetTelemetryTimingAtCallbackBoundary() noexcept
    {
        previousDisplayCallbackHostTime = 0.0;
        previousTargetTimestamp = 0.0;
        previousTargetPresentationTimestamp = 0.0;
    }

    void clearSpectrogramHistory(AtomicRenderTelemetry* const telemetry) noexcept
    {
        spectrogramHistoryRing.clear();
        spectrogramHistorySignature.reset();
        if (telemetry != nullptr)
            telemetry->spectrogramHistoryClears.fetch_add(1, std::memory_order_relaxed);
    }

    bool ensureSpectrogramHistoryTexture(const std::uint32_t rowCount,
        const std::uint32_t columnCount, AtomicRenderTelemetry& telemetry) noexcept
    {
        if (rowCount == 0 || rowCount > maximumSpectrogramRowCount || columnCount == 0
            || columnCount > maximumSpectrogramHistoryColumnCount || view == nil
            || view.device == nil) {
            return false;
        }

        if (spectrogramHistoryTexture != nil && spectrogramTextureRowCount == rowCount
            && spectrogramTextureColumnCount == columnCount) {
            spectrogramHistoryRing.configure(columnCount);
            return true;
        }

        auto* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                               width:rowCount
                                                              height:columnCount
                                                           mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> replacement = [view.device newTextureWithDescriptor:descriptor];
        if (replacement == nil) {
            [spectrogramHistoryTexture release];
            spectrogramHistoryTexture = nil;
            spectrogramTextureRowCount = 0;
            spectrogramTextureColumnCount = 0;
            spectrogramHistoryRing.configure(0);
            telemetry.spectrogramTextureRows.store(0, std::memory_order_relaxed);
            telemetry.spectrogramTextureColumns.store(0, std::memory_order_relaxed);
            telemetry.spectrogramTextureBytes.store(0, std::memory_order_relaxed);
            telemetry.spectrogramTextureAllocationFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        replacement.label = @"Audio Insight circular Spectrogram dB history";
        [spectrogramHistoryTexture release];
        spectrogramHistoryTexture = replacement;
        spectrogramTextureRowCount = rowCount;
        spectrogramTextureColumnCount = columnCount;
        spectrogramHistoryRing.configure(columnCount);
        const auto bytes = static_cast<std::uint64_t>(rowCount) * columnCount * sizeof(_Float16);
        telemetry.spectrogramTextureRows.store(rowCount, std::memory_order_relaxed);
        telemetry.spectrogramTextureColumns.store(columnCount, std::memory_order_relaxed);
        telemetry.spectrogramTextureBytes.store(bytes, std::memory_order_relaxed);
        telemetry.spectrogramTextureReallocations.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void writeSpectrogramGpuState(
        RenderBufferSlot& slot, const SpectrogramRenderSettings& settings) const noexcept
    {
        std::memcpy(slot.spectrogramValidityBuffer.contents,
            spectrogramHistoryRing.validity().data(), maximumSpectrogramHistoryColumnCount);
        const SpectrogramShaderUniforms uniforms {
            spectrogramHistoryRing.columnCount(),
            spectrogramTextureRowCount,
            spectrogramHistoryRing.nextWriteColumn(),
            spectrogramHistoryRing.timelineSpan(),
            static_cast<std::uint32_t>(settings.historyMode),
            static_cast<std::uint32_t>(settings.palette),
            settings.colorFloorDecibels,
            settings.colorCeilingDecibels,
            settings.colorResponse,
            0,
        };
        std::memcpy(slot.spectrogramUniformBuffer.contents, &uniforms, sizeof(uniforms));
    }

    PreparedSpectrogramUploads prepareSpectrogramUploads(RenderBufferSlot& slot,
        const SpectrogramRenderSettings& settings, AtomicRenderTelemetry& telemetry) noexcept
    {
        PreparedSpectrogramUploads prepared;
        const auto pendingConfigurationClear
            = spectrogramConfigurationClearPending.exchange(false, std::memory_order_acq_rel);
        const auto previousUploadFailed = sharedState->spectrogramUploadGate.consumeFailure();
        if (pendingConfigurationClear || previousUploadFailed)
            clearSpectrogramHistory(&telemetry);

        SpectrogramColumn column;
        for (std::size_t drained = 0; drained < maximumSpectrogramColumnsDrainedPerFrame;
            ++drained) {
            if (!source.copyNextSpectrogramColumn(column))
                break;

            telemetry.spectrogramColumnsRead.fetch_add(1, std::memory_order_relaxed);
            if (column.resetMarker) {
                clearSpectrogramHistory(&telemetry);
                prepared.count = 0;
                telemetry.spectrogramLastColumnSequence.store(
                    column.sequence, std::memory_order_relaxed);
                continue;
            }

            const auto historyColumns = calculateSpectrogramHistoryColumnCount(
                settings.historyDurationSeconds, static_cast<int>(column.requestedSliceRateHz));
            const auto hasSupportedFftSize = column.fftSize == 1024 || column.fftSize == 2048
                || column.fftSize == 4096 || column.fftSize == 8192 || column.fftSize == 16384;
            const auto metadataIsValid = column.sequence != 0 && column.captureGeneration != 0
                && column.fftGeneration != 0 && column.mappingGeneration != 0
                && column.resetEpoch != 0 && column.capturedFrameEnd != 0
                && std::isfinite(column.sampleRate) && column.sampleRate > 0.0
                && hasSupportedFftSize && column.binCount == (column.fftSize / 2) + 1
                && column.binCount <= maximumSpectrumBinCount && column.rowCount != 0
                && column.rowCount <= maximumSpectrogramRowCount && historyColumns != 0;
            if (!metadataIsValid) {
                telemetry.spectrogramColumnsRejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const SpectrogramHistorySignature signature { column.captureGeneration,
                column.fftGeneration, column.mappingGeneration, column.resetEpoch, column.fftSize,
                column.rowCount, historyColumns, column.requestedSliceRateHz, column.sampleRate };
            const auto transition
                = spectrogramHistoryTransition(spectrogramHistorySignature, signature);
            if (!transition.clear && !spectrogramHistorySignature.has_value()) {
                telemetry.spectrogramColumnsRejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (transition.clear) {
                clearSpectrogramHistory(&telemetry);
                prepared.count = 0;
            }

            if (!ensureSpectrogramHistoryTexture(column.rowCount, historyColumns, telemetry)) {
                telemetry.spectrogramColumnsRejected.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            spectrogramHistorySignature = signature;
            const auto timelineSlot = calculateSpectrogramTimelineSlot(
                column.capturedFrameEnd, column.sampleRate, column.requestedSliceRateHz);
            const auto advance = spectrogramHistoryRing.append(timelineSlot, column.sequence);
            if (!advance.accepted) {
                telemetry.spectrogramColumnsRejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            telemetry.spectrogramGapColumns.fetch_add(
                advance.gapColumnCount, std::memory_order_relaxed);
            telemetry.spectrogramLastColumnSequence.store(
                column.sequence, std::memory_order_relaxed);
            auto* const stagingBytes
                = static_cast<std::byte*>(slot.spectrogramStagingBuffer.contents);
            const auto sourceOffset = prepared.count * spectrogramStagingRowStride;
            auto* const staging = reinterpret_cast<_Float16*>(stagingBytes + sourceOffset);
            for (std::size_t row = 0; row < column.rowCount; ++row) {
                const auto decibels = std::isfinite(column.decibels[row])
                    ? std::clamp(column.decibels[row], -65'504.0F, 65'504.0F)
                    : minimumSpectrumDecibels;
                staging[row] = static_cast<_Float16>(decibels);
            }

            prepared.uploads[prepared.count++] = { static_cast<NSUInteger>(sourceOffset),
                static_cast<NSUInteger>(advance.writeColumn),
                static_cast<NSUInteger>(column.rowCount) };
        }

        writeSpectrogramGpuState(slot, settings);
        return prepared;
    }

    bool encodeSpectrogramUploads(id<MTLCommandBuffer> commandBuffer, RenderBufferSlot& slot,
        const PreparedSpectrogramUploads& prepared, AtomicRenderTelemetry& telemetry) noexcept
    {
        if (prepared.count == 0 || commandBuffer == nil || spectrogramHistoryTexture == nil)
            return prepared.count == 0;

        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        if (blit == nil)
            return false;

        blit.label = @"Audio Insight Spectrogram column uploads";
        for (std::size_t index = 0; index < prepared.count; ++index) {
            const auto& upload = prepared.uploads[index];
            [blit copyFromBuffer:slot.spectrogramStagingBuffer
                       sourceOffset:upload.sourceOffset
                  sourceBytesPerRow:spectrogramStagingRowStride
                sourceBytesPerImage:spectrogramStagingRowStride
                         sourceSize:MTLSizeMake(upload.rowCount, 1, 1)
                          toTexture:spectrogramHistoryTexture
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, upload.destinationRow, 0)];
        }
        [blit endEncoding];
        telemetry.spectrogramUploadCommands.fetch_add(prepared.count, std::memory_order_relaxed);
        telemetry.spectrogramUploadBytes.fetch_add(
            prepared.count * spectrogramTextureRowCount * sizeof(_Float16),
            std::memory_order_relaxed);
        return true;
    }

    void resetRendererStateWhileDisplayLinkIsPaused() noexcept
    {
        targetFrame = { };
        displayedSpectrum.fill(minimumSpectrumDecibels);
        displayedSpectrumPeakHold.fill(minimumSpectrumDecibels);
        lastGeneration = 0;
        lastFftGeneration = 0;
        lastSpectrumSequence = 0;
        lastMeterSequence = 0;
        lastStereoSequence = 0;
        lastLoudnessSequence = 0;
        hasDisplayFrame = false;
        stereoSnapshotAcceptedTime = { };
        clearSpectrogramHistory(callbackTelemetry.get());
        spectrogramConfigurationClearPending.store(false, std::memory_order_relaxed);
        previousInterpolationTime = { };
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
        clearSpectrogramHistory(callbackTelemetry.get());
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

    void acceptSnapshot(const VisualizationFrame& incoming, const Clock::time_point acceptedTime,
        AtomicRenderTelemetry& telemetry)
    {
        const auto generationChanged = incoming.generation != lastGeneration;
        const auto fftGenerationChanged = incoming.fftGeneration != lastFftGeneration;
        const auto stereoChanged = !hasDisplayFrame || generationChanged
            || incoming.stereoSequence != lastStereoSequence;
        const auto loudnessChanged = !hasDisplayFrame || generationChanged
            || incoming.loudnessSequence != lastLoudnessSequence;
        const auto isNew = !hasDisplayFrame || generationChanged || fftGenerationChanged
            || incoming.spectrumSequence != lastSpectrumSequence
            || incoming.meterSequence != lastMeterSequence || stereoChanged || loudnessChanged;

        if (!isNew)
            return;

        targetFrame = incoming;
        lastGeneration = incoming.generation;
        lastFftGeneration = incoming.fftGeneration;
        lastSpectrumSequence = incoming.spectrumSequence;
        lastMeterSequence = incoming.meterSequence;
        lastStereoSequence = incoming.stereoSequence;
        lastLoudnessSequence = incoming.loudnessSequence;
        if (stereoChanged)
            stereoSnapshotAcceptedTime = acceptedTime;
        telemetry.framesWithNewSnapshot.fetch_add(1, std::memory_order_relaxed);
        telemetry.lastSpectrumSequence.store(lastSpectrumSequence, std::memory_order_relaxed);
        telemetry.lastStereoSequence.store(lastStereoSequence, std::memory_order_relaxed);
        telemetry.lastLoudnessSequence.store(lastLoudnessSequence, std::memory_order_relaxed);
        if (loudnessChanged) {
            telemetry.loudnessMeasurementCapturedFrameEnd.store(
                incoming.loudnessMeasurementCapturedFrameEnd, std::memory_order_relaxed);
            telemetry.loudnessIntegratedCapturedFrameEnd.store(
                incoming.loudnessIntegratedCapturedFrameEnd, std::memory_order_relaxed);
            telemetry.loudnessMomentaryLufs.store(
                incoming.loudnessMomentaryLufs, std::memory_order_relaxed);
            telemetry.loudnessShortTermLufs.store(
                incoming.loudnessShortTermLufs, std::memory_order_relaxed);
            telemetry.loudnessIntegratedLufs.store(
                incoming.loudnessIntegratedLufs, std::memory_order_relaxed);
            telemetry.loudnessMomentaryValid.store(
                incoming.loudnessMomentaryValid, std::memory_order_relaxed);
            telemetry.loudnessShortTermValid.store(
                incoming.loudnessShortTermValid, std::memory_order_relaxed);
            telemetry.loudnessIntegratedValid.store(
                incoming.loudnessIntegratedValid, std::memory_order_relaxed);
        }

        if (!hasDisplayFrame || generationChanged || fftGenerationChanged) {
            displayedSpectrum.fill(minimumSpectrumDecibels);
            displayedSpectrumPeakHold.fill(minimumSpectrumDecibels);
            const auto activeBinCount
                = std::min<std::size_t>(targetFrame.spectrumBinCount, displayedSpectrum.size());

            for (std::size_t index = 0; index < activeBinCount; ++index) {
                displayedSpectrum[index]
                    = sanitiseSpectrumAnalysisDecibels(targetFrame.spectrumDecibels[index]);
                displayedSpectrumPeakHold[index] = targetFrame.spectrumPeakHoldValid
                    ? sanitiseSpectrumAnalysisDecibels(targetFrame.spectrumPeakHoldDecibels[index])
                    : minimumSpectrumDecibels;
            }

            hasDisplayFrame = true;
        }
    }

    void updateInterpolatedDisplayValues(Clock::time_point now)
    {
        if (!hasDisplayFrame)
            return;

        const auto elapsed = previousInterpolationTime != Clock::time_point { }
            ? std::chrono::duration<double>(now - previousInterpolationTime).count()
            : 1.0 / 60.0;
        previousInterpolationTime = now;

        // This short display-domain interpolation makes 60 Hz FFT snapshots
        // move smoothly on a 120 Hz display without becoming a user-visible
        // second averaging stage.
        constexpr auto rendererInterpolationSeconds = 0.006;
        const auto coefficient = smoothingCoefficient(elapsed, rendererInterpolationSeconds);
        const auto activeBinCount
            = std::min<std::size_t>(targetFrame.spectrumBinCount, displayedSpectrum.size());
        for (std::size_t index = 0; index < activeBinCount; ++index) {
            const auto target
                = sanitiseSpectrumAnalysisDecibels(targetFrame.spectrumDecibels[index]);
            displayedSpectrum[index] += coefficient * (target - displayedSpectrum[index]);

            const auto heldTarget = targetFrame.spectrumPeakHoldValid
                ? sanitiseSpectrumAnalysisDecibels(targetFrame.spectrumPeakHoldDecibels[index])
                : minimumSpectrumDecibels;
            displayedSpectrumPeakHold[index]
                += coefficient * (heldTarget - displayedSpectrumPeakHold[index]);
        }
    }

    VertexBatches populateVertices(MetalVertex* vertices, CGSize logicalSize,
        const DashboardTileLayout& dashboardLayout, SpectrumRenderSettings settings,
        const SpectrogramRenderSettings& spectrogramSettings,
        const LoudnessRenderSettings& loudnessSettings) const noexcept
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
        const auto appendSpectrogramQuad = [&](const RenderRect& bounds) noexcept {
            if (bounds.width() <= 0.0F || bounds.height() <= 0.0F)
                return;

            jassert(cursor + 4 <= maximumVertexCount);
            if (cursor + 4 > maximumVertexCount)
                return;

            constexpr auto opaqueWhite = simd_float4 { 1.0F, 1.0F, 1.0F, 1.0F };
            vertices[cursor++] = { pointToClip(bounds.left, bounds.bottom), opaqueWhite,
                simd_make_float2(0.0F, 1.0F) };
            vertices[cursor++] = { pointToClip(bounds.right, bounds.bottom), opaqueWhite,
                simd_make_float2(1.0F, 1.0F) };
            vertices[cursor++] = { pointToClip(bounds.left, bounds.top), opaqueWhite,
                simd_make_float2(0.0F, 0.0F) };
            vertices[cursor++] = { pointToClip(bounds.right, bounds.top), opaqueWhite,
                simd_make_float2(1.0F, 0.0F) };
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
        const auto appendLine
            = [&](const float startX, const float startY, const float endX, const float endY,
                  const float thickness, const simd_float4 colour) noexcept {
                  const auto deltaX = endX - startX;
                  const auto deltaY = endY - startY;
                  const auto length = std::sqrt((deltaX * deltaX) + (deltaY * deltaY));
                  if (length <= 0.0001F || thickness <= 0.0F)
                      return;

                  const auto halfThickness = thickness * 0.5F;
                  const auto normalX = -deltaY * halfThickness / length;
                  const auto normalY = deltaX * halfThickness / length;
                  appendVertex(startX + normalX, startY + normalY, colour);
                  appendVertex(startX - normalX, startY - normalY, colour);
                  appendVertex(endX + normalX, endY + normalY, colour);
                  appendVertex(endX + normalX, endY + normalY, colour);
                  appendVertex(startX - normalX, startY - normalY, colour);
                  appendVertex(endX - normalX, endY - normalY, colour);
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

            if (panel == DashboardPanel::spectrum || panel == DashboardPanel::peakRms
                || panel == DashboardPanel::stereoField || panel == DashboardPanel::loudness)
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
        const auto spectrumClearLayout = calculateSpectrumClearLayout(
            spectrumPanel.width(), spectrumPanel.height(), headerHeight(spectrumPanel));
        const auto toSpectrumPanelRect = [&](const PeakRmsLogicalRect& bounds) noexcept {
            return RenderRect { spectrumPanel.left + bounds.left,
                spectrumPanel.bottom + bounds.bottom, spectrumPanel.left + bounds.right,
                spectrumPanel.bottom + bounds.top };
        };
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

        const auto traceRed = srgbComponentToLinear(
            static_cast<float>((settings.traceColourRgb >> 16U) & 0xffU) / 255.0F);
        const auto traceGreen = srgbComponentToLinear(
            static_cast<float>((settings.traceColourRgb >> 8U) & 0xffU) / 255.0F);
        const auto traceBlue
            = srgbComponentToLinear(static_cast<float>(settings.traceColourRgb & 0xffU) / 255.0F);
        const auto spectrumColour = simd_make_float4(traceRed, traceGreen, traceBlue, 1.0F);
        const auto spectrumFillColour
            = simd_make_float4(traceRed, traceGreen, traceBlue, settings.fillOpacity);
        const auto spectrumHeldColour = simd_make_float4(traceRed, traceGreen, traceBlue, 0.62F);
        std::array<simd_float2, maximumSpectrumBinCount> spectrumPoints;
        std::size_t pointCount = 0;
        auto binFrequency = 0.0F;
        auto firstBin = std::size_t { 0 };
        auto finalBin = std::size_t { 0 };
        const auto canRenderSpectrum = spectrumPlot.width() > 0.0F && spectrumPlot.height() > 0.0F
            && hasDisplayFrame && targetFrame.spectrumValid && targetFrame.sampleRate > 0.0
            && detail::hasSupportedSpectrumMetadata(targetFrame);

        if (canRenderSpectrum) {
            binFrequency = static_cast<float>(
                targetFrame.sampleRate / static_cast<double>(targetFrame.spectrumFftSize));
            firstBin = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::ceil(minimumSpectrumFrequency / binFrequency)));
            finalBin = std::min<std::size_t>(targetFrame.spectrumBinCount - 1,
                static_cast<std::size_t>(std::floor(maximumFrequency / binFrequency)));
        }

        const auto buildSpectrumPoints = [&](const auto& decibels) noexcept {
            pointCount = 0;
            if (!canRenderSpectrum || binFrequency <= 0.0F || firstBin > finalBin)
                return;

            for (auto bin = firstBin; bin <= finalBin && pointCount < spectrumPoints.size();
                ++bin) {
                const auto frequency
                    = std::max(minimumSpectrumFrequency, static_cast<float>(bin) * binFrequency);
                const auto compensatedDecibels = decibels[bin]
                    + spectrumSlopeCompensationDecibels(frequency, settings.slopeDecibelsPerOctave);
                spectrumPoints[pointCount++] = simd_make_float2(frequencyToX(frequency),
                    decibelsToY(
                        compensatedDecibels, settings.floorDecibels, settings.ceilingDecibels));
            }
        };
        const auto appendThickTrace
            = [&](const float halfWidth, const simd_float4 colour) noexcept {
                  for (std::size_t index = 0; index < pointCount; ++index) {
                      const auto previous = spectrumPoints[index == 0 ? index : index - 1];
                      const auto next = spectrumPoints[index + 1 < pointCount ? index + 1 : index];
                      const auto deltaX = next.x - previous.x;
                      const auto deltaY = next.y - previous.y;
                      const auto length = std::sqrt((deltaX * deltaX) + (deltaY * deltaY));
                      const auto inverseLength = length > 0.0001F ? 1.0F / length : 0.0F;
                      const auto normalX = -deltaY * inverseLength * halfWidth;
                      const auto normalY = deltaX * inverseLength * halfWidth;

                      appendVertex(spectrumPoints[index].x + normalX,
                          spectrumPoints[index].y + normalY, colour);
                      appendVertex(spectrumPoints[index].x - normalX,
                          spectrumPoints[index].y - normalY, colour);
                  }
              };

        buildSpectrumPoints(displayedSpectrum);
        batches.spectrumFill.start = cursor;
        if (settings.fillOpacity > 0.0F) {
            for (std::size_t index = 0; index < pointCount; ++index) {
                appendVertex(spectrumPoints[index].x, plotBottom, spectrumFillColour);
                appendVertex(spectrumPoints[index].x, spectrumPoints[index].y, spectrumFillColour);
            }
        }
        batches.spectrumFill.count = cursor - batches.spectrumFill.start;

        batches.spectrum.start = cursor;
        appendThickTrace(0.75F, spectrumColour);
        batches.spectrum.count = cursor - batches.spectrum.start;

        batches.spectrumHeld.start = cursor;
        if (canRenderSpectrum && targetFrame.spectrumPeakHoldValid) {
            buildSpectrumPoints(displayedSpectrumPeakHold);
            appendThickTrace(0.50F, spectrumHeldColour);
        }
        batches.spectrumHeld.count = cursor - batches.spectrumHeld.start;

        batches.spectrumControls.start = cursor;
        if (spectrumClearLayout.visualBounds.width() > 0.0F) {
            constexpr auto clearButtonColour = simd_float4 { 0.055F, 0.075F, 0.105F, 1.0F };
            constexpr auto clearButtonBorderColour = simd_float4 { 0.20F, 0.27F, 0.36F, 0.90F };
            const auto clearBounds = toSpectrumPanelRect(spectrumClearLayout.visualBounds);
            appendQuad(clearBounds.left, clearBounds.bottom, clearBounds.right, clearBounds.top,
                clearButtonColour);
            appendBorder(clearBounds, 1.0F, clearButtonBorderColour);
        }
        batches.spectrumControls.count = cursor - batches.spectrumControls.start;
        const auto spectrogramPanel
            = toRenderRect(dashboardLayout[DashboardPanel::spectrogram], height);
        const auto spectrogramPlot = insetRenderRect(spectrogramPanel,
            maximumFrequencyLabelWidth + 8.0F, 8.0F, 8.0F, headerHeight(spectrogramPanel) + 7.0F);
        const auto spectrogramFrequencyTicks = selectFrequencyAxisTicks(
            frequencyMapping, spectrogramPlot.height(), frequencyLabelHeights, axisTextHeight);
        constexpr auto spectrogramTickColour = simd_float4 { 0.19F, 0.25F, 0.33F, 0.78F };
        batches.spectrogramHistory.start = cursor;
        if (spectrogramHistoryTexture != nil && spectrogramHistoryRing.columnCount() != 0
            && spectrogramTextureRowCount != 0) {
            appendSpectrogramQuad(spectrogramPlot);
        }
        batches.spectrogramHistory.count = cursor - batches.spectrogramHistory.start;
        batches.spectrogramAxis.start = cursor;

        if (spectrogramPlot.width() > 0.0F && spectrogramPlot.height() > 0.0F) {
            if (spectrogramSettings.historyMode == SpectrogramRenderHistoryMode::overwrite
                && spectrogramHistoryRing.columnCount() != 0) {
                constexpr auto seamColour = simd_float4 { 0.26F, 0.34F, 0.43F, 0.46F };
                const auto seamX = spectrogramPlot.left
                    + (static_cast<float>(spectrogramHistoryRing.nextWriteColumn())
                          / static_cast<float>(spectrogramHistoryRing.columnCount()))
                        * spectrogramPlot.width();
                const auto physicalPixelWidth = spectrogramLogicalPixelWidth(density);
                const auto seamLeft
                    = std::clamp(seamX - (physicalPixelWidth * 0.5F), spectrogramPlot.left,
                        std::max(spectrogramPlot.left, spectrogramPlot.right - physicalPixelWidth));
                appendQuad(seamLeft, spectrogramPlot.bottom,
                    std::min(spectrogramPlot.right, seamLeft + physicalPixelWidth),
                    spectrogramPlot.top, seamColour);
            }

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
        batches.stereoGuides.start = cursor;

        const auto stereoPanel = toRenderRect(dashboardLayout[DashboardPanel::stereoField], height);
        constexpr auto stereoTextScale = 0.78F;
        const auto stereoTextHeight = cachedGlyphCellHeight * stereoTextScale;
        const auto stereoLayout = calculateStereoFieldPanelLayout(
            stereoPanel.width(), stereoPanel.height(), headerHeight(stereoPanel), stereoTextHeight);
        const auto toStereoPanelRect = [&](const PeakRmsLogicalRect& bounds) noexcept {
            return RenderRect { stereoPanel.left + bounds.left, stereoPanel.bottom + bounds.bottom,
                stereoPanel.left + bounds.right, stereoPanel.bottom + bounds.top };
        };
        const auto stereoScope = toStereoPanelRect(stereoLayout.scopeBounds);
        const auto correlationTrack = toStereoPanelRect(stereoLayout.correlationTrackBounds);
        const auto stereoMono = hasDisplayFrame && targetFrame.stereoMono;
        const auto correlationReadout
            = classifyStereoCorrelationReadout(targetFrame.stereoCorrelation,
                hasDisplayFrame && !stereoMono && targetFrame.stereoCorrelationValid);
        const auto correlationColour
            = [](const StereoCorrelationColourRange range, const float alpha) noexcept {
                  switch (range) {
                  case StereoCorrelationColourRange::cyan:
                      return simd_float4 { 0.18F, 0.88F, 0.86F, alpha };
                  case StereoCorrelationColourRange::amber:
                      return simd_float4 { 1.0F, 0.62F, 0.16F, alpha };
                  case StereoCorrelationColourRange::neutral:
                      return simd_float4 { 0.52F, 0.58F, 0.65F, alpha };
                  }

                  return simd_float4 { 0.52F, 0.58F, 0.65F, alpha };
              };

        if (stereoScope.width() > 0.0F && stereoScope.height() > 0.0F) {
            constexpr auto scopeAxisColour = simd_float4 { 0.25F, 0.31F, 0.39F, 0.54F };
            constexpr auto scopeBoundaryColour = simd_float4 { 0.30F, 0.38F, 0.47F, 0.68F };
            const auto centreX = (stereoScope.left + stereoScope.right) * 0.5F;
            const auto centreY = (stereoScope.bottom + stereoScope.top) * 0.5F;

            appendLine(
                stereoScope.left, centreY, stereoScope.right, centreY, 0.75F, scopeAxisColour);
            appendLine(
                centreX, stereoScope.bottom, centreX, stereoScope.top, 0.75F, scopeAxisColour);
            appendLine(
                centreX, stereoScope.top, stereoScope.right, centreY, 0.75F, scopeBoundaryColour);
            appendLine(stereoScope.right, centreY, centreX, stereoScope.bottom, 0.75F,
                scopeBoundaryColour);
            appendLine(
                centreX, stereoScope.bottom, stereoScope.left, centreY, 0.75F, scopeBoundaryColour);
            appendLine(
                stereoScope.left, centreY, centreX, stereoScope.top, 0.75F, scopeBoundaryColour);
        }

        if (correlationTrack.width() > 0.0F && correlationTrack.height() > 0.0F) {
            constexpr auto correlationTrackColour = simd_float4 { 0.055F, 0.075F, 0.105F, 1.0F };
            constexpr auto correlationTickColour = simd_float4 { 0.36F, 0.43F, 0.52F, 0.78F };
            constexpr std::array<float, 5> correlationTicks { -1.0F, -0.5F, 0.0F, 0.5F, 1.0F };
            appendQuad(correlationTrack.left, correlationTrack.bottom, correlationTrack.right,
                correlationTrack.top, correlationTrackColour);

            if (correlationTrack.width() >= 1.0F) {
                for (const auto tick : correlationTicks) {
                    const auto x = correlationTrack.left
                        + (mapStereoCorrelationToUnit(tick) * correlationTrack.width());
                    const auto tickLeft = std::clamp(
                        x - 0.5F, correlationTrack.left, correlationTrack.right - 1.0F);
                    appendQuad(tickLeft, correlationTrack.bottom,
                        std::min(correlationTrack.right, tickLeft + 1.0F), correlationTrack.top,
                        correlationTickColour);
                }
            }

            if (correlationReadout.available) {
                const auto centreX = correlationTrack.left
                    + (mapStereoCorrelationToUnit(0.0F) * correlationTrack.width());
                const auto valueX = correlationTrack.left
                    + (mapStereoCorrelationToUnit(targetFrame.stereoCorrelation)
                        * correlationTrack.width());
                const auto fillLeft = std::min(centreX, valueX);
                const auto fillRight = std::max(centreX, valueX);
                appendQuad(fillLeft, correlationTrack.bottom + 1.0F, fillRight,
                    correlationTrack.top - 1.0F,
                    correlationColour(correlationReadout.colourRange, 0.52F));
                const auto markerWidth = std::min(2.0F, correlationTrack.width());
                const auto markerLeft = std::clamp(valueX - (markerWidth * 0.5F),
                    correlationTrack.left, correlationTrack.right - markerWidth);
                appendQuad(markerLeft, correlationTrack.bottom, markerLeft + markerWidth,
                    correlationTrack.top, correlationColour(correlationReadout.colourRange, 1.0F));
            }
        }

        batches.stereoGuides.count = cursor - batches.stereoGuides.start;
        batches.loudness.start = cursor;

        const auto loudnessPanel = toRenderRect(dashboardLayout[DashboardPanel::loudness], height);
        constexpr auto loudnessTextScale = 0.78F;
        const auto loudnessTextHeight = cachedGlyphCellHeight * loudnessTextScale;
        const auto maximumLoudnessReadoutWidth
            = (((static_cast<float>(maximumLoudnessReadoutGlyphs) - 1.0F) * cachedGlyphAdvance)
                  + cachedGlyphCellWidth)
            * loudnessTextScale;
        const auto loudnessLayout
            = calculateLoudnessPanelLayout(loudnessPanel.width(), loudnessPanel.height(),
                headerHeight(loudnessPanel), loudnessTextHeight, maximumLoudnessReadoutWidth);
        const auto toLoudnessPanelRect = [&](const PeakRmsLogicalRect& bounds) noexcept {
            return RenderRect { loudnessPanel.left + bounds.left,
                loudnessPanel.bottom + bounds.bottom, loudnessPanel.left + bounds.right,
                loudnessPanel.bottom + bounds.top };
        };
        const auto loudnessTrack = toLoudnessPanelRect(loudnessLayout.trackBounds);
        const auto physicalPixelWidth = spectrogramLogicalPixelWidth(density);
        const auto pixelAlignedHorizontalLine = [&](const float centreY) noexcept {
            return std::round((centreY - (physicalPixelWidth * 0.5F)) * density) / density;
        };
        constexpr auto loudnessTrackColour = simd_float4 { 0.055F, 0.075F, 0.105F, 1.0F };
        constexpr auto loudnessTrackBorderColour = simd_float4 { 0.20F, 0.27F, 0.36F, 0.90F };
        constexpr auto loudnessMomentaryColour = simd_float4 { 0.10F, 0.48F, 0.62F, 0.88F };
        constexpr auto loudnessShortTermColour = simd_float4 { 0.24F, 0.88F, 0.86F, 1.0F };
        constexpr auto loudnessReferenceColour = simd_float4 { 1.0F, 0.62F, 0.16F, 0.96F };
        constexpr auto loudnessResetColour = simd_float4 { 0.055F, 0.075F, 0.105F, 1.0F };
        constexpr auto loudnessResetBorderColour = simd_float4 { 0.20F, 0.27F, 0.36F, 0.90F };

        if (loudnessTrack.width() > 0.0F && loudnessTrack.height() > 0.0F) {
            appendQuad(loudnessTrack.left, loudnessTrack.bottom, loudnessTrack.right,
                loudnessTrack.top, loudnessTrackColour);
            appendBorder(loudnessTrack, 1.0F, loudnessTrackBorderColour);

            const auto momentaryReadout = classifyLoudnessReadout(targetFrame.loudnessMomentaryLufs,
                hasDisplayFrame && targetFrame.loudnessMomentaryValid);
            if (momentaryReadout.kind == LoudnessReadout::Kind::lufsTenths
                && targetFrame.loudnessMomentaryLufs > loudnessMinimumLufs) {
                const auto fillTop = loudnessTrack.bottom
                    + (mapLoudnessLufsToUnit(static_cast<float>(targetFrame.loudnessMomentaryLufs))
                        * loudnessTrack.height());
                appendQuad(loudnessTrack.left + 1.0F, loudnessTrack.bottom + 1.0F,
                    loudnessTrack.right - 1.0F, fillTop, loudnessMomentaryColour);
            }

            const auto shortTermReadout = classifyLoudnessReadout(targetFrame.loudnessShortTermLufs,
                hasDisplayFrame && targetFrame.loudnessShortTermValid);
            if (shortTermReadout.kind == LoudnessReadout::Kind::lufsTenths) {
                const auto centreY = loudnessTrack.bottom
                    + (mapLoudnessLufsToUnit(static_cast<float>(targetFrame.loudnessShortTermLufs))
                        * loudnessTrack.height());
                const auto lineBottom = pixelAlignedHorizontalLine(centreY);
                appendQuad(loudnessTrack.left - 2.0F, lineBottom, loudnessTrack.right + 2.0F,
                    lineBottom + physicalPixelWidth, loudnessShortTermColour);
            }

            const auto referenceY = loudnessTrack.bottom
                + (mapLoudnessLufsToUnit(loudnessSettings.referenceLufs) * loudnessTrack.height());
            const auto referenceBottom = pixelAlignedHorizontalLine(referenceY);
            appendQuad(loudnessTrack.left - 3.0F, referenceBottom, loudnessTrack.right + 3.0F,
                referenceBottom + physicalPixelWidth, loudnessReferenceColour);
        }

        if (loudnessLayout.resetVisualBounds.width() > 0.0F) {
            const auto resetBounds = toLoudnessPanelRect(loudnessLayout.resetVisualBounds);
            appendQuad(resetBounds.left, resetBounds.bottom, resetBounds.right, resetBounds.top,
                loudnessResetColour);
            appendBorder(resetBounds, 1.0F, loudnessResetBorderColour);
        }

        batches.loudness.count = cursor - batches.loudness.start;
        batches.dashboardSplitters.start = cursor;

        if (dashboardLayoutEditing.load(std::memory_order_acquire)) {
            constexpr auto handleColour = simd_float4 { 0.24F, 0.55F, 0.70F, 0.72F };
            constexpr auto focusedHandleColour = simd_float4 { 0.30F, 0.82F, 0.96F, 0.96F };
            constexpr auto activeHandleColour = simd_float4 { 0.64F, 0.92F, 1.0F, 1.0F };
            const auto splitterLayout = DashboardLayout::calculateSplitterLayout(
                { 0.0, 0.0, static_cast<double>(logicalSize.width),
                    static_cast<double>(logicalSize.height) },
                getDashboardLayoutSplits());
            const auto focusedIndex = static_cast<std::size_t>(
                focusedDashboardSplitterIndex.load(std::memory_order_acquire));
            const auto activeIndex = static_cast<std::size_t>(
                activeDashboardSplitterIndex.load(std::memory_order_acquire));

            for (std::size_t index = 0; index < splitterLayout.splitters.size(); ++index) {
                const auto bounds
                    = toRenderRect(splitterLayout.splitters[index].visualBounds, height);
                const auto colour = index == activeIndex ? activeHandleColour
                    : index == focusedIndex              ? focusedHandleColour
                                                         : handleColour;
                appendQuad(bounds.left, bounds.bottom, bounds.right, bounds.top, colour);
            }
        }

        batches.dashboardSplitters.count = cursor - batches.dashboardSplitters.start;

        constexpr auto titleColour = simd_float4 { 0.72F, 0.79F, 0.88F, 0.94F };
        constexpr auto axisTextColour = simd_float4 { 0.54F, 0.62F, 0.72F, 0.90F };

        for (std::size_t index = 0; index < dashboardPanelCount; ++index) {
            const auto panel = static_cast<DashboardPanel>(index);
            const auto bounds = toRenderRect(dashboardLayout[panel], height);
            const auto panelHeaderHeight = headerHeight(bounds);
            const auto& titleRun = cachedFixedTextRuns[panelTitleTextRunIndices[index]];
            auto titleAvailableWidth = std::max(0.0F, bounds.width() - 14.0F);
            if (panel == DashboardPanel::peakRms && meterLayout.clearVisualBounds.width() > 0.0F) {
                titleAvailableWidth = std::max(0.0F, meterLayout.clearVisualBounds.left - 12.0F);
            } else if (panel == DashboardPanel::spectrum
                && spectrumClearLayout.visualBounds.width() > 0.0F) {
                titleAvailableWidth = std::max(0.0F, spectrumClearLayout.visualBounds.left - 12.0F);
            } else if (panel == DashboardPanel::loudness
                && loudnessLayout.resetVisualBounds.width() > 0.0F) {
                titleAvailableWidth = std::max(0.0F, loudnessLayout.resetVisualBounds.left - 12.0F);
            }
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
                if (spectrumClearLayout.visualBounds.width() > 0.0F) {
                    constexpr auto clearTextColour = simd_float4 { 0.58F, 0.69F, 0.80F, 0.96F };
                    const auto clearBounds = toSpectrumPanelRect(spectrumClearLayout.visualBounds);
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
            } else if (panel == DashboardPanel::stereoField) {
                constexpr auto monoTextColour = simd_float4 { 0.64F, 0.76F, 0.84F, 0.94F };

                if (stereoLayout.showCorrelationReadout) {
                    const auto& readoutRun = correlationReadout.available
                        ? cachedStereoCorrelationTextRuns()[static_cast<std::size_t>(
                              correlationReadout.hundredths + 100)]
                        : cachedEmDashTextRun();
                    const auto readoutWidth = monospacedTextRunWidth(readoutRun) * stereoTextScale;
                    appendMonospacedTextRun(readoutRun,
                        stereoPanel.left + ((stereoPanel.width() - readoutWidth) * 0.5F),
                        stereoPanel.bottom + stereoLayout.correlationReadoutBottom, stereoTextScale,
                        correlationColour(correlationReadout.colourRange, 0.96F));
                }

                if (stereoMono && stereoScope.width() > 0.0F && stereoScope.height() > 0.0F) {
                    const auto& monoRun = cachedFixedTextRuns[monoStateTextRunIndex];
                    const auto monoScale = std::min(
                        0.72F, std::max(0.0F, (stereoScope.width() - 4.0F) / monoRun.width));
                    const auto monoWidth = monoRun.width * monoScale;
                    const auto monoHeight = monoRun.height * monoScale;
                    appendTextRun(monoRun,
                        stereoScope.left + ((stereoScope.width() - monoWidth) * 0.5F),
                        std::max(stereoScope.bottom + 2.0F, stereoScope.top - monoHeight - 2.0F),
                        monoScale, monoTextColour);
                }
            } else if (panel == DashboardPanel::loudness) {
                constexpr auto resetTextColour = simd_float4 { 0.58F, 0.69F, 0.80F, 0.96F };
                constexpr auto momentaryTextColour = simd_float4 { 0.24F, 0.88F, 0.86F, 0.98F };
                constexpr auto secondaryTextColour = simd_float4 { 0.67F, 0.75F, 0.84F, 0.94F };

                if (loudnessLayout.resetVisualBounds.width() > 0.0F) {
                    const auto resetBounds = toLoudnessPanelRect(loudnessLayout.resetVisualBounds);
                    const auto& resetRun = cachedFixedTextRuns[loudnessResetTextRunIndex];
                    const auto resetScale = std::min(0.78F,
                        std::min(resetBounds.width() / resetRun.width,
                            resetBounds.height() / resetRun.height));
                    appendTextRun(resetRun,
                        resetBounds.left
                            + ((resetBounds.width() - (resetRun.width * resetScale)) * 0.5F),
                        resetBounds.bottom
                            + ((resetBounds.height() - (resetRun.height * resetScale)) * 0.5F),
                        resetScale, resetTextColour);
                }

                const auto loudnessReadoutRun =
                    [](const LoudnessReadout& readout) noexcept -> const CachedMonospacedTextRun& {
                    switch (readout.kind) {
                    case LoudnessReadout::Kind::minusInfinity:
                        return cachedMinusInfinityTextRun();
                    case LoudnessReadout::Kind::lufsTenths:
                        return cachedLoudnessReadoutTextRuns()[static_cast<std::size_t>(
                            readout.lufsTenths - minimumCachedLoudnessReadoutTenths)];
                    case LoudnessReadout::Kind::emDash:
                        return cachedEmDashTextRun();
                    }

                    return cachedEmDashTextRun();
                };
                const auto appendLoudnessReadout = [&](const std::size_t labelIndex,
                                                       const LoudnessReadout& readout,
                                                       const PeakRmsLogicalRect& localBounds,
                                                       const simd_float4 colour) noexcept {
                    if (localBounds.width() <= 0.0F || localBounds.height() <= 0.0F)
                        return;

                    const auto row = toLoudnessPanelRect(localBounds);
                    const auto& labelRun = cachedFixedTextRuns[labelIndex];
                    const auto& valueRun = loudnessReadoutRun(readout);
                    const auto valueUnscaledWidth = monospacedTextRunWidth(valueRun);
                    constexpr auto unscaledGap = 4.0F / loudnessTextScale;
                    const auto unscaledWidth = labelRun.width + unscaledGap + valueUnscaledWidth;
                    const auto rowScale = std::min(loudnessTextScale,
                        unscaledWidth > 0.0F ? row.width() / unscaledWidth : 0.0F);
                    if (rowScale <= 0.0F)
                        return;

                    const auto gap = unscaledGap * rowScale;
                    const auto labelWidth = labelRun.width * rowScale;
                    const auto valueWidth = valueUnscaledWidth * rowScale;
                    const auto groupWidth = labelWidth + gap + valueWidth;
                    const auto originX = row.left + ((row.width() - groupWidth) * 0.5F);
                    const auto textHeight = cachedGlyphCellHeight * rowScale;
                    const auto originY
                        = row.bottom + std::max(0.0F, (row.height() - textHeight) * 0.5F);
                    appendTextRun(labelRun, originX, originY, rowScale, colour);
                    appendMonospacedTextRun(
                        valueRun, originX + labelWidth + gap, originY, rowScale, colour);
                };

                if (loudnessLayout.showMomentaryText) {
                    appendLoudnessReadout(monoChannelTextRunIndex,
                        classifyLoudnessReadout(targetFrame.loudnessMomentaryLufs,
                            hasDisplayFrame && targetFrame.loudnessMomentaryValid),
                        loudnessLayout.momentaryTextBounds, momentaryTextColour);
                }

                if (loudnessLayout.showSecondaryText) {
                    appendLoudnessReadout(shortTermTextRunIndex,
                        classifyLoudnessReadout(targetFrame.loudnessShortTermLufs,
                            hasDisplayFrame && targetFrame.loudnessShortTermValid),
                        loudnessLayout.shortTermTextBounds, secondaryTextColour);
                    appendLoudnessReadout(integratedTextRunIndex,
                        classifyLoudnessReadout(targetFrame.loudnessIntegratedLufs,
                            hasDisplayFrame && targetFrame.loudnessIntegratedValid),
                        loudnessLayout.integratedTextBounds, secondaryTextColour);
                }
            }

            batches.text[index].count = cursor - batches.text[index].start;
        }

        return batches;
    }
};
} // namespace audio_insight::detail

namespace {
NSString* makeDashboardAccessibilityString(const std::string_view value)
{
    if (value.empty())
        return @"";

    return [[[NSString alloc] initWithBytes:value.data()
                                     length:value.size()
                                   encoding:NSUTF8StringEncoding] autorelease];
}

NSString* makeDashboardAccessibilityString(const juce::String& value)
{
    return value.isEmpty() ? @"" : [NSString stringWithUTF8String:value.toRawUTF8()];
}
} // namespace

@implementation AIAudioInsightDashboardSplitterAccessibilityElement

- (instancetype)initWithOwnerView:(AIAudioInsightMetalView*)view splitterIndex:(NSUInteger)index
{
    self = [super init];

    if (self != nil) {
        ownerView = view;
        splitterIndex = index;
    }

    return self;
}

- (void)detachOwnerView
{
    ownerView = nil;
}

- (BOOL)isAccessibilityElement
{
    return ownerView != nil && [ownerView isDashboardLayoutEditing];
}

- (BOOL)isAccessibilityEnabled
{
    return [self isAccessibilityElement];
}

- (NSAccessibilityRole)accessibilityRole
{
    return NSAccessibilitySplitterRole;
}

- (NSString*)accessibilityLabel
{
    return ownerView != nil ? [ownerView dashboardSplitterAccessibilityLabelAtIndex:splitterIndex]
                            : nil;
}

- (NSString*)accessibilityIdentifier
{
    switch (splitterIndex) {
    case 0:
        return @"dashboardSplitter.horizontal";
    case 1:
        return @"dashboardSplitter.upper";
    case 2:
        return @"dashboardSplitter.lowerLeft";
    case 3:
        return @"dashboardSplitter.lowerRight";
    default:
        return nil;
    }
}

- (NSString*)accessibilityHelp
{
    return @"Use the arrow keys, Home, or End to resize the adjacent analyzer panels.";
}

- (id)accessibilityParent
{
    return ownerView;
}

- (id)accessibilityWindow
{
    return ownerView.window;
}

- (id)accessibilityTopLevelUIElement
{
    return ownerView.window;
}

- (NSRect)accessibilityFrame
{
    return ownerView != nil ? [ownerView dashboardSplitterAccessibilityFrameAtIndex:splitterIndex]
                            : NSZeroRect;
}

- (BOOL)isAccessibilityFocused
{
    return ownerView != nil && [ownerView isDashboardSplitterFocusedAtIndex:splitterIndex];
}

- (void)setAccessibilityFocused:(BOOL)focused
{
    if (focused && ownerView != nil)
        [ownerView focusDashboardSplitterAtIndex:splitterIndex];
}

- (id)accessibilityValue
{
    return ownerView != nil ? [ownerView dashboardSplitterAccessibilityValueAtIndex:splitterIndex]
                            : nil;
}

- (NSString*)accessibilityValueDescription
{
    return ownerView != nil
        ? [ownerView dashboardSplitterAccessibilityValueDescriptionAtIndex:splitterIndex]
        : nil;
}

- (id)accessibilityMinValue
{
    return ownerView != nil ? [ownerView dashboardSplitterAccessibilityMinimumAtIndex:splitterIndex]
                            : nil;
}

- (id)accessibilityMaxValue
{
    return ownerView != nil ? [ownerView dashboardSplitterAccessibilityMaximumAtIndex:splitterIndex]
                            : nil;
}

- (NSAccessibilityOrientation)accessibilityOrientation
{
    return ownerView != nil
        ? [ownerView dashboardSplitterAccessibilityOrientationAtIndex:splitterIndex]
        : NSAccessibilityOrientationUnknown;
}

- (BOOL)accessibilityPerformIncrement
{
    return ownerView != nil && [ownerView incrementDashboardSplitterAtIndex:splitterIndex];
}

- (BOOL)accessibilityPerformDecrement
{
    return ownerView != nil && [ownerView decrementDashboardSplitterAtIndex:splitterIndex];
}

@end

@implementation AIAudioInsightMetalView

- (void)attachRenderBackend:(audio_insight::detail::MetalRenderBackend*)backend
{
    renderBackend = backend;
    self.delegate = nil;
    self.paused = YES;
    self.accessibilityElement = YES;
    self.accessibilityRole = NSAccessibilityGroupRole;
    self.accessibilityLabel = @"Audio Insight analyzer dashboard";
    self.accessibilityHelp = @"Contains Spectrum, Peak/RMS, Spectrogram, and Stereo field with "
                             @"correlation visualizations, plus live Momentary, Short-term, and "
                             @"Integrated Loudness.";
    auto* clearAction = [[NSAccessibilityCustomAction alloc]
        initWithName:@"Clear Peak/RMS holds and OVER"
              target:self
            selector:@selector(performPeakRmsClearAccessibilityAction)];
    auto* spectrumClearAction = [[NSAccessibilityCustomAction alloc]
        initWithName:@"Clear Spectrum averaging and peak holds"
              target:self
            selector:@selector(performSpectrumClearAccessibilityAction)];
    auto* loudnessResetAction = [[NSAccessibilityCustomAction alloc]
        initWithName:@"Reset loudness integration"
              target:self
            selector:@selector(performLoudnessResetAccessibilityAction)];
    self.accessibilityCustomActions = @[ spectrumClearAction, clearAction, loudnessResetAction ];
    [loudnessResetAction release];
    [spectrumClearAction release];
    [clearAction release];

    auto* accessibilityElements =
        [[NSMutableArray alloc] initWithCapacity:audio_insight::dashboardSplitterCount];
    for (NSUInteger index = 0; index < audio_insight::dashboardSplitterCount; ++index) {
        auto* element =
            [[AIAudioInsightDashboardSplitterAccessibilityElement alloc] initWithOwnerView:self
                                                                             splitterIndex:index];
        [accessibilityElements addObject:element];
        [element release];
    }
    dashboardSplitterAccessibilityElements = accessibilityElements;

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

    for (AIAudioInsightDashboardSplitterAccessibilityElement* element in
             dashboardSplitterAccessibilityElements) {
        [element detachOwnerView];
    }
    [dashboardSplitterAccessibilityElements release];
    dashboardSplitterAccessibilityElements = nil;

    observedWindow = nil;
    renderBackend = nullptr;
}

- (void)dealloc
{
    [self detachRenderBackend];
    [super dealloc];
}

- (BOOL)acceptsFirstResponder
{
    return [self isDashboardLayoutEditing];
}

- (void)mouseDown:(NSEvent*)event
{
    if (renderBackend != nullptr) {
        const auto point = [self convertPoint:event.locationInWindow fromView:nil];

        if (renderBackend->isDashboardLayoutEditing()) {
            static_cast<void>(renderBackend->beginDashboardSplitterDrag(point));
            return;
        }

        if (renderBackend->tryClearSpectrumAt(point) || renderBackend->tryClearPeakRmsAt(point)
            || renderBackend->tryResetLoudnessAt(point)) {
            return;
        }
    }

    [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent*)event
{
    if (renderBackend != nullptr && renderBackend->isDashboardLayoutEditing()) {
        const auto point = [self convertPoint:event.locationInWindow fromView:nil];
        static_cast<void>(renderBackend->dragDashboardSplitter(point));
        return;
    }

    [super mouseDragged:event];
}

- (void)mouseUp:(NSEvent*)event
{
    if (renderBackend != nullptr && renderBackend->isDashboardLayoutEditing()) {
        const auto point = [self convertPoint:event.locationInWindow fromView:nil];
        static_cast<void>(renderBackend->endDashboardSplitterDrag(point));
        return;
    }

    [super mouseUp:event];
}

- (void)keyDown:(NSEvent*)event
{
    if (renderBackend != nullptr && renderBackend->handleDashboardLayoutKeyDown(event))
        return;

    [super keyDown:event];
}

- (void)resetCursorRects
{
    [super resetCursorRects];

    if (![self isDashboardLayoutEditing])
        return;

    for (NSUInteger index = 0; index < audio_insight::dashboardSplitterCount; ++index) {
        const auto bounds = [self dashboardSplitterLocalBoundsAtIndex:index];
        if (NSIsEmptyRect(bounds))
            continue;

        auto* cursor = [self dashboardSplitterAccessibilityOrientationAtIndex:index]
                == NSAccessibilityOrientationHorizontal
            ? [NSCursor resizeUpDownCursor]
            : [NSCursor resizeLeftRightCursor];
        [self addCursorRect:bounds cursor:cursor];
    }
}

- (NSArray*)accessibilityChildren
{
    return [self isDashboardLayoutEditing] ? dashboardSplitterAccessibilityElements : @[];
}

- (id)accessibilityValue
{
    return renderBackend != nullptr
        ? makeDashboardAccessibilityString(renderBackend->loudnessAccessibilityValue())
        : nil;
}

- (NSArray*)accessibilityVisibleChildren
{
    return [self accessibilityChildren];
}

- (NSArray<id<NSAccessibilityElement>>*)accessibilityChildrenInNavigationOrder
{
    return [self accessibilityChildren];
}

- (NSArray*)accessibilitySplitters
{
    return [self accessibilityChildren];
}

- (void)dashboardLayoutEditingStateChanged
{
    if (self.window != nil)
        [self.window invalidateCursorRectsForView:self];

    if (![self isDashboardLayoutEditing] && self.window.firstResponder == self)
        [self.window makeFirstResponder:nil];

    NSAccessibilityPostNotification(self, NSAccessibilityLayoutChangedNotification);
}

- (void)dashboardLayoutGeometryChanged
{
    if (![self isDashboardLayoutEditing])
        return;

    if (self.window != nil)
        [self.window invalidateCursorRectsForView:self];

    NSAccessibilityPostNotification(self, NSAccessibilityLayoutChangedNotification);
}

- (void)dashboardSplitterFocusChangedFromIndex:(NSUInteger)previousIndex
                                       toIndex:(NSUInteger)nextIndex
{
    juce::ignoreUnused(previousIndex);

    auto* focused = [self dashboardSplitterAccessibilityElementAtIndex:nextIndex];
    if (focused != nil)
        NSAccessibilityPostNotification(
            focused, NSAccessibilityFocusedUIElementChangedNotification);
}

- (void)dashboardSplitterValueChangedAtIndex:(NSUInteger)index
{
    auto* element = [self dashboardSplitterAccessibilityElementAtIndex:index];
    if (element != nil)
        NSAccessibilityPostNotification(element, NSAccessibilityValueChangedNotification);
}

- (BOOL)isDashboardLayoutEditing
{
    return renderBackend != nullptr && renderBackend->isDashboardLayoutEditing();
}

- (BOOL)isDashboardSplitterFocusedAtIndex:(NSUInteger)index
{
    return renderBackend != nullptr && index < audio_insight::dashboardSplitterCount
        && renderBackend->isDashboardSplitterFocused(index);
}

- (BOOL)focusDashboardSplitterAtIndex:(NSUInteger)index
{
    return renderBackend != nullptr && index < audio_insight::dashboardSplitterCount
        && renderBackend->focusDashboardSplitter(index);
}

- (BOOL)incrementDashboardSplitterAtIndex:(NSUInteger)index
{
    return renderBackend != nullptr && index < audio_insight::dashboardSplitterCount
        && renderBackend->adjustDashboardSplitter(index, 1);
}

- (BOOL)decrementDashboardSplitterAtIndex:(NSUInteger)index
{
    return renderBackend != nullptr && index < audio_insight::dashboardSplitterCount
        && renderBackend->adjustDashboardSplitter(index, -1);
}

- (NSRect)dashboardSplitterLocalBoundsAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return NSZeroRect;

    const auto logicalBounds = renderBackend->dashboardSplitterPointerHitBounds(index);
    const auto viewBounds = self.bounds;
    const auto y = self.isFlipped
        ? viewBounds.origin.y + logicalBounds.y
        : viewBounds.origin.y + viewBounds.size.height - logicalBounds.y - logicalBounds.height;
    return NSMakeRect(
        viewBounds.origin.x + logicalBounds.x, y, logicalBounds.width, logicalBounds.height);
}

- (NSRect)dashboardSplitterAccessibilityFrameAtIndex:(NSUInteger)index
{
    if (![self isDashboardLayoutEditing])
        return NSZeroRect;

    return NSAccessibilityFrameInView(self, [self dashboardSplitterLocalBoundsAtIndex:index]);
}

- (NSString*)dashboardSplitterAccessibilityLabelAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return nil;

    return makeDashboardAccessibilityString(
        renderBackend->dashboardSplitterAccessibilityValue(index).name);
}

- (NSString*)dashboardSplitterAccessibilityValueDescriptionAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return nil;

    const auto value = renderBackend->dashboardSplitterAccessibilityValue(index);
    auto* firstRegionName = makeDashboardAccessibilityString(value.firstRegionName);
    auto* secondRegionName = makeDashboardAccessibilityString(value.secondRegionName);
    return [NSString stringWithFormat:@"%@ %.1f%%, %@ %.1f%%", firstRegionName,
        value.firstRegionPercentage, secondRegionName, value.secondRegionPercentage];
}

- (NSNumber*)dashboardSplitterAccessibilityValueAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return nil;

    return [NSNumber numberWithInt:renderBackend->dashboardSplitterAccessibilityPosition(index)];
}

- (NSNumber*)dashboardSplitterAccessibilityMinimumAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return nil;

    return
        [NSNumber numberWithInt:renderBackend->dashboardSplitterAccessibilityRange(index).minimum];
}

- (NSNumber*)dashboardSplitterAccessibilityMaximumAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return nil;

    return
        [NSNumber numberWithInt:renderBackend->dashboardSplitterAccessibilityRange(index).maximum];
}

- (NSAccessibilityOrientation)dashboardSplitterAccessibilityOrientationAtIndex:(NSUInteger)index
{
    if (renderBackend == nullptr || index >= audio_insight::dashboardSplitterCount)
        return NSAccessibilityOrientationUnknown;

    return renderBackend->dashboardSplitterAxis(index)
            == audio_insight::DashboardSplitterAxis::horizontal
        ? NSAccessibilityOrientationHorizontal
        : NSAccessibilityOrientationVertical;
}

- (AIAudioInsightDashboardSplitterAccessibilityElement*)
    dashboardSplitterAccessibilityElementAtIndex:(NSUInteger)index
{
    if (index >= dashboardSplitterAccessibilityElements.count)
        return nil;

    return static_cast<AIAudioInsightDashboardSplitterAccessibilityElement*>(
        [dashboardSplitterAccessibilityElements objectAtIndex:index]);
}

- (BOOL)performPeakRmsClearAccessibilityAction
{
    return renderBackend != nullptr && renderBackend->performPeakRmsClearAction();
}

- (BOOL)performSpectrumClearAccessibilityAction
{
    return renderBackend != nullptr && renderBackend->performSpectrumClearAction();
}

- (BOOL)performLoudnessResetAccessibilityAction
{
    return renderBackend != nullptr && renderBackend->performLoudnessResetAction();
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

void MetalVisualization::setSpectrogramSettings(SpectrogramRenderSettings settings) noexcept
{
    impl->backend->setSpectrogramSettings(settings);
}

SpectrogramRenderSettings MetalVisualization::getSpectrogramSettings() const noexcept
{
    return impl->backend->getSpectrogramSettings();
}

void MetalVisualization::setLoudnessSettings(LoudnessRenderSettings settings) noexcept
{
    impl->backend->setLoudnessSettings(settings);
}

LoudnessRenderSettings MetalVisualization::getLoudnessSettings() const noexcept
{
    return impl->backend->getLoudnessSettings();
}

void MetalVisualization::setDashboardLayoutSplits(DashboardLayoutSplits splits) noexcept
{
    impl->backend->setDashboardLayoutSplits(splits);
}

DashboardLayoutSplits MetalVisualization::getDashboardLayoutSplits() const noexcept
{
    return impl->backend->getDashboardLayoutSplits();
}

void MetalVisualization::setDashboardLayoutEditing(const bool shouldEdit)
{
    impl->backend->setDashboardLayoutEditing(shouldEdit);
}

bool MetalVisualization::isDashboardLayoutEditing() const noexcept
{
    return impl->backend->isDashboardLayoutEditing();
}

void MetalVisualization::setDashboardLayoutEditCancelCallback(
    DashboardLayoutEditCancelCallback callback)
{
    impl->backend->setDashboardLayoutEditCancelCallback(std::move(callback));
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
