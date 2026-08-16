# Audio Insight architecture and project decisions

> **Status:** Living source of truth
>
> **Established:** 2026-08-15
>
> **Last updated:** 2026-08-16

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
| Dashboard interface | Keep one fixed-topology five-tile dashboard with one Spectrum and no separate focus mode. Spectrum, Peak/RMS, Spectrogram, Stereo/correlation, and standards-based Loudness are live. Allow only the four grid-snapped width/height splits defined in [the analyzer interface requirements](analyzer-ui.md). |
| Utility panels | Keep Settings, Metrics, and About mutually exclusive in presentation without letting About mutate their underlying state. When the dashboard is active on entry, About is a right-side sibling with a live Metal preview, preferring 50% width clamped to 360–700 logical points while leaving at least 320 points for Metal. From already-paused full-content Settings, About remains full-content and paused. Closing reveals current utility state rather than rolling it back. |
| Interface state | Save display pacing, analyzer settings, and the Metrics toggle as non-automatable per-instance state, and the four-split layout as one versioned per-user global preference. Do not serialize transient history, holds, integration, Settings/About visibility, or uncommitted edits. |
| Real-time handoff | The audio callback writes only to bounded, non-blocking data structures. Analysis and rendering never make the audio thread wait. |
| Overflow policy | Prefer current visual data: coalesce or discard the oldest unclaimed analysis input, detect discontinuities by sequence number, and reset temporal analysis state across a gap. Never overwrite a slot being read or delay audio. |
| Analysis scheduling | Do not create one thread per visualization. Each instance has a logical coordinator, not a dedicated thread, and submits fairly to a process-wide pool initially bounded to two workers. |
| Render handoff | Renderers consume stable, immutable snapshots rather than mutable analysis working memory. |
| Display timing | Offer per-instance **Fixed maximum** and **Adaptive** pacing, defaulting to Fixed maximum. Fixed maximum requests one exact rate equal to the active display's reported maximum; Adaptive requests from `min(60 Hz, maximum)` through that maximum and prefers the maximum. Reapply the selected request when the editor changes display. Both are best-effort Core Animation requests: measure the cadence and deadlines actually granted by the display-link update rather than inferring them from the request or advertised maximum. |
| Performance observability | Offer an opt-in, persisted, per-instance metrics panel in Release builds. Show every collected renderer and analysis metric, exact presented-frame pacing history, per-frame callback-to-presentation latency composition, derived rates, and copyable raw reports without mutating the host process. |
| DPI support | Treat layout units and render pixels separately and support both regular-density and Retina displays, including live movement between them. |
| Editor lifecycle | Stop sample capture, analysis, history, display-link activity, and Metal submission when the editor is closed, hidden, or occluded beyond a short debounce. Reopening starts with fresh analysis state. About preserves the lifecycle state present when it opens: an active dashboard remains live, while already-paused full-content Settings remains paused. |
| Analyzer milestone | The initial usable baseline was a large real-time FFT spectrum with compact mono/stereo sample-peak/RMS meters. The current dashboard also implements bounded shared-FFT Spectrogram history, fixed-scale Stereo field/correlation, and BS.1770-5 / EBU R128 M/S/I Loudness semantics. |
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

One producer-side overflow episode advances the public capture generation once.
Before any post-gap analyzer state can reach the renderer, the renderer must
independently consume both a tagged, fully invalid visualization frame and a
tagged Spectrogram reset marker for that generation. Renderer acknowledgements
use generation-qualified compare/exchange tokens, so a delayed copy from an old
lifecycle cannot clear a newer boundary. A destructively acquired raw chunk
keeps its slot's original discontinuity revision: pre-gap input is discarded,
while a post-gap raw handle or meter endpoint that discovers the gap is retained
across worker jobs. The producer's raw-gap revision is acknowledged only after a
post-gap immutable frame is successfully published; publication failure retries
the already-applied state without reprocessing input. Its cached frame covers
only the capture revision of the work that produced it, so audio arriving before
the retry remains schedulable afterward. This keeps one episode open during
sustained overload and prevents either blank-generation churn or loss of the
first recoverable data.

