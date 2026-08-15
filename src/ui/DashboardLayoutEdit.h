// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DashboardLayout.h"

namespace audio_insight {
/**
    Transactional state for one editor's constrained layout-edit session.

    The model has no persistence or UI dependencies. Callers publish working
    splits to the renderer while editing and persist committedSplits() only
    after finish() returns true.
*/
class DashboardLayoutEdit final {
public:
    explicit DashboardLayoutEdit(
        DashboardLayoutSplits initialSplits = DashboardLayout::defaultSplits) noexcept;

    [[nodiscard]] bool isEditing() const noexcept;
    [[nodiscard]] DashboardLayoutSplits displayedSplits() const noexcept;
    [[nodiscard]] DashboardLayoutSplits committedSplits() const noexcept;

    void begin() noexcept;
    void moveSplitter(DashboardSplitter splitter, int requestedGridIndex) noexcept;
    void resetWorkingLayout() noexcept;

    /** Commits the working layout and returns whether the committed value changed. */
    [[nodiscard]] bool finish() noexcept;

    /** Discards the working layout and returns whether an edit was cancelled. */
    [[nodiscard]] bool cancel() noexcept;

private:
    DashboardLayoutSplits committed;
    DashboardLayoutSplits working;
    bool editing = false;
};
} // namespace audio_insight
