# Analyzer dashboard interface requirements

> **Status:** Accepted target design, staged after the first usable release
>
> **Established:** 2026-08-15
>
> **Last updated:** 2026-08-15

This document is the durable source of truth for Audio Insight's analyzer
dashboard, layout editing, settings inspector, and per-panel presentation. The
interactive layout prototype is illustrative; when it conflicts with this
document, this document wins.

System architecture, real-time safety, rendering, lifecycle, and first-release
scope remain defined by [the architecture and decision record](architecture.md).
Unresolved details are collected under **Open questions** instead of being
silently assumed.

## Scope and milestones

The first usable release remains the deliberately narrow vertical slice already
accepted in the architecture document:

- one large real-time FFT Spectrum; and
- one compact vertical stereo sample-peak/RMS meter.

It may use a fixed default arrangement while those analyzers, host behavior, and
performance gates are being established.

The accepted target dashboard expands that surface with Spectrogram, Stereo
field/correlation, and Loudness panels plus constrained layout editing. These
are accepted product requirements, but this document does not move them into
the first usable release or determine their exact implementation order.

## Dashboard model

- Use one dashboard view containing one instance of each enabled analyzer.
- Do not add separate Overview and Focus Spectrum modes. In particular, do not
  duplicate Spectrum or hide the rest of the dashboard behind a focus switch.
- Treat Spectrum, Peak/RMS, Spectrogram, Stereo field/correlation, and Loudness
  as logical tile regions within the dashboard.
- Keep utility UI such as Settings, Metrics, and About outside the analyzer tile
  grid.
- Keep the whole editor resizable.

The default visual hierarchy is qualitative rather than a fixed column count:

| Panel | Default emphasis |
| --- | --- |
| Spectrum | Dominant, occupying most of the upper dashboard |
| Peak/RMS | Compact, narrow vertical strip beside Spectrum |
| Spectrogram | Largest history panel in the lower dashboard |
| Stereo field/correlation | Medium supporting panel |
| Loudness | Particularly slim vertical panel |

Exact default spans remain open. The prototype's grid proportions are examples,
not requirements.

### Constrained layout editing

- Provide an explicit **Edit layout** mode.
- Let the user adjust both tile width and tile height.
- Resize from tile edges or corners and snap results to a constrained dashboard
  grid.
- Keep tiles inside the dashboard and prevent the edit operation from producing
  overlapping, detached, or floating analyzer windows.
- Show resize affordances only while edit mode is active so the normal analyzer
  view remains visually quiet.

This is intentionally constrained layout editing, not an arbitrary windowing
system. Tile movement, hiding, reordering, collision resolution, minimum sizes,
reset behavior, keyboard editing, and persistence are still open.

## Settings inspector

Open analyzer settings from the editor toolbar in a temporary right-side
inspector. Settings must not become a permanent analyzer tile.

The native Metal surface cannot reliably be covered by a sibling JUCE component
in every plugin host. The implementation may therefore reserve space and narrow
or reflow the Metal canvas while the inspector is open instead of drawing JUCE
controls over it. The user-facing behavior remains a temporary right-side
inspector.

### Shared analysis controls

The inspector must provide room for:

- FFT size;
- FFT window function; and
- analysis update rate.

The exact choices, ranges, and defaults are open. The current 4096-point Hann
FFT at 60 analysis updates per second remains the initial implementation and
reference workload, not a permanent restriction on these controls.

FFT size and window changes are structural analysis changes. They must occur
off the audio callback, advance or replace the analysis generation, discard
incompatible snapshots, and reset overlap, smoothing, and Spectrogram history
as needed. They should not be treated as audio-rate automation.

Analysis update rate and display refresh rate are separate. Lowering the FFT or
Spectrogram update cadence must not reduce display-linked animation from 120 Hz
to the analysis rate; the renderer continues at the cadence granted by the
active display and consumes the newest complete analysis snapshot.

### Shared frequency spacing

- Provide one continuous frequency-spacing control ranging from Linear through
  intermediate blends to Logarithmic.
- Spectrum and Spectrogram use the same setting and the same coordinate
  transform; do not expose independent frequency scales.
