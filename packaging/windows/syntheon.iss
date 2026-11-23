; SPDX-License-Identifier: GPL-3.0-or-later
;
; Inno Setup script for Syntheon v2 (one per architecture, D9).
;
;   iscc packaging\windows\syntheon.iss /DAppVersion=0.4.3 /DArch=x64 /DSourceDir=C:\stage /DOutputDir=C:\out
;
;   AppVersion  display version (X.Y.Z, semver)
;   Arch        x64 or arm64
;   SourceDir   the `cmake --install` tree
;   OutputDir   where Syntheon-<ver>-windows-<arch>-setup.exe goes
;
; AppId is the OLD Python SynBench's (D10): installing v2 upgrades that install,
; and the x64 and ARM64 installers share it (N3), so installing one over the
; other replaces it. An install from before the rename sits in Program Files\SynBench,
; which is no longer {app}: [InstallDelete] removes that folder, its Start menu
; folder and its desktop icon, which Inno would not remove by itself.

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

#define AppName "Syntheon"
#define AppPublisher "Syntheon"
#define AppURL "https://github.com/dakotlyar/Syntheon"
#define AppExeName "Syntheon.exe"

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
UsePreviousGroup=no
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
OutputBaseFilename=Syntheon-{#AppVersion}-windows-{#Arch}-setup
SetupIconFile=..\..\assets\syntheon.ico
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
; Everything an install from before the rename left: the old program folder
; (with the Python build's leftovers), its Start menu folder and desktop icon.
; The user's history is never in these places.
Type: filesandordirs; Name: "{autopf}\SynBench"
Type: filesandordirs; Name: "{autoprograms}\SynBench"
Type: files; Name: "{autodesktop}\SynBench.lnk"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Tells the MSI which installer owns this machine (see Package.wxs).
Root: HKLM; Subkey: "Software\SynBench"; ValueType: string; ValueName: "Installer"; ValueData: "inno"; Flags: uninsdeletevalue uninsdeletekeyifempty
; ...and which architecture (x64 / arm64), so the other one's MSI or setup can refuse.
Root: HKLM; Subkey: "Software\SynBench"; ValueType: string; ValueName: "Arch"; ValueData: "{#Arch}"; Flags: uninsdeletevalue uninsdeletekeyifempty

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

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
    Log('Refusing /DIR=' + Requested + ': Syntheon installs only into ' + Allowed);
    if not WizardSilent then
      SuppressibleMsgBox('Syntheon installs only into ' + Allowed + '.', mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;

#if Arch == "x64"
  // N3: ARM64 over x64 is allowed (it replaces it); x64 over an ARM64 install is not.
  // An x64 setup must not silently replace the native ARM64 build.
  if RegQueryStringValue(HKLM, 'Software\SynBench', 'Arch', Owner) and (Owner = 'arm64') then
  begin
    Log('The ARM64 version is installed; refusing the x64 setup.');
    if not WizardSilent then
      SuppressibleMsgBox('The ARM64 version of Syntheon is installed. Use the ARM64 setup (or uninstall it first).', mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
#endif
  if RegQueryStringValue(HKLM, 'Software\SynBench', 'Installer', Owner) and (Owner = 'msi') then
  begin
    // Never block a silent run on a message box: SuppressibleMsgBox answers
    // with the default (OK) under /SUPPRESSMSGBOXES, and in a silent setup the
    // box is skipped altogether. Returning False ends Setup with a non-zero exit code.
    Log('Syntheon is installed with the MSI package; refusing to install over it.');
    if not WizardSilent then
      SuppressibleMsgBox('Syntheon was installed with the MSI package. Uninstall it first (Settings > Apps), then run this installer.',
                         mbError, MB_OK, IDOK);
    Result := False;
  end;
end;
