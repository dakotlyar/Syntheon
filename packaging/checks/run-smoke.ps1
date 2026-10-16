# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a development build's SYNBENCH_SMOKE=1 launch so it cannot hang CI: the
# Windows error mode is set so a missing DLL or a crash fails the process at
# once instead of opening a modal dialog nobody can click (the child inherits
# it), the wait is bounded, and the exit code is reported (0xC0000135 =
# a DLL was not found, 0xC0000005 = access violation).
#
#   pwsh packaging/checks/run-smoke.ps1 -Exe build/windows-x64-release/bin/SynBench.exe [-TimeoutSeconds 60]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path

Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class ErrorMode
{
    [DllImport("kernel32.dll")] static extern uint SetErrorMode(uint mode);
    // SEM_FAILCRITICALERRORS (no "missing DLL" box) | SEM_NOGPFAULTERRORBOX (no crash box)
    public static void Quiet() { SetErrorMode(0x0001 | 0x0002); }
}
'@
[ErrorMode]::Quiet()

$env:SYNBENCH_SMOKE = '1'
$p = Start-Process -FilePath $Exe -PassThru
$null = $p.Handle   # keep the handle so ExitCode is available
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    & taskkill.exe /PID $p.Id /T /F | Out-Null
    $global:LASTEXITCODE = 1
    Write-Host "::error::the smoke launch did not finish in $TimeoutSeconds s and was killed"
    exit 1
}
$code = $p.ExitCode
if ($code -ne 0) {
    Write-Host ("::error::the smoke launch exited with 0x{0:X8} ({0})" -f $code)
    exit 1
}
$global:LASTEXITCODE = 0
Write-Host 'smoke launch passed'
exit 0
