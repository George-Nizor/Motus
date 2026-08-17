# Windows bootstrap

Target Windows 11 x64 using an MSYS2 MinGW64 shell. WSL may edit files but is not the supported
runtime or packaging environment.

For the automated path, run `Instrumenta.cmd setup motus` from the workspace root. The steps below
describe the internal `scripts/bootstrap-windows.ps1` implementation and remain the
troubleshooting/reference path.

1. Install MSYS2 in `C:\msys64`.
2. In an MSYS2 terminal, update the base system, restart the terminal if requested, then install:

   ```bash
   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils \
     mingw-w64-x86_64-make mingw-w64-x86_64-pkgconf mingw-w64-x86_64-cmake \
     mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-declarative \
     mingw-w64-x86_64-qt6-multimedia mingw-w64-x86_64-ffmpeg
   ```

3. Complete M0 dependency selection in `toolchains/windows.dependencies.json`. Record the exact
   official Shotcut SDK URL and SHA-256; do not accept a floating “latest” URL.
4. Extract that SDK to a path without spaces, set `CMAKE_PREFIX_PATH` to the SDK prefix, and run
   the Windows preset shown in the README.
5. Preserve the completed lock manifest with benchmark results. CI should use the same archive
   and fail closed on a checksum mismatch.

Once the toolchain is present, Instrumenta owns the repetitive steps. Click Motus's refresh icon,
or run `Instrumenta.cmd build` from the workspace root. It configures, builds, tests, installs to
an isolated `Motus/dist/windows.next` staging directory, runs Qt's deployment script and the native
launch handshake, then promotes it to `Motus/dist/windows`. If deployment or verification fails, the
last working bundle is retained. The bootstrap explicitly stages `ffprobe.exe`/`ffmpeg.exe`, closes
their transitive DLL imports, and runs a synthetic probe/render/re-probe smoke with only system
directories on `PATH`. The result launches without MSYS2, FFmpeg, or a separate Qt installation on
the destination computer.

The staged runtime is also resolved back to the pacman packages that supplied each flattened binary.
Bootstrap writes `THIRD_PARTY_NOTICES.txt`, a machine-readable `third-party-packages.json`, available
package license texts, FFmpeg's embedded license/configuration output, and exact MSYS2 source-package
provenance into the bundle. Bundle verification fails when those artifacts are absent or stale.

This provenance gate is necessary but not sufficient for publication. The current MSYS2 FFmpeg 9.0
package identifies itself as GPL-3.0-or-later and is built with `--enable-gpl`, `--enable-version3`, and
`--enable-libx264`; its runtime closure includes the GPL-2.0-or-later x264 library. The binary packages
themselves install no FFmpeg/x264 COPYING files, so bootstrap stages the applicable GNU license texts
and the installed x264 header notice explicitly. Before public distribution, release engineering must
publish or accompany the exact complete corresponding sources and build materials and obtain a review
of the application/plugin license boundary. It must also acquire authoritative terms/notices for every
transitive package that installs no license file; the generated inventory names those gaps explicitly.
Source-package links establish provenance only; they are not a claim that a third party fulfils
Instrumenta's distribution duties.

Qt Quick Controls 2 is included by the current `qt6-declarative` package; MSYS2 does not publish a
separate Qt6 Quick Controls 2 package. The checked-in dependency file deliberately refuses to invent an SDK version or checksum. Those
values must come from the archive actually proven by the M0 media tests. Qt 6, MLT 7.40, FFmpeg
8.1, and SDL2 are the requested compatibility targets, but the archive's real contents win and
must be audited before release.
