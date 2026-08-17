[CmdletBinding()]
param(
    [switch]$ToolchainOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$MotusRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = $MotusRoot
$TemporaryRoot = $null
$MsysRoot = 'C:\msys64'
$Bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
$MingwBin = Join-Path $MsysRoot 'mingw64\bin'
$Packages = @(
    # Keep this list explicit. `mingw-w64-x86_64-toolchain` is a pacman group and
    # can open an interactive member-selection prompt when launched by Electron.
    'mingw-w64-x86_64-gcc',
    'mingw-w64-x86_64-binutils',
    'mingw-w64-x86_64-make',
    'mingw-w64-x86_64-pkgconf',
    'mingw-w64-x86_64-cmake',
    'mingw-w64-x86_64-ninja',
    'mingw-w64-x86_64-qt6-base',
    # Qt Quick Controls 2 is shipped by the current Qt6 declarative package;
    # there is no separate qt6-quickcontrols2 package in current MSYS2.
    'mingw-w64-x86_64-qt6-declarative',
    'mingw-w64-x86_64-qt6-multimedia',
    # Packaged interface glyphs are SVG assets. Linking Qt Svg makes both the
    # renderer and qsvg image-format plugin explicit deployment dependencies.
    'mingw-w64-x86_64-qt6-svg',
    # Motus invokes these executables directly for durable probing and the supported simple
    # original-media export preset. Their DLL closure is collected after they are staged.
    'mingw-w64-x86_64-ffmpeg'
)

function Write-Stage([string]$Message) {
    Write-Host "`n  $Message" -ForegroundColor Cyan
}

function Assert-Command([string]$Description) {
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE." }
}

function Invoke-MsysPacman([string]$Arguments, [string]$Description) {
    & $Bash -lc "pacman $Arguments"
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE. Close all MSYS2 terminals and run Instrumenta.cmd setup motus again from the workspace root."
    }
}

# The runtime collector is shared with Instrumenta's packaging step, so it is
# written once in JavaScript. Prefer a system Node.js; otherwise let MSYS2
# supply one so the internal bootstrap still works on a machine without Node.
function Resolve-Node {
    $System = Get-Command 'node.exe' -ErrorAction SilentlyContinue
    if ($System) { return $System.Source }
    $Mingw = Join-Path $MingwBin 'node.exe'
    if (Test-Path $Mingw) { return $Mingw }
    Write-Stage 'Installing the Node.js helper used to assemble the portable bundle…'
    Invoke-MsysPacman '-S --needed --noconfirm --noprogressbar mingw-w64-x86_64-nodejs' 'Node.js installation'
    if (-not (Test-Path $Mingw)) { throw "Node.js was not installed at $Mingw" }
    return $Mingw
}

if (-not (Test-Path $Bash)) {
    $Winget = Get-Command 'winget.exe' -ErrorAction SilentlyContinue
    if (-not $Winget) {
        throw 'MSYS2 is missing and winget is unavailable. Install MSYS2 from https://www.msys2.org, then run this file again.'
    }
    Write-Stage 'Installing the Motus native toolchain host (MSYS2)…'
    & winget.exe install --id MSYS2.MSYS2 --exact --accept-package-agreements --accept-source-agreements
    Assert-Command 'MSYS2 installation'
}

if (-not (Test-Path $Bash)) { throw "MSYS2 did not create $Bash" }

Write-Stage 'Updating MSYS2 package metadata…'
try {
    Invoke-MsysPacman '-Syu --noconfirm --noprogressbar' 'MSYS2 system update'
} catch {
    # pacman may replace msys2-runtime and close the process that invoked it.
    # A second invocation completes the update after that automatic restart.
    Write-Host '  MSYS2 requested a restart; continuing with the follow-up update…' -ForegroundColor Yellow
}
Invoke-MsysPacman '-Su --noconfirm --noprogressbar' 'MSYS2 follow-up update'

Write-Stage 'Installing the compiler, CMake, Ninja, and Qt 6…'
$PackageCommand = 'pacman -S --needed --noconfirm --noprogressbar ' + ($Packages -join ' ')
Invoke-MsysPacman (($PackageCommand -replace '^pacman\s+', '')) 'Motus toolchain installation'

if ($ToolchainOnly) {
    Write-Host "`n  Motus prerequisites are ready." -ForegroundColor Green
    exit 0
}

