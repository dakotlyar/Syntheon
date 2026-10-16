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

# Every process is waited for with a timeout: a hung installer (a message box
# nobody can click, a lock) must fail the test with its log, never hang the job.
$script:timeoutSeconds = 120

function Show-Logs {
    Write-Host '::group::installer logs (last 60 lines of each)'
    Get-ChildItem $logs -File -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "----- $($_.Name)"
        Get-Content $_.FullName -Tail 60 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    }
    Write-Host '::endgroup::'
}

# Starts a process and waits at most $timeoutSeconds; returns the exit code.
# On a timeout the whole process tree is killed and the test fails.
function Start-Waited([string]$file, [string[]]$arguments) {
    Write-Host "> $file $($arguments -join ' ')"
    $p = Start-Process -FilePath $file -ArgumentList $arguments -PassThru
    $null = $p.Handle   # keeps the process handle, or ExitCode can come back empty
    if (-not $p.WaitForExit($script:timeoutSeconds * 1000)) {
        & taskkill.exe /PID $p.Id /T /F | Out-Null
        throw "timed out after $($script:timeoutSeconds) s: $file $($arguments -join ' ')"
    }
    return $p.ExitCode
}

function Run([string]$file, [string[]]$arguments) {
    $code = Start-Waited $file $arguments
    if ($code -ne 0) { throw "$file $arguments exited with $code" }
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

try {
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
    $code = Start-Waited $Inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\inno-refused.log")
    if ($code -eq 0) { throw 'the Inno installer installed over an MSI install' }
    Write-Host "Inno refused with exit code $code, as it should"
    Run 'msiexec.exe' @('/x', $Msi, '/qn', '/norestart')
    Wait-Gone
    EndStep

    # 3a. One architecture per machine. The x64 runner can't install ARM64 binaries,
    # so the other architecture is SIMULATED with its registry marker: that is
    # exactly what each installer's refusal reads. (Untestable here: a real ARM64
    # install, and ARM64 setup replacing an x64 install on a Snapdragon, N3.)
    Step 'The other architecture blocks the installers'
    $key = 'HKLM:\SOFTWARE\SynBench'
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name 'Arch' -Value 'arm64'
    $code = Start-Waited $Inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$logs\inno-otherarch.log")
    if ($code -eq 0) { throw 'the x64 setup went over an ARM64 install' }
    $code = Start-Waited 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-otherarch.log")
    if ($code -eq 0) { throw 'the x64 MSI went over an ARM64 install' }
    if (Test-Path $exe) { throw 'a refused install still put SynBench.exe in place' }
    Remove-Item -Path $key -Recurse -Force
    EndStep

    # 3c. The MSI refuses over the old Python SynBench (Dave); the setup program is the upgrade path.
    # Simulated with the old install's uninstall key, which is all the MSI reads;
    # the real v0.2.3 upgrade by the setup program is step 4.
    Step 'The MSI refuses over the old Python SynBench'
    $oldKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{14A62E4F-2EF1-40BC-82D9-2418EC833479}_is1'
    New-Item -Path $oldKey -Force | Out-Null
    Set-ItemProperty -Path $oldKey -Name 'UninstallString' -Value '"C:\Program Files\SynBench\unins000.exe"'
    $code = Start-Waited 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart', '/l*v', "$logs\msi-oldpython.log")
    Remove-Item -Path $oldKey -Recurse -Force
    if ($code -eq 0) { throw 'the MSI installed over the old Python SynBench' }
    if (Test-Path $exe) { throw 'a refused MSI still put SynBench.exe in place' }
    EndStep

    # 3b. The install folder is locked to Program Files (Dave): a custom folder is refused
    Step 'A custom install folder is refused'
    $custom = Join-Path $tempRoot 'custom-install-dir'
    $code = Start-Waited $Inno @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$custom", "/LOG=$logs\inno-dir.log")
    if ($code -eq 0) { throw '/DIR= was accepted by the Inno installer' }
    if (Test-Path (Join-Path $custom 'SynBench.exe')) { throw "the Inno installer installed into $custom" }
    if (Test-Path $exe) { throw 'a refused /DIR= install still installed into Program Files' }
    # MSI: INSTALLFOLDER is not a public property any more; the install goes to Program Files regardless.
    Run 'msiexec.exe' @('/i', $Msi, '/qn', '/norestart', "INSTALLFOLDER=$custom", "InstallFolder=$custom", '/l*v', "$logs\msi-dir.log")
    if (Test-Path (Join-Path $custom 'SynBench.exe')) { throw "the MSI installed into $custom" }
    if (-not (Test-Path $exe)) { throw 'the MSI did not install into Program Files' }
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
} catch {
    Write-Host "::error::$($_.Exception.Message)"
    Show-Logs
    throw
}
