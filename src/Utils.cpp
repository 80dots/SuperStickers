#include "Utils.h"

#include <objbase.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <fstream>

namespace util {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring NewGuid() {
    GUID g{};
    CoCreateGuid(&g);
    wchar_t buf[64];
    swprintf_s(buf, L"%08lx-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
               g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
               g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

std::wstring NowIso8601() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    wchar_t buf[40];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring GetUiDir() { return GetExeDir() + L"\\ui"; }

std::wstring GetAppDataDir() {
    wchar_t* raw = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw))) {
        dir = raw;
        CoTaskMemFree(raw);
    }
    return dir + L"\\SuperSticker";
}

bool EnsureDir(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f.good()) return false;
    }
    // 직전 성공본을 .bak으로 보존한 뒤 원자적 교체
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    }
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::optional<std::string> ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return data;
}

bool ClampRectToWorkArea(int& x, int& y, int& w, int& h) {
    RECT r{x, y, x + w, y + h};
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT& wa = mi.rcWork;
    int nx = x, ny = y, nw = w, nh = h;
    int waW = wa.right - wa.left, waH = wa.bottom - wa.top;
    if (nw > waW) nw = waW;
    if (nh > waH) nh = waH;
    if (nx + nw > wa.right) nx = wa.right - nw;
    if (ny + nh > wa.bottom) ny = wa.bottom - nh;
    if (nx < wa.left) nx = wa.left;
    if (ny < wa.top) ny = wa.top;
    bool changed = nx != x || ny != y || nw != w || nh != h;
    x = nx;
    y = ny;
    w = nw;
    h = nh;
    return changed;
}

std::vector<BYTE> Base64Decode(const std::string& b64) {
    DWORD len = 0;
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, nullptr, &len,
                              nullptr, nullptr))
        return {};
    std::vector<BYTE> out(len);
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, out.data(),
                              &len, nullptr, nullptr))
        return {};
    out.resize(len);
    return out;
}

}  // namespace util
