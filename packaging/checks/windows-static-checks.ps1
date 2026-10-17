# SPDX-License-Identifier: GPL-3.0-or-later
#
# All the static checks on a Windows stage tree, as ONE script that really
# fails. (In a PowerShell CI step only the LAST native command's exit code
# counts, so a failing no-dev-hooks.ps1 followed by a passing check used to go
# unnoticed.) Each check's exit status is tested here and the script throws.
#
#   pwsh packaging/checks/windows-static-checks.ps1 -Tree <stage> -Arch x64|arm64
param(
    [Parameter(Mandatory = $true)][string]$Tree,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Arch
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not (Test-Path $Tree)) { throw "no such tree: $Tree" }

function Check([string]$name, [scriptblock]$run) {
    $global:LASTEXITCODE = 0
    & $run
    if ($LASTEXITCODE -ne 0) { throw "static check failed: $name (exit code $LASTEXITCODE)" }
    Write-Host "ok: $name"
}

Check 'no developer hooks in the binaries' { & "$here/no-dev-hooks.ps1" -Tree $Tree }
Check 'no test-only binaries' { & bash "$here/no-test-binaries.sh" ($Tree -replace '\\', '/') }
Check "every file is $Arch and every dependency is present" { & "$here/pe-check.ps1" -Tree $Tree -Arch $Arch }

# The C/C++ runtime ships app-local (P16): the installers have no redistributable
# step, so these must be IN the tree. (vcruntime140_1.dll is the x64-only helper.)
$crt = @('vcruntime140.dll', 'msvcp140.dll')
if ($Arch -eq 'x64') { $crt += 'vcruntime140_1.dll' }
$missing = $crt | Where-Object { -not (Test-Path (Join-Path $Tree $_)) }
if ($missing) { throw "the app-local VC++ runtime is missing from the tree: $($missing -join ', ')" }
Write-Host "ok: the VC++ runtime is in the tree ($($crt -join ', '))"

# The CPU test's encryption worker ships next to SynBench.exe (Phase 2 §11.3);
# pe-check.ps1 above has checked its architecture and dependencies.
if (-not (Test-Path (Join-Path $Tree 'synbench-cpucrypto.exe'))) { throw 'synbench-cpucrypto.exe (the encryption worker) is missing from the tree' }
Write-Host 'ok: the encryption worker is in the tree'

$global:LASTEXITCODE = 0
exit 0