try {
    if ($MotusRoot.StartsWith('\') -or $MotusRoot.StartsWith('//')) {
        $TemporaryRoot = Join-Path $env:TEMP ("instrumenta-motus-" + [guid]::NewGuid().ToString('N'))
        $BuildRoot = Join-Path $TemporaryRoot 'Motus'
        New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
        Write-Stage "Motus is on a Windows UNC share; building in a local mirror at ${BuildRoot}…"
        $Excluded = @('.git', 'build', 'dist')
        Get-ChildItem -LiteralPath $MotusRoot -Force |
            Where-Object { $Excluded -notcontains $_.Name } |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $BuildRoot $_.Name) -Recurse -Force }
    }

    $env:Path = "$MingwBin;$env:Path"
    $Node = Resolve-Node
    Set-Location -LiteralPath $BuildRoot
    Write-Stage 'Verifying packaged Motus interface icons…'
    & $Node (Join-Path $BuildRoot 'scripts\generate-qml-icons.cjs') '--check'
    Assert-Command 'Motus QML icon verification'
    Write-Stage 'Configuring Motus…'
    & (Join-Path $MingwBin 'cmake.exe') --preset windows-mingw-release
    Assert-Command 'Motus configuration'

    Write-Stage 'Building Motus…'
    & (Join-Path $MingwBin 'cmake.exe') --build --preset windows-mingw-release
    Assert-Command 'Motus build'

    Write-Stage 'Testing Motus…'
    & (Join-Path $MingwBin 'ctest.exe') --test-dir 'build/windows-mingw-release' --output-on-failure
    Assert-Command 'Motus tests'

    Write-Stage 'Creating the portable Motus bundle…'
    $SourceDistRoot = Join-Path $MotusRoot 'dist'
    $BuildDistRoot = Join-Path $BuildRoot 'dist'
    $CurrentBundle = Join-Path $SourceDistRoot 'windows'
    $StagedBundle = Join-Path $BuildDistRoot 'windows.next'
    $PublishedStagedBundle = Join-Path $SourceDistRoot 'windows.next'
    $PreviousBundle = Join-Path $SourceDistRoot 'windows.previous'
    New-Item -ItemType Directory -Force -Path $BuildDistRoot,$SourceDistRoot | Out-Null
    Remove-Item -LiteralPath $StagedBundle -Recurse -Force -ErrorAction SilentlyContinue
    & (Join-Path $MingwBin 'cmake.exe') --install 'build/windows-mingw-release' --prefix $StagedBundle
    Assert-Command 'Motus deployment'

    if (-not (Test-Path (Join-Path $StagedBundle 'motus.exe')) -or
        -not (Test-Path (Join-Path $StagedBundle 'motus-bundle.json'))) {
        throw 'The Motus core built, but the desktop executable was not produced. Confirm that the Qt packages installed successfully.'
    }

    foreach ($MediaTool in @('ffmpeg.exe', 'ffprobe.exe')) {
        $MediaToolSource = Join-Path $MingwBin $MediaTool
        if (-not (Test-Path $MediaToolSource)) {
            throw "The FFmpeg package did not provide $MediaToolSource"
        }
        Copy-Item -LiteralPath $MediaToolSource -Destination (Join-Path $StagedBundle $MediaTool) -Force
    }

    # Qt's deployment helper copies the Qt libraries into a `bin` folder and the
    # QML/plugin tree into `share`, but it never copies the MSYS2 libraries that
    # Qt itself imports (libpcre2, libicu*, zlib1, libharfbuzz, …).  On this
    # machine the loader still finds them through MSYS2 on PATH; anywhere else
    # the process dies with 0xC0000135.  Walk the real PE import tables instead
    # and copy the whole transitive closure beside motus.exe.
    $RootQtConfig = Join-Path $StagedBundle 'qt.conf'
    @'
