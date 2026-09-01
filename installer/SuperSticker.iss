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
; 실행 중이면 종료 안내 (앱의 단일 인스턴스 뮤텍스)
AppMutex=Local\SuperSticker.Instance
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
english.AutoStart=Start automatically with Windows
korean.AutoStart=Windows 시작 시 자동 실행
english.DeleteData=Do you also want to delete all memos, groups and settings?%n%nThis cannot be undone. (Choose No to keep your data for a future install.)
korean.DeleteData=메모·그룹·설정 데이터도 함께 삭제할까요?%n%n이 작업은 되돌릴 수 없습니다. (아니요를 선택하면 데이터가 보존되어 다시 설치할 때 그대로 복원됩니다.)

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
