# Analyzer dashboard interface requirements

> **Status:** Accepted target design and implementation defaults
>
> **Established:** 2026-08-15
>
> **Last updated:** 2026-08-16

This document is the durable source of truth for Audio Insight's analyzer
dashboard, layout editing, settings inspector, and per-panel presentation. The
interactive layout prototype is illustrative; when it conflicts with this
document, this document wins.

System architecture, real-time safety, rendering, lifecycle, and distribution
remain defined by [the architecture and decision record](architecture.md).
Numeric defaults below are accepted starting values. They may be tuned from
profiling, standards conformance tests, or user visual review, with the reason
recorded in the architecture decision log.

## Scope and implementation sequence

The current implementation contains:

- one large real-time FFT Spectrum;
- one compact vertical mono/stereo sample-peak/RMS meter; and
- one bounded, shared-FFT Spectrogram history view;
- one fixed-scale Stereo vectorscope with all-sample correlation; and
- one BS.1770-5 / EBU R128 Momentary, Short-term, and Integrated Loudness meter.

The complete dashboard shell, Settings inspector, constrained layout editor,
Spectrum, Peak/RMS, Spectrogram, Stereo field/correlation, and Loudness are
implemented.

Implement the remaining dashboard in this order:

1. Implemented: add the five-tile shell, numeric-axis text infrastructure, four
   layout splitters, edit mode, global layout persistence, and Settings
   inspector.
2. Implemented: finish Spectrum and Peak/RMS presentation and connect the shared
   settings.
3. Implemented: replace the Spectrogram placeholder with the shared-FFT history
   view.
4. Implemented: replace the Stereo field/correlation placeholder.
5. Implemented: replace the Loudness placeholder after its standards-based
   measurements pass reference tests.

Replacing a placeholder must preserve its tile identity and default position.
This sequence does not require an unfinished placeholder to run background
analysis.

## Dashboard model

- Use one dashboard view containing one instance of every panel.
- Do not add Overview or Focus Spectrum modes, duplicate Spectrum, or hide the
  rest of the dashboard behind a focus switch.
- Keep the panel order and adjacency fixed.
- Do not allow moving, reordering, hiding, detaching, or floating panels.
- Keep utility UI such as Settings, Metrics, and About outside the analyzer
  tile grid.
- Keep the whole editor resizable.

All tiles are logical regions of one Metal canvas, not separate native views or
renderers.

### Default tile geometry

Lay out the dashboard below the toolbar on a normalized 48-column by 40-row
grid. Grid bounds below are half-open. Use an 8-logical-point outer inset and
8-logical-point gutters; split percentages describe the available track span
before those fixed gutters are subtracted.

The compiled default split indices are:

- horizontal row split: `22 / 40`;
- upper-row column split: `36 / 48`; and
- lower-row column splits: `28 / 48` and `40 / 48`.

| Panel | Grid bounds | Default proportion |
| --- | --- | --- |
| Spectrum | columns `0..36`, rows `0..22` | 75% of upper width, 55% of dashboard height |
| Peak/RMS | columns `36..48`, rows `0..22` | 25% of upper width |
| Spectrogram | columns `0..28`, rows `22..40` | 58.33% of lower width, 45% of dashboard height |
| Stereo field/correlation | columns `28..40`, rows `22..40` | 25% of lower width |
| Loudness | columns `40..48`, rows `22..40` | 16.67% of lower width |

These proportions encode the requested dominant Spectrum, narrow Peak/RMS,
large Spectrogram, supporting Stereo field, and particularly slim Loudness
panel.

### Constrained layout editing

Provide an explicit **Edit layout** mode with exactly four adjustable splitters:

- the horizontal row boundary;
- the Spectrum/Peak-RMS boundary;
- the Spectrogram/Stereo boundary; and
- the Stereo/Loudness boundary.

Dragging a splitter transfers space between adjacent panels. It does not push
other tiles, reorder them, create overlap, or move an outer dashboard edge.
Outer-edge resizing remains editor-window resizing.

Snap each splitter to the normalized grid and clamp before publishing one
immutable layout snapshot. The constraints are:

- horizontal split: rows `14..26`, leaving at least 35% height in each band;
- upper split: columns `24..40`, leaving Spectrum at least 50% and Peak/RMS at
  least 16.67% of the upper width;
