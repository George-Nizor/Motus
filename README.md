# Motus

Motus is Instrumenta's Windows-first, local rough-cut editor for long-form 4K talking-head footage. This repository
contains a usable but deliberately narrow rough-cut slice and its tested editing foundation. It is not yet a
complete editor or an M0-qualified release.

## Implemented foundation

- Exact rational `MediaTime` with checked conversion and explicit rounding.
- Canonical project, asset, sequence, track, clip, effect, keyframe, marker, transcript, cleanup,
  and job types independent of MLT.
- Transactional add, linked split, ripple delete, replace, undo, and redo commands.
- Sequence-scoped linked move and trim commands with independent link/effect identities after splits.
- Safe cleanup filtering and one-command creation of a duplicate cleaned sequence.
- Versioned, human-readable `.veproj` JSON; atomic save, backup, recovery snapshots, Unicode
  paths, offline states, fingerprints, persisted FFprobe stream metadata, and v1/v2-to-v3 loading.
- Source-specific cache identity/invalidation.
- Cancellable background scheduler with CPU concurrency and a hard one-GPU-job limit.
- Versioned out-of-process analysis-provider contract.
- Deterministic MLT XML snapshot generation with preview-only proxies and original-only render.
- Optional Qt 6/QML shell with a searchable media bin, selected-clip Qt Multimedia preview,
  inspector, markers, and a controller-backed two-lane rough-cut timeline.
- FFprobe-on-import/relink plus a fail-closed simple H.264/AAC MP4 export from original media,
  with progress, cancellation, diagnostics, and atomic staged publication.
- A local C++ MCP server for probed import, integrity/relink, complete rough-cut edits, inspection,
  diagnostic graph output, and the supported simple native render.

## Build

Motus is a standalone repository. Its source, native core tests, desktop shell, MCP server, and
portable-bundle tooling live here; Instrumenta is an optional suite integration discovered through
`instrumenta/product.json`.

On the planned Windows toolchain, launch an MSYS2 MinGW64 terminal:

```bash
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --test-dir build/windows-mingw-release --output-on-failure
cmake --install build/windows-mingw-release --prefix dist/windows
```

For a fresh Windows machine, run `scripts/bootstrap-windows.ps1` from this repository. It installs
MSYS2 through winget when needed, installs the current compiler/CMake/Ninja/Qt Multimedia/FFmpeg
package set (Quick Controls 2 is provided by Qt declarative), then configures,
builds, tests, and deploys Motus to `dist/windows`.

Qt is optional at configure time. Without Qt, `ve_core` and its tests still build. The final
desktop integration will require the pinned Shotcut SDK defined during the M0 feasibility gate.
See [Windows bootstrap](docs/windows-bootstrap.md) and [milestone status](docs/milestones.md).

## Portable bundle

`dist/windows` is a portable folder application: `motus.exe` plus every library it needs. It runs on a
Windows computer with no Qt, MSYS2, MinGW, CMake, or Ninja installed, and Instrumenta embeds it
directly.

Qt's own deployment step is not sufficient to produce that. It copies Qt's libraries, QML modules, and
plugins, but not the MSYS2 libraries Qt itself imports, so the result only runs where MSYS2 is on
`PATH`. `scripts/bundle-runtime.cjs` closes that gap by walking the real PE import tables of every
executable, DLL, plugin, and QML module in the bundle and copying the transitive closure beside
`motus.exe`:

```powershell
node scripts\bundle-runtime.cjs complete dist\windows --search C:\msys64\mingw64\bin --flatten
node scripts\bundle-runtime.cjs check dist\windows
```

`check` takes no search path and copies nothing. It also requires bundled `ffprobe.exe` and
`ffmpeg.exe`, then proves offline that the bundle needs nothing from
the computer that built it, and names the missing library and its importer when it does — the useful
form of what Windows otherwise reports as `0xC0000135`. The bootstrap, `Instrumenta.cmd test`, and the
Instrumenta packaging step all run it. Its own tests are in `tests/bundle_runtime_tests.cjs` and run
anywhere:

```bash
node --test tests/bundle_runtime_tests.cjs
```

The bundle carries a versioned `motus-bundle.json` launch manifest. The bootstrap installs to a staging
folder, completes the runtime closure, verifies it, then runs the native `--instrumenta-launch-check`
handshake with a bare system `PATH` so a developer machine cannot mask a missing library. A second
clean-PATH smoke creates a visible-counter, 90-frame 30000/1001 H.264 plus 48 kHz stereo AAC fixture;
checks probe fields and Qt load/seek/play/pause/EOF state; exercises controller export cancellation,
retry, missing-media failure, and identity-preserving relink; then re-probes the rendered output.
The launch handshake initialises `QGuiApplication` and compiles the root QML component — exercising the platform
plugin and the QML import tree — then exits before a window is created. The new bundle is published
only after all of that succeeds.

