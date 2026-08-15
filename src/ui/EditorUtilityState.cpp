// SPDX-License-Identifier: AGPL-3.0-or-later

#include "EditorUtilityState.h"

namespace audio_insight {
EditorUtilityState::EditorUtilityState(const bool requested) noexcept : metricsRequested(requested)
{
}

void EditorUtilityState::setMetricsRequested(const bool requested) noexcept
{
    metricsRequested = requested;
}

bool EditorUtilityState::isMetricsRequested() const noexcept
{
    return metricsRequested;
}

bool EditorUtilityState::isMetricsVisible() const noexcept
{
    return metricsRequested && !settingsOpen;
}

bool EditorUtilityState::isMetricsTemporarilyHidden() const noexcept
{
    return metricsRequested && settingsOpen;
}

void EditorUtilityState::openSettings() noexcept
{
    settingsOpen = true;
}

void EditorUtilityState::closeSettings() noexcept
{
    settingsOpen = false;
}

void EditorUtilityState::toggleSettings() noexcept
{
    settingsOpen = !settingsOpen;
}

bool EditorUtilityState::isSettingsOpen() const noexcept
{
    return settingsOpen;
}

std::optional<bool> EditorUtilityState::pressMetrics() noexcept
{
    if (settingsOpen) {
        settingsOpen = false;

        if (metricsRequested)
            return std::nullopt;

        metricsRequested = true;
        return true;
    }

    metricsRequested = !metricsRequested;
    return metricsRequested;
}

SettingsPresentation EditorUtilityState::settingsPresentation(const int contentWidth) const noexcept
{
    if (!settingsOpen)
        return SettingsPresentation::closed;

    return contentWidth >= minimumSideInspectorContentWidth ? SettingsPresentation::sideInspector
                                                            : SettingsPresentation::fullContent;
}

bool EditorUtilityState::shouldRenderDashboard(const int contentWidth) const noexcept
{
    return settingsPresentation(contentWidth) != SettingsPresentation::fullContent;
}
} // namespace audio_insight
