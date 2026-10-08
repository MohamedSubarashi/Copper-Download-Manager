#define MyAppName "Copper Download Manager"
#define MyAppVersion "0.4.0"
#define MyAppPublisher "Mohamed Subarashi"
#define MyAppExeName "CopperDownloadManager.exe"

[Setup]
AppId={{8A2C1E5F-9B76-4D4E-B2D4-6C5A9E7B3F12}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=.
OutputBaseFilename=CopperDownloadManager-{#MyAppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
; --- Code signing (0.4.0 preparation) -------------------------------------
; No certificate was available for this release, so nothing is signed.
; When one exists, register a sign tool on the ISCC command line, e.g.
;   ISCC /S"CopperSign=sign tool /f cert.pfx /p <pw> $f" CopperDownloadManager.iss
; and uncomment the next two lines (see RELEASE.md section 6):
; SignTool=CopperSign
; SignedUninstaller=yes
; ---------------------------------------------------------------------------
SetupIconFile=..\Assets\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "release\0.4.0\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent