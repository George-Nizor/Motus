# Milestone status

The product plan is intentionally larger than this greenfield bootstrap. Status is explicit so a
passing core test suite is not confused with M0 or a usable release.

## Bootstrap delivered

- Canonical model and exact time: implemented and unit-tested.
- Transaction commands and linked rough-cut primitives: implemented and unit-tested.
- Persistence, migration, atomic save, recovery, and cache invalidation: implemented and tested.
- Cleanup acceptance safety rules and duplicate-sequence application: implemented and tested.
- Scheduler and analysis boundary: implemented at the core/API level.
- MLT graph projection: implemented; runtime execution is pending the Windows SDK.
- QML workspace: project create/open/save, searchable probed media references, two-lane clip
  selection, exact-frame linked split/move/trim, selected-range and in/out ripple delete, markers,
  track states, snapping, zoom, undo/redo, and MLT graph generation are bound to the canonical model.
  Qt Multimedia provides a truthful time-based selected-clip preview with audio; it is not described
  as frame-accurate program playback.
- Native media slice: bundled FFprobe metadata is persisted; a supported single-video-lane plus
  mirrored-audio subset exports originals through bundled FFmpeg with progress, cancel, diagnostics,
  staged output, and a synthetic end-to-end packaging smoke.
- MCP: local stdio server implements project creation/inspection, probed media import, integrity and
  relink, linked
  split, ripple delete, and MLT graph generation through shared workflows.

## M0 — blocked on representative Windows hardware/media

M0 cannot honestly be passed in this Linux development container. It requires:

- selecting a specific official Shotcut Windows SDK archive and recording its SHA-256 in
  `toolchains/windows.dependencies.json`;
- frame-accurate MLT program seeking/SDL output, proxy switching, and representative H.264
  and HEVC 4K files;
- a one-hour drift/render run and measured 30 fps proxy playback;
- 20 minutes of manually labelled personal speech for Medium/Small transcription benchmarks.
- publication or accompaniment of the exact corresponding source/build materials for the staged GPL
  FFmpeg/x264 runtime, review of the Motus/Qt-plugin/FFmpeg license boundary, and authoritative notices
  for transitive packages whose MSYS2 binary package installs none. Generated package provenance and
  notices expose this gate but do not by themselves satisfy it.

Do not expand the UI beyond functional binding until these gates pass.

## M1–M4 — pending

Media generation, frame-accurate program playback, complete timeline UX, acoustic analysis, model hosting,
full export presets, multitrack composition, visual effects, transitions, direct manipulation,
face tracking, and face smoothing remain future implementation. The original acceptance
thresholds—especially zero words clipped by bulk-safe cleanup—remain release gates, not assumed
properties.

See [the executable implementation plan](implementation-plan.md) for ordered work packages and
the gates that prevent placeholder interfaces from being mistaken for completed milestones.