- Spectrogram width: at least 16 columns;
- Stereo field width: at least 8 columns;
- Loudness width: 6 to 12 columns; and
- lower splitters cannot cross.

Because adjacency is fixed, collision handling is only clamping ordered
splitters; there is no tile-placement algorithm.

### Edit interaction and accessibility

- Show subtle visual handles only in edit mode, but give each at least a
  24-logical-point pointer hit band.
- Tab and Shift-Tab cycle the four splitters.
- Axis-appropriate arrow keys move the focused splitter by one grid track.
- Home and End move it to its legal extremes.
- Expose each handle as an accessible adjustable separator. Its name and value
  identify both affected panels and their current percentages.
- **Done** commits and persists the working layout.
- **Cancel** or Escape restores the layout present when edit mode opened.
- **Reset layout** loads the compiled defaults into the working edit; **Done**
  must still be pressed to persist them.
- Closing or destroying an editor during an edit discards the uncommitted
  working layout.

### Global layout persistence

Layout is one versioned, per-user global presentation preference shared by AUv2
and VST3. It is not a host parameter and is not serialized into plugin-instance
or DAW-project state.

Persist only the four integer split indices after **Done**, using a
non-audio-thread user preference store with atomic replacement and
cross-process serialization. Last completed edit wins. New editors load the
global layout; already-open editors do not live-sync. Missing, malformed, or
unknown versions use the compiled defaults.

## Utility panels

### Settings inspector

Open Settings from the toolbar as a temporary right-side inspector, not an
analyzer tile. A sibling JUCE component cannot reliably overlap the native Metal
view, so Settings reserves space beside the canvas when there is room.

Use a 360-logical-point inspector only when at least 720 logical points remain
for the dashboard. At narrower editor widths, Settings takes the complete
content area below the toolbar, becomes vertically scrollable, and the covered
Metal canvas pauses. The tile topology itself never reflows.

Organize controls into Display, Shared analysis, Spectrum, Peak/RMS,
Spectrogram, Stereo, and Loudness sections. Settings documented in each panel
section below live in that inspector. Changes apply immediately and are saved
with the plugin instance; there is no separate Apply step. Give each section
with adjustable settings a reset-to-default action. Stereo's initial
presentation is deliberately fixed, so its live section states that it has no
adjustable settings and leaves Reset visibly unavailable. Loudness exposes its
presentation-only reference setting; the section reset restores `-23 LUFS` and
does not reset the transient Integrated measurement. Integration is reset only
by the Loudness tile's explicit RESET action and the lifecycle boundaries
specified below.

Floor, Ceiling, and Temporal averaging live in the Spectrum section rather than
the toolbar; do not duplicate them. Temporal averaging exposes Off plus a
logarithmic 25–2000 ms range and defaults to a responsive 75 ms. It controls the
average-power analyzer defined below, not display interpolation. The Metal
renderer separately applies a fixed 6 ms interpolation to bridge analysis
snapshots smoothly at display refresh rate; that short presentation step is not
user-adjustable or serialized.

Temporal averaging is the Settings-inspector replacement for the earlier
normalized `Smooth` control. Its 75 ms default is approximately `0.37` on that
legacy control's response curve, keeping the initial amount within the requested
`0.3–0.5` range without retaining an ambiguous unitless control or duplicating it
in the toolbar.

### Display pacing

| Setting | Accepted choices | Default |
| --- | --- | --- |
| Frame pacing | Fixed maximum, Adaptive | Fixed maximum |

**Fixed maximum** requests one exact frame rate equal to the active display's
reported maximum. Audio Insight does not cap that request at 120 Hz, so a future
display reporting a higher maximum receives that higher request. If no valid
display maximum is available, use 60 Hz as the fallback.

**Adaptive** requests a Core Animation range from
`min(60 Hz, display maximum)` through the display maximum and prefers the
maximum. A display whose maximum is below 60 Hz therefore receives an exact
request for its own maximum.

Both choices are best-effort requests: macOS, the host, and the compositor may
deliver another cadence. Actual display-link callbacks and presentation
timestamps remain authoritative. The setting is non-automatable per-instance
state, applies immediately, and is reapplied after movement to another display.
Display Reset restores Fixed maximum. Switching modes changes presentation
timing only; it does not advance analysis or lifecycle generations, clear
Spectrogram history, Spectrum state, meter holds, Stereo state, or Loudness
integration, or change process/thread scheduling priority.

