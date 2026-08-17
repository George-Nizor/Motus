# Motus executable implementation plan

This document turns the product milestones into implementation work packages. A package is only
complete when its tests and hardware gate pass; source code existing for an interface is not enough.

## Current vertical slice

The canonical model, exact time, persistence, edit transactions, cleanup application, job scheduler,
MLT XML projection, desktop project controller, and MCP automation all execute against the same
core. The current GUI can create/open/save projects, probe/fingerprint references, split, ripple
delete, move/trim selected linked clips by exact frame, manage in/out ranges, markers, track states,
snap and zoom, undo/redo, and emit MLT XML. It also provides time-based Qt selected-clip decode/audio
preview and a fail-closed simple original-media H.264/AAC export with progress/cancel. These do not
meet the full frame-accurate MLT preview, proxy, hardware, or drift gates below.

## M0 — native media feasibility

1. Pin one official Shotcut SDK archive and checksum in `toolchains/windows.dependencies.json`.
2. Add `MltRuntime`: repository initialization, producer creation, SDL2 consumer, seek/play/pause,
   and deterministic teardown on a dedicated media thread.
3. Extend the delivered FFprobe `QProcess` worker (codecs, rates/time bases, rotation,
   format/stream duration, audio layout, and basic color properties) with complete HDR metadata,
   stronger VFR evidence, and a shared scheduler/result bridge.
4. Implement proxy/original switching at the graph-builder boundary and DNxHR-LB proxy jobs.
5. Implement render consumer progress/cancel plus an exact-frame test harness.
6. Run the representative H.264/HEVC fixture matrix on Windows/NVIDIA and record results under
   `benchmarks/m0/`.

Gate: one-hour output has less than one frame A/V drift, no boundary black frames, and 720p proxy
playback sustains 30 fps. UI expansion does not waive this gate.

## M1 — complete rough-cut loop

1. Desktop and advertised MCP import now probe and fingerprint; retire the hidden provisional MCP compatibility
   API with the same worker/result contract.
2. Persist SQLite cache metadata and generate thumbnails plus multiresolution waveform tiles.
3. Bind source/program consumers to the QML viewer and expose frame-step/J-K-L state.
4. Continue the controller-backed timeline slice: selection, append, exact-frame linked trim/move,
   snap, markers, in/out marks, track toggles, and linked targeting now work. Replace its lightweight
   QML lane projection with thumbnail/waveform-backed virtualization, pointer drag/trim, insert modes,
   and source/program viewer coordination.
5. Single-asset missing/modified relink now preserves durable asset and clip identity; add batch
   relink, low-disk policy, recovery offer, and interrupted-job retry.

Gate: synthetic media matrix, 500-asset stress project, forced termination during each job type,
and clean recovery without touching originals.

## M2 — safe dialogue cleanup

1. Add FFmpeg 16 kHz mono extraction and noise-floor, VAD, word-gap, and `silencedetect` workers.
2. Implement the named-pipe JSON-RPC provider host and CrisperWhisper adapter behind
   `IAnalysisProvider`; model download requires license display, acceptance, checksum, and removal.
3. Fuse evidence, snap boundaries to frame/low-energy points, and classify unsafe handles as
   manual-only. Contextual phrases remain review-only by default.
4. Bind transcript context, waveform, audition, filters, accept/reject/skip, and stale-state UI.
5. Add DNxHR-HQ/PCM and YouTube H.264/AAC export presets and Windows packaging.

Gate: labelled-corpus precision/recall targets, median boundary error below 80 ms, and zero lexical
words clipped by any suggestion classified bulk-safe.

## M3 — visual composition

Add dynamic tracks, lock/mute/solo/visibility, captions, text/image/color generators, speed/freeze,
audio metering and processing, transforms/crop/opacity, keyframes, transitions, normalized scene
presets, and preset serialization. Each feature mutates the canonical model first and projects to
MLT; direct MLT mutations are prohibited.

Gate: serialization migration, keyframe interpolation, mixed-rate edit, transition boundary, and
render-golden coverage for every new model feature.

## M4 — camera and workflow polish

Add proxy-resolution MediaPipe landmark jobs, smoothed disposable tracks, conservative masked face
smoothing, manual mask fallback, captions/SRT, repeated-take review, and project/channel templates.
Face shaping, makeup, teeth, and eye modification remain explicitly out of scope.

## MCP evolution rules

- MCP uses the canonical project workflows and command transactions; no separate project format.
- Tool calls must require explicit destination paths for writes and must never overwrite media.
- Probe-derived operations reject assets whose durable `probe` record is absent; fingerprints are
  integrity identity and are never overloaded as probe state.
- Long jobs will return job IDs and use the same scheduler/cancellation state as the desktop UI.
- Adding export or cleanup tools requires the same safety gates as their GUI equivalents.
