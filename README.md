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

## Metal performance HUD

The editor's **HUD** toggle enables Apple's Metal Performance HUD directly on
Audio Insight's Metal layer. It works in Release builds, is off by default, and
is saved with the plugin state. The focused HUD layout shows frame rate, frame
interval and pacing graph, GPU and Metal CPU time, presentation delay, and the
layer's pixel size and scale.

For a useful visual report, let the plugin render for at least 30 seconds, then
capture the HUD while audio is playing. Include the host, display refresh rate,
editor size, and whether the display is Retina. The HUD adds some diagnostic
overhead, so final performance measurements should also be confirmed with it
disabled. Enable it on only one Audio Insight instance at a time so Metal's
process-level metric attribution remains unambiguous.

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
