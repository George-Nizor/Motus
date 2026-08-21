![Motus banner](docs/images/motus-banner.png)

# Motus

Motus is a Windows-first rough-cut video editor for long talking-head footage. The current 0.1.0
build covers a narrow editing slice with a tested native core, Qt desktop shell, local MCP server, and
one conservative H.264/AAC export path.

It is still under development. The unfinished parts are listed plainly because optimism is not a
media backend.

## What works

The desktop can create, open, validate, recover, and atomically save `.veproj` projects. Imported
media is probed with FFprobe and referenced in place.

The current edit surface supports:

- searchable media and offline/relink state;
- linked video/audio selection on two lanes;
- exact-frame move, trim, split, and ripple removal;
- marked in/out ripple deletion;
- markers and track lock/mute/visibility;
- snapping and timeline zoom;
- undo and redo;
- selected-clip video and source-audio preview through Qt Multimedia;
- diagnostic MLT XML from the canonical timeline;
- cancellable simple MP4 export from original media.

The preview is time-based selected-clip playback. Frame-accurate composited MLT program playback is an
M0 gate and is not claimed here.

## Build on Windows

The simplest fresh-machine path is:

```powershell
.\scripts\bootstrap-windows.ps1
```

The script installs MSYS2 through winget when needed, installs the pinned compiler/CMake/Ninja/Qt and
FFmpeg packages, configures the project, runs tests, and deploys a portable folder to `dist/windows`.

The equivalent commands in an MSYS2 MinGW64 terminal are:

```bash
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --test-dir build/windows-mingw-release --output-on-failure
cmake --install build/windows-mingw-release --prefix dist/windows
```

Qt is optional for the core build. Without Qt, `ve_core` and its tests still compile.

## Open through Instrumenta

Motus is an independent repository with an optional
[`instrumenta/product.json`](instrumenta/product.json) contract. Instrumenta launches only a deployed
`dist/windows` or `prebuilt/windows` bundle with a valid `motus-bundle.json`.

Before launch, Instrumenta checks path containment, the bundled runtime, and Motus's hidden
`--instrumenta-launch-check` handshake. A raw executable under `build/` is ignored.

From the sibling workspace:

```powershell
.\Instrumenta.cmd build motus
```

This runs the same bootstrap and publishes the verified portable bundle.

## The portable bundle

`dist/windows` contains `motus.exe`, Qt, QML modules, plugins, `ffmpeg.exe`, `ffprobe.exe`, and the
MSYS2 runtime libraries they import.

Qt's deployment utility does not collect that complete transitive runtime. Motus closes the gap by
walking PE imports and copying the required libraries:

```powershell
node scripts\bundle-runtime.cjs complete dist\windows --search C:\msys64\mingw64\bin --flatten
node scripts\bundle-runtime.cjs check dist\windows
```

`check` runs without a search path and copies nothing. It reports the exact missing library and its
importer. The staging gate also launches the QML root under a clean end-user `PATH` and runs a
synthetic probe, preview, cancellation, relink, and export smoke.

A deployed bundle includes `THIRD_PARTY_NOTICES.txt`, `third-party-packages.json`, and collected
licence material.

## Export boundary

The supported preset accepts one gapless visible video lane beginning at zero and an optional
mirrored linked audio lane. Every source must be online and probed.

Speed changes, fades, effects, transitions, gaps, and extra active lanes fail preflight. Motus does
not quietly drop an unsupported edit. It renders originals to a sibling `.motus-partial.mp4`, removes
the partial after failure or cancellation, and publishes the requested MP4 only after FFmpeg exits
successfully.

Multitrack composition, still-image duration, proxy playback, hardware encode gates, effects, and
frame-accurate program output remain unfinished.

## Project files and cache

`.veproj` is versioned, human-readable JSON. Saves are atomic and keep recovery snapshots. The loader
accepts older v1/v2 projects and writes the current v3 shape.

Original media is read-only and stays where the user put it. Disposable material belongs in a sibling
`.ve-cache` directory:

```text
proxies
waveforms
thumbnails
transcripts
model files
render logs
analysis output
```

Render graphs always resolve original media even when a proxy exists.

## MCP server

Build the CMake-free POSIX/WSL server with:

```bash
node scripts/build-mcp.cjs
```

The result is `build/agent/motus-mcp`. A Windows deployment carries
`dist/windows/motus-mcp.exe`. The stdio tools cover project creation and inspection, probed imports,
integrity/relink, linked edits, markers, diagnostic graphs, and the supported native render.

See [the MCP guide](mcp/README.md) for the exact contracts and a smoke test.

## Release gate

The currently staged MSYS2 FFmpeg reports GPL and x264 configuration. Public binary distribution
still needs the matching source/build offer, a review of the Motus/Qt-plugin/FFmpeg boundary, and
authoritative notices for packages whose binary bundle provides none.

Runtime checks and SHA-256 hashes prove that a bundle is intact. They do not settle licence terms.
[Milestone status](docs/milestones.md) records this separately from the remaining media gates.

## Verify a change

```bash
node --test tests/bundle_runtime_tests.cjs
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --test-dir build/windows-mingw-release --output-on-failure
```

## Documentation

- [Architecture](docs/architecture.md)
- [Milestone status and current limits](docs/milestones.md)
- [Windows bootstrap](docs/windows-bootstrap.md)
- [Ordered implementation plan](docs/implementation-plan.md)
- [MCP tools](mcp/README.md)

Motus source is MIT licensed. A distributed native bundle also carries the licences and notices for
its exact third-party contents.
