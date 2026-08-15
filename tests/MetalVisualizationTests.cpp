// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/MetalVisualization.h"

#include <juce_core/juce_core.h>

#include <cstdint>

namespace audio_insight {
namespace {
class StubVisualizationDataSource final : public VisualizationDataSource {
public:
    void requestAnalysis() noexcept override
    {
    }

    void setVisualizationActive(bool) noexcept override
    {
    }

    [[nodiscard]] bool copyLatestVisualizationFrame(VisualizationFrame&) const noexcept override
    {
        return false;
    }
};

class MetalVisualizationTests final : public juce::UnitTest {
public:
    MetalVisualizationTests() : UnitTest("Metal visualization", "audio-insight")
    {
    }

    void runTest() override
    {
        beginTest("Telemetry reset publishes immediately while rendering is paused");

        StubVisualizationDataSource dataSource;
        MetalVisualization visualization(dataSource);
        expect(!visualization.isEffectivelyRendering());

        auto effectiveActivityChanges = 0;
        visualization.setEffectiveActivityCallback([&](bool) { ++effectiveActivityChanges; });
        visualization.setRenderingActive(false);
        visualization.setRenderingActive(true);
        visualization.setRenderingActive(true);
        expectEquals(effectiveActivityChanges, 0,
            "Detached render requests are not effective-activity transitions");

        const auto beforeReset = visualization.getRenderTelemetry();
        visualization.resetRenderTelemetry();
        const auto afterReset = visualization.getRenderTelemetry();

        expectEquals(afterReset.epoch, beforeReset.epoch + 1);
        expect(!afterReset.resetPending);
        expectEquals(afterReset.displayLinkCallbacks, std::uint64_t { 0 });
        expectEquals(afterReset.presentedFrames, std::uint64_t { 0 });
        expectEquals(afterReset.gpuTimingSamples, std::uint64_t { 0 });
        expectEquals(afterReset.presentationLatenessSamples, std::uint64_t { 0 });
        expect(afterReset.metalAvailable == beforeReset.metalAvailable);
        expect(afterReset.renderingRequested == beforeReset.renderingRequested);
        expect(afterReset.effectivelyRendering == beforeReset.effectivelyRendering);

        visualization.resetRenderTelemetry();
        const auto afterSecondReset = visualization.getRenderTelemetry();
        expectEquals(afterSecondReset.epoch, afterReset.epoch + 1);
        expect(!afterSecondReset.resetPending);

        visualization.setEffectiveActivityCallback({ });
    }
};

static MetalVisualizationTests metalVisualizationTests;
} // namespace
} // namespace audio_insight
