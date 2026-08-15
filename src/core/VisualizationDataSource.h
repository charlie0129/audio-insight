// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SpectrogramColumn.h"
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

    // Called only from non-real-time UI code. Clears Spectrum averaging and
    // peak holds while preserving FFT overlap and persistent settings.
    virtual void resetSpectrum() noexcept = 0;

    // Called only from non-real-time UI code. Clears Peak/RMS holds and OVER
    // latches while preserving the current live measurements.
    virtual void resetPeakRms() noexcept = 0;

    // Called only from non-real-time UI code. Starts a fresh Integrated
    // Loudness measurement while preserving the rolling Momentary and
    // Short-term windows.
    virtual void resetLoudness() noexcept = 0;

    // Copies the most recent complete immutable frame without waiting. Copying
    // a tagged capture-boundary frame also acknowledges its renderer delivery;
    // callers must therefore have one logical consumer.
    [[nodiscard]] virtual bool copyLatestVisualizationFrame(
        VisualizationFrame& destination) const noexcept = 0;

    // Copies the oldest retained immutable Spectrogram column. Copying a tagged
    // capture-boundary marker acknowledges its renderer delivery independently
    // of the visualization-frame stream. The default keeps non-Spectrogram
    // test/host adapters source-compatible while the processor and renderer
    // adopt this analysis-side stream.
    [[nodiscard]] virtual bool copyNextSpectrogramColumn(
        SpectrogramColumn& destination) const noexcept
    {
        static_cast<void>(destination);
        return false;
    }

    // Called only from non-real-time UI code so a renderer-owned history reset
    // cannot be repopulated by ordinary columns queued before the reset
    // boundary. Implementations preserve an undelivered capture-boundary marker.
    virtual void discardPendingSpectrogramColumns() noexcept
    {
    }
};
} // namespace audio_insight
