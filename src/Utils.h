#pragma once
#include <windows.h>
#include <string>
#include <optional>
#include <utility>
#include <vector>

namespace util {

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

std::wstring NewGuid();
std::wstring NowIso8601();

std::wstring GetExeDir();
std::wstring GetUiDir();       // <exe>\ui
std::wstring GetAppDataDir();  // %APPDATA%\SuperSticker

bool EnsureDir(const std::wstring& path);
// 폴더 항목 열거 ({이름, 디렉터리 여부}, "."/".." 제외).
// FindNextFile 도중 삭제하면 항목이 건너뛰어지므로 항상 먼저 모아서 처리한다.
std::vector<std::pair<std::wstring, bool>> ListDirEntries(const std::wstring& dir);
bool RemoveDirRecursive(const std::wstring& dir);
bool CopyDirRecursive(const std::wstring& src, const std::wstring& dst);
// 폴더 이동. 같은 볼륨이면 MoveFile, 실패하면 복사 후 원본 삭제.
bool MoveDirTo(const std::wstring& src, const std::wstring& dst);
// 자식 프로세스를 창 없이 실행하고 종료를 기다린다. 종료 코드 0이면 true.
bool RunProcessWait(const std::wstring& cmdLine, DWORD timeoutMs = 5 * 60 * 1000);
// PowerShell 작은따옴표 문자열용 이스케이프 (' -> '')
std::wstring PsQuote(const std::wstring& s);
// 폴더 내용물 전체를 zip으로 압축 / zip을 폴더로 해제 (PowerShell 이용).
// zipPath는 .zip 확장자여야 한다 (Compress-Archive 제약).
bool ZipDir(const std::wstring& srcDir, const std::wstring& zipPath);
bool UnzipDir(const std::wstring& zipPath, const std::wstring& destDir);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
std::optional<std::string> ReadFileBytes(const std::wstring& path);

std::vector<BYTE> Base64Decode(const std::string& b64);
std::string UriDecode(const std::string& s);  // %XX 퍼센트 인코딩 해제

// 창 사각형을 가장 가까운 모니터의 작업 영역 안으로 보정.
// 작업 영역보다 크면 크기도 줄인다. 변경이 있으면 true.
bool ClampRectToWorkArea(int& x, int& y, int& w, int& h);

}  // namespace util