Peak/RMS measurements use a separate bounded coalescing accumulator so an
overloaded raw-sample queue does not silently erase the largest recent peak.
Its 300 ms RMS, live sample-peak release, held-peak decay, and OVER latch are
owned by the audio producer and advanced from every sample in source order. The
handoff publishes complete endpoint snapshots, so worker scheduling and endpoint
coalescing cannot change the temporal result. Host blocks larger than one raw
capture slot are processed in matching chunks so a detected reclaim/drop resets
the meter at that chunk boundary instead of retroactively resetting the start of
the host block. This producer work is bounded but must be included in audio
callback profiling before the preliminary budget is considered met.

Stereo correlation uses the same producer-owned endpoint principle. The audio
producer advances `E[L*R]`, `E[L^2]`, and `E[R^2]` from every sample in source
order with one 300 ms exponential time constant and publishes a bounded complete
endpoint. Worker scheduling, endpoint coalescing, and vectorscope display
decimation cannot change the result. Actual mono/stereo metadata accompanies the
endpoint; mono never becomes a duplicated pair or a synthetic `+1` correlation.
This bounded producer work is included in audio-callback profiling.

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

The implemented callback self-timer uses `mach_continuous_time()`. Its timebase
is initialized off the audio thread, and callback conversion uses only checked
64-bit quotient/remainder arithmetic. Five fixed cumulative histograms cover
exact 64, 128, 256, 512, and 1024-frame host blocks with 1 microsecond regular
buckets and one explicit `>= 1024 us` overflow bucket. The per-size budget is 2%
of block duration, with the 128-frame budget additionally capped at 25
microseconds. The p99 shown in Metrics is the conservative upper edge of its
histogram bucket; an overflow p99 is reported as unbounded rather than invented.
Histogram arrays are serialized only when Copy is requested.

Self-timing starts immediately before plugin callback work and takes its end
timestamp immediately after that work, so it necessarily excludes the final
clock read and bounded atomic-recording tail. External tracing remains the
authoritative `processBlock` entry-to-exit budget measurement. The in-process
bounded detector covers concurrent entry into one processor instance and
monotonic-clock regression only. Runtime allocation and lock/wait detectors are
not installed inside an arbitrary host process; Metrics reports both as
inactive. Their absence must never be presented as proof that no allocation or
lock occurred.

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

The Stereo worker path consumes retained raw chunks before Spectrum freshness
coalescing and owns only the transient vectorscope point history. It uniformly
decimates the latest 250 ms of captured-audio time to at most 4096 immutable
display points. Capture gaps reset that history instead of joining unrelated
segments; correlation remains producer-owned and uses every source sample.

There is one logical coordinator per plugin instance, but a coordinator is not a
thread. Coordinators submit work to a scheduler shared by all instances loaded
from the same plugin module in the current host process. The scheduler initially
owns two lower-priority, non-real-time worker threads.

At most one job per instance may be running or queued, plus a coalesced
"newer data available" indication. Schedule instances fairly so a costly view
cannot monopolize the pool. Worker count is configurable for benchmarks but may
change as a product default only from measurements. AUv2 and VST3 modules, or
hosts that isolate plugins in separate processes, naturally have separate pools.

Scheduler timing uses its steady monotonic clock and follows latest-wins
semantics literally. Queue wait begins at the request timestamp of the newest
retained request and ends when a worker begins that invocation. Job turnaround
uses the same retained timestamp and ends after the invocation returns. A
regular coordinator request carries its current analysis-period budget as a
diagnostic relative deadline; reset and reconfiguration jobs may remain
untagged. A miss is counted only when elapsed time is strictly greater than the
budget, and deadlines never cancel or delay work. Metrics keeps bounded
UI-sampled histories of the latest queue-wait and turnaround values when their
sample counters advance. Their p95/p99 values are useful polling-edge samples,
not exhaustive per-job percentiles.

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
When the editor becomes active again, advance the capture/lifecycle generation,
discard stale snapshots, reset holds/smoothing/history, and warm up from current
audio.

| Editor state | Sample handoff | Analysis | Display link and Metal |
| --- | --- | --- | --- |
| Attached and visibly active | Enabled | Enabled as needed | Enabled at display cadence |
| Minimized, host-hidden, or occluded beyond the debounce | Disabled | Cancelled and quiesced | Stopped |
| Detached, closed, or destroyed | Disabled | Cancelled and quiesced | Stopped and resources released as appropriate |

Returning from either inactive state begins a new capture/lifecycle generation;
the first release does not attempt to reconstruct the missing interval.

A sibling utility panel sharing the editor content area is not host occlusion.
Metrics retains a compact live Metal preview. About does the same when the
dashboard is active as it opens, preserving capture, analysis, history, display
timing, Metal submission, holds, Stereo state, and Loudness integration without
advancing the capture/lifecycle generation. If About opens from already-paused
full-content Settings, it remains full-content and preserves that paused state
instead of reactivating analysis. Closing About reveals current utility state.
If Settings is revealed on the other side of its 1080-point presentation
threshold after a resize, its existing lifecycle transition applies. Detailed
behavior lives in [the analyzer interface requirements](analyzer-ui.md).

JUCE's non-real-time `prepareToPlay` boundary reports the active sample rate and
mono/stereo layout to the coordinator. A changed format closes capture, cancels
and drains the old worker generation, retires its pending handoffs, publishes a
fully invalid fresh snapshot, and then reopens capture under a new generation.
Reporting the same format is a no-op. This prevents a short new-format meter
update from being combined with a still-valid spectrum from the previous format.

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

The per-instance Display setting configures that link with one of two Core
Animation frame-rate ranges. **Fixed maximum**, the default, requests
`(maximum, maximum, maximum)` using the active screen's reported maximum without
an Audio Insight 120 Hz cap. **Adaptive** requests
`(min(60 Hz, maximum), maximum, maximum)`, preferring the maximum while allowing
macOS to vary the cadence. An unavailable screen maximum falls back to 60 Hz.
Changing modes reapplies only this presentation request. Changing screens
reapplies it alongside the normal screen and backing-scale update. Neither
transition advances an analysis or lifecycle generation, clears histories or
measurements, or changes the host process's scheduling policy or any thread
priority. Core Animation may still deliver a different cadence from either
requested range.

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

Spectrogram Scroll keeps stored history on the captured-audio timeline but
paces its visible head from the display link's target-presentation clock. Start
one FFT slice behind the newest accepted timeline column, move discrete cells
by fractional-column offsets at the requested slice rate, and do not blend dB
values across time or across black timestamp gaps. Ordinary column bursts move
the data frontier without moving the display head backward. Clamp a delayed
head to one future cell; when that cushion is exhausted or the retained head has
expired, the next accepted column deliberately establishes a fresh one-slice
anchor. Overwrite mode retains its discrete physical-ring presentation.

Allow at most one Spectrogram texture-upload transaction in flight. While it is
outstanding, later display callbacks defer queue draining and ring mutation but
continue submitting the complete dashboard. Those later frames mask the fixed
set of destination columns until completion is observed. Matching success
promotes them by removing the mask; matching failure clears history because a
Metal command may have written only part of the set. History and texture
revisions make lifecycle-invalidated completions stale. Upload deferrals are
observable, while a full-frame upload-backpressure drop remains an expected-zero
regression alarm.

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
The raw report also identifies the configured pacing mode, active display's
reported maximum or 60 Hz fallback, and requested minimum, preferred, and
maximum rates so the request can be compared with actual callback and
presentation cadence.
Native effective-activity transitions stop and restart collection; retained
values are explicitly marked paused and may be stale. The full report is
assembled lazily when the user presses Copy, and neither exact 240-entry history
nor the five callback-duration histograms is serialized during live polling.
The panel sits beside the native Metal view
because an overlapping JUCE component cannot reliably appear above an
`NSViewComponent`. It defaults to off because formatting and painting
diagnostics has measurable overhead; important results should also be confirmed
with it hidden.

