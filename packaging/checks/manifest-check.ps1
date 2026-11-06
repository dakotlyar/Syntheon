# SPDX-License-Identifier: GPL-3.0-or-later
#
# The built Syntheon.exe must carry exactly one manifest, and it must be ours:
# requireAdministrator (P12/D1), PerMonitorV2 and long-path awareness, and no
# asInvoker left over from the linker's default. Prints the embedded manifest.
#
# Reads the exe's RT_MANIFEST resources directly (LoadLibraryEx as a data file +
# EnumResourceNames/FindResource), so it needs no MSVC tools and works on an
# ARM64 exe from an x64 machine. (mt.exe -inputresource:...;#1 failed on a
# manifest that was there: it looks for the neutral language only.)
#
#   pwsh packaging/checks/manifest-check.ps1 -Exe <Syntheon.exe>
param([Parameter(Mandatory = $true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class ManifestReader
{
    const uint LOAD_LIBRARY_AS_DATAFILE = 0x2;
    static readonly IntPtr RT_MANIFEST = (IntPtr)24;

    delegate bool EnumNameProc(IntPtr module, IntPtr type, IntPtr name, IntPtr param);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryEx(string file, IntPtr zero, uint flags);
    [DllImport("kernel32", SetLastError = true)]
    static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool EnumResourceNames(IntPtr module, IntPtr type, EnumNameProc proc, IntPtr param);
    [DllImport("kernel32", SetLastError = true)]
    static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32", SetLastError = true)]
    static extern IntPtr LoadResource(IntPtr module, IntPtr res);
    [DllImport("kernel32")]
    static extern IntPtr LockResource(IntPtr data);
    [DllImport("kernel32")]
    static extern uint SizeofResource(IntPtr module, IntPtr res);

    // The ids of the RT_MANIFEST resources, and their text.
    public static Dictionary<long, string> Read(string file)
    {
        IntPtr module = LoadLibraryEx(file, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE);
        if (module == IntPtr.Zero)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "LoadLibraryEx " + file);
        try
        {
            var ids = new List<long>();
            EnumNameProc proc = (m, t, n, p) => { ids.Add(n.ToInt64()); return true; };
            EnumResourceNames(module, RT_MANIFEST, proc, IntPtr.Zero);   // fails if there is no RT_MANIFEST at all
            var result = new Dictionary<long, string>();
            foreach (long id in ids)
            {
                IntPtr res = FindResource(module, (IntPtr)id, RT_MANIFEST);
                if (res == IntPtr.Zero) { result[id] = null; continue; }
                IntPtr data = LockResource(LoadResource(module, res));
                byte[] bytes = new byte[SizeofResource(module, res)];
                Marshal.Copy(data, bytes, 0, bytes.Length);
                result[id] = Encoding.UTF8.GetString(bytes);
            }
            return result;
        }
        finally { FreeLibrary(module); }
    }
}
'@

$found = [ManifestReader]::Read($Exe)
Write-Host "RT_MANIFEST resource ids in ${Exe}: $(if ($found.Count) { ($found.Keys | Sort-Object) -join ', ' } else { '(none)' })"
if ($found.Count -eq 0) { throw "$Exe has no manifest resource at all: the linker did not embed Syntheon.manifest" }
if ($found.Count -gt 1) { throw "$Exe has $($found.Count) manifest resources; there must be exactly one" }
$text = $found.Values | Select-Object -First 1
Write-Host $text
if ($text -notmatch 'requireAdministrator') { throw 'the embedded manifest does not request requireAdministrator' }
if ($text -match 'asInvoker') { throw 'the embedded manifest still has an asInvoker execution level' }
if ($text -notmatch 'PerMonitorV2') { throw 'the embedded manifest lacks PerMonitorV2' }
if ($text -notmatch 'longPathAware') { throw 'the embedded manifest lacks longPathAware' }
Write-Host "OK: one manifest in $Exe, requireAdministrator"
