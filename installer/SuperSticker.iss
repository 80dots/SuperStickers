; Super Sticker 설치 프로그램 (Inno Setup 6)
; 사용자 단위 설치(권한 상승 불필요) — 데이터/자동시작이 모두 HKCU·%APPDATA% 기반

#define MyAppName "Super Sticker"
#define MyAppVersion "1.0.0"
#define MyAppExeName "SuperSticker.exe"

[Setup]
AppId={{8D2E7C54-1B7A-4C36-9E1D-5A9E4B7F2C10}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=SuperSticker
DefaultDirName={autopf}\Super Sticker
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=SuperSticker-Setup-{#MyAppVersion}
SetupIconFile=..\src\icons\app.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
; 실행 중이면 종료 안내 (앱의 단일 인스턴스 뮤텍스)
AppMutex=Local\SuperSticker.Instance
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
english.AutoStart=Start automatically with Windows
korean.AutoStart=Windows 시작 시 자동 실행

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:AutoStart}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\release\SuperSticker.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "redist\MicrosoftEdgeWebView2Setup.exe"; Flags: dontcopy

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "SuperSticker"; \
  ValueType: string; ValueData: """{app}\{#MyAppExeName}"" --hidden"; Tasks: autostart; \
  Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; \
  Flags: nowait postinstall skipifsilent

[Code]
// WebView2 런타임 설치 여부 (머신/사용자 단위 모두 확인)
function IsWebView2Installed(): Boolean;
var
  pv: string;
begin
  Result :=
    (RegQueryStringValue(HKLM,
      'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
      'pv', pv) and (pv <> '') and (pv <> '0.0.0.0')) or
    (RegQueryStringValue(HKCU,
      'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
      'pv', pv) and (pv <> '') and (pv <> '0.0.0.0'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and (not IsWebView2Installed()) then
  begin
    ExtractTemporaryFile('MicrosoftEdgeWebView2Setup.exe');
    Exec(ExpandConstant('{tmp}\MicrosoftEdgeWebView2Setup.exe'), '/silent /install', '',
         SW_SHOW, ewWaitUntilTerminated, ResultCode);
  end;
end;

// 언인스톨 시: 앱 안에서 켠 자동 시작 등록도 제거 (사용자 데이터 %APPDATA%는 보존)
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'SuperSticker');
end;
