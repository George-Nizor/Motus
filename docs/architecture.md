# Architecture

## State boundaries

`ve::Project` is the source of truth. UI and MLT objects are projections and must never become
the only copy of an edit. Every mutation runs through `ProjectCommand`; commands validate the
result and roll back on failure. Bulk cleanup constructs a replacement snapshot and therefore
appears as one undo operation.

Linked-group identity is sequence-local and identifies one contemporaneous A/V edit pair. A split
creates one new shared linked-group identity for the right-side pair, and a duplicated cleaned
sequence remaps clip, link, effect, transition, and marker identifiers so later edits cannot leak
back into its source sequence. Ripple edits collapse markers inside the removed interval, shift
later markers, remap outgoing transitions to surviving right fragments, and discard transitions
whose endpoints no longer exist.

The model uses half-open ranges (`[start, end)`). MLT uses inclusive clip out-points, so
`buildMltGraph` performs the conversion in exactly one place (`out = in + duration - 1`). Preview
graphs may substitute registered proxies. Render graphs ignore proxies by design.

## Durable and disposable files

```text
Show/
  edit.veproj                 versioned JSON manifest
  edit.veproj.bak             previous atomic save
  edit.ve-cache/              disposable
    metadata.sqlite
    proxies/
    thumbnails/
    waveforms/
    transcripts/
    analysis/
    diagnostics/
```

An asset records its absolute path, optional project-relative path, byte size, modification time,
and a SHA-256 of sampled head/tail content. Cache keys additionally include stream, component,
component version, and canonical settings. A fingerprint mismatch marks dependent cleanup
suggestions stale but does not mutate or silently relink the source.

## Analysis process

`IAnalysisProvider` is the in-process boundary corresponding to protocol version 1. The Windows
adapter will send newline-delimited JSON-RPC 2.0 over a local named pipe. Requests carry only an
asset fingerprint, audio stream, language, model, and settings; media is exchanged by an
explicit local extraction job, not embedded in messages. Results carry timestamped words/events,
confidence, provider/model versions, and a cache key.

Model downloads must remain an explicit UI action following license display and acceptance.
CrisperWhisper's non-commercial terms mean commercial or monetized use remains blocked until the
model is replaced or licensed.

## Threading and jobs

Proxy, waveform, transcription, and face-analysis work goes through
`JobScheduler`. Work receives a stop token and bounded progress callback. CPU concurrency is
configurable; regardless of worker count, only one GPU job can run. The journal is diagnostic
state, not project state. A future startup loader will classify unfinished journal records as
interrupted and offer retry.

Desktop FFprobe and FFmpeg export currently use cancellable `QProcess` workers because they need
typed GUI-thread completion and native process diagnostics not yet exposed by `JobScheduler`.
Probe metadata is durable project state. Exports snapshot the canonical project, validate the
supported simple-timeline subset, render originals to a sibling partial file, and publish only on
success.

## Dependency seams still to implement

- MLT repository/consumer lifecycle and SDL2 audio/video output.
- FFmpeg extraction, waveform, silence, and proxy workers; richer export presets.
- Windows named-pipe JSON-RPC transport and Python provider host.
- Qt model/view bindings and timeline interaction layer.
- Drag-based timeline manipulation and a thumbnail/waveform-backed virtualized timeline (the current
  controller provides exact-frame selection, move/trim/remove, markers, track toggles, snap, and zoom).
- SQLite cache index and disk-space policy.