### Utility-panel exclusivity

Settings and Metrics do not coexist:

- Opening Settings records whether Metrics was visible, temporarily hides it
  without changing its persisted toggle, then opens Settings.
- Closing Settings with its close action, toolbar button, or Escape restores
  Metrics if that recorded value was true.
- Pressing Metrics while Settings is open is a switch-to action: it cancels
  automatic restoration, closes Settings, enables Metrics if necessary, and
  shows it. To turn Metrics off, switch to it and press Metrics again.
- Accessibility state distinguishes “Metrics requested but temporarily hidden”
  from Metrics being off.

Metrics always keeps a live Metal preview so the act of observing renderer
performance does not stop the renderer. Prefer 43% of the content width for the
Metrics panel, clamped to 360–720 logical points while leaving at least 320
logical points for the preview. The compact preview keeps the five-tile
topology, but may suppress secondary labels and layout editing. Metrics' own
vblank-paced graphs and throttled numeric/table updates continue normally.

About is mutually exclusive in presentation with Settings and Metrics, but it
does not change either panel's underlying state. Opening About temporarily hides
the current utility presentation. Closing it reveals the then-current Settings,
Metrics, or dashboard state, including a legitimate Metrics request or other
state change observed while About was open; it does not roll state back to an
entry snapshot.

When the dashboard is active as About opens, show About as a right-side sibling
panel with a compact live Metal preview. Prefer 50% of the content width, clamp
the panel to 360–700 logical points, and always leave at least 320 logical points
for Metal. This presentation keeps sample capture, analysis, history
accumulation, display-link callbacks, and Metal submission active, without
advancing a capture/lifecycle generation or resetting Spectrogram history,
Spectrum averaging or peak holds, Peak/RMS holds or OVER latches, Stereo state,
or Loudness integration.

If About opens while narrow-width, full-content Settings has already paused the
dashboard, show About full-content as well and preserve that existing paused
lifecycle; do not reactivate and immediately reset analysis merely to provide a
preview. About's body scrolls vertically as needed in either presentation, while
its title and Close control remain fixed and reachable.

Closing About applies Settings' presentation rule at the editor's current
width. Consequently, resizing across Settings' 1080-logical-point side/full
threshold while About is open can legitimately cross the existing Settings
lifecycle boundary when Settings is revealed. That transition belongs to the
current Settings state and width, not to an About state rollback.

## Saved settings and host automation

In a DAW, an **automatable** parameter can appear on a timeline automation lane
and be changed by the host during playback. Audio Insight's layout and analyzer
presentation settings do not alter audio, and rapid FFT/window changes would
rebuild analysis state. Therefore none of the following is host-automatable in
the initial product:

- layout;
- display frame pacing;
- FFT size, window, or requested FFT slice rate;
- shared frequency spacing;
- Spectrum or Spectrogram presentation settings;
- Spectrogram history mode or duration; or
- panel-specific meter, scope, or loudness presentation settings.

Save display pacing and analyzer configuration as non-automatable per-instance
plugin state so a DAW project can recall different analyzer views on different
instances. Keep layout in the global store described above. Metrics remains a
non-automatable per-instance toggle. Settings visibility, edit-mode working
state, active history samples, holds, and loudness integration are transient and
are not serialized.

After a user-originated analyzer configuration edit, notify the host that
non-parameter plugin state changed so it can mark the project for re-saving.
Loading or restoring existing state must not itself be reported as a new edit.

The current analyzer-configuration schema is version 2. It adds Display pacing.
An analyzer-configuration subtree from schema 1 or any other unknown version is
intentionally rejected and replaced with current defaults; settings backward
compatibility is not a requirement before the first public release. Missing or
malformed values inside a structurally recognized schema-2 tree use their
documented defaults.

The older `spectrumFloor`, `spectrumCeiling`, and `spectrumSmoothing` parameter
IDs remain deprecated, non-automatable compatibility shims with their original
normalized mappings. A state predating the analyzer-configuration subtree may
still seed the current defaults from those shims:

- preserve the physical floor and ceiling values, clamp them to the new ranges,
  and lower the floor if necessary to enforce the 24 dB minimum span;
