# SPDX-License-Identifier: GPL-3.0-or-later
#
# Silent install -> launch -> uninstall for the Inno installer and the MSI
# (design 8, CI windows-x64), plus an upgrade over the REAL Python v0.2.3
# Inno install (D10). Needs an elevated shell.
#
#   pwsh packaging/windows/test-install.ps1 -Inno <setup.exe> -Msi <file.msi> -OldInstaller <SynBench-Setup.exe> -Version 2026.11.0
param(
    [Parameter(Mandatory = $true)][string]$Inno,
    [Parameter(Mandatory = $true)][string]$Msi,
    [string]$OldInstaller,
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'

# The app asks for administrator rights; from a non-elevated shell Start-Process
# would raise a UAC prompt (and, unattended, wait for ever). The CI runner's
# account is an elevated administrator, so no prompt appears; anywhere else,
# stop with a clear message instead of hanging.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this from an elevated (administrator) shell: the installers and SynBench.exe need it, and a UAC prompt would hang an unattended run'
}

# Absolute paths: installers and msiexec resolve relative ones against their own idea of the folder.
$Inno = (Resolve-Path $Inno).Path
$Msi = (Resolve-Path $Msi).Path
if ($OldInstaller) { $OldInstaller = (Resolve-Path $OldInstaller).Path }

$app = Join-Path $env:ProgramFiles 'SynBench'
$exe = Join-Path $app 'SynBench.exe'
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$logs = Join-Path $tempRoot 'install-logs'
New-Item -ItemType Directory -Force $logs | Out-Null

function Step($text) { Write-Host "::group::$text" }
function EndStep { Write-Host '::endgroup::' }

function Run([string]$file, [string[]]$arguments) {
    $p = Start-Process -FilePath $file -ArgumentList $arguments -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$file $arguments exited with $($p.ExitCode)" }
}

# Starts the installed app and checks it keeps running (a packaged build has no
# smoke switch), reports its window, then closes it.
function Test-Launch {
    $p = Start-Process -FilePath $exe -PassThru
    Start-Sleep -Seconds 10
    if ($p.HasExited) { throw "SynBench exited with code $($p.ExitCode) right after launch" }
    $p.Refresh()
    Write-Host "SynBench is running: pid $($p.Id), window handle $($p.MainWindowHandle), responding $($p.Responding)"
    if (-not $p.Responding) { throw 'SynBench is not responding 10 s after launch' }
    $p | Stop-Process -Force
    $p.WaitForExit()
}

# The Inno uninstaller hands over to a copy of itself in %TEMP% and returns at
# once, so "uninstalled" is something to wait for, not to assume.
function Wait-Gone([int]$seconds = 90) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Test-Path $exe) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    if (Test-Path $exe) { throw "$exe is still there $seconds s after the uninstall" }
}

function Get-InnoUninstaller {
    $u = Get-ChildItem $app -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $u) { throw "no Inno uninstaller (unins*.exe) in $app" }
    $u.FullName
}

function Assert-Version {
    $v = (Get-Item $exe).VersionInfo.ProductVersion
    if ($v -ne $Version) { throw "installed ProductVersion is $v, expected $Version" }
}

# 1. Inno: install, launch, uninstall
Step 'Inno: install / launch / uninstall'
Run $Inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\inno-install.log")
if (-not (Test-Path $exe)) { throw 'SynBench.exe missing after the Inno install' }
Assert-Version
Test-Launch
Run (Get-InnoUninstaller) @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
Wait-Gone
EndStep

# 2. MSI: install, launch, uninstall (msiexec returns when it is done)
Step 'MSI: install / launch / uninstall'
Run 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-install.log")
if (-not (Test-Path $exe)) { throw 'SynBench.exe missing after the MSI install' }
Test-Launch
Run 'msiexec.exe' @('/x', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-uninstall.log")
Wait-Gone
EndStep

# 3. Inno and MSI refuse each other
Step 'Inno and MSI do not coexist'
Run 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-install-2.log")
$p = Start-Process -FilePath $Inno -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\inno-refused.log") -Wait -PassThru
if ($p.ExitCode -eq 0) { throw 'the Inno installer installed over an MSI install' }
Write-Host "Inno refused with exit code $($p.ExitCode), as it should"
Run 'msiexec.exe' @('/x', $Msi, '/qn', '/norestart')
Wait-Gone
EndStep

# 4. Upgrade over the real Python v0.2.3 install (D10)
if ($OldInstaller) {
    Step 'Upgrade over the Python v0.2.3 install'
    Run $OldInstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\old-install.log")
    if (-not (Test-Path (Join-Path $app '_internal'))) { throw 'the old install has no _internal: not the Python build?' }
    New-Item -ItemType Directory -Force (Join-Path $app 'python-embed') | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $app 'synbench_plugins') | Out-Null
    Run $Inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\inno-upgrade.log")
    foreach ($leftover in '_internal', 'python-embed', 'synbench_plugins', 'SynBench-CLI.exe') {
        if (Test-Path (Join-Path $app $leftover)) { throw "Python leftover $leftover survived the upgrade" }
    }
    Assert-Version
    Test-Launch
    # One entry in Apps & Features, not two (same AppId).
    $roots = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    $entries = foreach ($root in $roots) {
        Get-ChildItem $root -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '{14A62E4F-2EF1-40BC-82D9-2418EC833479}*' }
    }
    if (@($entries).Count -ne 1) { throw "expected one uninstall entry for the shared AppId, found $(@($entries).Count)" }
    Run (Get-InnoUninstaller) @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    Wait-Gone
    EndStep
}
Write-Host 'install tests passed'
