#define MyAppName "EVO-X2 P-MODE Widget"
#define MyAppVersion "0.3.3-beta.4"
#define MyAppPublisher "Andreas Ruhl"
#define MyAppExeName "evox2-pmode-overlay.exe"

[Setup]
AppId={{F740F42F-8832-4672-A1C5-25BF46E8AFB3}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/James22bot/evox2-pmode-widget
AppSupportURL=https://github.com/James22bot/evox2-pmode-widget/issues
AppUpdatesURL=https://github.com/James22bot/evox2-pmode-widget/releases
VersionInfoVersion=0.3.3.4
DefaultDirName={autopf}\EVO-X2 P-MODE Overlay
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
CloseApplicationsFilter=evox2-pmode-overlay.exe
RestartApplications=no
OutputDir=..\dist
OutputBaseFilename=EVO-X2-PMode-Widget-Setup-v{#MyAppVersion}
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
MinVersion=10.0.22000

[Files]
Source: "..\build\windows\evox2-pmode-overlay.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\PawnIO.Modules-0.1.6\LpcACPIEC.bin"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\SECURITY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\PawnIO.Modules-0.1.6\COPYING"; DestDir: "{app}"; DestName: "COPYING-PawnIO-Modules-LGPL-2.1.txt"; Flags: ignoreversion
Source: "..\third_party\PawnIO.Modules-0.1.6\LpcACPIEC.p"; DestDir: "{app}"; Flags: ignoreversion
Source: "Register-Autostart.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "Remove-Autostart.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"

[Run]
; postinstall otherwise defaults to the original non-elevated token, which cannot start this requireAdministrator EXE.
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; WorkingDir: "{app}"; Flags: nowait postinstall runascurrentuser skipifsilent

[Code]
#include "lifecycle-receipt.iss"

function PowerShellPath: String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function Quoted(const Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function LifecycleLogNonceSuffix: String;
var
  Nonce: String;
  I: Integer;
begin
  Nonce := ExpandConstant('{param:LIFECYCLELOGNONCE|}');
  if Length(Nonce) > 64 then
    RaiseException('Lifecycle log nonce is too long.');
  for I := 1 to Length(Nonce) do
    if not (((Nonce[I] >= '0') and (Nonce[I] <= '9')) or
            ((Nonce[I] >= 'A') and (Nonce[I] <= 'Z')) or
            ((Nonce[I] >= 'a') and (Nonce[I] <= 'z')) or
            (Nonce[I] = '-')) then
      RaiseException('Lifecycle log nonce contains an invalid character.');
  if Nonce = '' then
    Result := ''
  else
    Result := '-' + Nonce;
end;

procedure RegisterAutostart;
var
  ResultCode: Integer;
  ScriptPath, AppPath, WorkDir, LogPath, Params: String;
  LogText: AnsiString;
begin
  ScriptPath := ExpandConstant('{app}\Register-Autostart.ps1');
  AppPath := ExpandConstant('{app}\{#MyAppExeName}');
  WorkDir := ExpandConstant('{app}');
  LogPath := ExpandConstant('{tmp}\evox2-autostart-register') + LifecycleLogNonceSuffix + '.log';
  DeleteFile(LogPath);
  Params := '-NoProfile -ExecutionPolicy Bypass -File ' + Quoted(ScriptPath) +
    ' -AppPath ' + Quoted(AppPath) + ' -WorkingDirectory ' + Quoted(WorkDir) +
    ' -LogPath ' + Quoted(LogPath);
  if not Exec(PowerShellPath, Params, WorkDir, SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Autostart registration process could not be started.');
  if ResultCode <> 0 then
  begin
    if not LoadStringFromFile(LogPath, LogText) then
      LogText := '(no diagnostic log was produced)';
    RaiseException(Format('Autostart registration failed (exit code %d).', [ResultCode]) + #13#10 + #13#10 + LogText);
  end;
  if not LoadStringFromFile(LogPath, LogText) then
    RaiseException('Autostart registration produced no receipt.');
  { Keep helper output in Inno's private temp directory; mirror only the success token into the persistent setup log. }
  LogText := ValidateLifecycleReceipt(
    LogText,
    'AUTOSTART_TASK=PASS EVO-X2 P-MODE Widget S-1-');
  Log('EVOX2_LIFECYCLE_RECEIPT ' + Trim(LogText));
end;

procedure RemoveAutostart;
var
  ResultCode: Integer;
  ScriptPath, WorkDir, LogPath, Params: String;
  LogText: AnsiString;
begin
  ScriptPath := ExpandConstant('{app}\Remove-Autostart.ps1');
  if not FileExists(ScriptPath) then
    exit;
  WorkDir := ExpandConstant('{app}');
  LogPath := ExpandConstant('{tmp}\evox2-autostart-remove') + LifecycleLogNonceSuffix + '.log';
  DeleteFile(LogPath);
  Params := '-NoProfile -ExecutionPolicy Bypass -File ' + Quoted(ScriptPath) +
    ' -LogPath ' + Quoted(LogPath);
  if not Exec(PowerShellPath, Params, WorkDir, SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Autostart removal process could not be started.');
  if ResultCode <> 0 then
  begin
    if not LoadStringFromFile(LogPath, LogText) then
      LogText := '(no diagnostic log was produced)';
    RaiseException(Format('Autostart removal failed (exit code %d).', [ResultCode]) + #13#10 + #13#10 + LogText);
  end;
  if not LoadStringFromFile(LogPath, LogText) then
    RaiseException('Autostart removal produced no receipt.');
  LogText := ValidateLifecycleReceipt(
    LogText,
    'AUTOSTART_REMOVAL=PASS EVO-X2 P-MODE Widget S-1-');
  Log('EVOX2_LIFECYCLE_RECEIPT ' + Trim(LogText));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RegisterAutostart;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveAutostart;
end;
