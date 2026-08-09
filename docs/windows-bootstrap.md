# Windows bootstrap

Target Windows 11 x64 using an MSYS2 MinGW64 shell. WSL may edit files but is not the supported
runtime or packaging environment.

1. Install MSYS2 in `C:\msys64`.
2. In an MSYS2 terminal, update the base system, restart the terminal if requested, then install:

   ```bash
   pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
     mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-declarative \
     mingw-w64-x86_64-qt6-quickcontrols2
   ```

3. Complete M0 dependency selection in `toolchains/windows.dependencies.json`. Record the exact
   official Shotcut SDK URL and SHA-256; do not accept a floating “latest” URL.
4. Extract that SDK to a path without spaces, set `CMAKE_PREFIX_PATH` to the SDK prefix, and run
   the Windows preset shown in the README.
5. Preserve the completed lock manifest with benchmark results. CI should use the same archive
   and fail closed on a checksum mismatch.

The checked-in dependency file deliberately refuses to invent an SDK version or checksum. Those
values must come from the archive actually proven by the M0 media tests. Qt 6, MLT 7.40, FFmpeg
8.1, and SDL2 are the requested compatibility targets, but the archive's real contents win and
must be audited before release.

