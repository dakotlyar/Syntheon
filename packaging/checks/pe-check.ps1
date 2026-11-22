# SPDX-License-Identifier: GPL-3.0-or-later
#
# Static checks for a Windows tree (design P27); nothing is executed, so it
# also covers the ARM64 tree that no CI machine can run. Needs `dumpbin` (an
# MSVC developer environment).
#
#   pwsh packaging/checks/pe-check.ps1 -Tree <folder> -Arch x64|arm64
#
# For every .exe/.dll: the machine matches (no stray x64 DLL in the ARM64 tree)
# and every dependency is in the tree or is a Windows system DLL.
param(
    [Parameter(Mandatory = $true)][string]$Tree,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Arch
)
$ErrorActionPreference = 'Stop'
$expected = if ($Arch -eq 'arm64') { 'AA64 machine (ARM64)' } else { '8664 machine (x64)' }
$files = Get-ChildItem -Path $Tree -Recurse -File -Include *.exe, *.dll
if (-not $files) { throw "no .exe/.dll under $Tree" }
$inTree = @{}
$files | ForEach-Object { $inTree[$_.Name.ToLowerInvariant()] = $true }
$system = Join-Path $env:SystemRoot 'System32'
$problems = @()

foreach ($f in $files) {
    $rel = $f.FullName.Substring((Resolve-Path $Tree).Path.Length).TrimStart('\')
    $headers = & dumpbin /nologo /headers $f.FullName 2>&1 | Out-String
    if ($headers -notmatch [regex]::Escape($expected)) {
        $m = [regex]::Match($headers, '([0-9A-F]{4}) machine \(([^)]*)\)')
        $problems += "${rel}: machine is '$($m.Value)', expected '$expected'"
        continue
    }
    $deps = & dumpbin /nologo /dependents $f.FullName 2>&1 | Out-String
    foreach ($line in ($deps -split "`r?`n")) {
        $name = $line.Trim()
        if ($name -notmatch '^[\w.\-]+\.dll$') { continue }
        $lower = $name.ToLowerInvariant()
        if ($inTree.ContainsKey($lower)) { continue }
        if ($lower -like 'api-ms-win-*' -or $lower -like 'ext-ms-win-*') { continue }   # API sets
        # The GenAI shim's openvino* imports are the user's P-VENDOR runtime
        # (DD-1 §5.4): LoadLibrary'd at run time, never bundled — the inverse of
        # the GPU worker's dlopen'd-driver rule in elf-check.py, same mechanism.
        if ($rel -like '*syn_openvino_genai_shim.dll' -and $lower -like 'openvino*.dll') { continue }
        if (Test-Path (Join-Path $system $name)) { continue }                            # a Windows system DLL
        $problems += "${rel}: needs $name, which is neither in the tree nor a system DLL"
    }
}
if ($problems) {
    $problems | Sort-Object -Unique | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    $missing = $problems | ForEach-Object { if ($_ -match 'needs (\S+), which is neither') { $Matches[1] } } | Sort-Object -Unique
    if ($missing) { Write-Host "Unsatisfied DLLs: $($missing -join ', ')" }
    exit 1
}
Write-Host "OK: $($files.Count) files, all $Arch, every dependency accounted for"
$global:LASTEXITCODE = 0
exit 0
