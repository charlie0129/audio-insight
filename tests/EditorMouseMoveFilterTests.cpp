// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/EditorMouseMoveFilter.h"

#include <juce_core/juce_core.h>

#include <limits>

namespace audio_insight {
namespace {
class EditorMouseMoveFilterTests final : public juce::UnitTest {
public:
    EditorMouseMoveFilterTests() : UnitTest("Editor mouse-move filter", "audio-insight")
    {
    }

    void runTest() override
    {
        int firstTarget = 0;
        int secondTarget = 0;

        beginTest("The first move is forwarded and redundant same-target moves are suppressed");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0, 1.01));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0, 1.02));
        }

        beginTest("A bounded heartbeat preserves same-target pointer updates");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0, 1.05));
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.07));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0, 1.08));
        }

        beginTest("A deepest-component transition is always forwarded");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(coalescer.shouldForward(true, false, &secondTarget, 0, 1.01));
            expect(!coalescer.shouldForward(true, false, &secondTarget, 0, 1.02));
        }

        beginTest("Modifier changes are preserved for the same target");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(coalescer.shouldForward(true, false, &firstTarget, 1, 1.01));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 1, 1.02));
        }

        beginTest("Outside-editor and layout-edit moves pass through and reset coalescing");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0, 1.01));
            expect(coalescer.shouldForward(false, false, &firstTarget, 0, 1.02));
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.03));
            expect(coalescer.shouldForward(true, true, &firstTarget, 0, 1.04));
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.05));
        }

        beginTest("A missing target is never consumed");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, nullptr, 0, 1.0));
            expect(coalescer.shouldForward(true, false, nullptr, 0, 1.01));
        }

        beginTest("Explicit reset makes the next same-target move observable");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0, 1.01));
            coalescer.reset();
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.02));
        }

        beginTest("Invalid or regressed timestamps are never consumed");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 1.0));
            expect(coalescer.shouldForward(true, false, &firstTarget, 0, 0.5));
            expect(coalescer.shouldForward(
                true, false, &firstTarget, 0, std::numeric_limits<double>::quiet_NaN()));
        }
    }
};

EditorMouseMoveFilterTests editorMouseMoveFilterTests;
} // namespace
} // namespace audio_insight
