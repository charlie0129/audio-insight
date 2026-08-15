// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DashboardLayout.h"

#include <juce_core/juce_core.h>

namespace audio_insight {
/**
    Versioned per-user persistence for the four dashboard split indices.

    The default location is independent of the plugin wrapper, so AUv2 and VST3
    instances for the same user share one preference. An explicit file can be
    supplied by tests or tools that must not touch the user's real preference.

    Every operation may allocate, block briefly, and access the filesystem. The
    caller must never use this class from an audio or other real-time thread.
*/
class DashboardLayoutStore final {
public:
    static constexpr int schemaVersion = 1;

    DashboardLayoutStore();
    explicit DashboardLayoutStore(juce::File storageFile);

    [[nodiscard]] static juce::File defaultStorageFile();
    [[nodiscard]] const juce::File& getStorageFile() const noexcept;

    /**
        Loads one complete saved layout.

        Missing, unreadable, malformed, unknown-version, and geometrically
        invalid data all return the compiled defaults. Individual fields are
        never applied piecemeal. Lock acquisition is bounded; failure to acquire
        the lock also returns the defaults.
    */
    [[nodiscard]] DashboardLayoutSplits load() const;

    /**
        Explicitly commits a complete layout, intended for the Edit layout Done
        action. Valid layouts are atomically replaced under a bounded
        cross-process lock. Each successful commit replaces the whole previous
        value, so the last successfully completed edit wins.

        Returns false without changing the preference if the layout is invalid,
        the lock cannot be acquired, or the write/replacement fails.
    */
    [[nodiscard]] bool commit(const DashboardLayoutSplits& splits) const;

private:
    juce::File storageFile;
};
} // namespace audio_insight
