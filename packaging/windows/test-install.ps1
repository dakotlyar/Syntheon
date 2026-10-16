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
$app = Join-Path $env:ProgramFiles 'SynBench'
$exe = Join-Path $app 'SynBench.exe'
$logs = Join-Path $env:RUNNER_TEMP 'install-logs'
if (-not $env:RUNNER_TEMP) { $logs = Join-Path $env:TEMP 'synbench-install-logs' }
New-Item -ItemType Directory -Force $logs | Out-Null

function Step($text) { Write-Host "::group::$text" }
function EndStep { Write-Host '::endgroup::' }

function Run([string]$file, [string[]]$arguments) {
    $p = Start-Process -FilePath $file -ArgumentList $arguments -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$file $arguments exited with $($p.ExitCode)" }
}

# Starts the installed app and checks it stays up (it has no smoke switch in a
# packaged build), then closes it.
function Test-Launch {
    $p = Start-Process -FilePath $exe -PassThru
    Start-Sleep -Seconds 10
    if ($p.HasExited) { throw "SynBench exited with code $($p.ExitCode) right after launch" }
    $p | Stop-Process -Force
    $p.WaitForExit()
}

function Get-InnoUninstaller { Join-Path $app 'unins000.exe' }

function Assert-Version {
    $v = (Get-Item $exe).VersionInfo.ProductVersion
    if ($v -ne $Version) { throw "installed ProductVersion is $v, expected $Version" }
}

function Assert-Gone {
    if (Test-Path $exe) { throw "$exe is still there after uninstall" }
}

# 1. Inno: install, launch, uninstall
Step 'Inno: install / launch / uninstall'
Run $Inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\inno-install.log")
if (-not (Test-Path $exe)) { throw 'SynBench.exe missing after the Inno install' }
Assert-Version
Test-Launch
Run (Get-InnoUninstaller) @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
Start-Sleep -Seconds 3
Assert-Gone
EndStep

# 2. MSI: install, launch, uninstall
Step 'MSI: install / launch / uninstall'
Run 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-install.log")
if (-not (Test-Path $exe)) { throw 'SynBench.exe missing after the MSI install' }
Test-Launch
Run 'msiexec.exe' @('/x', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-uninstall.log")
Assert-Gone
EndStep

# 3. Inno and MSI refuse each other
Step 'Inno and MSI do not coexist'
Run 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart')
$p = Start-Process -FilePath $Inno -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -Wait -PassThru
if ($p.ExitCode -eq 0) { throw 'the Inno installer installed over an MSI install' }
Run 'msiexec.exe' @('/x', $Msi, '/qn', '/norestart')
Run 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart')
Run 'msiexec.exe' @('/x', $Msi, '/qn', '/norestart')
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
    $entries = Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -like '{14A62E4F-2EF1-40BC-82D9-2418EC833479}*' }
    if (@($entries).Count -ne 1) { throw "expected one uninstall entry for the shared AppId, found $(@($entries).Count)" }
    Run (Get-InnoUninstaller) @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    Start-Sleep -Seconds 3
    Assert-Gone
    EndStep
}
Write-Host 'install tests passed'
