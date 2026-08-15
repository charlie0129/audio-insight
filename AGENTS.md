# Repository working instructions

Before planning or changing this repository, read:

- `docs/architecture.md` for the accepted product and technical decisions.
- `docs/analyzer-ui.md` for the accepted dashboard, settings, and panel design.
- `docs/macos-distribution.md` for the macOS signing and release policy.

Treat decisions marked **Accepted** as project requirements. Do not silently
replace them with a different framework, renderer, threading model, platform
scope, or distribution policy. If the user's latest explicit direction changes
a decision, follow it and update the relevant document in the same change.

In particular:

- Keep the audio callback bounded and real-time safe: no allocation, locks,
  waits, logging, file or network access, UI calls, or GPU work.
- Preserve the JUCE plugin shell, macOS-first scope, native Metal renderer, and
  shared-analysis design unless an accepted decision is deliberately revised.
- During initial development, target macOS 15 on arm64 and build AUv2 plus VST3.
  Keep core code independent of plugin format and the Metal backend.
- Use project-owned code under AGPL-3.0-or-later and preserve all third-party
  license notices. JUCE is a pinned submodule used under its upstream AGPLv3
  terms, not relicensed by this project.
- Add SPDX and confirmed copyright notices to project source files, and preserve
  an in-plugin About/Legal path with the notices required by the AGPL.
- Keep a discreet, user-initiated sponsorship link in About/Legal pointing to
  `https://github.com/sponsors/charlie0129`; do not add tracking or automatic
  network access.
- Use CMake with the pinned JUCE submodule or an explicit local JUCE source path;
  normal configuration must not silently download dependencies.
- Developer ID signing and notarization are out of scope. Do not add either
  unless the user explicitly changes the distribution policy.
- Add proportionate validation and performance instrumentation as code is
  introduced; smooth frame pacing and low host overhead are product features.
- Make small, coherent git commits as verified milestones land instead of
  accumulating the whole implementation into one final commit.
- If a required development tool or local environment component is missing or
  broken, report it to the user and ask them to fix it rather than changing the
  machine-wide development environment without direction.
- Keep the user informed at runnable visual milestones. When appearance,
  animation feel, Retina behavior, or DAW integration cannot be verified
  confidently from the available environment, ask the user to test and return
  observations or screenshots instead of guessing that the result is correct.
- Record unresolved choices as open questions instead of guessing silently.
