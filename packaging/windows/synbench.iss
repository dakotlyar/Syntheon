; SPDX-License-Identifier: GPL-3.0-or-later
;
; Inno Setup script for SynBench v2 (one per architecture, D9).
;
;   iscc packaging\windows\synbench.iss /DAppVersion=2026.11.0 /DArch=x64 /DSourceDir=C:\stage /DOutputDir=C:\out
;
;   AppVersion  display version (2026.11.N)
;   Arch        x64 or arm64
;   SourceDir   the `cmake --install` tree
;   OutputDir   where SynBench-<ver>-windows-<arch>-setup.exe goes
;
; AppId is the OLD Python SynBench's (D10): installing v2 upgrades that install
; in place, and the x64 and ARM64 installers share it (N3), so installing one
; over the other replaces it. [InstallDelete] removes what the PyInstaller
; install left behind, which Inno would not remove by itself.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\out"
#endif

#define AppName "SynBench"
#define AppPublisher "SynBench"
#define AppURL "https://github.com/dakotlyar/SynBench"
#define AppExeName "SynBench.exe"

[Setup]
; Keep this GUID stable across releases and across both architectures.
AppId={{14A62E4F-2EF1-40BC-82D9-2418EC833479}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
; Locked to Program Files (Dave): the app runs elevated, so it must live where a
; normal user can't write. No folder page, no remembered custom folder, and a
; /DIR= on the command line is refused in [Code].
DisableDirPage=yes
UsePreviousAppDir=no
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
; An x64 build also installs on Windows ARM64, where it runs emulated (D9: the
; app then shows a banner and records "emulated").
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
OutputDir={#OutputDir}
OutputBaseFilename=SynBench-{#AppVersion}-windows-{#Arch}-setup
SetupIconFile=..\..\assets\synbench.ico
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0.17763
LicenseFile={#SourceDir}\LICENSE.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[InstallDelete]
; The Python SynBench (PyInstaller one-folder + its optional side-car) left
; these in {app}. Removed before the new files are copied.
Type: filesandordirs; Name: "{app}\_internal"
Type: filesandordirs; Name: "{app}\python-embed"
Type: filesandordirs; Name: "{app}\synbench_plugins"
Type: files; Name: "{app}\SynBench-CLI.exe"
Type: files; Name: "{app}\base_library.zip"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Tells the MSI which installer owns this machine (see Package.wxs).
Root: HKLM; Subkey: "Software\SynBench"; ValueType: string; ValueName: "Installer"; ValueData: "inno"; Flags: uninsdeletevalue uninsdeletekeyifempty

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\synbench_plugins"

[Code]
// Inno and the MSI must not both own an install: refuse if the MSI is there.
function InitializeSetup(): Boolean;
var
  Owner, Requested, Allowed: String;
begin
  Result := True;
  // /DIR=<folder> would put an elevated program somewhere writable. Only the
  // Program Files default is accepted (same folder, case-insensitive).
  Requested := RemoveBackslash(ExpandConstant('{param:DIR|}'));
  Allowed := RemoveBackslash(ExpandConstant('{autopf}\{#AppName}'));
  if (Requested <> '') and (CompareText(Requested, Allowed) <> 0) then
  begin
    Log('Refusing /DIR=' + Requested + ': SynBench installs only into ' + Allowed);
    if not WizardSilent then
      SuppressibleMsgBox('SynBench installs only into ' + Allowed + '.', mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;

  if RegQueryStringValue(HKLM, 'Software\SynBench', 'Installer', Owner) and (Owner = 'msi') then
  begin
    // Never block a silent run on a message box: SuppressibleMsgBox answers
    // with the default (OK) under /SUPPRESSMSGBOXES, and in a silent setup the
    // box is skipped altogether. Returning False ends Setup with a non-zero exit code.
    Log('SynBench is installed with the MSI package; refusing to install over it.');
    if not WizardSilent then
      SuppressibleMsgBox('SynBench was installed with the MSI package. Uninstall it first (Settings > Apps), then run this installer.',
                         mbError, MB_OK, IDOK);
    Result := False;
  end;
end;
