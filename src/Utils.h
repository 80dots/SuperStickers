#pragma once
#include <windows.h>
#include <string>
#include <optional>
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
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
std::optional<std::string> ReadFileBytes(const std::wstring& path);

std::vector<BYTE> Base64Decode(const std::string& b64);

// 창 사각형을 가장 가까운 모니터의 작업 영역 안으로 보정.
// 작업 영역보다 크면 크기도 줄인다. 변경이 있으면 true.
bool ClampRectToWorkArea(int& x, int& y, int& w, int& h);

}  // namespace util
