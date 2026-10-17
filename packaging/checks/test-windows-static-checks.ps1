# SPDX-License-Identifier: GPL-3.0-or-later
#
# Negative control for windows-static-checks.ps1: plants each kind of defect in
# a COPY of the stage tree and asserts the checks FAIL on it, as the Linux job
# does for elf-check. A check that can't fail proves nothing.
#
#   pwsh packaging/checks/test-windows-static-checks.ps1 -Tree <stage> -Arch x64|arm64
param(
    [Parameter(Mandatory = $true)][string]$Tree,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Arch
)
$ErrorActionPreference = 'Stop'
$checks = Join-Path $PSScriptRoot 'windows-static-checks.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) "synbench-negative-control-$Arch"

function Fresh-Copy {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item $Tree $work -Recurse
}

# Runs the checks on the (damaged) copy in a child shell, so a throw or an exit code can't leak out.
function Assert-Fails([string]$what) {
    $out = & pwsh -NoProfile -File $checks -Tree $work -Arch $Arch 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) { throw "the static checks PASSED on a tree with $what" }
    Write-Host "ok: the checks reject $what"
}

try {
    # 0. the real tree passes (so the failures below are caused by the plants)
    $null = & pwsh -NoProfile -File $checks -Tree $Tree -Arch $Arch 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'the unmodified tree does not pass the static checks' }

    # 1. a developer hook name inside a binary (8-bit and UTF-16)
    Fresh-Copy
    [IO.File]::AppendAllText((Join-Path $work 'SynBench.exe'), 'SYNBENCH_SMOKE', [Text.Encoding]::ASCII)
    Assert-Fails 'a SYNBENCH_ string (8-bit) in SynBench.exe'
    Fresh-Copy
    $bytes = [Text.Encoding]::Unicode.GetBytes('SYNBENCH_TOUR')
    $fs = [IO.File]::Open((Join-Path $work 'SynBench.exe'), 'Append'); $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
    Assert-Fails 'a SYNBENCH_ string (UTF-16) in SynBench.exe'

    # 2. a test-only binary
    Fresh-Copy
    Set-Content (Join-Path $work 'synbench-helper-test.exe') 'x'
    Assert-Fails 'a synbench-helper-test binary'

    # 3. a DLL of the wrong architecture
    Fresh-Copy
    $other = if ($Arch -eq 'x64') { 'arm64' } else { 'x64' }
    $sys = Join-Path $env:SystemRoot 'System32\kernel32.dll'   # the runner's own (x64) DLL
    if ($Arch -eq 'arm64') { Copy-Item $sys (Join-Path $work 'wrongarch.dll'); Assert-Fails 'an x64 DLL in the ARM64 tree' }
    else { Write-Host "skip: no $other DLL is available on this runner to plant in the x64 tree" }

    # 4. the app-local runtime is gone
    Fresh-Copy
    Remove-Item (Join-Path $work 'vcruntime140.dll')
    Assert-Fails 'vcruntime140.dll removed'

    # 5. a dependency nobody ships
    Fresh-Copy
    Remove-Item (Join-Path $work 'Qt6Core.dll')
    Assert-Fails 'Qt6Core.dll removed'

    # 6. the encryption worker is gone (Phase 2 §11.3)
    Fresh-Copy
    Remove-Item (Join-Path $work 'synbench-cpucrypto.exe')
    Assert-Fails 'synbench-cpucrypto.exe removed'

    # 7. an encryption worker of the wrong architecture
    if ($Arch -eq 'arm64') {
        Fresh-Copy
        Copy-Item (Join-Path $env:SystemRoot 'System32\whoami.exe') (Join-Path $work 'synbench-cpucrypto.exe') -Force
        Assert-Fails 'an x64 synbench-cpucrypto.exe in the ARM64 tree'
    } else { Write-Host 'skip: no arm64 executable is available on this runner to plant in the x64 tree' }

    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
} catch {
    Write-Host "::error::$($_.Exception.Message)"
    exit 1
}
# The last native command above was a check that was SUPPOSED to fail; in a CI
# step the step's exit code is the last native exit code, so reset it.
$global:LASTEXITCODE = 0
Write-Host 'the negative controls all failed as they should'
exit 0
