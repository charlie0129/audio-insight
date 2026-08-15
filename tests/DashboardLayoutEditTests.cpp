// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DashboardLayoutEdit.h"

#include <juce_core/juce_core.h>

namespace audio_insight {
namespace {
class DashboardLayoutEditTests final : public juce::UnitTest {
public:
    DashboardLayoutEditTests() : UnitTest("Dashboard layout editing", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Invalid initial state falls back to compiled defaults");
        {
            const DashboardLayoutEdit edit { DashboardLayoutSplits { 0, 0, 0, 0 } };
            expect(edit.committedSplits() == DashboardLayout::defaultSplits);
            expect(edit.displayedSplits() == DashboardLayout::defaultSplits);
            expect(!edit.isEditing());
        }

        beginTest("Working changes remain transient until Done");
        {
            DashboardLayoutEdit edit;
            edit.begin();
            edit.moveSplitter(DashboardSplitter::upper, 30);

            expect(edit.isEditing());
            expectEquals(edit.displayedSplits().upper, 30);
            expect(edit.committedSplits() == DashboardLayout::defaultSplits);
            expect(edit.finish());
            expect(!edit.isEditing());
            expectEquals(edit.committedSplits().upper, 30);
        }

        beginTest("Cancel and Escape semantics restore the layout present at begin");
        {
            DashboardLayoutEdit edit { DashboardLayoutSplits { 20, 32, 24, 38 } };
            const auto before = edit.committedSplits();
            edit.begin();
            edit.moveSplitter(DashboardSplitter::horizontal, 26);
            edit.moveSplitter(DashboardSplitter::lowerRight, 42);

            expect(edit.displayedSplits() != before);
            expect(edit.cancel());
            expect(!edit.isEditing());
            expect(edit.displayedSplits() == before);
            expect(edit.committedSplits() == before);
        }

        beginTest("Reset changes only the working edit until Done");
        {
            constexpr DashboardLayoutSplits custom { 20, 32, 24, 38 };
            DashboardLayoutEdit edit { custom };
            edit.begin();
            edit.resetWorkingLayout();

            expect(edit.displayedSplits() == DashboardLayout::defaultSplits);
            expect(edit.committedSplits() == custom);
            expect(edit.cancel());
            expect(edit.committedSplits() == custom);

            edit.begin();
            edit.resetWorkingLayout();
            expect(edit.finish());
            expect(edit.committedSplits() == DashboardLayout::defaultSplits);
        }

        beginTest("Commands outside edit mode are inert");
        {
            DashboardLayoutEdit edit;
            edit.moveSplitter(DashboardSplitter::upper, 24);
            edit.resetWorkingLayout();
            expect(!edit.finish());
            expect(!edit.cancel());
            expect(edit.committedSplits() == DashboardLayout::defaultSplits);
        }

        beginTest("Done completes and must persist even when values are unchanged");
        {
            DashboardLayoutEdit edit;
            edit.begin();
            expect(edit.finish());
            expect(!edit.isEditing());
            expect(edit.committedSplits() == DashboardLayout::defaultSplits);
        }

        beginTest("Beginning an active edit does not replace its working value");
        {
            DashboardLayoutEdit edit;
            edit.begin();
            edit.moveSplitter(DashboardSplitter::lowerLeft, 20);
            edit.begin();
            expectEquals(edit.displayedSplits().lowerLeft, 20);
        }
    }
};

DashboardLayoutEditTests dashboardLayoutEditTests;
} // namespace
} // namespace audio_insight