The same staging gate generates `THIRD_PARTY_NOTICES.txt`, `third-party-packages.json`, and
`licenses/` from the files actually deployed and the local pacman ownership database. The inventory
records exact package versions, bundled files, installed license metadata/texts, MSYS2 source-package
provenance, and FFmpeg's embedded license/configuration reports. This is an audit aid, not a declaration
of license compliance: the current MSYS2 FFmpeg is GPL-3.0-or-later, reports `--enable-gpl`,
`--enable-version3`, and `--enable-libx264`, and brings `libx264-165.dll` into the bundle. Public
distribution remains blocked until the exact corresponding source/build materials are offered with the
binary distribution, the Motus/Qt-plugin/FFmpeg license boundary has been reviewed, and authoritative
terms/notices are obtained for transitive packages whose MSYS2 binary package installs no license file.
The current bundle must not be treated as a publishable release merely because its runtime tests pass.

A verified bundle from another machine can be dropped into `prebuilt/windows` instead, or pointed at
with `MOTUS_BUNDLE`. The launcher and the packaging step accept either, so Motus can be packaged into
Instrumenta on a computer that has no native toolchain.

With the native dependencies installed, the same configure,
build, test, and deploy sequence is available from the Motus refresh icon or from the workspace:

```powershell
.\Instrumenta.cmd build
```

Instrumenta deliberately ignores executables under `build/`. It launches only a deployed manifest-backed
bundle, checks that its path is contained within Motus, verifies the Qt runtime immediately before launch,
and starts the process without a shell.

The portable core gate compiles the non-Qt sources with strict warnings as errors and runs the
core and bundle-runtime tests. Run the bundle test directly from this repository:

```bash
node --test tests/bundle_runtime_tests.cjs
```

When Motus is checked out beside Instrumenta, the suite entrypoint `Instrumenta.cmd build motus`
can run this same bootstrap and publish the verified bundle for the launcher.

## Repository structure

`src/` and `include/` contain the editor core, `app/` contains the optional Qt/QML desktop shell,
`tests/` contains native and runtime tests, `scripts/` contains build/deployment helpers, and
`instrumenta/product.json` declares the optional suite integration contract.

## Desktop vertical slice

The current shell creates, opens, validates, and atomically saves projects. Desktop import/relink
runs the bundled FFprobe first, persists streams/duration/provenance, then references the file
read-only. It can search the media bin, select linked clips on separate video/audio lanes,
move and trim them by exact frame, split at the playhead, ripple-remove a selected clip's full time
range across unlocked tracks, ripple-delete marked in/out ranges, manage markers and track
lock/mute/visibility state, snap to edit boundaries, zoom the timeline, undo/redo, and generate
original-media diagnostic MLT XML. The Program panel decodes and seeks the active clip through Qt
Multimedia, including source audio, and advances across the supported gapless cut. This is explicitly
a time-based selected-clip preview, not a frame-accurate composited MLT program monitor.

The first real export preset supports one visible gapless video lane starting at zero and an optional
exactly mirrored linked audio lane. Sources must be online and probed; speed changes, fades, effects,
transitions, gaps, and additional active lanes fail preflight rather than being dropped. It renders
originals to a sibling `.motus-partial.mp4`, removes partial output after failure/cancel, and atomically
replaces the requested `.mp4` only after FFmpeg succeeds. Still-image duration, proxy playback,
multitrack composition, and representative H.264/HEVC hardware/drift gates remain unfinished.

The compact command bar uses packaged 24 px SVG icons with full labels on hover or keyboard
focus. Motus keeps the Instrumenta family surfaces while using clay for edit selection and cyan only
for timecode, marks, snapping, and playhead/navigation state.

## MCP

Run `node scripts/build-mcp.cjs` for a CMake-free POSIX/WSL build at
`build/agent/motus-mcp`, or use the published Windows `dist/windows/motus-mcp.exe`. Configure it as
a local stdio server. Available tools and a smoke
test are documented in [mcp/README.md](mcp/README.md).

## Project data policy

Original media is referenced in place and is never written. `.veproj` is durable project state;
proxies, waveforms, thumbnails, transcripts, model files, render logs, and analysis belong in a
disposable sibling `.ve-cache` directory. Render graphs always resolve original paths even when a
proxy is available to preview.

There is no cloud processing, telemetry, direct upload, or recurring service dependency.
