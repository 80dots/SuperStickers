# Super Sticker 빌드 스크립트
# 사용법:
#   .\scripts\build.ps1              # Release 빌드
#   .\scripts\build.ps1 -Debug      # Debug 빌드
#   .\scripts\build.ps1 -Installer  # Release 빌드 + 설치 프로그램 생성
param(
    [switch]$Debug,
    [switch]$Installer
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
$root = Split-Path $PSScriptRoot
$preset = if ($Debug) { "debug" } else { "release" }

# Visual Studio 개발자 환경에서 CMake/Ninja/cl 사용
$vsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vsDevCmd)) {
    # 다른 에디션/버전 탐색
    $vsDevCmd = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter "VsDevCmd.bat" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $vsDevCmd) { throw "Visual Studio(VsDevCmd.bat)를 찾을 수 없습니다." }
}

cmd /c "`"$vsDevCmd`" -arch=x64 -no_logo 2>nul && cd /d `"$root`" && cmake --preset $preset && cmake --build --preset $preset"
if ($LASTEXITCODE -ne 0) { throw "빌드 실패 (exit $LASTEXITCODE)" }
Write-Host "`n빌드 완료: $root\build\$preset\SuperSticker.exe" -ForegroundColor Green

if ($Installer) {
    $iscc = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw "Inno Setup 6이 설치되어 있지 않습니다: winget install JRSoftware.InnoSetup" }

    $bootstrapper = "$root\installer\redist\MicrosoftEdgeWebView2Setup.exe"
    if (-not (Test-Path $bootstrapper)) {
        Write-Host "WebView2 부트스트래퍼 다운로드 중..."
        New-Item -ItemType Directory -Force (Split-Path $bootstrapper) | Out-Null
        Invoke-WebRequest -Uri "https://go.microsoft.com/fwlink/p/?LinkId=2124703" -OutFile $bootstrapper -UseBasicParsing
    }

    # 버전 단일 출처: CMakeLists.txt의 project(... VERSION ...)를 읽어 ISCC에 넘긴다
    $cml = Get-Content "$root\CMakeLists.txt" -Raw
    if ($cml -notmatch 'project\(SuperSticker VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
        throw "CMakeLists.txt에서 버전을 찾지 못했습니다."
    }
    $ver = $Matches[1]
    Write-Host "설치 프로그램 버전: $ver"

    & $iscc "/DMyAppVersion=$ver" "$root\installer\SuperSticker.iss"
    if ($LASTEXITCODE -ne 0) { throw "설치 프로그램 빌드 실패" }
    Write-Host "`n설치 프로그램: $root\installer\Output\" -ForegroundColor Green
}
