// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DashboardLayoutEdit.h"

namespace audio_insight {
DashboardLayoutEdit::DashboardLayoutEdit(const DashboardLayoutSplits initialSplits) noexcept
    : committed(DashboardLayout::validOrDefault(initialSplits)), working(committed)
{
}

bool DashboardLayoutEdit::isEditing() const noexcept
{
    return editing;
}

DashboardLayoutSplits DashboardLayoutEdit::displayedSplits() const noexcept
{
    return editing ? working : committed;
}

DashboardLayoutSplits DashboardLayoutEdit::committedSplits() const noexcept
{
    return committed;
}

void DashboardLayoutEdit::begin() noexcept
{
    if (editing)
        return;

    working = committed;
    editing = true;
}

void DashboardLayoutEdit::moveSplitter(
    const DashboardSplitter splitter, const int requestedGridIndex) noexcept
{
    if (!editing)
        return;

    working = DashboardLayout::moveSplitter(working, splitter, requestedGridIndex);
}

void DashboardLayoutEdit::resetWorkingLayout() noexcept
{
    if (editing)
        working = DashboardLayout::defaultSplits;
}

bool DashboardLayoutEdit::finish() noexcept
{
    if (!editing)
        return false;

    const auto changed = working != committed;
    committed = working;
    editing = false;
    return changed;
}

bool DashboardLayoutEdit::cancel() noexcept
{
    if (!editing)
        return false;

    working = committed;
    editing = false;
    return true;
}
} // namespace audio_insight
