// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/EditorMouseMoveFilter.h"

#include <juce_core/juce_core.h>

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
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0));
        }

        beginTest("Same-target passive motion remains coalesced until state changes");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            for (auto move = 0; move < 1'000; ++move)
                expect(!coalescer.shouldForward(true, false, &firstTarget, 0));
        }

        beginTest("A deepest-component transition is always forwarded");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(coalescer.shouldForward(true, false, &secondTarget, 0));
            expect(!coalescer.shouldForward(true, false, &secondTarget, 0));
        }

        beginTest("Modifier changes are preserved for the same target");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(coalescer.shouldForward(true, false, &firstTarget, 1));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 1));
        }

        beginTest("Outside-editor and layout-edit moves pass through and reset coalescing");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(coalescer.shouldForward(false, false, &firstTarget, 0));
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(coalescer.shouldForward(true, true, &firstTarget, 0));
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
        }

        beginTest("A missing target is never consumed");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, nullptr, 0));
            expect(coalescer.shouldForward(true, false, nullptr, 0));
        }

        beginTest("Explicit reset makes the next same-target move observable");
        {
            detail::RedundantMouseMoveCoalescer coalescer;
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
            expect(!coalescer.shouldForward(true, false, &firstTarget, 0));
            coalescer.reset();
            expect(coalescer.shouldForward(true, false, &firstTarget, 0));
        }
    }
};

EditorMouseMoveFilterTests editorMouseMoveFilterTests;
} // namespace
} // namespace audio_insight
