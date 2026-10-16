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
    $qtpaths = Join-Path $TargetQt 'bin\qtpaths.bat'
    if (-not (Test-Path $qtpaths)) { $qtpaths = Join-Path $TargetQt 'bin\qtpaths6.bat' }
    if (-not (Test-Path $qtpaths)) { throw "no qtpaths(.bat) in $TargetQt\bin" }
    $args = @('--qtpaths', $qtpaths) + $args
}
& $tool @args
if ($LASTEXITCODE -ne 0) { throw "windeployqt exited with $LASTEXITCODE" }
# Only the Basic style is used; the other QML styles would just add size.
foreach ($style in 'Material', 'Universal', 'Fusion', 'Imagine', 'FluentWinUI3', 'Windows') {
    Remove-Item -Recurse -Force (Join-Path $Stage "qml\QtQuick\Controls\$style") -ErrorAction SilentlyContinue
}
Write-Host "deployed Qt into $Stage"
