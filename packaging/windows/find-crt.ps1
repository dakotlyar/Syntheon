# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints the folders holding the TARGET architecture's VC++ runtime and
# Universal CRT DLLs, ';'-separated, for -DSYNBENCH_CRT_DIRS=... (P16: the CRT
# ships app-local, so neither installer needs a redistributable step). Asking
# for the target architecture explicitly is the only reliable way to get the
# ARM64 files in an x64 -> ARM64 cross build.
#
#   pwsh packaging/windows/find-crt.ps1 -Arch x64|arm64
param([Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Arch)
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio not found' }

$redist = Join-Path $vs 'VC\Redist\MSVC'
$version = Get-ChildItem $redist -Directory | Where-Object { $_.Name -match '^\d' } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $version) { throw "no VC++ redistributable under $redist" }
$crt = Get-ChildItem (Join-Path $version.FullName $Arch) -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crt) { throw "no $Arch Microsoft.VC*.CRT folder under $($version.FullName)" }

$ucrt = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Redist\ucrt\DLLs\$Arch"
if (-not (Test-Path $ucrt)) { throw "no UCRT redistributable folder $ucrt" }

"$($crt.FullName);$ucrt"