The rendering toolbox should favor simple, predictable GPU operations:

- line or triangle geometry for plots and scopes;
- instanced geometry for repeated meter elements;
- a scale-aware glyph atlas or equivalent GPU text solution.

The first Metal glyph atlas uses macOS's system monospaced font. This avoids a
bundled font asset and its additional redistribution/license obligations while
keeping compact analyzer labels predictable. Glyphs and fixed runs are built
outside the display callback and rebuilt only for density-dependent lifecycle
changes. A future visual-identity pass may deliberately replace the system font
after choosing and auditing an AGPL-compatible asset.

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

The Spectrogram renders time-frequency energy over a literal black background.
Its intensity palette should read as energy traces: by default, dark blue for
lower energy rising through orange and near-white for stronger energy. It must
not look like broad decorative color stripes.

Spectrogram chrome shows only numeric frequency tick labels, using Hz or kHz as
appropriate. Omit axis-title text, time/history labels, and dB/color-legend
labels.

## First usable release

The first usable release deliberately has one layout and two visualizations:

- a large real-time FFT spectrum occupying most of the surface, using the
  shared adjustable frequency-spacing transform; and
- a compact vertical meter strip showing honest sample peak and RMS, with one
  meter for mono input or distinct L/R meters for stereo.

The implemented five-tile dashboard keeps Spectrum, Peak/RMS, Spectrogram,
Stereo field/correlation, and Loudness live. Analyzer controls live in the
Settings inspector. Four grid-snapped splitters resize adjacent fixed tiles;
movement, reordering, hiding, detaching, overlap, and free-form windowing remain
out of scope. Do not label sample peak as true peak; that name is reserved for a
correctly oversampled true-peak implementation.

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

With the shell, Settings inspector, Spectrogram history, Stereo
field/correlation, and standards-validated Loudness implemented, Phase,
surround layouts, multi-instance aggregation, Loudness Range, and true peak
remain later work.

## Performance and validation

Performance is a product requirement, not a final optimization pass. Add
instrumentation early enough to observe:

- delivered frame cadence, frame-time distribution, and dropped/late frames;
- render CPU time and GPU time;
- analysis job latency and missed analysis deadlines;
- audio-callback CPU cost and any real-time safety violation;
- data drops, discontinuities, queue high-water mark, and snapshot age; and
- hidden-editor and multi-instance overhead.

Stereo observability separates producer correlation samples/endpoints from
worker vectorscope chunks and point publications. It also exposes scoped reset
and discontinuity counts, channel/correlation validity, current point count,
capture endpoints, snapshot sequences, and derived rates. As with all collected
telemetry, each raw field appears exactly once in Metrics and its copied report.

Loudness observability exposes worker input/completion counts, exact-gate block
counts, classified resets, readiness and completed-silence values, capture
endpoints, the 24-hour block capacity, capacity overflow, and renderer-accepted
state. Only cumulative counters receive derived rates. Exact Integrated gating
stores each finite block energy strictly above the fixed absolute gate once in a
preallocated, high-occupancy B+ tree. The first gate uses a cumulative exact
sum/count; the strict relative-gate query visits one boundary leaf plus bounded
subtree aggregates instead of rescanning the retained history. Metrics exposes
the index's reserved bytes, topology, query count, and last/maximum node,
aggregate, and boundary-value reads so its long-session cost remains visible.

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
| Frame delivery | At least 99.5% of display-requested presentations in Fixed maximum at the active display's reported maximum and in Adaptive across the cadence macOS grants; no sustained run of more than two plugin-attributable late frames. |
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
  bounded record, never a log entry. The built-in self-timer's omitted final
  clock-read/recording tail is not part of this external entry-to-exit definition.
