; Inno Setup script for the oshot Windows installer.
;
; Usage (from the repo root, after packaging the portable folder):
;   iscc /DAppVersion=0.5.0-rc2 /DAppNumVersion=0.5.0 /DArch=x64 /DSourceDir=..\package\oshot /DOutputName=oshot-windows-x64-setup scripts\oshot.iss
;
; SourceDir is relative to this script. It must contain oshot.exe, its DLLs and models\eng.traineddata,
; i.e. the same folder that goes into the portable zip.
; Arch is what the binaries in it were built for: x64 (the default) or arm64.
;
; The uninstaller asks whether to delete the user data (%APPDATA%\oshot, %LOCALAPPDATA%\oshot and
; %LOCALAPPDATA%\oshotpm). Silent uninstalls keep it unless /PURGE is passed: unins000.exe /SILENT /PURGE

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
  #define OutputName "oshot-windows-x64-setup"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
; x64compatible also covers ARM64 Windows 11, which runs x64 programs emulated
#if Arch == "arm64"
  #define ArchAllowed "arm64"
#elif Arch == "x64"
  #define ArchAllowed "x64compatible"
#else
  #error Arch must be x64 or arm64
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
SetupIconFile=..\assets\windows\oshot.ico
UninstallDisplayIcon={app}\oshot.exe
OutputDir=..\package
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed={#ArchAllowed}
ArchitecturesInstallIn64BitMode={#ArchAllowed}
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

[UninstallDelete]
; Also remove what the installer didn't put there (leftovers of a running oshot, models added by hand, ...)
Type: filesandordirs; Name: "{app}"; Check: IsOshotDir

[Code]
var
  DeleteUserData: Boolean;

// Never wipe a folder the user pointed the installer at, like C:\Program Files.
// Evaluated at install time, when the entry is written to the uninstall log.
function IsOshotDir: Boolean;
begin
  Result := Pos('oshot', Lowercase(ExtractFileName(ExpandConstant('{app}')))) > 0;
end;

function HasParam(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

// The tray instance usually runs at sign-in; its locked files would otherwise be left behind.
// Only kill processes started from {app}, so a portable oshot elsewhere keeps running.
procedure StopRunningOshot;
var
  AppDir:     String;
  ResultCode: Integer;
begin
  AppDir := ExpandConstant('{app}\');
  StringChangeEx(AppDir, '''', '''''', True);
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
       '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "' +
       'Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and ' +
       '$_.ExecutablePath.StartsWith(''' + AppDir + ''', ''OrdinalIgnoreCase'') } | ' +
       'Invoke-CimMethod -MethodName Terminate | Out-Null"',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  case CurUninstallStep of
    usUninstall:
      begin
        // Silent uninstalls keep the user data unless /PURGE is passed
        if UninstallSilent then
          DeleteUserData := HasParam('/PURGE')
        else
          DeleteUserData := MsgBox('Do you also want to delete your oshot settings, themes, OCR models and plugins?' + #13#10#13#10 +
                                   ExpandConstant('{userappdata}\oshot') + #13#10 +
                                   ExpandConstant('{localappdata}\oshot') + #13#10 +
                                   ExpandConstant('{localappdata}\oshotpm'),
                                   mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
        StopRunningOshot;
      end;
    usPostUninstall:
      if DeleteUserData then
      begin
        DelTree(ExpandConstant('{userappdata}\oshot'), True, True, True);
        DelTree(ExpandConstant('{localappdata}\oshot'), True, True, True);
        DelTree(ExpandConstant('{localappdata}\oshotpm'), True, True, True);
      end;
  end;
end;
