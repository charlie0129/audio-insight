// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace juce {
class Component;
}

namespace audio_insight {
namespace detail {
/** Pure decision state for coalescing redundant same-target mouse moves. */
class RedundantMouseMoveCoalescer final {
public:
    [[nodiscard]] bool shouldForward(bool isInEditorScope, bool bypass, const void* target,
        std::uint64_t modifierFlags) noexcept;
    void reset() noexcept;

private:
    const void* previousTarget_ = nullptr;
    std::uint64_t previousModifierFlags_ = 0;
    bool hasPreviousMove_ = false;
};
} // namespace detail

/** Point-in-time counters for the editor's scoped AppKit mouse-move filter. */
struct EditorMouseMoveFilterTelemetry final {
    std::uint64_t mouseMovedEvents = 0;
    std::uint64_t forwardedMouseMovedEvents = 0;
    std::uint64_t suppressedMouseMovedEvents = 0;
    std::uint64_t layoutEditBypassedMouseMovedEvents = 0;
    bool filterActive = false;
};

/**
    Prevents redundant macOS mouse-move delivery from monopolising a plugin
    host's message thread while preserving JUCE target transitions.

    The native monitor is scoped to one editor and must be created and
    destroyed on the JUCE message thread.
*/
class EditorMouseMoveFilter final {
public:
    using BypassProvider = std::function<bool()>;

    EditorMouseMoveFilter(juce::Component& editor, BypassProvider bypassProvider);
    ~EditorMouseMoveFilter();

    void resetTarget() noexcept;
    [[nodiscard]] EditorMouseMoveFilterTelemetry getTelemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace audio_insight
