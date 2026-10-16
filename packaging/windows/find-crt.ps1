# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints ONE folder holding the TARGET architecture's VC++ runtime and
# Universal CRT DLLs (copied there, machine type checked from each PE header),
# for -DSYNBENCH_CRT_DIRS=... (P16: the CRT
# ships app-local, so neither installer needs a redistributable step). Asking
# for the target architecture explicitly is the only reliable way to get the
# ARM64 files in an x64 -> ARM64 cross build.
#
# Folder layouts differ between Visual Studio / SDK versions, so every known
# layout is searched and the newest hit wins:
#   VC++ runtime  <VS>\VC\Redist\MSVC\<ver>\<arch>\Microsoft.VC*.CRT
#   UCRT          <Kits>\10\Redist\<sdk ver>\ucrt\DLLs\<arch>   (newer SDKs)
#                 <Kits>\10\Redist\ucrt\DLLs\<arch>             (older SDKs)
# The UCRT is part of Windows 10 (the baseline, 1809+), so a missing UCRT
# redistributable is a warning; a missing VC++ runtime is an error. On any
# miss the directory listing is printed, to see what the machine has.
#
#   pwsh packaging/windows/find-crt.ps1 -Arch x64|arm64
param([Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Arch)
$ErrorActionPreference = 'Stop'

function Show-Listing([string]$path, [int]$depth = 2) {
    [Console]::Error.WriteLine("--- listing of $path (depth $depth)")
    if (Test-Path $path) {
        Get-ChildItem $path -Directory -Recurse -Depth $depth -ErrorAction SilentlyContinue |
            ForEach-Object { [Console]::Error.WriteLine($_.FullName) }
    } else {
        [Console]::Error.WriteLine('(does not exist)')
    }
}

function Sort-ByVersion($dirs) {
    # "10.0.26100.0" and "14.44.35208" sort numerically, not as text.
    $dirs | Sort-Object { try { [version]($_.Name -replace '[^0-9.]', '') } catch { [version]'0.0' } } -Descending
}

# The machine type from a PE file's header (0x8664 x64, 0xAA64 ARM64).
function Get-PeMachine([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $b = New-Object byte[] 64
        [void]$fs.Read($b, 0, 64)
        $peOffset = [BitConverter]::ToInt32($b, 0x3C)
        $fs.Position = $peOffset
        $h = New-Object byte[] 6
        [void]$fs.Read($h, 0, 6)
        if ($h[0] -ne 0x50 -or $h[1] -ne 0x45) { return 'not a PE file' }
        switch ([BitConverter]::ToUInt16($h, 4)) { 0x8664 { 'x64' } 0xAA64 { 'arm64' } 0x14C { 'x86' } default { 'other' } }
    } finally { $fs.Dispose() }
}

# Collects the DLLs of the TARGET architecture only. Anything else found in a
# redist folder (an x64 DLL in the ARM64 tree, e.g. vcruntime140_1.dll, which
# only exists for x64) is reported and left out, never shipped: a mixed tree is
# what pe-check.ps1 exists to reject, and if a shipped binary really imports a
# left-out DLL, pe-check says so by name. The log shows what each folder holds.
function Copy-MatchingDlls([string]$folder, [string]$destination) {
    foreach ($dll in Get-ChildItem $folder -Filter *.dll -File) {
        $m = Get-PeMachine $dll.FullName
        if ($m -eq $Arch) {
            [Console]::Error.WriteLine("  keep  $($dll.Name)  ($m)")
            Copy-Item $dll.FullName $destination -Force
        } else {
            [Console]::Error.WriteLine("  SKIP  $($dll.Name)  is $m, not $Arch")
        }
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { throw 'Visual Studio not found' }

# --- VC++ runtime
$redist = Join-Path $vs 'VC\Redist\MSVC'
$crt = $null
foreach ($ver in (Sort-ByVersion (Get-ChildItem $redist -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d' }))) {
    $crt = Get-ChildItem (Join-Path $ver.FullName $Arch) -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($crt) { break }
}
if (-not $crt) {
    Show-Listing $redist 3
    throw "no $Arch Microsoft.VC*.CRT folder under $redist"
}

# --- Universal CRT
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Redist'
$candidates = @()
foreach ($ver in (Sort-ByVersion (Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d' }))) {
    $candidates += Join-Path $ver.FullName "ucrt\DLLs\$Arch"
}
$candidates += Join-Path $kits "ucrt\DLLs\$Arch"
$ucrt = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ucrt) {
    Show-Listing $kits 4
    [Console]::Error.WriteLine("warning: no $Arch UCRT redistributable under $kits; relying on the UCRT that ships with Windows 10")
}

$out = Join-Path ($(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() })) "synbench-crt-$Arch"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($folder in @($crt.FullName, $ucrt) | Where-Object { $_ }) {
    [Console]::Error.WriteLine("runtime folder: $folder")
    Copy-MatchingDlls $folder $out
}
if (-not (Get-ChildItem $out -Filter *.dll)) { throw "no $Arch runtime DLLs found" }
$out
