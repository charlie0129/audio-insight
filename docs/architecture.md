# Audio Insight architecture and project decisions

> **Status:** Living source of truth
>
> **Established:** 2026-08-15
>
> **Last updated:** 2026-08-15

This document records the requirements and technical choices agreed for Audio
Insight. Keep accepted decisions current as the implementation evolves. Items
under **Open questions** are intentionally not decided yet.

## Product intent

Under the working title Audio Insight, this will be an open-source audio analysis
and visualization plugin in the same broad product category as iZotope Insight 2
and Excite Audio VISION 4X. Its defining quality should be smooth, correctly
paced graphics without compromising the DAW's real-time audio processing.

The initial product is an analyzer: it observes audio and passes it through
unchanged. Any future audio-altering behavior must be an explicit feature, not
an incidental result of analysis.

## Accepted decisions

| Area | Decision |
| --- | --- |
| Platform priority | Build for macOS first. Initial development targets macOS 15 and arm64 only; broader deployment support comes after the implementation is stable. |
| Plugin formats | Build AUv2 and VST3 initially. AUv3 is newer, but its app-extension packaging and uncertain SoundSource compatibility are blockers for the first release. |
| Language and shell | Use modern C++ and JUCE for the host-facing plugin, parameters, state, audio plumbing, and editor integration. |
| Build system | Use CMake and generate an Xcode project for initial macOS development. Normal configuration does not download dependencies implicitly. |
| License | License project-owned code as AGPL-3.0-or-later and use JUCE under its upstream AGPLv3 option. Preserve all third-party notices. |
| Dependencies | Pin JUCE as a Git submodule rather than vendoring its source in this repository or downloading it implicitly during CMake configure. |
| Graphics | Use a native Metal renderer, hosted in a MetalKit view inside the JUCE editor. On the initial macOS baseline, drive its Metal layer with `CAMetalDisplayLink` rather than JUCE repaint timers or MetalKit's internal timer. Do not use deprecated macOS OpenGL. |
| Signal analysis | Initially use CPU analysis and Apple's Accelerate/vDSP where useful. GPU compute is deferred until profiling demonstrates a benefit. |
| Frequency presentation | Expose one continuous Linear-to-Logarithmic frequency-spacing control, including intermediate mappings. Spectrum and Spectrogram share the same setting and coordinate transform; they do not have independent frequency scales. |
| Dashboard interface | Keep one dashboard view with one Spectrum and no separate focus mode. After the fixed first-release vertical slice, add constrained grid-snapped width/height editing and the staged panels defined in [the analyzer interface requirements](analyzer-ui.md). |
| Real-time handoff | The audio callback writes only to bounded, non-blocking data structures. Analysis and rendering never make the audio thread wait. |
| Overflow policy | Prefer current visual data: coalesce or discard the oldest unclaimed analysis input, detect discontinuities by sequence number, and reset temporal analysis state across a gap. Never overwrite a slot being read or delay audio. |
| Analysis scheduling | Do not create one thread per visualization. Each instance has a logical coordinator, not a dedicated thread, and submits fairly to a process-wide pool initially bounded to two workers. |
| Render handoff | Renderers consume stable, immutable snapshots rather than mutable analysis working memory. |
| Display timing | Pace frames from the active display's refresh cycle, with smooth 60 Hz and 120 Hz/ProMotion behavior where the host and display permit it. Measure the cadence and deadlines actually granted by the display-link update rather than inferring them from the screen's advertised maximum refresh rate. |
| Performance observability | Offer an opt-in, persisted, per-instance metrics panel in Release builds. Show every collected renderer and analysis metric, exact presented-frame pacing history, per-frame callback-to-presentation latency composition, derived rates, and copyable raw reports without mutating the host process. |
| DPI support | Treat layout units and render pixels separately and support both regular-density and Retina displays, including live movement between them. |
| Editor lifecycle | Stop sample capture, analysis, history, display-link activity, and Metal submission when the editor is closed, hidden, or occluded beyond a short debounce. Reopening starts with fresh analysis state. |
| First usable release | Show a large real-time FFT spectrum with compact stereo sample-peak/RMS meters in one resizable layout. Defer history-based and stereo-field views. |
| Portability | Keep DSP, analysis, and product state independent of Metal behind a small renderer boundary. The architecture accommodates a future Windows backend, but Windows is not a near-term supported target. |
| Distribution | A paid Apple Developer account, Developer ID signing, and notarization are out of scope unless this policy is explicitly revisited. See [macOS distribution](macos-distribution.md). |
| Visual review | Deliver runnable visual checkpoints and ask the user for DAW testing, observations, or screenshots when appearance and interaction cannot be verified confidently in the development environment. |
| Sponsorship | Include a discreet sponsorship message and user-initiated link to `https://github.com/sponsors/charlie0129` in About/Legal. Do not track users or contact the network automatically. |

