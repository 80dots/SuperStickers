#include "Utils.h"

#include <objbase.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>

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

// 폴더 항목을 미리 모아 반환한다. FindNextFile 순회 도중 항목을 삭제하면
// 다음 항목이 건너뛰어지는 문제가 있어, 변경 작업은 반드시 이 결과로 루프를 돈다.
std::vector<std::pair<std::wstring, bool>> ListDirEntries(const std::wstring& dir) {
    std::vector<std::pair<std::wstring, bool>> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.emplace_back(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// 폴더를 내용물과 함께 삭제. 읽기 전용 파일도 속성을 풀어 지운다.
bool RemoveDirRecursive(const std::wstring& dir) {
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;  // 이미 없음
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(dir.c_str()) != 0;
    for (auto& [name, isDir] : ListDirEntries(dir)) {
        std::wstring child = dir + L"\\" + name;
        if (isDir) {
            RemoveDirRecursive(child);
        } else {
            SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(child.c_str());
        }
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

bool CopyDirRecursive(const std::wstring& src, const std::wstring& dst) {
    if (!EnsureDir(dst)) return false;
    bool ok = true;
    for (auto& [name, isDir] : ListDirEntries(src)) {
        std::wstring s2 = src + L"\\" + name, d2 = dst + L"\\" + name;
        if (isDir) {
            if (!CopyDirRecursive(s2, d2)) ok = false;
        } else if (!CopyFileW(s2.c_str(), d2.c_str(), FALSE)) {
            ok = false;
        }
    }
    return ok;
}

bool MoveDirTo(const std::wstring& src, const std::wstring& dst) {
    // 같은 볼륨이면 rename 한 번으로 끝. 다른 볼륨이거나 잠긴 파일이 있으면
    // MOVEFILE_COPY_ALLOWED로도 폴더는 이동되지 않으므로 복사 후 원본 삭제로 폴백.
    if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED)) return true;
    if (!CopyDirRecursive(src, dst)) return false;
    RemoveDirRecursive(src);
    return true;
}

bool RunProcessWait(const std::wstring& cmdLine, DWORD timeoutMs) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi))
        return false;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

std::wstring PsQuote(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        out.push_back(c);
        if (c == L'\'') out.push_back(c);  // PowerShell 작은따옴표 문자열에서 '는 ''로 이스케이프
    }
    return out;
}

// 시스템 PowerShell로 zip 압축/해제. WinRT/서드파티 없이 zip을 다루는 가장 단순한 방법.
// -NoProfile/-NonInteractive로 사용자 프로필 스크립트·프롬프트 개입을 차단한다.
bool ZipDir(const std::wstring& srcDir, const std::wstring& zipPath) {
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command \"Compress-Archive "
                       L"-Path '" + PsQuote(srcDir) + L"\\*' -DestinationPath '" +
                       PsQuote(zipPath) + L"' -Force\"";
    return RunProcessWait(cmd, 10 * 60 * 1000) &&
           GetFileAttributesW(zipPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool UnzipDir(const std::wstring& zipPath, const std::wstring& destDir) {
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command \"Expand-Archive "
                       L"-Path '" + PsQuote(zipPath) + L"' -DestinationPath '" +
                       PsQuote(destDir) + L"' -Force\"";
    return RunProcessWait(cmd, 10 * 60 * 1000);
}

// URL을 파일로 내려받는다. filePath가 비면 outBody에 담는다.
// onData(받은 바이트, 전체 바이트)로 진행률을 알리고, abort가 서면 중단한다.
// WinHTTP 기본 정책상 리다이렉트는 따라간다 (Hugging Face는 CDN으로 302를 준다).
bool HttpGetToFile(const std::wstring& url, const std::wstring& filePath, std::string* outBody,
                   std::function<void(uint64_t, uint64_t)> onData,
                   std::atomic<bool>* abort) {
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof(c);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    c.lpszHostName = host;
    c.dwHostNameLength = 255;
    c.lpszUrlPath = path;
    c.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &c)) return false;

    HINTERNET session = WinHttpOpen(L"SuperStickers/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 60000);
    HINTERNET connect = WinHttpConnect(session, host, c.nPort, 0);
    HINTERNET request =
        connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                : nullptr;
    bool ok = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            uint64_t total = 0;
            wchar_t lenBuf[32]{};
            DWORD lenSize = sizeof(lenBuf);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                                    WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize,
                                    WINHTTP_NO_HEADER_INDEX))
                total = _wtoi64(lenBuf);
            if (!filePath.empty()) {
                file = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) status = 0;
            }
            if (status == 200) {
                uint64_t received = 0;
                ok = true;
                for (;;) {
                    if (abort && abort->load()) { ok = false; break; }
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    std::vector<char> buf(avail < 65536 ? avail : 65536);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buf.data(), (DWORD)buf.size(), &read) ||
                        read == 0) {
                        if (read == 0) break;
                        ok = false;
                        break;
                    }
                    received += read;
                    if (file != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        if (!WriteFile(file, buf.data(), read, &written, nullptr)) {
                            ok = false;
                            break;
                        }
                    } else if (outBody) {
                        outBody->append(buf.data(), read);
                    }
                    if (onData) onData(received, total);
                }
            }
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// CryptoAPI로 SHA-256 (bcrypt를 새로 링크하지 않으려고 advapi32/crypt32 조합을 쓴다).
// 4GB 모델 파일도 다루므로 스트리밍으로 읽는다.
std::string Sha256File(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "";

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        std::vector<BYTE> buf(1 << 20);
        DWORD read = 0;
        bool ok = true;
        while (ReadFile(file, buf.data(), (DWORD)buf.size(), &read, nullptr) && read > 0) {
            if (!CryptHashData(hash, buf.data(), read, 0)) { ok = false; break; }
        }
        BYTE digest[32]{};
        DWORD len = sizeof(digest);
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
            static const char* kHex = "0123456789abcdef";
            for (DWORD i = 0; i < len; ++i) {
                out += kHex[digest[i] >> 4];
                out += kHex[digest[i] & 0xF];
            }
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(file);
    return out;
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

std::string UriDecode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

}  // namespace util
