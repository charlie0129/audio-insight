// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "VisualizationFrame.h"

namespace audio_insight {
class VisualizationDataSource {
public:
    virtual ~VisualizationDataSource() = default;

    // Called only from non-real-time editor/render code. Implementations should
    // coalesce repeated requests rather than queueing unbounded work.
    virtual void requestAnalysis() noexcept = 0;

    // Called from editor lifecycle code, never from the audio callback.
    virtual void setVisualizationActive(bool shouldBeActive) noexcept = 0;

    // Copies the most recent complete immutable frame without waiting.
    [[nodiscard]] virtual bool copyLatestVisualizationFrame(
        VisualizationFrame& destination) const noexcept = 0;
};
} // namespace audio_insight