- map a legacy normalized smoothing value `x <= 0` to Off, otherwise map it to
  `clamp(15 + 435 * x^2, 25, 2000)` milliseconds, approximating the legacy
  renderer's release time; and
- write the result into the versioned per-instance configuration. On later
  loads that new configuration wins over the compatibility shims.

Legacy DAW automation lanes are intentionally not supported after this
pre-release migration. The retained shims do not imply compatibility with
schema-1 analyzer configuration.

## Shared analysis controls

| Setting | Accepted choices | Default |
| --- | --- | --- |
| FFT size | 1024, 2048, 4096, 8192, 16384 | 4096 |
| Window | Rectangular, periodic Hann, four-term Blackman-Harris, five-term flat-top | periodic Hann |
| Requested FFT slice rate | 15, 30, 60, 120 Hz | 60 Hz |

All non-rectangular windows use the periodic convention. For sample index `n`
in a transform of length `N`, let `x = 2 * pi * n / N` and use:

```text
Hann:              0.5 - 0.5 cos(x)
Blackman-Harris:   0.35875 - 0.48829 cos(x) + 0.14128 cos(2x)
                   - 0.01168 cos(3x)
Flat-top:          0.21557895 - 0.41663158 cos(x) + 0.277263158 cos(2x)
                   - 0.083578947 cos(3x) + 0.006947368 cos(4x)
```

Correct every window by its coherent gain and use one-sided FFT normalization so
a bin-centered full-scale sine reads 0 dB internally. FFT size, window, and
requested slice-rate changes occur off the audio callback, advance the FFT
generation, discard incompatible snapshots, and reset overlap, Spectrum
temporal state, and Spectrogram history.

The requested FFT slice rate controls only new Spectrum FFT snapshots and
Spectrogram history columns. It does not throttle Peak/RMS capture from every
audio block, Stereo field/correlation sampling, or Loudness' fixed 100 ms
measurement completions. It is also not the render rate: Metal follows the
selected Display pacing request, potentially above 120 Hz when the active
display reports it, and consumes or interpolates the newest complete snapshot.
The fair, latest-wins scheduler may skip stale FFT work rather than building a
backlog. Telemetry reports both requested and achieved FFT slice rates.

For the single Spectrum and Spectrogram views, combine stereo channels using the
greater per-bin power so out-of-phase material does not cancel. Analyze a mono
input once rather than duplicating it as a synthetic stereo pair.

### Invalidation scopes

Use scoped generations rather than one counter that resets unrelated analyzers:

- A capture/lifecycle generation changes after editor reactivation, sample-rate
  or channel-layout changes, and capture discontinuities. It invalidates every
  analyzer's temporal state.
- An FFT generation changes after FFT size, window, or requested slice-rate
  changes. It invalidates FFT overlap, Spectrum averaging/holds, and Spectrogram
  history, but not Peak/RMS, Stereo, or Loudness state.
- A Spectrogram-mapping generation changes with shared frequency spacing. It
  clears mapped Spectrogram history but does not invalidate FFT results or any
  other analyzer.
- Display pacing, layout, palette, color response/range, and loudness-reference
  changes are presentation-only and do not advance an analysis generation.

## Shared frequency spacing

Store one spacing value `s` in `[0, 1]`:

- `0` is Linear;
- `1` is Logarithmic and is the default; and
- `0.25`, `0.5`, and `0.75` are labeled intermediate marks, while the control
  remains continuous.

For displayed frequency `f` between `f0` and `f1`:

```text
linear(f) = (f - f0) / (f1 - f0)
log(f)    = ln(f / f0) / ln(f1 / f0)
u(f, s)   = (1 - s) * linear(f) + s * log(f)
```

Use `f0 = 20 Hz` and `f1 = min(20 kHz, Nyquist)`. Spectrum maps `x = u`.
Spectrogram maps `y = 1 - u` so high frequency is at the top. Data, traces,
gridlines, hit testing, and numeric labels all use the same transform.

Build any inverse map or lookup table off the audio thread when spacing, bounds,
or sample rate changes. A presentation-only spacing change reuses the current
FFT and does not calculate another FFT. Spectrum remaps immediately;
Spectrogram starts a new history because its stored rows use the prior mapping.