## Platform and plugin baseline

Initial development uses the current reference environment:

- macOS 15.6.1 with a macOS 15 deployment target;
- Apple silicon (`arm64`), specifically an Apple M1 Max with 64 GiB RAM; and
- Xcode 16.4.

This is a development simplification, not an assertion that the product needs a
macOS 15 feature. Avoid unnecessary dependence on new APIs and keep availability
boundaries clear so the deployment target can be lowered later. Add `x86_64` and
Universal Binary builds only after the arm64 implementation and validation path
are stable.

### Why AUv2 is first

AUv3 is Apple's newer Audio Unit model, but on macOS it is an app extension
contained in an application rather than a drop-in `.component`. JUCE's AUv3
target requires the Xcode generator and produces an `.appex` plus a framework. A
distributable AUv3 also needs a containing app, typically produced by building
JUCE's AUv3 and Standalone formats together, plus nested signing and a different
installation lifecycle.

The intended SoundSource host documents discovery from the standard AUv2
Components directories, while current Logic documentation continues to support
that format. Therefore the first build targets AUv2 and VST3. Core DSP, analysis,
state, and UI code must remain wrapper-neutral so AUv3 can be added later without
an architectural rewrite. SoundSource does not explicitly state that AUv3 is
unsupported; that is an inference from its documented scan locations and must be
checked with an AUv3 smoke test before being treated as conclusive.

References:

