# Community release process

`opensensor/PrusaSlicer` produces native community-fork bundles without relying
on Prusa's private build-action repository. The release source is always a
single commit on `master`, including the MMU auto-colorization history merged
from `origin/mmu-auto-colorize`.

## Release tags

Stable release tags use this form:

```text
community-v<upstream-version>.<community-sequence>
```

For example, the first community release based on PrusaSlicer 2.9.6 is
`community-v2.9.6.1`. Release candidates append `-rcN`, such as
`community-v2.9.6.2-rc1`.

The first three version fields must match `SLIC3R_VERSION` in `version.inc`.
The fourth field is incremented for each community release based on that
upstream version. The build embeds an `opensensor` build ID so diagnostic output
can distinguish these binaries from official Prusa builds.

## Build, draft, and publish cycle

1. Run **Community release build** manually against `master`. This is an
   artifact-only rehearsal: it builds and tests Linux x64, Windows x64, and
   macOS Intel bundles but creates no tag or release.
2. Merge or fix forward until that rehearsal is green.
3. Create the annotated community tag on the tested `master` commit and push
   it. The same workflow rebuilds all three native bundles, verifies their
   structure, creates `SHA256SUMS.txt`, and creates a draft GitHub release.
4. Inspect the draft assets and native job logs. Run **Publish community
   release** with that tag. It downloads every draft asset, checks the hashes,
   tests the ZIP and tar archives, then publishes the release.

Release candidates are always marked as prereleases. Published stable tags may
be marked as the repository's latest release.

## Current artifact support

- Linux: portable tarball built on Ubuntu 24.04 x64. The bundled PrusaSlicer
  dependencies are static, while normal desktop libraries such as GTK and
  WebKitGTK are supplied by the host system.
- Windows: portable x64 ZIP containing the GUI, console, G-code viewer,
  resources, and required runtime DLLs.
- macOS: Intel `.dmg` targeting macOS 13 or newer.

The initial community artifacts are not vendor-signed. The macOS application
is ad-hoc signed for bundle-integrity validation but is not Apple-notarized.
Every release page and checksum manifest identifies the artifacts as community
fork builds, not official Prusa Research releases. Platform signing and Apple
notarization require community-owned signing identities and should be added as
an independent follow-up without weakening the draft/publish gate.