Generate a bounded hierarchy of scale-appropriate “nice” frequency candidates
from the current logical axis length and measured glyph bounds; do not maintain
a hand-enumerated label list. Reserve exact endpoints first, prioritize
`1/2/5 * 10^n` anchors, then fill the widest mapped gaps with progressively
finer decimal candidates only while their measured labels remain separated.
Apply the same deterministic selector to Spectrum and Spectrogram. Use integer
Hz below 1 kHz and compact kHz above it.

## Spectrum

Spectrum remains the dominant real-time FFT visualization.

### Presentation

- Show only numeric horizontal frequency ticks in Hz/kHz and vertical level
  ticks such as `-48 dB`.
- Omit `X FREQUENCY`, `Y LEVEL`, and `dBFS` axis-title chrome. Values are
  calibrated internally as dBFS even though compact ticks say `dB`.
- Display 20 Hz through `min(20 kHz, Nyquist)`.
- When either visible endpoint falls between FFT-bin centres, interpolate the
  two bracketing bins in linear power and clip the trace and fill to that exact
  endpoint. This removes a decorative empty edge without claiming finer FFT
  resolution than the selected transform provides.
- Select vertical tick steps from 6, 12, 24, or 48 dB according to panel height,
  retaining at least 28 logical points between labels.
- Use the shared continuous frequency transform.

### Settings and defaults

| Setting | Range or choices | Default |
| --- | --- | --- |
| Floor | -180 to -36 dB, 1 dB steps | -90 dB |
| Ceiling | -24 to +12 dB, 1 dB steps | 0 dB |
| Minimum visible span | 24 dB | 90 dB from defaults |
| Temporal averaging | Off or 25–2000 ms, logarithmic control | 75 ms |
| Slope compensation | 0, +3, +4.5, +6 dB/octave, referenced at 1 kHz | 0 dB/octave |
| Peak-hold duration | Off, 0.25–10 seconds, or Infinite | Off; 2 seconds when first enabled |
| Hold decay | Fixed at 12 dB/s after a finite hold | 12 dB/s |
| Trace color | sRGB color value | project cyan |
| Fill opacity | 0–50%; zero means Off | 18% |

Average power, not already-converted dB values:

```text
a = exp(-elapsed / timeConstant)
averagePower = a * previousPower + (1 - a) * currentPower
```

Rendering interpolation remains separate from this analyzer averaging. Slope
compensation is presentation-only and the default zero keeps the dB axis
literal. Peak hold operates on unsmoothed per-bin power; apply the current slope
to both live and held traces only at presentation. Explicit Clear, capture
discontinuity, editor reactivation, and FFT/window/sample-rate changes reset
averaging and holds.

## Peak/RMS

- Use a compact, narrow, vertical stereo L/R strip.
- Use a linear-in-dB scale from -60 to +3 dBFS with major ticks at -60, -48,
  -36, -24, -12, -6, 0, and +3.
- Measure sample peak from the maximum absolute value of every captured sample;
  do not decimate this path.
- Measure RMS as an unweighted exponential mean square with a 300 ms time
  constant, then take its square root and convert with `20 log10`.
- Do not apply an AES17 +3.01 dB sine calibration offset. A full-scale sine
  therefore reads approximately -3.01 dBFS RMS.
- Render RMS as the wider muted fill and sample peak as a thinner brighter
  indicator.
- Sample peak has instantaneous attack and 20 dB/s visual release.
- Hold sample peak for 2 seconds, then decay the hold marker at 20 dB/s.
- Use neutral/project cyan below -6 dBFS, amber from -6 to below 0 dBFS, and red
  only at or above 0 dBFS.
- Latch an `OVER` indicator at or above nominal full scale. Do not label it
  `CLIP` because floating-point samples at 0 dBFS do not prove waveform
  clipping.
- A user reset clears both channel holds and `OVER` latches.
- Show one-decimal sample-peak readouts when space permits and `-∞` below
  the measurement floor.
- For mono input, show one centered meter labeled `M`; do not render two
  identical meters labeled L and R.

Capture/lifecycle-generation changes, capture discontinuities, and editor
reactivation reset temporal meter state. FFT-generation changes do not. Host
transport stop, seek, or loop alone does not clear a hold or `OVER` latch.

Do not claim IEC PPM ballistics or true peak. Oversampled true peak/dBTP,
selectable RMS standards, and long-term maximum history are deferred.

## Spectrogram

### Presentation

