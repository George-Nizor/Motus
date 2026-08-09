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
- QML workspace: static shell only; it is not bound to the model yet.

## M0 — blocked on representative Windows hardware/media

M0 cannot honestly be passed in this Linux development container. It requires:

- selecting a specific official Shotcut Windows SDK archive and recording its SHA-256 in
  `toolchains/windows.dependencies.json`;
- native import, frame seeking, SDL audio, proxy switching, and export with representative H.264
  and HEVC 4K files;
- a one-hour drift/render run and measured 30 fps proxy playback;
- 20 minutes of manually labelled personal speech for Medium/Small transcription benchmarks.

Do not expand the UI beyond functional binding until these gates pass.

## M1–M4 — pending

Media probing/generation, live playback, complete timeline UX, acoustic analysis, model hosting,
exports, packaging, multitrack composition, visual effects, transitions, direct manipulation,
face tracking, and face smoothing remain future implementation. The original acceptance
thresholds—especially zero words clipped by bulk-safe cleanup—remain release gates, not assumed
properties.