[Paths]
Prefix = .
Plugins = share/qt6/plugins
QmlImports = share/qt6/qml
'@ | Set-Content -LiteralPath $RootQtConfig -Encoding UTF8

    Write-Stage 'Collecting the portable Motus runtime…'
    $BundleRuntimeScript = Join-Path $BuildRoot 'scripts\bundle-runtime.cjs'
    & $Node $BundleRuntimeScript 'complete' $StagedBundle '--search' $MingwBin '--flatten'
    Assert-Command 'Motus runtime collection'

    # Flattening the runtime severs the visible relationship between each DLL
    # and the MSYS2 package that supplied it. Rebuild that relationship from
    # pacman's local database, preserve the installed license texts, and record
    # FFmpeg's own license/configuration output before the bundle is accepted.
    Write-Stage 'Recording third-party packages, licenses, and source provenance…'
    $NoticeScript = Join-Path $BuildRoot 'scripts\generate-third-party-notices.cjs'
    $PacmanDatabase = Join-Path $MsysRoot 'var\lib\pacman\local'
    $MingwRoot = Join-Path $MsysRoot 'mingw64'
    & $Node $NoticeScript $StagedBundle '--pacman-db' $PacmanDatabase '--mingw-root' $MingwRoot
    Assert-Command 'Motus third-party notice generation'

    Write-Stage 'Verifying that the bundle needs nothing from this computer…'
    & $Node $BundleRuntimeScript 'check' $StagedBundle
    Assert-Command 'Motus bundle self-containment check'

    # Run the launch handshake the way an end user's computer will: without
    # MSYS2, Qt, or any other developer tool on PATH.  Probing with the build
    # PATH is what previously let an incomplete bundle look healthy here and
    # then fail during packaging.
    Write-Stage 'Checking the deployed Qt runtime without developer tools on PATH…'
    $ProbeRoot = Join-Path $env:TEMP ("instrumenta-motus-probe-" + [guid]::NewGuid().ToString('N'))
    $ProbeMarker = Join-Path $ProbeRoot 'ready.txt'
    $IconMarker = Join-Path $ProbeRoot 'icons.txt'
    $IconScreenshot = Join-Path $ProbeRoot 'icons.png'
    New-Item -ItemType Directory -Force -Path $ProbeRoot | Out-Null
    $BuildPath = $env:Path
    try {
        $SystemRoot = if ($env:SystemRoot) { $env:SystemRoot } else { 'C:\Windows' }
        $env:Path = "$SystemRoot\system32;$SystemRoot;$SystemRoot\system32\Wbem"
        $ProbeProcess = Start-Process -FilePath (Join-Path $StagedBundle 'motus.exe') `
            -ArgumentList @('--instrumenta-launch-check', $ProbeMarker) `
            -WorkingDirectory $StagedBundle -Wait -PassThru -WindowStyle Hidden
        $IconProcess = Start-Process -FilePath (Join-Path $StagedBundle 'motus.exe') `
            -ArgumentList @('--instrumenta-icon-check', $IconMarker, $IconScreenshot) `
            -WorkingDirectory $StagedBundle -Wait -PassThru
    } finally {
        $env:Path = $BuildPath
    }
    if ($ProbeProcess.ExitCode -ne 0 -or -not (Test-Path $ProbeMarker) -or
        ((Get-Content -LiteralPath $ProbeMarker -Raw) -notmatch 'MOTUS_LAUNCH_OK\s+\d+\.\d+\.\d+')) {
        Remove-Item -LiteralPath $ProbeRoot -Recurse -Force -ErrorAction SilentlyContinue
        throw "Motus native runtime check failed with exit code $($ProbeProcess.ExitCode)."
    }
    if ($IconProcess.ExitCode -ne 0 -or -not (Test-Path $IconMarker) -or
        ((Get-Content -LiteralPath $IconMarker -Raw) -notmatch 'MOTUS_ICONS_OK\s+folder=\d+\s+magnet=\d+\s+zoomIn=\d+')) {
        Remove-Item -LiteralPath $ProbeRoot -Recurse -Force -ErrorAction SilentlyContinue
        throw "Motus packaged icon render check failed with exit code $($IconProcess.ExitCode)."
    }
    $IconAuditImage = Join-Path $MotusRoot 'build\motus-icon-audit.png'
    $IconAuditLog = Join-Path $MotusRoot 'build\motus-icon-audit.txt'
    Copy-Item -LiteralPath $IconScreenshot -Destination $IconAuditImage -Force
    Copy-Item -LiteralPath $IconMarker -Destination $IconAuditLog -Force
    Remove-Item -LiteralPath $ProbeRoot -Recurse -Force -ErrorAction SilentlyContinue

    # The normal CTest run happens before installation and can see MSYS2 tools on PATH. Repeat
    # the executable end-to-end media smoke beside the staged bundle with a clean system PATH so
    # a package missing ffprobe/ffmpeg or their codec DLLs cannot be promoted.
    Write-Stage 'Probing and rendering synthetic media from the staged offline bundle…'
    $NativeSmoke = Join-Path $BuildRoot 'build\windows-mingw-release\motus_native_media_smoke.exe'
    if (-not (Test-Path $NativeSmoke)) { throw "Native media smoke executable was not built at $NativeSmoke" }
    $NativeSmokeBundle = Join-Path $StagedBundle 'motus_native_media_smoke.exe'
    Copy-Item -LiteralPath $NativeSmoke -Destination $NativeSmokeBundle -Force
    $BuildPath = $env:Path
    try {
        $SystemRoot = if ($env:SystemRoot) { $env:SystemRoot } else { 'C:\Windows' }
        $env:Path = "$SystemRoot\system32;$SystemRoot;$SystemRoot\system32\Wbem"
        & $NativeSmokeBundle
        Assert-Command 'Motus staged native media smoke'
    } finally {
        $env:Path = $BuildPath
        Remove-Item -LiteralPath $NativeSmokeBundle -Force -ErrorAction SilentlyContinue
    }

    if ($StagedBundle -ne $PublishedStagedBundle) {
        Remove-Item -LiteralPath $PublishedStagedBundle -Recurse -Force -ErrorAction SilentlyContinue
        Copy-Item -LiteralPath $StagedBundle -Destination $PublishedStagedBundle -Recurse -Force
    }

    Write-Stage 'Publishing the verified Motus bundle…'
    try {
        if (Test-Path $CurrentBundle) { Move-Item -LiteralPath $CurrentBundle -Destination $PreviousBundle }
        Move-Item -LiteralPath $PublishedStagedBundle -Destination $CurrentBundle
        Remove-Item -LiteralPath $PreviousBundle -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
        if (-not (Test-Path $CurrentBundle) -and (Test-Path $PreviousBundle)) {
            Move-Item -LiteralPath $PreviousBundle -Destination $CurrentBundle
        }
        throw
    }

    Write-Host "`n  Motus is verified and ready at $CurrentBundle\motus.exe" -ForegroundColor Green
} finally {
    if ($TemporaryRoot -and (Test-Path $TemporaryRoot)) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