- Render time-frequency energy as intensity-colored traces over literal black.
- The default Blue Fire palette rises from black through dark navy/blue and
  orange toward near-white, matching the supplied reference.
- Preserve real trace-like energy. Do not substitute broad decorative bands,
  rainbow gradients, or a generic striped field.
- Use the shared frequency transform for both energy and tick placement.
- Show only numeric frequency ticks in Hz or kHz.
- Omit axis-title text, time/history labels, and a visible dB/color legend.
  Time/history exists internally despite the unlabeled horizontal axis.
- Use unsmoothed FFT slices; Spectrum averaging must not smear history.

### Settings and defaults

| Setting | Range or choices | Default |
| --- | --- | --- |
| Palette | Blue Fire, Inferno, Viridis, Grayscale | Blue Fire |
| Color response | -2 to +2 | 0 |
| Color floor | -180 to -36 dB, 1 dB steps | -120 dB |
| Color ceiling | -24 to +12 dB, 1 dB steps | 0 dB |
| Minimum color span | 24 dB | 120 dB from defaults |
| History duration | 2, 5, 10, 20, 30, 60 seconds | 10 seconds |
| History mode | Scroll or Overwrite | Scroll |

Every palette begins at black and has monotonically increasing perceived
luminance. Store calibrated dB, normalize it, then apply response:

```text
v = clamp((dB - floor) / (ceiling - floor), 0, 1)
gamma = 2 ^ response
paletteCoordinate = v ^ gamma
```

Zero is linear in dB. Negative response reveals quieter detail; positive
response suppresses low energy and isolates stronger traces. This is unrelated
to frequency spacing.

In Scroll mode, newest history is at the right and older history moves left. In
Overwrite mode, the write head advances left-to-right, wraps, and shows a subtle
one-pixel seam. Switching mode reinterprets the same ring and does not clear
history.

### Bounded history storage

- Keep one renderer-owned circular `R16Float` Metal texture containing dB, not
  precolored RGBA.
- Palette, dB range, and response changes recolor existing history in the
  shader without clearing it.
- Use `min(1024, usable FFT bins)` frequency rows. Each row represents one
  uniform interval in the current shared coordinate `u`, not a linear FFT-bin
  index. The shader reverses row direction so high frequency appears at top.
- On a non-audio worker, inverse-map each row interval to frequency. Preserve a
  narrow trace by taking the greatest calibrated bin power in that interval; if
  it contains no bin center, linearly interpolate power at its center. Convert
  that result to dB for the stored column.
- Allocate `ceil(history seconds * requested FFT slice rate)` columns, capped at
  8192.
- Upload immutable columns through preallocated staging buffers in the normal
  frame command buffer. Never upload or allocate from the audio callback.
- Scroll mode remaps circular texture coordinates; never copy the whole texture
  to scroll pixels. Overwrite mode draws physical ring order.
- In Scroll mode, start the visible head one requested-rate slice behind the
  newest accepted column and advance it fractionally from display target time.
  Shift discrete time cells without interpolating their dB values; stored black
  gaps remain black. Ordinary analysis bursts do not move the head backward.
  Freeze at one future cell if analysis stalls, then re-anchor when new data
  arrives; re-anchor as well if the retained display head has expired.
- A small bounded non-audio SPSC column queue may let a slower render consume
  multiple analysis columns without stretching time.
- Keep no more than one texture-upload transaction in flight. During a busy
  upload, submit the rest of the dashboard normally, do not drain another
  Spectrogram column, and mask that transaction's destination columns in later
  frames until success is observed. Clear history after a matching failure and
  ignore completion made stale by a later history or texture revision.
- Advance history by timestamps. Missing/coalesced intervals become black gap
  columns rather than making history appear slower than real time.

FFT size, window, sample rate, FFT slice rate, or history-duration changes clear
history and reallocate it if a texture dimension changed. Frequency-spacing
changes clear the now-incompatible row mapping without reallocating an
unchanged-size texture; seed the next column from the latest FFT snapshot rather
than performing a new FFT. Palette, color range, response, layout resize, and
Scroll/Overwrite changes neither clear nor reallocate history.

Stopping or hiding the editor clears Spectrogram history under the accepted
zero-background-work lifecycle.

## Stereo field and correlation

- Use a conventional rotated vectorscope:

  ```text
  x = (right - left) / 2
  y = (left + right) / 2
  ```

