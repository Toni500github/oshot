; Inno Setup script for the oshot Windows installer.
;
; Usage (from the repo root, after packaging the portable folder):
;   iscc /DAppVersion=0.5.0-rc2 /DAppNumVersion=0.5.0 /DSourceDir=..\package\oshot /DOutputName=oshot-windows-setup scripts\oshot.iss
;
; SourceDir is relative to this script. It must contain oshot.exe, its DLLs and models\eng.traineddata,
; i.e. the same folder that goes into the portable zip.

#ifndef AppVersion
  #error AppVersion must be defined (/DAppVersion=x.y.z)
#endif
; Windows version resources must be purely numeric (no "-rc2")
#ifndef AppNumVersion
  #define AppNumVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\package\oshot"
#endif
#ifndef OutputName
  #define OutputName "oshot-windows-setup"
#endif

[Setup]
; Never change AppId: it's how upgrades and uninstalls find the existing install
AppId={{BDB64392-C6C0-4B0D-AE48-D1E49C993D0B}
AppName=oshot
AppVersion={#AppVersion}
AppVerName=oshot {#AppVersion}
VersionInfoVersion={#AppNumVersion}
AppPublisher=Toni500
AppPublisherURL=https://github.com/Toni500github/oshot
AppSupportURL=https://github.com/Toni500github/oshot/issues
AppUpdatesURL=https://github.com/Toni500github/oshot/releases
DefaultDirName={autopf}\oshot
DefaultGroupName=oshot
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
SetupIconFile=..\oshot.ico
UninstallDisplayIcon={app}\oshot.exe
OutputDir=..\package
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Install per-user by default (no admin prompt); the user can still pick "all users"
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Close a running oshot (usually the tray instance) before replacing its files
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start oshot in the system tray when I sign in"; GroupDescription: "Startup:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\oshot"; Filename: "{app}\oshot.exe"
Name: "{group}\oshot (tray)"; Filename: "{app}\oshot.exe"; Parameters: "--tray"
Name: "{group}\Uninstall oshot"; Filename: "{uninstallexe}"
Name: "{autodesktop}\oshot"; Filename: "{app}\oshot.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "oshot"; \
    ValueData: """{app}\oshot.exe"" --tray"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\oshot.exe"; Parameters: "--tray"; Description: "Start oshot in the system tray"; \
    Flags: nowait postinstall skipifsilent
