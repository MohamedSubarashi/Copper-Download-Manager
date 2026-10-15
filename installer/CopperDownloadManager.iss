#define MyAppName "Copper Download Manager"
#define MyAppVersion "0.5.0"
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
; Always offer the install-mode page: "Install for all users" (elevates, lands
; in Program Files) or "Install for me only" (no elevation, per-user). The
; default is per-user; choosing all-users elevates mid-wizard.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
; --- Code signing (0.4.1 preparation) -------------------------------------
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
Source: "release\0.5.0\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Registry]
; The app writes the browser native-messaging host registration into HKCU at
; runtime (Google\Chrome, Microsoft\Edge, Mozilla). Removing those keys on
; uninstall keeps a later reinstall from inheriting a stale host entry.
Root: HKCU; Subkey: "Software\Google\Chrome\NativeMessagingHosts\com.copper.dm"; ValueType: none; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Edge\NativeMessagingHosts\com.copper.dm"; ValueType: none; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Mozilla\NativeMessagingHosts\com.copper.dm"; ValueType: none; Flags: uninsdeletekey

[UninstallDelete]
; The host manifest(s) and the host config live in the per-user local app data.
Type: filesandordirs; Name: "{localappdata}\Copper\Copper Download Manager\NativeMessagingHosts"
Type: files; Name: "{localappdata}\Copper\copper_host_config.json"