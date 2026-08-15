<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Audio Insight

Audio Insight is a work-in-progress, open-source AUv2 and VST3 audio analyzer
for macOS. The project prioritizes smooth display-synchronized Metal graphics,
real-time-safe audio processing, and low overhead in the host.

The first usable release will combine a real-time FFT spectrum with stereo
sample-peak and RMS meters. See [the architecture and decision
record](docs/architecture.md) for the accepted design and current open questions.

## Development build

Current development targets macOS 15 on Apple silicon with Xcode 16.4 or newer
and CMake 3.25 or newer.

```sh
git clone --recurse-submodules https://github.com/charlie0129/audio-insight.git
cd audio-insight
cmake --preset macos-arm64
cmake --build --preset macos-arm64-debug --target AudioInsight_AU AudioInsight_VST3
```

The final configure message, such as:

```text
Build files have been written to: /path/to/audio-insight/build/xcode
```

refers to the generated Xcode project, not the finished plugins. The Debug
command above writes the plugin bundles to:

```text
build/xcode/AudioInsight_artefacts/Debug/AU/Audio Insight.component
build/xcode/AudioInsight_artefacts/Debug/VST3/Audio Insight.vst3
```

For optimized Release bundles, build with:

```sh
cmake --build --preset macos-arm64-release --target AudioInsight_AU AudioInsight_VST3
```

The Release bundles are written to:

```text
build/xcode/AudioInsight_artefacts/Release/AU/Audio Insight.component
build/xcode/AudioInsight_artefacts/Release/VST3/Audio Insight.vst3
```

To install the plugins system-wide, copy each complete bundle to its matching
macOS plugin directory:

```text
Audio Insight.component -> /Library/Audio/Plug-Ins/Components/
Audio Insight.vst3      -> /Library/Audio/Plug-Ins/VST3/
```

System-wide installation may require administrator privileges. For a per-user
installation, use the corresponding directories under `~/Library` instead:

```text
~/Library/Audio/Plug-Ins/Components/
~/Library/Audio/Plug-Ins/VST3/
```

Restart or rescan the audio host after copying the bundle.

## Performance metrics

The editor's **Metrics** toggle opens a per-instance observability panel beside
the visualization. It works in Release builds, is off by default, and is saved
with the plugin state. The live summary separates two measurements that should
not be conflated:

- **Presented-frame pacing** uses one stem per exact presentation interval from
  the latest 240 intervals. Its height shows the time between frames reaching
  the display, so 8.33 ms corresponds to 120 Hz and 16.67 ms to 60 Hz. Green
  stems stay within 1.25 times the current target, amber stems exceed that
  tolerance, horizontal guides mark target multiples, and red dashed markers
  identify presentation-sequence gaps.
- **Per-frame latency composition** uses one stacked bar per correlated frame.
  The segments are CPU encode, Submit + queue, GPU execute, and
  compositor/display wait. Their sum is that frame's callback-to-presentation
  latency, not the interval between presentations; several frames can overlap
  in the rendering pipeline. These bars use the same presentation-sequence
  positions as the pacing graph; a red X marks unavailable or unclassifiable
  component timing.

Reusable Metal render buffers are released as soon as their GPU command buffer
completes. Presentation tracking has independent per-submission lifetime state,
so a compositor retaining a drawable for several refresh periods does not
artificially exhaust the render-buffer pool and halve the submission cadence.

The scrollable detail view exposes every raw renderer, audio-capture, meter,
scheduler, analysis-job, publication, and freshness metric currently collected,
plus derived rates and frame-interval statistics. **Copy** exports stable field
names with raw values and units. Timing counters distinguish valid, unavailable,
and unclassifiable samples so a missing Metal timestamp cannot masquerade as
zero latency. **Reset render** starts a new renderer telemetry epoch without
resetting lifetime analysis counters. At narrow editor sizes, detail rows stack
their labels and full-width values instead of squeezing the value column.

The lightweight graphs refresh from the editor window's vblank callback while
either graph is visible in the metrics viewport, so they can remain smooth at
the active display's actual refresh rate. Numeric summary values are throttled
to at most ten updates per second, and the full raw metrics model, table, and
accessibility hierarchy update four times per second. Exact histories are
recorded once per presented frame without allocation or audio-thread work.
Native occlusion or minimization stops collection and marks retained values
**PAUSED**; it restarts when rendering becomes effective again. The complete
text export, including both exact histories and every component value, is
formatted only when **Copy** is pressed; neither 240-entry history is serialized
during live polling. The panel still has some diagnostic overhead, so confirm
important performance measurements with it disabled. For a useful report, let
the plugin render for at least 30 seconds and include the host, display refresh
rate, editor size, and whether the display is Retina.

Audio Insight cannot reliably offer an Apple Metal Performance HUD switch from
inside a plugin. Apple's HUD must be enabled for the hosting process before that
process creates its first Metal device; configuring Audio Insight's layer later
does not activate it. The built-in panel therefore remains the supported Release
diagnostics path.

For an existing clone, initialize the pinned JUCE dependency with:

```sh
git submodule update --init --recursive
```

The build does not install plugins into user or system directories
automatically. Signing, local installation, and quarantine guidance lives in
[the macOS distribution policy](docs/macos-distribution.md).

## Formatting

After configuring the project once, format all project-owned C++, headers, and
Objective-C++ sources with:

```sh
cmake --build --preset format
```

To check formatting without modifying files:

```sh
cmake --build --preset format-check
```

The targets require clang-format 22, use the repository's `.clang-format`, and
intentionally exclude JUCE, generated build trees, and other third-party code. Set
`AUDIO_INSIGHT_CLANG_FORMAT_EXECUTABLE` during CMake configuration to select a
specific clang-format 22 executable. Ordinary builds do not require the formatter.

## License and support

Project-owned code is licensed under
[GNU AGPL version 3 or later](LICENSE). JUCE and other dependencies retain their
own licenses and notices.

If Audio Insight is useful to you, you can
[sponsor its development](https://github.com/sponsors/charlie0129).
