# SPDX-License-Identifier: GPL-3.0-or-later
#
# Windows counterpart of no-dev-hooks.sh: no .exe/.dll in the tree may contain
# the text "SYNTHEON_" or the former "SYNBENCH_" (8-bit or UTF-16, which is how QStringLiteral is stored).
#
#   pwsh packaging/checks/no-dev-hooks.ps1 -Tree <folder>
param([Parameter(Mandatory = $true)][string]$Tree)
$ErrorActionPreference = 'Stop'
$ascii = [Text.Encoding]::ASCII.GetBytes('SYNTHEON_')
$utf16 = [Text.Encoding]::Unicode.GetBytes('SYNTHEON_')
$asciiOld = [Text.Encoding]::ASCII.GetBytes('SYNBENCH_')
$utf16Old = [Text.Encoding]::Unicode.GetBytes('SYNBENCH_')

function Test-Contains([byte[]]$bytes, [byte[]]$needle) {
    $limit = $bytes.Length - $needle.Length
    for ($i = 0; $i -le $limit; $i++) {
        if ($bytes[$i] -ne $needle[0]) { continue }
        $j = 1
        while ($j -lt $needle.Length -and $bytes[$i + $j] -eq $needle[$j]) { $j++ }
        if ($j -eq $needle.Length) { return $true }
    }
    return $false
}

$bad = $false
Get-ChildItem -Path $Tree -Recurse -File -Include *.exe, *.dll | ForEach-Object {
    $bytes = [IO.File]::ReadAllBytes($_.FullName)
    if ((Test-Contains $bytes $ascii) -or (Test-Contains $bytes $utf16) -or
        (Test-Contains $bytes $asciiOld) -or (Test-Contains $bytes $utf16Old)) {
        Write-Error "developer hook text found in a packaged binary: $($_.FullName)" -ErrorAction Continue
        $bad = $true
    }
}
if ($bad) { exit 1 }
Write-Host "no developer hook names in: $Tree"
