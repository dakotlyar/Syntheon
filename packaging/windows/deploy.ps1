# SPDX-License-Identifier: GPL-3.0-or-later
#
# Adds Qt (DLLs, plugins, QML imports) to the `cmake --install` tree with
# windeployqt. For an ARM64 cross build the HOST windeployqt is run with
# --qtpaths pointing at the TARGET Qt's qtpaths (Qt's documented cross route),
# so it deploys ARM64 binaries.
#
#   pwsh packaging/windows/deploy.ps1 -Stage <folder> -HostQt <x64 Qt> [-TargetQt <ARM64 Qt>]
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$HostQt,
    [string]$TargetQt
)
$ErrorActionPreference = 'Stop'
$tool = Join-Path $HostQt 'bin\windeployqt.exe'
$qmlDir = Join-Path $PSScriptRoot '..\..\src\app\qml'
$args = @('--release', '--no-translations', '--no-compiler-runtime', '--no-opengl-sw', '--no-system-d3d-compiler',
          '--qmldir', $qmlDir, (Join-Path $Stage 'SynBench.exe'))
if ($TargetQt) {
    # The aqt "arm64_cross_compiled" Qt ships host-qtpaths.bat: a wrapper that
    # runs the HOST qtpaths with the target's configuration (target_qt.conf).
    # That is the one windeployqt wants (its own qtpaths.exe is an ARM64 program
    # an x64 machine can't run). Using the host's plain qtpaths instead would
    # quietly deploy x64 DLLs into the ARM64 tree, so there is no such fallback.
    $bin = Join-Path $TargetQt 'bin'
    # The target Qt's configuration names the host Qt as its sibling folder
    # (HostPrefix=../../msvc2022_64) and host-qtpaths.bat runs qtpaths from
    # there. When the host Qt was installed elsewhere, point a junction at it.
    $sibling = Join-Path (Split-Path $TargetQt -Parent) 'msvc2022_64'
    if (-not (Test-Path $sibling)) {
        Write-Host "linking $sibling -> $HostQt (where the target Qt expects its host Qt)"
        New-Item -ItemType Junction -Path $sibling -Target $HostQt | Out-Null
    }
    $qtpaths = 'host-qtpaths.bat', 'host-qtpaths6.bat', 'qtpaths.bat', 'qtpaths6.bat' |
        ForEach-Object { Join-Path $bin $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $qtpaths) {
        [Console]::Error.WriteLine("--- qtpaths* in $bin")
        Get-ChildItem $bin -Filter '*qtpaths*' -ErrorAction SilentlyContinue | ForEach-Object { [Console]::Error.WriteLine($_.FullName) }
        [Console]::Error.WriteLine("--- everything in $bin")
        Get-ChildItem $bin -ErrorAction SilentlyContinue | ForEach-Object { [Console]::Error.WriteLine($_.Name) }
        throw "no host-qtpaths(.bat)/qtpaths(.bat) in $bin"
    }
    Write-Host "windeployqt --qtpaths $qtpaths"
    $args = @('--qtpaths', $qtpaths) + $args
}
& $tool @args
if ($LASTEXITCODE -ne 0) { throw "windeployqt exited with $LASTEXITCODE" }
# The GPU worker (synbench-gpu.exe, next to SynBench.exe) runs its probe and its headless mode on Qt's
# offscreen platform plugin. windeployqt analysed SynBench.exe only and deploys qwindows.dll alone, so
# qoffscreen.dll is copied here, from the TARGET Qt (the ARM64 one on the cross route, never the host's
# x64 copy; pe-check.ps1 then checks its architecture and its imports like every other DLL).
$qtRoot = if ($TargetQt) { $TargetQt } else { $HostQt }
$offscreen = Join-Path $qtRoot 'plugins\platforms\qoffscreen.dll'
if (-not (Test-Path $offscreen)) { throw "no offscreen platform plugin at ${offscreen}: the GPU worker's probe needs it" }
$platforms = Join-Path $Stage 'platforms'   # where windeployqt put qwindows.dll
if (-not (Test-Path (Join-Path $platforms 'qwindows.dll'))) { $platforms = Join-Path $Stage 'plugins\platforms' }
if (-not (Test-Path (Join-Path $platforms 'qwindows.dll'))) { throw "windeployqt deployed no qwindows.dll under $Stage" }
Copy-Item $offscreen $platforms -Force
Write-Host "copied $offscreen -> $platforms"
# Plugins SynBench does not use. The SQL drivers other than SQLite need client
# libraries we do not ship (OCI.dll, LIBPQ.dll, fbclient.dll, MIMAPI64.dll...),
# which pe-check.ps1 rightly rejects; and nothing here needs the others below.
# What is kept is named, so a new plugin has to be added on purpose.
$keep = @{
    'sqldrivers'   = @('qsqlite.dll')
    'imageformats' = @('qsvg.dll', 'qjpeg.dll', 'qico.dll', 'qgif.dll')
    'platforms'    = @('qwindows.dll', 'qoffscreen.dll')
}
foreach ($folder in $keep.Keys) {
    $dir = Join-Path $Stage "plugins\$folder"
    if (-not (Test-Path $dir)) { $dir = Join-Path $Stage $folder }   # windeployqt puts plugin folders next to the exe
    if (-not (Test-Path $dir)) { continue }
    Get-ChildItem $dir -File | Where-Object { $keep[$folder] -notcontains $_.Name.ToLowerInvariant() } | ForEach-Object {
        Write-Host "removing unused plugin $folder\$($_.Name)"
        Remove-Item -Force $_.FullName
    }
}
foreach ($unused in 'generic', 'qmltooling', 'styles') {
    foreach ($base in @($Stage, (Join-Path $Stage 'plugins'))) {
        Remove-Item -Recurse -Force (Join-Path $base $unused) -ErrorAction SilentlyContinue
    }
}
# Only the Basic style is used; the other QML styles would just add size.
foreach ($style in 'Material', 'Universal', 'Fusion', 'Imagine', 'FluentWinUI3', 'Windows') {
    Remove-Item -Recurse -Force (Join-Path $Stage "qml\QtQuick\Controls\$style") -ErrorAction SilentlyContinue
}
Write-Host "deployed Qt into $Stage"
