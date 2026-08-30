; Inno Setup script for SynBench.
;
; Builds a single self-extracting Windows installer (with a matching
; uninstaller registered in "Apps & Features") from the PyInstaller output
; in dist\SynBench\.
;
; Build:
;   iscc installer.iss /DMyAppVersion=1.2.3
;
; MyAppVersion defaults to a placeholder if not passed on the command line
; (CI always passes the real version derived from the git tag).

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif

#define MyAppName "SynBench"
#define MyAppPublisher "SynBench"
#define MyAppURL "https://github.com/dakotlyar/SynBench"
#define MyAppExeName "SynBench.exe"

[Setup]
; Keep this GUID stable across releases — it's how Windows/Inno Setup
; recognizes "this is an upgrade of the same app" vs. a separate install.
AppId={{14A62E4F-2EF1-40BC-82D9-2418EC833479}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=installer_output
OutputBaseFilename=SynBench-Setup
SetupIconFile=assets\synbench.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "dist\SynBench\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The optional GPU/NPU plugin installer creates this at runtime — Inno Setup
; only auto-removes files it installed, so clean this up explicitly.
Type: filesandordirs; Name: "{app}\synbench_plugins"
