# Architecture

## State boundaries

`ve::Project` is the source of truth. UI and MLT objects are projections and must never become
the only copy of an edit. Every mutation runs through `ProjectCommand`; commands validate the
result and roll back on failure. Bulk cleanup constructs a replacement snapshot and therefore
appears as one undo operation.

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

Probe, proxy, waveform, transcription, face analysis, and render work goes through
`JobScheduler`. Work receives a stop token and bounded progress callback. CPU concurrency is
configurable; regardless of worker count, only one GPU job can run. The journal is diagnostic
state, not project state. A future startup loader will classify unfinished journal records as
interrupted and offer retry.

## Dependency seams still to implement

- MLT repository/consumer lifecycle and SDL2 audio/video output.
- FFmpeg probe, extraction, waveform, silence, proxy, and export workers.
- Windows named-pipe JSON-RPC transport and Python provider host.
- Qt model/view bindings and timeline interaction layer.
- SQLite cache index and disk-space policy.

