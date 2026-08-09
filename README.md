# Video Editor

A Windows-first, local rough-cut editor for long-form 4K talking-head footage. This repository
currently contains the tested editing foundation and a Qt/QML desktop shell; it is not yet an
end-user-capable video editor.

## Implemented foundation

- Exact rational `MediaTime` with checked conversion and explicit rounding.
- Canonical project, asset, sequence, track, clip, effect, keyframe, marker, transcript, cleanup,
  and job types independent of MLT.
- Transactional add, linked split, ripple delete, replace, undo, and redo commands.
- Safe cleanup filtering and one-command creation of a duplicate cleaned sequence.
- Versioned, human-readable `.veproj` JSON; atomic save, backup, recovery snapshots, Unicode
  paths, offline states, fingerprints, and v1-to-v2 loading.
- Source-specific cache identity/invalidation.
- Cancellable background scheduler with CPU concurrency and a hard one-GPU-job limit.
- Versioned out-of-process analysis-provider contract.
- Deterministic MLT XML snapshot generation with preview-only proxies and original-only render.
- Optional Qt 6/QML shell laying out the media bin, viewer, transcript/cleanup, inspector, jobs,
  and timeline workspaces.

## Build

On the planned Windows toolchain, launch an MSYS2 MinGW64 terminal:

```bash
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --test-dir build/windows-mingw-release --output-on-failure
```

Qt is optional at configure time. Without Qt, `ve_core` and its tests still build. The final
desktop integration will require the pinned Shotcut SDK defined during the M0 feasibility gate.
See [Windows bootstrap](docs/windows-bootstrap.md) and [milestone status](docs/milestones.md).

This development container has a compiler but no CMake/Ninja. A direct strict-warning build was
used to verify all core sources and tests:

```bash
mkdir -p build/manual
c++ -std=c++20 -pthread -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -Iinclude -Itests src/*.cpp tests/*.cpp -o build/manual/ve_core_tests
./build/manual/ve_core_tests
```

## Project data policy

Original media is referenced in place and is never written. `.veproj` is durable project state;
proxies, waveforms, thumbnails, transcripts, model files, render logs, and analysis belong in a
disposable sibling `.ve-cache` directory. Render graphs always resolve original paths even when a
proxy is available to preview.

There is no cloud processing, telemetry, direct upload, or recurring service dependency.

