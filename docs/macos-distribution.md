# macOS development and distribution policy

> **Status:** Accepted policy; exact artifact names and build commands will be
> finalized with the build system.
>
> **Established:** 2026-08-15

Audio Insight's project-owned code is licensed under AGPL-3.0-or-later and must
remain buildable and usable without a paid Apple Developer Program membership.
JUCE and other dependencies retain their own compatible licenses and notices.

## Accepted policy

- Local development builds use ad-hoc signing when macOS or a host requires a
  signature.
- Users may build and install the plugins from source.
- The project may publish prebuilt AUv2 and VST3 artifacts for convenience.
- Prebuilt bundles will not be Developer ID signed or Apple-notarized.
- Developer ID signing, notarization, and a paid Apple Developer account are out
  of scope unless the project owner explicitly changes this policy.
- Release notes must be explicit about the trust and Gatekeeper limitations of
  prebuilt artifacts.

This policy does not promise the same installation experience as notarized
commercial software. macOS behavior can vary by OS version, download mechanism,
host, and system security policy.

## Local/ad-hoc signing

An ad-hoc signature uses `-` as the signing identity and does not require a
certificate or Apple account. Once artifact names exist, the build should apply
ad-hoc signing automatically for local development where practical. The
`.component` example below matches the initial AUv2 target. AUv3 is out of scope
for the first release and would require an app-extension container and a
different nested signing flow. The manual equivalent for a simple plugin bundle
is:

```sh
codesign --force --sign - "/path/to/AudioInsight.component"
codesign --force --sign - "/path/to/AudioInsight.vst3"
```

Verify the resulting bundle rather than assuming the command succeeded:

```sh
codesign --verify --strict --verbose=2 "/path/to/AudioInsight.component"
codesign --verify --strict --verbose=2 "/path/to/AudioInsight.vst3"
```

Do not use `codesign --deep` as a substitute for understanding bundle contents.
If the project later embeds frameworks or other nested code, sign those items
inside-out before signing the outer plugin bundle. After doing so, `--deep` may
be added to the verification command to verify nested code recursively; it is
not a shortcut for signing it correctly.

## Installing a local build

Prefer the current user's plugin directories so installation does not require
administrator access. The `.component` is the initial AUv2 artifact:

```text
/Users/your-name/Library/Audio/Plug-Ins/Components/AudioInsight.component
/Users/your-name/Library/Audio/Plug-Ins/VST3/AudioInsight.vst3
```

System-wide installation under `/Library/Audio/Plug-Ins` is optional and may
require administrator privileges. The final build documentation will provide
exact copy/install targets after the product and bundle identifiers are chosen.

## Using a downloaded prebuilt bundle

A browser or archive tool may attach the `com.apple.quarantine` attribute to a
download. A user who trusts the artifact may need to remove that attribute and
apply a fresh ad-hoc signature before a DAW will scan it. The `.component`
commands are for the initial AUv2 target; a future AUv3 target would need its own
container-specific instructions.

The safe order is:

1. Verify the published SHA-256 checksum of the downloaded release archive.
2. Extract it, then clear quarantine only from the intended plugin bundle.
3. Apply the ad-hoc signature.
4. Verify the resulting signature with `codesign --verify --strict`.

For steps 2 and 3:

```sh
xattr -dr com.apple.quarantine "/path/to/AudioInsight.component"
codesign --force --sign - "/path/to/AudioInsight.component"

xattr -dr com.apple.quarantine "/path/to/AudioInsight.vst3"
codesign --force --sign - "/path/to/AudioInsight.vst3"
```

These commands deliberately target one explicit plugin bundle. Users should not
recursively clear quarantine from a broad directory. Removing quarantine and
ad-hoc signing do not establish the publisher identity or make untrusted code
safe; users should do this only for source or binaries whose provenance they
trust.

Prebuilt releases should publish SHA-256 checksums through the project's normal
release channel. Check the downloaded archive before removing quarantine or
re-signing; local signing mutates the extracted bundle, so a checksum of that
bundle afterward will not match a checksum of the published artifact. Checksums
help detect corruption or mismatched downloads, but they are not a replacement
for a trusted distribution channel.

Because JUCE is a Git submodule, GitHub's automatically generated source archive
does not contain its source. Every prebuilt release must therefore include a
complete Corresponding Source archive for that exact binary. It must contain all
project source, shaders and other source-form assets, configuration, build and
installation instructions, the pinned JUCE tree, any local changes, and required
third-party license notices.

After installation, restart or rescan the relevant DAW as needed. Development
and release instructions will include `auval`, Steinberg validator, and selected
DAW scan checks once identifiers and supported hosts are defined.