- [Apple Audio Unit v2 API](https://developer.apple.com/documentation/audiotoolbox/audio-unit-v2-c-api)
- [Apple AUv3 sample and packaging](https://developer.apple.com/documentation/avfaudio/creating-custom-audio-effects)
- [Apple Logic Audio Unit installation](https://support.apple.com/en-us/102239)
- [JUCE 9.0.1 CMake plugin formats](https://github.com/juce-framework/JUCE/blob/9.0.1/docs/CMake%20API.md#formats)
- [SoundSource Audio Unit discovery](https://rogueamoeba.com/support/manuals/soundsource?page=audio-unit-effects)

## Runtime architecture

The intended data flow is:

```text
DAW audio callback
    -> bounded lock-free sample/measurement handoff
    -> analysis coordinator and shared analysis graph
    -> immutable display snapshots
    -> display-synchronized Metal renderer
```

The stages run at independent rates. A display may render at 120 frames per
second while a costly analysis updates less often. The renderer should smooth or
interpolate suitable values between snapshots instead of coupling every frame to
a new FFT.

### Audio callback

The host's audio callback is the highest-priority part of the system. It must:

- pass audio through transparently;
- do a bounded amount of work for every block;
- avoid heap allocation and deallocation;
- avoid mutexes, waits, blocking atomics, and thread coordination;
- avoid logging, file access, networking, UI calls, and GPU calls; and
- tolerate host changes to sample rate, block size, and channel layout.

It may update trivial fixed-size measurements and copy samples into a
preallocated lock-free structure. If downstream work falls behind, the handoff
uses a latest-wins, discontinuity-aware policy:

- stamp captured chunks with a plugin-owned monotonically increasing frame and
  chunk sequence; host transport position may be metadata but cannot diagnose
  queue loss because seeking and looping legitimately make it jump;
- coalesce queued work and retire the oldest unclaimed input first;
- never overwrite storage while the consumer owns or reads it;
- if no old slot can be reclaimed safely, drop the incoming analysis chunk;
- count drops, discontinuities, queue high-water mark, and snapshot age; and
- reset FFT overlap, RMS integration, smoothing, and other temporal state after
  any detected gap instead of presenting discontinuous input as continuous.

The implementation must use explicit slot ownership or an equivalently safe
protocol; a producer overwriting an SPSC payload while it may be read is a C++
data race. Audio pass-through is never affected by analysis overflow.

Peak/RMS measurements use a separate bounded coalescing accumulator so an
overloaded raw-sample queue does not silently erase the largest recent peak.

Publishing samples, measurements, and bounded atomic state is the end of the
audio callback's responsibility. It must not signal a semaphore or condition
variable, enqueue through a general-purpose task queue, or otherwise wake a
worker. A non-real-time coordinator/display scheduler observes published state
and submits analysis work.

Audio-thread telemetry follows the same constraints as audio data. Use
single-writer counters or fixed-width atomics proven always lock-free on the
target, with a bounded number of operations. Do not use unbounded
compare-exchange loops. Derive high-water marks and other aggregate diagnostics
on the consumer where practical.

### Analysis

The analysis subsystem should be organized as a dependency graph or equivalent
coordinator-owned set of jobs. For example, several visualizations can reuse the
same channel levels, windowed samples, FFT results, frequency bins, loudness
measurements, and stereo statistics.

Each analysis has its own required cadence. Disabled or invisible analyses
inside an otherwise active editor should stop or run less frequently when doing
so does not break an explicitly supported history feature. The editor lifecycle
state is authoritative: when the editor is inactive, all first-release analyses
stop. Rendering must be able to read the newest complete snapshot without
waiting for an analysis job.

There is one logical coordinator per plugin instance, but a coordinator is not a
thread. Coordinators submit work to a scheduler shared by all instances loaded
from the same plugin module in the current host process. The scheduler initially
owns two lower-priority, non-real-time worker threads.

At most one job per instance may be running or queued, plus a coalesced
"newer data available" indication. Schedule instances fairly so a costly view
cannot monopolize the pool. Worker count is configurable for benchmarks but may
change as a product default only from measurements. AUv2 and VST3 modules, or
hosts that isolate plugins in separate processes, naturally have separate pools.

Instance destruction must cancel or drain its queued work safely before analysis
state is released. No pool operation, lifetime wait, or cancellation path may
run from the audio callback.

The shared scheduler is a reference-counted module service, not an unmanaged
static with unspecified destruction order. Jobs carry cancellation generations
and lifetime-safe state handles rather than raw processor/editor pointers. When
the last instance releases the service, cancel queued work and join the workers
before the host can unload code from the plugin module.

### Closed and hidden editors

The first release retains no analysis history for time during which its editor
is closed. When the editor is no longer visible or attached, stop display-link
activity, Metal submissions, analysis jobs, and audio sample handoff. Transparent
pass-through and host-required parameter/state handling continue.

Some hosts cache an editor instead of destroying it, so lifecycle control must
consider visibility, attachment, and occlusion rather than relying only on the
editor destructor. A short debounce is acceptable to avoid start/stop churn.
When the editor becomes active again, advance an analysis generation, discard
stale snapshots, reset holds/smoothing/history, and warm up from current audio.

| Editor state | Sample handoff | Analysis | Display link and Metal |
| --- | --- | --- | --- |
| Attached and visibly active | Enabled | Enabled as needed | Enabled at display cadence |
| Minimized, host-hidden, or occluded beyond the debounce | Disabled | Cancelled and quiesced | Stopped |
| Detached, closed, or destroyed | Disabled | Cancelled and quiesced | Stopped and resources released as appropriate |

Returning from either inactive state begins a new analysis generation; the first
release does not attempt to reconstruct the missing interval.

### Metal rendering

The primary editor surface will be a custom Metal renderer embedded through a
native MetalKit view. On the macOS 15 development baseline, the MetalKit view's
internal drawing loop stays paused and a `CAMetalDisplayLink` attached to its
`CAMetalLayer` supplies each drawable plus its target and target-presentation
timestamps. This makes callback delay, CPU commit misses, GPU deadline misses,
and actual presentation separate measurements instead of guessing from
`NSScreen.maximumFramesPerSecond`. Because `CAMetalDisplayLink` requires macOS
14, lowering the deployment target below macOS 14 will require a separately
validated display-link fallback.

Commit the encoded Metal work before calling plain `present()` on the drawable
provided by the display-link update. `CAMetalDisplayLink` owns presentation
timing, so its timestamps are telemetry and deadline inputs rather than values
for `presentAtTime` or other timed presentation APIs, which assert for these
drawables.

Release reusable per-frame buffers when their GPU command buffers complete, not
when their drawables are eventually presented. Presentation and compositor
retention can span several display periods even when GPU execution is short;
coupling buffer ownership to that later event can exhaust an otherwise healthy
in-flight pool and turn 120 display-link callbacks per second into roughly 60
submissions. Each submitted frame instead has lifetime-safe correlation state
that independently receives the GPU-completion and presentation callbacks. It
must not retain a raw renderer pointer after teardown.

The editor exposes a built-in performance metrics panel rather than relying on
Apple's Metal Performance HUD. On macOS 15, `CAMetalLayer.developerHUDProperties`
configures a HUD only after the hosting process was launched with Apple's
documented `MTL_HUD_ENABLED=1` environment setting before its first Metal device.
The layer property alone cannot load the process-wide HUD runtime afterward. An
Audio Unit or VST3 cannot safely relaunch or mutate its host to impose that launch
state, and the API offers no reliable per-plugin availability query.

The built-in panel separates exact presentation cadence from per-frame latency.
Presented handlers insert actual timestamps into a fixed 241-timestamp window
under a tiny non-audio-thread lock. Sorting by timestamp makes the resulting 240
presentation intervals exact even if Metal invokes handlers concurrently or out
of order. A separate fixed history correlates each submitted frame's display-link
callback, command submission, GPU start/end, and actual presentation into four
coarse components: CPU encode, Submit + queue, GPU execute, and
compositor/display wait. Their sum is callback-to-presentation latency. It is not
the presented-frame interval because multiple frames overlap in the pipeline.

Lightweight graph snapshots and repaints follow the editor window's vblank at
the active display cadence while at least one graph intersects the visible
metrics viewport. Numeric summaries update at no more than ten hertz, and the
full renderer/analysis model, raw table, and accessibility hierarchy update at
four hertz on the message thread while the renderer is effectively active.
The panel exposes every raw field, derived rates and interval statistics, and a
copyable text report. GPU completion timing fields and histories are copied
under short non-audio-thread locks so values and validity counters describe
coherent groups. Explicit valid, unavailable, and unclassifiable counters keep
missing Metal timestamps distinct from a measured zero duration or lateness.
Native effective-activity transitions stop and restart collection; retained
values are explicitly marked paused and may be stale. The full report is
assembled lazily when the user presses Copy, and neither exact 240-entry history
is serialized during live polling. The panel sits beside the native Metal view
because an overlapping JUCE component cannot reliably appear above an
`NSViewComponent`. It defaults to off because formatting and painting
diagnostics has measurable overhead; important results should also be confirmed
with it hidden.

The rendering toolbox should favor simple, predictable GPU operations:

- line or triangle geometry for plots and scopes;
- instanced geometry for repeated meter elements;
- a scale-aware glyph atlas or equivalent GPU text solution.

Future scrolling history views may add circular or tiled textures after the
first-release spectrum and meters meet their performance gates.

Rendering should be driven by a display-linked mechanism rather than a generic
UI timer. V-sync prevents tearing, while stable frame pacing and consistently
short frame times provide perceived smoothness; both must be measured.

The editor must stop rendering when hidden or occluded, following the lifecycle
rules above. Live resize, GPU/device availability, window attachment and
detachment, and movement between displays must not leave stale render resources
or background render loops behind.

### Retina and regular-density displays

UI layout is expressed in logical points. The Metal drawable is expressed in
physical pixels, conceptually:

```text
drawable size = view bounds in points x current backing scale
```

This relationship is conceptual, not an instruction to multiply by a hard-coded
scale. Prefer MetalKit's automatic drawable resizing where it fits the plugin
host; otherwise use Cocoa backing-coordinate conversion. Observe backing
property changes and query the attached view/window rather than a global
main-screen value.

When the window changes display, scale, or size, update the drawable and rebuild
only resources whose resolution depends on pixels, such as glyph atlases. Do not
assume that the only possible scale values are exactly 1 and 2. Geometry,
strokes, text, and hit testing must remain visually consistent and crisp on both
regular-density and Retina displays.

## Analyzer presentation

Detailed layout, settings, labeling, and per-panel requirements live in
[the analyzer interface requirements](analyzer-ui.md). The following invariants
remain architectural because analysis and rendering share them.

Spectrum and Spectrogram use the same continuous frequency-coordinate mapping.
The control ranges from linear through intermediate spacing to logarithmic and
applies to the Spectrum frequency axis and the Spectrogram frequency axis.

The Spectrogram renders time-frequency energy over a near-black background. Its
intensity palette should read as energy traces: by default, dark blue for lower
energy rising through orange and near-white for stronger energy. It must not
look like broad decorative color stripes.

Spectrogram chrome shows only numeric frequency tick labels, using Hz or kHz as
appropriate. Omit axis-title text, time/history labels, and dB/color-legend
labels.

## First usable release

The first usable release deliberately has one layout and two visualizations:

- a large real-time FFT spectrum occupying most of the surface, using the
  shared adjustable frequency-spacing transform; and
- a compact vertical stereo meter strip showing honest sample peak and RMS.

A small controls row may expose essential spectrum and meter settings. The
window is resizable. The first vertical slice may retain its fixed default
arrangement; the accepted later dashboard adds constrained, grid-snapped tile
resizing in both dimensions. Detached panels, overlapping floating windows, and
arbitrary free-form windowing remain out of scope. Do not label sample peak as
true peak; that name is reserved for a correctly oversampled true-peak
implementation.

Before distributing a binary, the controls must include an About/Legal path that
shows the confirmed project copyright, AGPL/no-warranty notice, how to view the
license and corresponding source, and applicable third-party notices. This is
part of first-release acceptance, not optional polish.

The same panel includes a brief message such as “If Audio Insight is useful to
you, consider sponsoring its development,” linking to
`https://github.com/sponsors/charlie0129`. Opening the external page requires an
explicit click. The message must not interrupt use, consume visualization space,
display repeatedly as a prompt, collect telemetry, or initiate network access on
its own.

Spectrogram history, LUFS and loudness history, vectorscope/goniometer,
correlation, phase, surround layouts, and multi-instance aggregation can follow
after this vertical slice is smooth, correct, and measured.

## Performance and validation

Performance is a product requirement, not a final optimization pass. Add
instrumentation early enough to observe:

- delivered frame cadence, frame-time distribution, and dropped/late frames;
- render CPU time and GPU time;
- analysis job latency and missed analysis deadlines;
- audio-callback CPU cost and any real-time safety violation;
- data drops, discontinuities, queue high-water mark, and snapshot age; and
- hidden-editor and multi-instance overhead.

### Preliminary reference workload and budgets

Use an optimized arm64 build on the M1 Max reference Mac, stereo audio at 48 kHz,
a 4096-point Hann-window FFT at 60 analysis updates per second, and meters fed
from every block. Run separate callback tests at 64, 128, 256, 512, and 1024
samples with identical warm-up and percentile sample counts. The main rendering
case is 1200 x 800 logical points at 2400 x 1600 physical pixels; also test a
1200 x 800-pixel regular-density case. Always log actual backing scale and
drawable pixel dimensions rather than assuming them. Measure ten minutes of
steady state after warm-up.

| Area | Preliminary target |
| --- | --- |
| Audio callback | p99 added time no greater than 25 microseconds at 128 samples and p99 no greater than 2% of block duration across 64–1024 samples; zero allocation, locks, or detected real-time violations. |
| Spectrum analysis | p99 execution no greater than 1 ms; newest-edge snapshot age p99 no greater than 33 ms. |
| Meter freshness | Source-data age p99 no greater than 16.7 ms. |
| Render CPU encoding | p99 no greater than 1 ms per frame. |
| GPU execution | p99 no greater than 2 ms per frame at the reference Retina size. |
| Frame delivery | At least 99.5% of display-requested presentations at fixed 60 Hz and host/display-permitted 120 Hz; no sustained run of more than two plugin-attributable late frames. |
| Active CPU | Mean total plugin CPU work no greater than 5% of one M1 Max performance core for one visible instance. |
| Closed editor | Within 250 ms: zero analysis jobs, worker wakeups attributable to that instance, Metal submissions, and sample-handoff copies. Over the following 60 seconds, attributed background CPU is no greater than 0.1% of one core. |
| Multiple instances | With all processors receiving stereo audio, sixteen instances with one visible retain that visible instance's single-visible latency/frame budgets and all fifteen closed instances meet the closed-editor budget. With four visible at 60 Hz, each delivers at least 99.0% of requested frames, has spectrum snapshot age p99 no greater than 50 ms, and never goes more than 100 ms without publishing a fresh spectrum snapshot. |

For ProMotion and variable refresh, measure against the display-link cadence
actually granted by macOS rather than assuming a constant 120 Hz. Report host
main-thread stalls and compositor delays separately from plugin CPU/GPU misses.
These are initial engineering gates: revise them only from captured measurements
and record the reason in the decision log.

### Measurement definitions

- **Audio callback time** is elapsed monotonic time from `processBlock` entry to exit,
  including pass-through and capture, with OS preemption noted separately when
  the profiler can identify it. Measure externally where possible; any in-callback
  timestamps use a proven real-time-safe monotonic source and publish only a
  bounded record, never a log entry.
- **Analysis execution time** measures job-body execution separately from queue
  wait. **Snapshot age** is presentation time minus the end time of the newest
  represented input block; meter freshness uses the same presented-frame
  endpoint.
- **Render CPU time** includes drawable acquisition, buffer updates, command
  encoding, and command-buffer submission. **GPU time** uses command-buffer GPU
  start/end timestamps where available.
- **Presented-frame pacing** is the interval between consecutive actual
  presentation timestamps. **Per-frame latency** starts at that frame's
  display-link callback and ends at its actual presentation. Decompose the
  latter into CPU encode, Submit + queue, GPU execute, and compositor/display
  wait. Do not sum overlapping frames or present per-frame latency as cadence.
- A frame has a specific deadline supplied by the display-linked timing source.
  Attribute a miss to the plugin when its CPU submission or GPU completion
  crosses that deadline. Classify a callback that arrives already late, or
  externally unavailable drawable/compositor service, as host/compositor delay;
  record unclassifiable misses as unknown rather than silently excluding them.
- Per-instance submitted, executed, cancelled, and published-job counters provide
  closed-editor and fairness attribution in a shared pool.

Validate AUv2 builds with Apple's `auval` and VST3 builds with Steinberg's
validator. Integration tests and representative multi-instance stress tests
supplement, not replace, those validators.

### Initial host matrix

- Logic Pro is the primary AUv2 DAW target; record the exact tested version when
  it becomes available on the reference machine.
- SoundSource 6.0.6 is the primary non-DAW AUv2 host and is installed on the
  reference machine.
- The Steinberg VST3 validator and a development plugin host cover VST3 initially.
  A real VST3 DAW remains to be selected for the compatibility matrix.

### Human-in-the-loop visual validation

Automated checks, captured metrics, and any available local render harness come
first, but they cannot completely judge motion quality or behavior across DAWs
and displays. At meaningful runnable milestones, summarize what changed and give
the user a short, specific checklist to verify. The user can report observations
or provide screenshots. Treat that feedback as validation evidence, reproduce
issues where possible, and record resulting architectural decisions here.

Useful checkpoints include initial host loading, first Metal output, movement
between regular-density and Retina displays, 60/120 Hz frame pacing, live resize,
and each substantially new visualization. Do not claim visual correctness merely
because the project compiles or a validator passes.

## Licensing and dependency acquisition

Unless a file says otherwise, project-owned code is licensed under
**AGPL-3.0-or-later**; the complete license text is in the repository `LICENSE`
file. JUCE remains under its own upstream AGPLv3 terms and is not relicensed by
Audio Insight. Retain the notices for JUCE and its bundled dependencies. JUCE's
bundled VST3 SDK is MIT-licensed; no separate VST3 SDK checkout is needed.

References:

- [JUCE 9.0.1 license](https://github.com/juce-framework/JUCE/blob/9.0.1/LICENSE.md)
- [JUCE-bundled VST3 SDK license](https://github.com/juce-framework/JUCE/blob/9.0.1/modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt)

Pin JUCE 9.0.1 at commit
`e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8` as `external/JUCE`, using a Git
submodule. The main repository stores the submodule URL and commit rather than a
copy of JUCE's source history. Document recursive cloning and provide an optional
CMake source-directory override for developers with an existing JUCE checkout.
Do not perform an implicit network download during normal CMake configuration.

GitHub-generated source archives omit submodule contents. Any binary release
must therefore ship alongside a complete Corresponding Source archive for that
exact binary. It includes all project source, shader and source-form assets,
configuration, build and installation instructions, the pinned JUCE tree, local
modifications, and required license notices. Audit the licenses of every added
font, asset, shader source, and library. Apple SDK frameworks such as Metal and
Accelerate remain system dependencies supplied by Xcode.

Keep the canonical GNU text in `LICENSE` unchanged. Once the copyright holder's
display name is confirmed, add a project notice and mark project-owned source
files with that copyright plus `SPDX-License-Identifier: AGPL-3.0-or-later`.

## Implementation sequence

1. Add the confirmed project notice, pinned JUCE submodule, and build system for
   macOS 15 arm64 AUv2 and VST3 targets.
2. Implement transparent pass-through, parameter/state persistence, lifecycle
   instrumentation, and format-validator coverage.
3. Embed a display-synchronized Metal surface with dynamic backing-scale support
   and frame-pacing instrumentation.
4. Add the bounded handoff, shared worker pool, immutable snapshots, and stereo
   sample-peak/RMS meters.
5. Add the shared 4096-point FFT path and main spectrum visualization.
6. Meet the preliminary budgets, run host and multi-instance checks, gather user
   visual feedback, and document source/prebuilt distribution.

This sequence is a starting plan rather than a promise about the exact set or
order of visualizations.

## Open questions

- After the arm64 implementation stabilizes, how low can the macOS deployment
  target go without harming the Metal/display-timing design?
- When should `x86_64` and Universal Binary builds be added and tested?
- Which exact Logic Pro version and which real VST3 DAW join the compatibility
  matrix?
- Which future feature first justifies retaining history while the editor is
  closed and revisiting the current zero-background-work rule?
- What concrete host or platform requirement would justify adding AUv3?
- When, if ever, does Windows move from architectural accommodation to an
  implementation commitment, and which GPU backend should it use?
- Which fonts and visual assets will be used, and under which AGPL-compatible
  licenses?
- What copyright-holder name should appear in project and in-plugin legal
  notices?
- Is Audio Insight the final display name or only the working title, and what
  bundle identifiers, manufacturer code, and plugin subtype will be registered?

## Decision log

### 2026-08-15

- Chose macOS 15 and arm64 as the initial development baseline on the M1 Max
  reference machine, while preserving a path to older macOS and `x86_64` later.
- Chose AUv2 and VST3 initially; deferred AUv3 because of its container/signing
  complexity and uncertain SoundSource support.
- Chose native Metal rendering with `CAMetalDisplayLink` frame pacing and actual
  presentation-deadline telemetry on the initial macOS baseline.
- Required regular-density, Retina, and live cross-display scale handling.
- Chose CPU/vDSP analysis initially and GPU rendering, with rates decoupled.
- Rejected per-visualization threads; chose per-instance logical coordinators on
  a process-wide pool initially bounded to two workers.
- Chose latest-wins, discontinuity-aware overflow with explicit safe slot
  ownership and telemetry.
- Chose to stop capture, analysis, history, display timing, and rendering when
  the editor is closed, then warm up fresh on reopen.
- Scoped the first usable release to stereo sample-peak/RMS meters and a
  real-time FFT spectrum in one resizable layout.
- Set preliminary M1 Max performance budgets for audio, analysis, rendering,
  frame pacing, closed editors, and multiple instances.
- Licensed project-owned code as AGPL-3.0-or-later to match JUCE's AGPL route and
  chose a pinned JUCE 9.0.1 Git submodule.
- Required an in-plugin About/Legal path and complete corresponding-source
  archives for binary releases; the copyright-holder display name remains open.
- Added a non-intrusive, user-initiated GitHub Sponsors link in About/Legal, with
  no tracking or automatic network access.
- Chose CMake with an Xcode generator and no implicit dependency download during
  normal configuration.
- Chose a non-notarized distribution model using local/ad-hoc signing when
  signing is needed.
- Preserved an architectural path to a future Windows backend without treating
  Windows as a supported or near-term target.
- Began the host matrix with Logic Pro and SoundSource for AUv2; a real VST3 DAW
  remains open.
- Added explicit user review checkpoints for visual and DAW behavior that cannot
  be assessed confidently in the development environment.
- Replaced the host-dependent Metal Performance HUD experiment with an opt-in,
  per-instance metrics panel and exact presented-frame pacing history.
- Decoupled reusable render-buffer release from drawable presentation and added
  exact per-frame callback-to-presentation latency composition beside the
  separate pacing history. Lightweight graphs follow display vblank while the
  full raw metrics table remains throttled.
- Chose one continuous Linear-to-Logarithmic frequency mapping shared by
  Spectrum and Spectrogram, including intermediate spacing.
- Set the Spectrogram presentation target to intensity-colored energy traces
  over near-black, with only numeric frequency ticks shown.
- Accepted one dashboard with no Focus Spectrum mode, a temporary right-side
  Settings inspector, constrained width/height tile editing after the first
  vertical slice, and staged presentation requirements for every analyzer
  panel. See `docs/analyzer-ui.md`.
