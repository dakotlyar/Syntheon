# SPDX-License-Identifier: GPL-3.0-or-later
#
# The built SynBench.exe must carry exactly one manifest, and it must be ours:
# requireAdministrator (P12/D1), PerMonitorV2 and long-path awareness, and no
# asInvoker left over from the linker's default. Prints the embedded manifest.
# Needs `mt.exe` (an MSVC developer environment); works on an ARM64 exe too.
#
#   pwsh packaging/checks/manifest-check.ps1 -Exe <SynBench.exe>
param([Parameter(Mandatory = $true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$out = Join-Path ([IO.Path]::GetTempPath()) 'synbench-embedded.manifest'
Remove-Item $out -ErrorAction SilentlyContinue
& mt.exe -nologo "-inputresource:$Exe;#1" "-out:$out"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out)) { throw "no manifest resource #1 in $Exe" }
$text = Get-Content $out -Raw
Write-Host $text
# Any other manifest resource id would be a second manifest.
& mt.exe -nologo "-inputresource:$Exe;#2" "-out:$out.2" 2>$null
if ($LASTEXITCODE -eq 0 -and (Test-Path "$out.2")) { throw "$Exe has a second manifest resource (#2)" }
if ($text -notmatch 'requireAdministrator') { throw 'the embedded manifest does not request requireAdministrator' }
if ($text -match 'asInvoker') { throw 'the embedded manifest still has an asInvoker execution level' }
if ($text -notmatch 'PerMonitorV2') { throw 'the embedded manifest lacks PerMonitorV2' }
if ($text -notmatch 'longPathAware') { throw 'the embedded manifest lacks longPathAware' }
Write-Host "OK: one manifest in $Exe, requireAdministrator"