- Mono is vertical, anti-phase is horizontal, left-only slopes upper-left, and
  right-only slopes upper-right.
- Use fixed full-scale coordinates rather than per-frame auto-normalization, so
  quiet signals do not falsely appear full-scale.
- Draw a bounded point cloud from at most 4096 uniformly decimated sample pairs
  covering the latest 250 ms of captured-audio time. The existing shared worker
  coordinator owns this transient history and its display decimation. It
  consumes retained capture chunks before Spectrum freshness coalescing, fades
  point alpha with age, and never connects decimated points with misleading
  lines.
- Use a near-black background, subdued neutral axes, and project-cyan points
  that brighten where density overlaps.
- Compute correlation from all samples, not decimated display points. The audio
  producer advances its bounded correlation accumulator from every sample in
  source order and publishes complete endpoint snapshots; worker scheduling,
  display decimation, and endpoint coalescing therefore cannot change the
  temporal result. Accumulate all three expectation terms with the same 300 ms
  exponential time constant:

  ```text
  correlation = E[left * right] / sqrt(E[left^2] * E[right^2])
  ```

- Show an em dash when either exponentially averaged channel RMS is below
  -90 dBFS because correlation is undefined there.
- Integrate a -1 to +1 correlation strip in the same tile, with minor ticks at
  -0.5 and +0.5.
- Use cyan for positive values, neutral gray near zero, and amber for negative
  values; avoid green/red pass/fail semantics.
- For mono input, show a centered vertical mono trace and an explicit `MONO`
  state, but show correlation as an em dash rather than claiming `+1` for a
  synthetic duplicated pair.

Correlation has no peak hold or manual reset. Capture/lifecycle-generation
changes and discontinuities reset its smoothing and point history;
FFT-generation changes do not. Adjustable scope gain, alternate polar modes,
density heatmaps, correlation history, and multichannel scopes are deferred.

Stereo telemetry distinguishes producer-owned correlation input and endpoint
publication from worker-owned point-history processing. Report the applicable
sample/chunk counts, resets and discontinuities, current point count, channel
mode, correlation validity, capture/frame endpoint, snapshot sequence, and
derived rates in Metrics. Every collected Stereo field appears in the raw
Metrics table and copied report exactly once.

## Loudness

Implement current ITU-R BS.1770-5 K-weighting and channel summation with EBU R128
measurement semantics. Published alignment and relative-gating reference cases
must pass before exposing any LUFS label. Describe the result as
“BS.1770-5 / EBU R128 M/S/I semantics,” not complete “EBU Mode compliance,”
which also requires controls and measurements deliberately outside this scope.

Initial measurements are:

- Momentary (`M`): ungated 400 ms;
- Short-term (`S`): ungated 3 seconds; and
- Integrated (`I`): gated 400 ms blocks with 75% overlap, a strict `>-70 LUFS`
  absolute gate, then one non-iterative strict `>-10 LU` relative gate computed
  from the absolute-passing blocks.

Complete measurements on rational nearest-sample 100 ms boundaries so unusual
sample rates alternate adjacent integer hop lengths without cumulative drift.
Momentary and Short-term retain literal nearest-sample 400 ms and 3 second
rectangular windows. Apply no additional user smoothing to Loudness. Rendering
remains display-linked but does not invent new values between analysis updates.

- Use a particularly slim tile with a centered vertical bar.
- The bar and primary value above it show Momentary loudness.
- A thin bar marker shows Short-term loudness.
- Put Short-term and Integrated numeric readings below the bar.
- Use a linear -60 to 0 LUFS scale and one-decimal readouts.
- Use a neutral/project-cyan bar and an amber reference line.
- The presentation-only reference is adjustable from -36 to -9 LUFS in 0.5 LU
  steps and defaults to -23 LUFS; it does not alter measurement or gating.
- Show an em dash until a measurement window is ready and `-∞` for a
  completed silent window.

Integrated loudness begins when its panel becomes active. An explicit **Reset
loudness** begins a new integration. Its state is transient and also resets on
sample-rate/channel-layout change, capture discontinuity, or editor
reactivation. Do not reset merely on host play, stop, seek, or loop, and do not
serialize integrated state.

