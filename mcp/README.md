# Motus MCP

`motus-mcp` is a native, local, newline-delimited stdio MCP server built from the same C++ core as
the desktop app. It references and fingerprints originals read-only; no tool writes source media.

## Build and launch

From any POSIX/WSL/macOS checkout with Node and a C++20 `g++` (or `CXX`) compiler:

```bash
node scripts/build-mcp.cjs
./build/agent/motus-mcp
```

On Windows, use the verified published executable at `dist/windows/motus-mcp.exe`. A CMake source
build remains available as `build/windows-mingw-release/motus-mcp.exe`.

Configure clients with stdio transport and no arguments. The server supports MCP protocol
`2025-11-25`, `2025-06-18`, and `2025-03-26`; unsupported client versions negotiate to
`2025-11-25` rather than being echoed.

## Tools

| Tool | Durable operation |
| --- | --- |
| `motus_create_project` | Create a `.veproj`; no overwrite by default. |
| `motus_inspect_project` | Return profiles, full probe metadata, assets, tracks, clips, transitions, and markers. |
| `motus_refresh_media_integrity` | Re-fingerprint originals and persist online/missing/modified state. |
| `motus_import_media` | Execute FFprobe, fingerprint, persist metadata, and append the real A/V lanes. |
| `motus_relink_media` | Re-probe and relink missing/modified media without changing asset or clip IDs. |
| `motus_split_clip` | Split a contemporaneous linked pair. |
| `motus_move_clip` | Move a contemporaneous linked pair. |
| `motus_trim_clip_start` / `motus_trim_clip_end` | Trim a linked pair at an exact frame. |
| `motus_ripple_delete` | Remove a half-open range across unlocked tracks and close the gap. |
| `motus_add_marker` / `motus_remove_marker` | Create or delete durable sequence markers. |
| `motus_set_track_state` | Set lock, mute, and visibility flags. |
| `motus_generate_mlt_graph` | Atomically write diagnostic original-media MLT XML. |
| `motus_export_simple` | Atomically render the supported gapless linked rough-cut subset to H.264/AAC MP4. |

Every project mutation requires `expectedRevision`; a stale value returns `revision_conflict`
without writing. Input schemas reject unknown fields. Results use `structuredContent`, closed
per-tool success/error output schemas, stable error codes, and explicit read/destructive/open-world
annotations. Graph/export paths cannot collide with the project or source media, and existing
outputs require `overwrite: true`.

`motus_export_simple` is synchronous. It has a bounded `timeoutSeconds` (default 1800, maximum
7200), kills the child process tree on timeout, and removes private partial output on failure.
The stdio server does not advertise mid-request cancellation; terminate the client/server process
to abort earlier. Desktop preview, in/out selection, zoom, playhead state, and undo history remain
desktop-session state rather than durable MCP operations.

`motus_append_media_reference` remains a hidden compatibility call for old clients. New clients
must use `motus_import_media`, which records real FFprobe metadata and is eligible for preview and
the supported native export.
