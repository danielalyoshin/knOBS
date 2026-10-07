; SPDX-License-Identifier: GPL-2.0-or-later
;
; The installer, built with Inno Setup 6 by packaging\package.ps1, which
; passes the defines below (and Sign, with a signtool command, to sign the
; uninstaller and the installer). It installs for the current user only, with no
; admin rights, into %LocalAppData%\Programs\knobs: knobs's Run entry and its
; data are per user too. Uninstalling closes knobs, removes its Run entry
; and %LocalAppData%\knobs (the copy of OBS's files and the logs), and asks
; before removing %AppData%\knobs (the settings). A portable copy's Run entry
; is left alone.

#ifndef AppName
  #error Run packaging\package.ps1, which passes AppName, AppVersion, StageDir and OutputDir.
#endif

[Setup]
AppId={{1FE50EC5-3715-4EE5-BD51-381BF3374BA2}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Daniel Alyoshin
AppPublisherURL=https://github.com/danielalyoshin/knobs
AppSupportURL=https://github.com/danielalyoshin/knobs/issues
AppUpdatesURL=https://github.com/danielalyoshin/knobs/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup
SetupIconFile=..\assets\knobs.ico
UninstallDisplayIcon={app}\{#AppName}.exe
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Setup closes a running knobs itself (PrepareToInstall).
CloseApplications=no
#ifdef Sign
; package.ps1 -Sign defines the signtool command.
SignTool=signtool
SignedUninstaller=yes
#endif

[Files]
Source: "{#StageDir}\{#AppName}.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppName}.exe"

[Run]
Filename: "{app}\{#AppName}.exe"; Description: "Start {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The copy of OBS's files, the logs, and the dev tools' copies.
Type: filesandordirs; Name: "{localappdata}\{#AppName}"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  ApprovedKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run';
  // The tray's window, which quits on WM_CLOSE (src/main.cpp).
  TrayWindowClass = '{#AppName}.tray';
  WM_CLOSE = $0010;

// Asks a running knobs to quit, and waits up to 15 s for it. False if it's
// still running.
function CloseRunningApp(): Boolean;
var
  Window: HWND;
  Waited: Integer;
begin
  Window := FindWindowByClassName(TrayWindowClass);
  if Window <> 0 then
    PostMessage(Window, WM_CLOSE, 0, 0);
  Waited := 0;
  while (FindWindowByClassName(TrayWindowClass) <> 0) and (Waited < 15000) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Result := FindWindowByClassName(TrayWindowClass) = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not CloseRunningApp() then
    Result := '{#AppName} is still running. Quit it from its tray menu, then run Setup again.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := CloseRunningApp();
  if not Result then
    MsgBox('{#AppName} is still running. Quit it from its tray menu, then uninstall again.', mbError, MB_OK);
end;

// Removes the Run entry if it starts this install's knobs.exe, not a
// portable copy's.
procedure RemoveRunEntry();
var
  Command: String;
begin
  if RegQueryStringValue(HKCU, RunKey, '{#AppName}', Command) and
     (Pos(Lowercase(ExpandConstant('{app}\{#AppName}.exe')), Lowercase(Command)) > 0) then
  begin
    RegDeleteValue(HKCU, RunKey, '{#AppName}');
    RegDeleteValue(HKCU, ApprovedKey, '{#AppName}');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Settings: String;
begin
  if CurUninstallStep = usUninstall then
    RemoveRunEntry();
  if CurUninstallStep = usPostUninstall then
  begin
    Settings := ExpandConstant('{userappdata}\{#AppName}');
    if DirExists(Settings) and not UninstallSilent() and
       (MsgBox('Also delete your {#AppName} settings?' + #13#10#13#10 +
               'They''re your choice of mic and cable, and whether {#AppName} pauses while OBS is open. ' +
               'Keep them if you might install {#AppName} again.',
               mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(Settings, True, True, True);
  end;
end;