Retain exact Integrated history for at most 24 hours (`864,000` 100 ms blocks).
Count every completed block against that limit. Because the absolute gate is
fixed, retain one exact copy only of each finite energy strictly above it, plus
the exact cumulative sum/count needed by the first gate; omitting energies that
can never pass the absolute gate is lossless. Store the retained values in a
preallocated high-occupancy ordered index. The complete index owns 7,606,712
bytes (about 7.25 MiB) in the reference arm64 build and must remain below an
8 MiB arena budget, excluding allocator bookkeeping outside the owned object and
arena storage.

Reserve that storage away from the audio callback and perform no allocation
while processing a captured chunk. A strict relative-gate query visits one
boundary leaf plus a bounded number of subtree aggregates instead of rescanning
the history. Expose reserved storage, topology, and last/maximum query work in
Metrics. If the first excess total block arrives, invalidate Integrated, expose
the capacity-exceeded state and overflow count, and require RESET or a full
lifecycle reset before integration can resume. Do not silently roll the window
or substitute quantized/approximate gating.

Carry the actual host channel layout into Loudness analysis. A mono sample is
summed once with the BS.1770 mono channel weight; never pass duplicated mono as
L/R, which would overstate loudness. Only mono and stereo layouts are in the
initial scope.

Do not add a loudness history graph initially. Loudness Range and true peak are
deferred. When LRA is added, implement EBU Tech 3342 correctly instead of
shipping an approximation; do not expose `LRA`, `dBTP`, or true-peak labels
before reference tests pass.

## Rendering, analysis, and threading

- Keep dashboard tiles as logical regions of one native Metal surface.
- Use one display-linked frame, drawable, command buffer, and render pass for
  the dashboard rather than one native view or renderer per tile.
- Use scissor rectangles, logical panel bounds, shared geometry/text resources,
  and textures as appropriate inside that surface.
- Do not create one analysis thread per panel.
- Reuse channel measurements, windowed samples, FFT bins, and other compatible
  results through the existing per-instance coordinator and process-wide worker
  pool.
- Carry actual channel-count/layout metadata through the capture and immutable
  snapshot path; pointer duplication is not channel-layout metadata.
- Publish stable immutable snapshots to the renderer. Settings and layout
  changes must not make the render thread read mutable analysis working memory.
- Keep the audio callback bounded and non-blocking regardless of visible panels.
- Loudness consumes every retained raw capture chunk on the existing shared
  worker path before Spectrum freshness coalescing. It does not add a thread or
  perform work on the audio callback.

## Resizing, Retina, and responsive behavior

- Keep the existing 720 by 420 logical-point editor minimum.
- Keep the same dashboard topology at every allowed editor size; do not wrap,
  reorder, or hide tiles automatically.
- Reduce minor ticks, labels, grid detail, and optional numeric readouts before
  compromising primary readings at smaller sizes.
- Store dashboard geometry in normalized grid coordinates and compute it in
  logical points, not drawable pixels.
- Derive physical Metal drawable dimensions from the current backing scale.
- Support regular-density and Retina displays plus live movement between them;
  do not assume the only scale factors are exactly 1 and 2.
- Keep plot strokes, text, tick labels, hit targets, and resize affordances crisp
  and visually consistent across backing-scale changes.
- Rebuild only pixel-density-dependent resources, such as a glyph atlas, when
  backing scale changes.
- Preserve the selected display-linked Fixed maximum or Adaptive animation
  during live editor and tile resize, including active-display maximums above
  120 Hz, subject to accepted performance gates.

## Editor lifecycle

The accepted lifecycle policy applies to every panel:

- while attached and visibly active, run only analysis required by implemented
  panels and render at display cadence;
- after the hidden, minimized, occluded, detached, or closed debounce, stop
  sample handoff, analysis jobs, all history accumulation, display-link
  callbacks, and Metal submissions; and
- on reactivation, advance the capture/lifecycle generation, discard stale
  snapshots, reset temporal state, and warm up from current audio.

Opening side Settings, Metrics, or side-panel About while the live Metal preview
remains visible does not make the editor inactive. About opened from already
paused full-content Settings remains full-content and paused, so that switch
also causes no lifecycle transition. Closing About reveals the current
underlying utility state; if Settings is then full-content because its state or
the editor width changed while About was open, the normal Settings lifecycle
boundary applies. No analyzer retains or reconstructs a genuinely inactive
interval.
