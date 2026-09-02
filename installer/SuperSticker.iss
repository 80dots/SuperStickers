; Super Stickers 설치 프로그램 (Inno Setup 6)
; 사용자 단위 설치(권한 상승 불필요) — 데이터/자동시작이 모두 HKCU·%APPDATA% 기반

#define MyAppName "Super Stickers"
; 버전은 scripts/build.ps1이 CMakeLists.txt에서 읽어 /DMyAppVersion으로 넘긴다.
; ISCC를 직접 실행하면 아래 기본값이 쓰인다(파일명이 실제 버전과 달라지므로 주의).
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppExeName "SuperSticker.exe"

[Setup]
AppId={{8D2E7C54-1B7A-4C36-9E1D-5A9E4B7F2C10}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Minkyu Park
DefaultDirName={autopf}\Super Stickers
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=SuperStickers-Setup-{#MyAppVersion}
SetupIconFile=..\src\icons\app.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
; 실행 중인 앱은 AppMutex(직접 닫으라는 안내)가 아니라 아래 [Code]에서 물어보고 대신 닫는다.
; 뮤텍스 이름은 main.cpp의 단일 인스턴스 뮤텍스와 같아야 한다.
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
english.AutoStart=Start automatically with Windows
korean.AutoStart=Windows 시작 시 자동 실행
english.DeleteData=Do you also want to delete all memos, groups and settings?%n%nThis cannot be undone. (Choose No to keep your data for a future install.)
korean.DeleteData=메모·그룹·설정 데이터도 함께 삭제할까요?%n%n이 작업은 되돌릴 수 없습니다. (아니요를 선택하면 데이터가 보존되어 다시 설치할 때 그대로 복원됩니다.)
english.AppRunningInstall=Super Stickers is currently running.%n%nSetup will close it and then continue. Your memos are saved before it closes.%n%nContinue?
korean.AppRunningInstall=Super Stickers가 실행 중입니다.%n%n종료한 뒤 설치를 계속합니다. 종료 전에 메모는 저장됩니다.%n%n계속할까요?
english.AppRunningUninstall=Super Stickers is currently running.%n%nUninstall will close it and then continue. Your memos are saved before it closes.%n%nContinue?
korean.AppRunningUninstall=Super Stickers가 실행 중입니다.%n%n종료한 뒤 제거를 계속합니다. 종료 전에 메모는 저장됩니다.%n%n계속할까요?
english.AppCloseFailed=Super Stickers could not be closed. Please exit it from the tray icon and run this again.
korean.AppCloseFailed=Super Stickers를 종료하지 못했습니다. 트레이 아이콘에서 직접 종료한 뒤 다시 실행해 주세요.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:AutoStart}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\release\SuperSticker.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
; MIT 라이선스는 배포본에 저작권·허가 고지를 포함할 것을 요구한다 (앱의 정보 탭에도 전문이 있다)
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
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
const
  AppMutexName = 'Local\SuperSticker.Instance';  // main.cpp의 단일 인스턴스 뮤텍스
  AppWndClass = 'SuperStickerApp';               // App.cpp가 등록하는 숨김 창 클래스
  WM_CLOSE_MSG = $0010;

function AppIsRunning(): Boolean;
begin
  Result := CheckForMutexes(AppMutexName);
end;

// 뮤텍스가 사라질 때까지 기다린다 (앱이 완전히 끝나면 풀린다). 최대 TimeoutMs.
function WaitForExit(TimeoutMs: Integer): Boolean;
var
  Waited: Integer;
begin
  Waited := 0;
  while (Waited < TimeoutMs) and AppIsRunning() do
  begin
    Sleep(200);
    Waited := Waited + 200;
  end;
  Result := not AppIsRunning();
end;

// 실행 중인 앱을 닫는다. 숨김 창에 WM_CLOSE를 보내면 앱이 저장을 마치고 스스로 끝난다
// (App::WndProc의 WM_CLOSE → Quit). 응답이 없을 때만 강제 종료로 넘어간다.
// taskkill을 먼저 쓰지 않는 이유: 모든 최상위 창에 WM_CLOSE가 가는데, 메모 창은 그것을
// '숨기기'로 처리해 hidden=true가 저장된다 — 재설치 후 메모가 전부 사라진 것처럼 보인다.
function CloseRunningApp(): Boolean;
var
  Wnd: HWND;
  ResultCode: Integer;
begin
  Wnd := FindWindowByClassName(AppWndClass);
  if Wnd <> 0 then
  begin
    PostMessage(Wnd, WM_CLOSE_MSG, 0, 0);
    if WaitForExit(10000) then
    begin
      Result := True;
      exit;
    end;
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM SuperSticker.exe /F', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := WaitForExit(5000);
end;

// 실행 중이면 물어보고 대신 닫는다. 사일런트 설치에서는 묻지 않고 닫는다.
function AskAndCloseApp(const MessageName: String): Boolean;
begin
  Result := True;
  if not AppIsRunning() then exit;
  if SuppressibleMsgBox(CustomMessage(MessageName), mbConfirmation, MB_YESNO, IDYES) <> IDYES then
  begin
    Result := False;
    exit;
  end;
  Result := CloseRunningApp();
  if not Result then
    SuppressibleMsgBox(CustomMessage('AppCloseFailed'), mbError, MB_OK, IDOK);
end;

function InitializeSetup(): Boolean;
begin
  Result := AskAndCloseApp('AppRunningInstall');
end;

function InitializeUninstall(): Boolean;
begin
  Result := AskAndCloseApp('AppRunningUninstall');
end;

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

// 언인스톨 시: 데이터 삭제 여부를 묻고(기본 보존), 자동 시작 등록 제거.
// 사일런트 제거에서는 묻지 않고 데이터를 보존한다.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir, CustomDir: String;
  Buf: AnsiString;
begin
  if (CurUninstallStep = usUninstall) and (not UninstallSilent) then
  begin
    if MsgBox(CustomMessage('DeleteData'), mbConfirmation,
              MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DataDir := ExpandConstant('{userappdata}\SuperSticker');
      // 데이터 탭에서 저장 경로를 바꾼 경우: datadir.txt가 가리키는 폴더도 삭제
      if LoadStringFromFile(DataDir + '\datadir.txt', Buf) then
      begin
        CustomDir := Trim(String(Buf));
        if (CustomDir <> '') and DirExists(CustomDir) then
        begin
          if not DelTree(CustomDir, True, True, True) then
          begin
            Sleep(1200);  // 방금 종료된 앱이 잡고 있던 핸들 해제 대기 후 재시도
            DelTree(CustomDir, True, True, True);
          end;
        end;
      end;
      if not DelTree(DataDir, True, True, True) then
      begin
        Sleep(1200);
        DelTree(DataDir, True, True, True);
      end;
    end;
  end;
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'SuperSticker');
end;
