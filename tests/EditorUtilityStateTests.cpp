// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/EditorUtilityState.h"

#include <juce_core/juce_core.h>

namespace audio_insight {
namespace {
class EditorUtilityStateTests final : public juce::UnitTest {
public:
    EditorUtilityStateTests() : UnitTest("Editor utility state", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Settings temporarily hides requested Metrics without changing its request");
        {
            EditorUtilityState state { true };
            expect(state.isMetricsVisible());

            state.openSettings();
            expect(state.isSettingsOpen());
            expect(state.isMetricsRequested());
            expect(state.isMetricsTemporarilyHidden());
            expect(!state.isMetricsVisible());

            state.closeSettings();
            expect(!state.isSettingsOpen());
            expect(state.isMetricsRequested());
            expect(state.isMetricsVisible());
        }

        beginTest("Closing Settings does not invent a Metrics request");
        {
            EditorUtilityState state;
            state.openSettings();
            expect(!state.isMetricsTemporarilyHidden());
            state.closeSettings();
            expect(!state.isMetricsRequested());
            expect(!state.isMetricsVisible());
        }

        beginTest("Metrics while Settings is open switches to an existing request");
        {
            EditorUtilityState state { true };
            state.openSettings();
            const auto parameterChange = state.pressMetrics();

            expect(!parameterChange.has_value());
            expect(!state.isSettingsOpen());
            expect(state.isMetricsRequested());
            expect(state.isMetricsVisible());
        }

        beginTest("Metrics while Settings is open enables a missing request");
        {
            EditorUtilityState state;
            state.openSettings();
            const auto parameterChange = state.pressMetrics();

            expect(parameterChange == std::optional<bool> { true });
            expect(!state.isSettingsOpen());
            expect(state.isMetricsVisible());

            const auto secondPress = state.pressMetrics();
            expect(secondPress == std::optional<bool> { false });
            expect(!state.isMetricsVisible());
        }

        beginTest("External Metrics parameter changes remain hidden only while Settings is open");
        {
            EditorUtilityState state;
            state.openSettings();
            state.setMetricsRequested(true);
            expect(state.isMetricsTemporarilyHidden());
            state.setMetricsRequested(false);
            expect(!state.isMetricsTemporarilyHidden());
            state.closeSettings();
            expect(!state.isMetricsVisible());
        }

        beginTest("Settings uses the exact accepted side-inspector width threshold");
        {
            EditorUtilityState state;
            expect(state.settingsPresentation(2000) == SettingsPresentation::closed);
            expect(state.shouldRenderDashboard(0));

            state.openSettings();
            expect(state.settingsPresentation(1079) == SettingsPresentation::fullContent);
            expect(!state.shouldRenderDashboard(1079));
            expect(state.settingsPresentation(1080) == SettingsPresentation::sideInspector);
            expect(state.shouldRenderDashboard(1080));
        }

        beginTest("Settings toolbar toggles do not alter the Metrics request");
        {
            EditorUtilityState state { true };
            state.toggleSettings();
            expect(state.isSettingsOpen());
            expect(state.isMetricsRequested());
            state.toggleSettings();
            expect(!state.isSettingsOpen());
            expect(state.isMetricsVisible());
        }
    }
};

EditorUtilityStateTests editorUtilityStateTests;
} // namespace
} // namespace audio_insight