- Apply the transform to Spectrum data and numeric ticks along its horizontal
  axis.
- Apply the same transform to Spectrogram energy traces and numeric ticks along
  its vertical axis.
- A presentation-only spacing change should reuse the existing FFT result rather
  than trigger another FFT merely to move coordinates.

The precise interpolation formula, frequency limits, and tick-selection rules
remain open.

### Spectrum controls

The accepted Spectrum control set includes:

- trace color;
- temporal smoothing;
- fill visibility or opacity beneath the trace; and
- displayed dB floor and ceiling.

Exact ranges, defaults, and whether the fill can be disabled independently are
open.

### Spectrogram controls

The accepted Spectrogram control set includes:

- intensity palette or trace colors;
- color response/intensity transfer, including a linear response and other
  useful responses between low-contrast and high-contrast presentations;
- dB floor and ceiling used for the color mapping; and
- visible history duration.

"Color response" describes how analyzed intensity maps into the palette; it is
not a second frequency-spacing control. Its final product name, mathematical
transfer function, range, and defaults remain open.

Changing the visible history duration does not authorize background history
collection. History stops and resets according to the editor lifecycle rules.

### State and automation

The processor already saves some settings per plugin instance. Before adding
the expanded controls, explicitly decide:

- which analyzer values are host-automatable parameters;
- which are non-automatable per-instance configuration;
- whether any presentation preferences are also global user defaults; and
- how missing or older state versions migrate to safe defaults.

Do not expose structural FFT reconfiguration as unrestricted audio-rate
automation. Layout persistence and reset behavior remain open questions.

## Panel requirements

### Spectrum

- Spectrum is the dominant real-time FFT visualization and the first analyzer
  to implement fully.
- Show numeric frequency ticks along the horizontal axis, using Hz or kHz as
  appropriate.
- Show numeric signal-level ticks along the vertical axis in dB.
- Omit redundant axis-title text such as `X FREQUENCY` and `Y LEVEL`; numeric
  ticks and their units are sufficient.
- Use the shared continuous frequency-spacing transform.
- Apply Spectrum trace color, smoothing, fill, floor, and ceiling settings
  without blocking analysis or rendering.

The visible `dB` versus `dBFS` suffix, frequency range, tick density, slope
compensation, averaging, and peak-hold behavior remain open.

### Peak/RMS

- Use a compact, narrow, vertical stereo meter strip.
- Show distinct left and right channels.
- Present sample peak and RMS honestly and make the two measurements visually
  distinguishable.
- Do not label sample peak as true peak. Reserve that name for a correctly
  oversampled true-peak implementation if one is added later.
- Keep the meter substantially narrower than the dominant Spectrum panel.

Meter ballistics, peak hold/reset, numeric readouts, scale ticks, and warning
threshold colors remain open.

### Spectrogram

- Render time-frequency energy as intensity-colored traces over a black or
  near-black background.
- The default visual direction is dark blue for lower energy, rising through
  orange and toward near-white for stronger energy.
- Preserve a trace-like representation of real energy. Do not substitute broad
  decorative color bands or a generic striped gradient.
- Use the shared continuous frequency transform for both energy placement and
  frequency tick placement.
- Show only numeric frequency ticks in Hz or kHz around the plot.
- Omit axis-title text, time/history labels, and a visible dB/color legend.
  Time/history still exists internally and may be adjustable even though the
  horizontal axis is unlabeled.
- Stop capture and history accumulation when the editor is inactive. Reopening
  starts fresh rather than reconstructing the missing interval.

Palette choices, color-response math, history direction, texture resolution,
history duration, and whether the background is literal black or near-black
remain open.

### Stereo field and correlation

- Reserve one dashboard tile for a Stereo field visualization.
- Keep correlation integrated with that tile rather than introducing a separate
  permanent dashboard tile for it.
- Treat this as a post-first-release analyzer.

The vectorscope/goniometer algorithm, normalization, persistence, correlation
scale, smoothing, and visual style are not yet decided.

### Loudness

- Use a particularly slim tile with a vertical loudness bar.
- Center the bar horizontally.
- Place the primary loudness value above the bar.
- Place secondary readings below the bar rather than stacking all text in a
  column to its left.
