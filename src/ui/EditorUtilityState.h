// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <optional>

namespace audio_insight {
enum class SettingsPresentation {
    closed,
    sideInspector,
    fullContent,
};

/**
    Pure state machine for the mutually exclusive Settings and Metrics UI.

    The persisted Metrics parameter remains the source of truth. Settings can
    temporarily hide a requested Metrics panel without mutating that parameter.
*/
class EditorUtilityState final {
public:
    static constexpr int settingsInspectorWidth = 360;
    static constexpr int minimumDashboardWidthBesideSettings = 720;
    static constexpr int minimumSideInspectorContentWidth
        = settingsInspectorWidth + minimumDashboardWidthBesideSettings;

    explicit EditorUtilityState(bool metricsRequested = false) noexcept;

    void setMetricsRequested(bool requested) noexcept;
    [[nodiscard]] bool isMetricsRequested() const noexcept;
    [[nodiscard]] bool isMetricsVisible() const noexcept;
    [[nodiscard]] bool isMetricsTemporarilyHidden() const noexcept;

    void openSettings() noexcept;
    void closeSettings() noexcept;
    void toggleSettings() noexcept;
    [[nodiscard]] bool isSettingsOpen() const noexcept;

    /**
        Handles the toolbar Metrics action.

        While Settings is open this is a switch-to action: Settings closes and
        Metrics becomes requested if needed. Otherwise it toggles the persisted
        request. A returned value must be written to the Metrics parameter;
        nullopt means the request was already correct and must not be toggled.
    */
    [[nodiscard]] std::optional<bool> pressMetrics() noexcept;

    [[nodiscard]] SettingsPresentation settingsPresentation(int contentWidth) const noexcept;
    [[nodiscard]] bool shouldRenderDashboard(int contentWidth) const noexcept;

private:
    bool metricsRequested = false;
    bool settingsOpen = false;
};
} // namespace audio_insight
