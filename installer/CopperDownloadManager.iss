[Setup]
AppName=Copper Download Manager
AppVersion=0.1.5
AppPublisher=Mohamed Subarashi
AppPublisherURL=https://github.com/MohamedSubarashi
AppSupportURL=https://github.com/MohamedSubarashi/Copper-Download-Manager/issues
AppUpdatesURL=https://github.com/MohamedSubarashi/Copper-Download-Manager/releases
DefaultDirName={autopf}\Copper Download Manager
DefaultGroupName=Copper Download Manager
OutputDir=..\installer
OutputBaseFilename=CopperDownloadManager-0.1.5-Setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
SetupIconFile=..\Assets\app.ico
UninstallDisplayIcon={app}\CopperDownloadManager.exe
WizardStyle=modern
VersionInfoVersion=0.1.5.0
VersionInfoCompany=Mohamed Subarashi
VersionInfoDescription=Copper Download Manager Setup
VersionInfoProductName=Copper Download Manager
VersionInfoProductVersion=0.1.5

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associateurls"; Description: "Associate copper:// protocol with Copper Download Manager"; GroupDescription: "File Associations:"; Flags: checkedonce

[Files]
Source: "..\releases\0.1.5\CopperDownloadManager.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\releases\0.1.5\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\releases\0.1.5\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\releases\0.1.5\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion
Source: "..\releases\0.1.5\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion
Source: "..\releases\0.1.5\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion
Source: "..\releases\0.1.5\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion
Source: "..\releases\0.1.5\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion
Source: "..\releases\0.1.5\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion
Source: "..\releases\0.1.5\sqldrivers\*"; DestDir: "{app}\sqldrivers"; Flags: ignoreversion
Source: "..\releases\0.1.5\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion
Source: "..\extensions\Copper Download Manager Chrome\Copper Download Manager Chrome.zip"; DestDir: "{app}\extensions"; Flags: ignoreversion
Source: "..\extensions\Copper Download Manager Firefox\Copper Download Manager Firefox.zip"; DestDir: "{app}\extensions"; Flags: ignoreversion

[Icons]
Name: "{group}\Copper Download Manager"; Filename: "{app}\CopperDownloadManager.exe"
Name: "{group}\Uninstall Copper Download Manager"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Copper Download Manager"; Filename: "{app}\CopperDownloadManager.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\copper\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\CopperDownloadManager.exe"" ""%1"""; Tasks: associateurls; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\copper"; ValueType: string; ValueName: ""; ValueData: "URL:Copper Protocol"; Tasks: associateurls; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\copper"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: associateurls
Root: HKA; Subkey: "Software\Classes\copper\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\CopperDownloadManager.exe,0"; Tasks: associateurls; Flags: uninsdeletekey

[Run]
Filename: "{app}\CopperDownloadManager.exe"; Description: "Launch Copper Download Manager"; Flags: nowait postinstall skipifsilent