- **Analysis execution time** measures job-body execution separately from queue
  wait. **UI-edge pipeline freshness** uses the attempted-capture frame frontier
  minus the latest Spectrum or Peak/RMS captured-frame endpoint. Frames are
  converted to nanoseconds only while capture generation and sample rate form a
  stable valid snapshot; zero age is a valid measurement. Bounded UI-side p95
  and p99 histories reset when the capture generation changes. These values are
  not presentation-time snapshot age, host/device latency, or proof of the final
  presentation budget. Presentation-age p99 still requires capture-to-Metal
  correlation or an external measurement.
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
  closed-editor and fairness attribution in a shared pool. Queue-wait and
  request-to-completion samples refer to the latest retained latest-wins request;
  their deadline counters use the request's optional relative analysis budget.

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
between regular-density and Retina displays, Fixed maximum and Adaptive pacing
across available display rates, live resize, and each substantially new
visualization. Do not claim visual correctness merely because the project
compiles or a validator passes.

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
7. Build the fixed-topology five-tile Metal shell, numeric-axis text
   infrastructure, four constrained splitters, edit interaction, and global
   layout persistence. Keep unfinished tiles inert.
8. Add the mutually exclusive Settings inspector, migrate analyzer configuration
   to versioned non-automatable per-instance state, connect the shared FFT and
   frequency controls, and finish Spectrum and Peak/RMS presentation.
9. Implement the literal-black Spectrogram with shared frequency mapping,
   bounded Scroll/Overwrite history, and palette/response controls.
10. Implemented: add the mono-aware Stereo field from worker-owned, uniformly
    decimated captured-sample history and all-sample, producer-owned correlation
    statistics.
11. Implemented: add Momentary, Short-term, and Integrated Loudness after the
    ITU-R BS.1770-5 / EBU R128 path passes published alignment and relative-gate
    reference tests.

The detailed defaults and acceptance behavior for steps 7–11 live in
[the analyzer interface requirements](analyzer-ui.md). A placeholder does not
authorize fake values, background work, or speculative resource allocation.

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
- Should the first-pass macOS system monospaced analyzer font eventually be
  replaced, and which audited AGPL-compatible visual assets will accompany it?
- What copyright-holder name should appear in project and in-plugin legal
  notices?
- Is Audio Insight the final display name or only the working title, and what
  bundle identifiers, manufacturer code, and plugin subtype will be registered?

## Decision log

### 2026-08-16

- Added per-instance Fixed maximum and Adaptive display pacing, defaulting to
  Fixed maximum. Fixed maximum requests the active display's complete reported
  maximum without a 120 Hz cap; Adaptive permits `min(60 Hz, maximum)` through
  that maximum and prefers it. Both Core Animation requests are best-effort,
  remain presentation-only, leave analyzer state and host scheduling untouched,
  and expose their requested range beside actual cadence telemetry.
- Made Spectrogram Scroll presentation-time fractional with a one-slice cushion
  while retaining discrete dB cells and black timestamp gaps. Upload contention
  now defers column draining and masks pending destinations instead of skipping
  a whole dashboard frame; revisions reject stale completion results.
- Made About presentation lifecycle-preserving rather than state-restoring. An
  active dashboard keeps a right-side About panel and compact live Metal preview
  at a preferred 50% width, clamped to 360–700 logical points while leaving at
  least 320 points for Metal. Already-paused full-content Settings instead keeps
  About full-content and paused. About does not mutate or roll back Settings or
  Metrics state; closing reveals the current state under the current-width
  Settings presentation rule.
- Made capture-discontinuity rollover a two-artifact renderer fence with
  generation-qualified acknowledgements, cancellation-safe episode commit,
  post-gap destructive-acquisition retention, and delayed producer
  acknowledgement after a successful recovery-frame publication.
- Added fixed-storage audio-callback self-timing for exact 64–1024-frame blocks,
  conservative lifetime p99 bounds, explicit overflow, and honest detector
  coverage. External profiling remains required for true callback entry-to-exit
  validation.
- Added latest-retained scheduler queue-wait and request-to-completion timing
  with diagnostic analysis-period deadlines and strict miss accounting.
- Defined Spectrum and Peak/RMS freshness at the UI telemetry sampling edge and
  kept it distinct from presentation-time age and end-to-end audio latency.

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
- Scoped the first usable release to mono/stereo sample-peak/RMS meters and a
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
  over literal black, with only numeric frequency ticks shown.
- Accepted one fixed-topology, five-tile dashboard with no Focus Spectrum mode,
  tile movement, reordering, hiding, detaching, or overlap. Four clamped splitters
  provide width/height adjustment on a normalized grid.
- Chose one versioned per-user global layout. Analyzer settings remain
  non-automatable per-instance project state, while live histories, holds,
  Loudness integration, Settings/About visibility, and unfinished layout edits
  remain transient; the Metrics toggle remains persisted per instance.
- Made Settings and Metrics mutually exclusive with deterministic Metrics
  restoration. Settings may cover and pause the canvas at narrow widths;
  Metrics always retains a compact live Metal preview so observation does not
  stop the renderer.
- Chose inert placeholders for unfinished dashboard panels, followed by
  Spectrogram, Stereo field/correlation, and standards-validated Loudness in
  that order.
- Accepted both Scroll and Overwrite Spectrogram modes on bounded circular
  `R16Float` history, with frequency rows encoded in the shared mapping.
- Required real mono/stereo channel-layout metadata throughout analysis. Mono is
  never duplicated for BS.1770 summation or presented as a synthetic stereo
  correlation result.
- Resolved the remaining panel algorithms, scales, labels, controls, defaults,
  persistence, accessibility, and reset behavior in `docs/analyzer-ui.md`.
- Moved Floor, Ceiling, and time-based Temporal averaging from the toolbar into
  the Spectrum settings section. Temporal averaging now operates on calibrated
  power with Off plus a logarithmic 25–2000 ms range and a responsive 75 ms
  default. A separate fixed 6 ms renderer interpolation only bridges analysis
  snapshots at display cadence and is not exposed as a setting.
- Chose the macOS system monospaced font for the first scale-aware Metal glyph
  atlas, avoiding a bundled font asset while keeping rasterization and cached
  run construction outside the display callback.
- Moved Peak/RMS temporal math to producer-owned sample-domain ballistics and
  made the bounded meter handoff publish complete endpoint snapshots. Worker
  polling and coalescing therefore cannot change RMS, release, hold, or OVER
  results.
- Made changed host sample rates and mono/stereo layouts start a clean capture
  generation at `prepareToPlay`; identical format notifications are no-ops.
- Implemented the fixed-scale Stereo vectorscope and correlation tile. The
  audio producer owns all-sample 300 ms correlation expectations, while shared
  workers own only the uniformly decimated 250 ms point cloud.
- Scoped Stereo resets to capture/lifecycle generations and discontinuities;
  FFT-only changes preserve Stereo state. Added separate producer, worker,
  publication, validity, endpoint, and reset telemetry to Metrics.
- Implemented BS.1770-5 K-weighted mono/stereo Momentary, Short-term, and exact
  two-gate Integrated Loudness on the shared worker path. Rational nearest-sample
  100 ms boundaries preserve literal 400 ms and 3 second windows without drift.
- Bounded exact Integrated retention at 864,000 blocks (24 hours). The first
  excess block invalidates I and reports capacity overflow instead of rolling or
  approximating the gate.
- Replaced twice-per-update linear Integrated Loudness rescans with a
  preallocated exact ordered index. The reference arm64 build reserves
  7,606,712 bytes (about 7.25 MiB) of owned index storage at full capacity,
  remains below the accepted 8 MiB arena budget, and performs no processing-time
  allocation.
- Kept Loudness RESET boundary-aware across queued capture data, made stale input
  clear only M/S, and preserved Loudness across FFT-only configuration changes.
