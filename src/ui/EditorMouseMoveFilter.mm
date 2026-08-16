// SPDX-License-Identifier: AGPL-3.0-or-later

#include "EditorMouseMoveFilter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#import <AppKit/AppKit.h>

#include <utility>

namespace audio_insight {
bool detail::RedundantMouseMoveCoalescer::shouldForward(const bool isInEditorScope,
    const bool bypass, const void* const target, const std::uint64_t modifierFlags) noexcept
{
    if (!isInEditorScope || bypass || target == nullptr) {
        reset();
        return true;
    }

    const auto shouldForward
        = !hasPreviousMove_ || previousTarget_ != target || previousModifierFlags_ != modifierFlags;
    previousTarget_ = target;
    previousModifierFlags_ = modifierFlags;
    hasPreviousMove_ = true;
    return shouldForward;
}

void detail::RedundantMouseMoveCoalescer::reset() noexcept
{
    previousTarget_ = nullptr;
    previousModifierFlags_ = 0;
    hasPreviousMove_ = false;
}

class EditorMouseMoveFilter::Impl final {
public:
    Impl(juce::Component& editorToUse, BypassProvider bypassProviderToUse)
        : editor_(editorToUse), bypassProvider_(std::move(bypassProviderToUse))
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

        const auto mask = NSEventMaskMouseMoved | NSEventMaskMouseEntered | NSEventMaskMouseExited;
        monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                         handler:^NSEvent*(NSEvent* event) {
                                                             return handleEvent(event);
                                                         }];
        telemetry_.filterActive = monitor_ != nil;
    }

    ~Impl()
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

        if (monitor_ != nil) {
            [NSEvent removeMonitor:monitor_];
            monitor_ = nil;
        }

        resetTarget();
        telemetry_.filterActive = false;
    }

    void resetTarget() noexcept
    {
        coalescer_.reset();
        previousTarget_ = nullptr;
    }

    [[nodiscard]] EditorMouseMoveFilterTelemetry getTelemetry() const noexcept
    {
        return telemetry_;
    }

private:
    [[nodiscard]] NSView* nativeEditorView() const noexcept
    {
        const auto* peer = editor_.getPeer();
        return peer != nullptr ? static_cast<NSView*>(peer->getNativeHandle()) : nil;
    }

    [[nodiscard]] bool eventBelongsToEditor(
        NSEvent* const event, NSView* const editorView, NSPoint& localPoint) const noexcept
    {
        if (event == nil || editorView == nil || event.window == nil
            || event.window != editorView.window || [editorView isHiddenOrHasHiddenAncestor]) {
            return false;
        }

        localPoint = [editorView convertPoint:event.locationInWindow fromView:nil];
        if (!NSPointInRect(localPoint, editorView.bounds))
            return false;

        // Keep the filter inside the actual native subtree as well as its
        // rectangle. This avoids consuming host events if another sibling view
        // temporarily covers an embedded plugin editor.
        auto* const contentView = event.window.contentView;
        if (contentView == nil)
            return false;

        const auto contentPoint = [contentView convertPoint:event.locationInWindow fromView:nil];
        auto* const hitView = [contentView hitTest:contentPoint];
        return hitView == editorView || (hitView != nil && [hitView isDescendantOf:editorView]);
    }

    NSEvent* handleEvent(NSEvent* const event)
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

        if (event == nil)
            return event;

        if (event.type == NSEventTypeMouseEntered || event.type == NSEventTypeMouseExited) {
            resetTarget();
            return event;
        }

        if (event.type != NSEventTypeMouseMoved)
            return event;

        NSPoint localPoint { };
        auto* const editorView = nativeEditorView();
        const auto isInEditorScope = eventBelongsToEditor(event, editorView, localPoint);
        if (!isInEditorScope) {
            resetTarget();
            return event;
        }

        ++telemetry_.mouseMovedEvents;

        const auto layoutEditBypass = bypassProvider_ && bypassProvider_();
        const auto pressedButtonBypass = NSEvent.pressedMouseButtons != 0;
        const auto tabletBypass = event.subtype == NSEventSubtypeTabletPoint
            || event.subtype == NSEventSubtypeTabletProximity;
        const auto bypass = layoutEditBypass || pressedButtonBypass || tabletBypass;

        if (layoutEditBypass)
            ++telemetry_.layoutEditBypassedMouseMovedEvents;

        if (previousTarget_.getComponent() == nullptr)
            coalescer_.reset();

        auto* const target = editor_.getComponentAt(juce::Point<float> {
            static_cast<float>(localPoint.x), static_cast<float>(localPoint.y) });
        const auto shouldForward = coalescer_.shouldForward(
            true, bypass, target, static_cast<std::uint64_t>(event.modifierFlags));

        if (bypass || target == nullptr)
            previousTarget_ = nullptr;
        else if (previousTarget_.getComponent() != target)
            previousTarget_ = target;

        if (shouldForward) {
            ++telemetry_.forwardedMouseMovedEvents;
            return event;
        }

        ++telemetry_.suppressedMouseMovedEvents;
        return nil;
    }

    juce::Component& editor_;
    BypassProvider bypassProvider_;
    detail::RedundantMouseMoveCoalescer coalescer_;
    juce::Component::SafePointer<juce::Component> previousTarget_;
    EditorMouseMoveFilterTelemetry telemetry_;
    id monitor_ = nil;
};

EditorMouseMoveFilter::EditorMouseMoveFilter(juce::Component& editor, BypassProvider bypassProvider)
    : impl_(std::make_unique<Impl>(editor, std::move(bypassProvider)))
{
}

EditorMouseMoveFilter::~EditorMouseMoveFilter() = default;

void EditorMouseMoveFilter::resetTarget() noexcept
{
    impl_->resetTarget();
}

EditorMouseMoveFilterTelemetry EditorMouseMoveFilter::getTelemetry() const noexcept
{
    return impl_->getTelemetry();
}
} // namespace audio_insight