- Treat this as a post-first-release analyzer.

The current mockup's LUFS-I, short-term, and range values are illustrative. The
supported standard, gating, reset behavior, history, target/reference, and exact
set of momentary, short-term, integrated, and range measurements remain open.
Do not ship labels until their underlying measurements are implemented
correctly.

### Performance metrics

The existing opt-in Metrics panel remains a utility and observability surface,
not an analyzer tile. Its collection cadence, exact histories, raw report, and
visibility lifecycle remain governed by the architecture document and README.
How Metrics and Settings share the right side when both are requested is open.

## Rendering, analysis, and threading

- Keep dashboard tiles as logical regions of one native Metal surface.
- Prefer one display-linked frame, drawable, command buffer, and render pass for
  the dashboard rather than one native view or renderer per tile.
- Use scissor rectangles, logical panel bounds, shared geometry/text resources,
  and textures as appropriate inside that surface.
- Do not create one analysis thread per panel.
- Reuse channel measurements, windowed samples, FFT bins, and other compatible
  results through the existing per-instance coordinator and process-wide worker
  pool.
- Publish stable immutable snapshots to the renderer. Settings and layout
  changes must not make the render thread read mutable analysis working memory.
- Keep the audio callback bounded and non-blocking regardless of how many
  panels are visible.

If panels can eventually be hidden or disabled, their unneeded analysis should
stop or reduce cadence without affecting audio pass-through or other panels.

## Resizing, Retina, and responsive behavior

- Store and compute dashboard geometry in logical points or normalized layout
  coordinates, not drawable pixels.
- Derive physical Metal drawable dimensions from the current backing scale.
- Support regular-density and Retina displays plus live movement between them;
  do not assume the only scale factors are exactly 1 and 2.
- Keep plot strokes, text, tick labels, hit targets, and resize affordances crisp
  and visually consistent across backing-scale changes.
- Rebuild only pixel-density-dependent resources, such as a glyph atlas, when
  the backing scale changes.
- Enforce enough space for numeric labels and analyzer content during resize.
  The exact minimum tile sizes and narrow-window reflow rules remain open.
- Preserve display-linked 60 Hz and host/display-permitted 120 Hz animation
  during live editor and tile resize, subject to the accepted performance gates.

## Editor lifecycle

The accepted lifecycle policy applies to every future panel:

- while attached and visibly active, run only the analysis required by visible
  panels and render at display cadence;
- after the hidden, minimized, occluded, detached, or closed debounce, stop
  sample handoff, analysis jobs, all history accumulation, display-link
  callbacks, and Metal submissions; and
- on reactivation, advance the analysis generation, discard stale snapshots,
  reset temporal state, and warm up from current audio.

Opening a side Settings inspector while the canvas remains visible does not by
itself make the editor inactive. About may continue to hide and pause the canvas
as currently implemented.

## Open questions

- When do constrained layout editing and each post-first-release panel enter the
  implementation sequence?
- What are the exact default tile positions and proportions?
- Can tiles be moved, reordered, hidden, or restored as well as resized?
- What are the collision rules, minimum sizes, keyboard controls, reset action,
  and narrow-window reflow behavior?
- Is layout saved per plugin instance, as a global default, or both?
- How do Settings and Metrics coexist when both are open?
- Which settings are automatable, per-instance non-automatable state, or global
  presentation preferences?
- Which FFT sizes, window functions, and analysis update rates are supported,
  and what are their product defaults?
- What interpolation formula defines intermediate Linear-to-Logarithmic
  frequency spacing, and how are ticks selected without overlap?
- What are the Spectrum frequency limits, level-unit suffix, averaging, slope,
  and peak-hold behaviors?
- Which Spectrogram palettes are included, what does color response control
  mathematically, and what are the dB and history ranges/defaults?
- Which direction does Spectrogram history move, and what texture/history
  storage strategy meets the performance budget?
- What meter ballistics, holds, numeric scales, and warning thresholds does
  Peak/RMS use?
- Which Stereo field algorithm and correlation presentation are correct?
- Which loudness standard and exact measurements are implemented, and how do
  their reset, gating, targets, and history behave?
