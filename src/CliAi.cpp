#include "CliAi.h"

#include <shlobj.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "Utils.h"

using json = nlohmann::json;

namespace {

constexpr DWORD kChatTimeoutMs = 10 * 60 * 1000;  // 긴 메모의 번역도 넉넉히 끝나는 시간
constexpr DWORD kProbeTimeoutMs = 30 * 1000;      // --version·로그인 확인

// 파일이 있는지 (폴더 제외)
bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// %VAR%를 풀어 준다
std::wstring Expand(const std::wstring& s) {
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring EnvVar(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring out(n, L'\0');
    GetEnvironmentVariableW(name, out.data(), n);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

// 레지스트리의 Path 값 (REG_EXPAND_SZ면 풀어서)
std::wstring RegistryPath(HKEY root, const wchar_t* subkey) {
    HKEY k;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return L"";
    DWORD type = 0, size = 0;
    std::wstring out;
    if (RegQueryValueExW(k, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size) {
        std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(k, L"Path", nullptr, &type, (LPBYTE)buf.data(), &size) ==
            ERROR_SUCCESS) {
            buf.resize(wcsnlen(buf.c_str(), buf.size()));
            out = type == REG_EXPAND_SZ ? Expand(buf) : buf;
        }
    }
    RegCloseKey(k);
    return out;
}

// 찾아볼 폴더들: 이 프로세스의 PATH + 레지스트리의 최신 PATH.
// 앱이 로그인 때 받은 PATH는 CLI를 나중에 설치했으면 낡아 있다 — 레지스트리를 함께 본다.
std::vector<std::wstring> SearchDirs() {
    std::wstring all = EnvVar(L"PATH") + L";" + RegistryPath(HKEY_CURRENT_USER, L"Environment") +
                       L";" +
                       RegistryPath(HKEY_LOCAL_MACHINE,
                                    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    std::vector<std::wstring> dirs;
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find(L';', start);
        if (end == std::wstring::npos) end = all.size();
        std::wstring d = all.substr(start, end - start);
        while (!d.empty() && (d.back() == L'\\' || d.back() == L' ')) d.pop_back();
        if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
        if (!d.empty()) {
            bool dup = std::any_of(dirs.begin(), dirs.end(), [&](const std::wstring& x) {
                return _wcsicmp(x.c_str(), d.c_str()) == 0;
            });
            if (!dup) dirs.push_back(d);
        }
        start = end + 1;
    }
    return dirs;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// npm으로 설치한 Codex의 네이티브 exe (npmDir = codex.cmd가 있는 폴더)
std::wstring CodexNativeIn(const std::wstring& npmDir) {
    const wchar_t* arches[][2] = {{L"x64", L"x86_64-pc-windows-msvc"},
                                  {L"arm64", L"aarch64-pc-windows-msvc"}};
    for (auto& a : arches) {
        std::wstring pkg = std::wstring(L"@openai\\codex-win32-") + a[0];
        std::wstring vendor = std::wstring(L"\\vendor\\") + a[1];
        const std::wstring cands[] = {
            npmDir + L"\\node_modules\\@openai\\codex\\node_modules\\" + pkg + vendor +
                L"\\bin\\codex.exe",
            npmDir + L"\\node_modules\\" + pkg + vendor + L"\\bin\\codex.exe",
            npmDir + L"\\node_modules\\@openai\\codex\\vendor\\" + a[1] + L"\\codex\\codex.exe",
        };
        for (auto& c : cands)
            if (FileExists(c)) return c;
    }
    return L"";
}

// Windows 명령줄 규칙(CommandLineToArgvW)에 맞게 인자 하나를 따옴표로 감싼다
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == a.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(a[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

// CLI를 돌릴 작업 폴더. 빈 폴더라 프로젝트 설정(CLAUDE.md 등)이 끼어들지 않는다.
std::wstring WorkDir() {
    std::wstring dir;
    wchar_t* local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        dir = std::wstring(local) + L"\\SuperSticker\\cli";
        CoTaskMemFree(local);
    }
    if (!dir.empty()) util::EnsureDir(dir);
    return dir;
}

// 요청마다 쓰는 임시 파일 이름의 앞부분
std::wstring TempPrefix(const std::wstring& dir) {
    static std::atomic<unsigned> counter{0};
    return dir + L"\\req-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++counter);
}

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) &&
              written == data.size();
    CloseHandle(f);
    return ok;
}

// 앞뒤 공백 제거 + 길이 제한
std::string Clip(std::string s, size_t max = 400) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    if (s.size() > max) s = s.substr(0, max) + "...";
    return s;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// 오류 문장을 페이지가 알아보는 꼴로. 로그인 문제는 따로 알려 준다 (터미널에서 로그인해야 한다).
std::string Classify(const std::string& raw) {
    std::string msg = Clip(raw);
    std::string l = Lower(msg);
    static const char* authWords[] = {"not logged in", "/login",           "log in",
                                      "login",         "invalid api key",  "api key",
                                      "unauthorized",  "authentication",   "401",
                                      "oauth",         "credential"};
    for (auto w : authWords)
        if (l.find(w) != std::string::npos) return "cli-auth: " + msg;
    return "cli: " + (msg.empty() ? std::string("failed") : msg);
}

// Codex는 오류 메시지 안에 API 응답 JSON을 통째로 넣어 준다 — 사람이 읽을 부분만 꺼낸다
std::string InnerMessage(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    if (j.is_object()) {
        if (j.contains("error") && j["error"].is_object() && j["error"].contains("message") &&
            j["error"]["message"].is_string())
            return j["error"]["message"];
        if (j.contains("message") && j["message"].is_string()) return j["message"];
    }
    return s;
}

// 메시지 내용 → 글자 (문자열이거나 [{type:"text", text}] 배열)
std::string ContentText(const json& c) {
    if (c.is_string()) return c.get<std::string>();
    std::string out;
    if (c.is_array()) {
        for (auto& part : c)
            if (part.is_object() && part.contains("text") && part["text"].is_string())
                out += part["text"].get<std::string>();
    }
    return out;
}

// OpenAI 구조화 출력(strict)이 받는 꼴로 스키마를 고친다: 객체마다 모든 속성을 required,
// additionalProperties=false. 크기 제약 키워드는 거부될 수 있어 뺀다 (형식은 프롬프트가 지킨다).
json StrictSchema(json s) {
    if (s.is_object()) {
        for (auto k : {"maxItems", "minItems", "maxLength", "minLength", "pattern", "format"})
            s.erase(k);
        if (s.value("type", "") == "object" && s.contains("properties") &&
            s["properties"].is_object()) {
            json req = json::array();
            json& props = s["properties"];
            for (auto it = props.begin(); it != props.end(); ++it) {
                it.value() = StrictSchema(it.value());
                req.push_back(it.key());
            }
            s["required"] = req;
            s["additionalProperties"] = false;
        }
        if (s.contains("items")) s["items"] = StrictSchema(s["items"]);
    }
    return s;
}

struct ExecResult {
    bool launched = false;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string stderrText;  // 끝부분만 보관
};

// 자식 프로세스 실행: stdin에 글을 넣고, stdout은 줄 단위로 onLine에 넘기고, stderr는 모은다.
// 잡 오브젝트에 넣어 중단·시간 초과·앱 종료 때 손자 프로세스까지 함께 끝낸다.
ExecResult Exec(const std::wstring& exe, const std::vector<std::wstring>& args,
                const std::string& stdinText, const std::wstring& cwd, DWORD timeoutMs,
                const std::shared_ptr<CliAi::Run>& run,
                const std::function<void(const std::string&)>& onLine) {
    ExecResult r;

    std::wstring cmd = QuoteArg(exe);
    for (auto& a : args) cmd += L" " + QuoteArg(a);
    std::wstring app;  // 비우면 명령줄 첫 토큰으로 찾는다
    if (EndsWithI(exe, L".cmd") || EndsWithI(exe, L".bat")) {
        // 배치 파일은 cmd.exe가 돌려야 한다. /s: 바깥 따옴표 한 쌍만 벗기고 나머지는 그대로.
        // (여기 들어가는 인자는 경로·고정 플래그·검증한 모델 이름뿐이다 — 사용자 글은 stdin)
        std::wstring comspec = EnvVar(L"ComSpec");
        if (comspec.empty()) comspec = L"cmd.exe";
        app = comspec;
        cmd = QuoteArg(comspec) + L" /d /s /c \"" + cmd + L"\"";
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr,
           errW = nullptr;
    auto closeH = [](HANDLE& h) {
        if (h) CloseHandle(h);
        h = nullptr;
    };
    // stdin 버퍼를 넉넉히 — 자식이 읽기 전에 써도 막히지 않게 (넘치면 쓰기 스레드가 기다린다)
    if (!CreatePipe(&inR, &inW, &sa, 1 << 20) || !CreatePipe(&outR, &outW, &sa, 1 << 16) ||
        !CreatePipe(&errR, &errW, &sa, 1 << 16)) {
        closeH(inR); closeH(inW); closeH(outR); closeH(outW); closeH(errR); closeH(errW);
        return r;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // 이 세 핸들만 물려준다. 동시에 도는 다른 요청의 파이프를 물려받으면 그쪽 읽기가
    // 이 프로세스가 끝날 때까지 EOF를 못 받는다.
    HANDLE inheritList[3] = {inR, outW, errW};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    bool attrOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                            inheritList, sizeof(inheritList), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrOk ? attrs : nullptr;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    BOOL created = CreateProcessW(app.empty() ? nullptr : app.c_str(), mutableCmd.data(), nullptr,
                                  nullptr, TRUE, flags, nullptr,
                                  cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    if (attrOk) DeleteProcThreadAttributeList(attrs);
    closeH(inR);
    closeH(outW);
    closeH(errW);
    if (!created) {
        closeH(inW); closeH(outR); closeH(errR);
        if (job) CloseHandle(job);
        return r;
    }
    r.launched = true;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    {
        std::lock_guard<std::mutex> lock(run->mutex);
        run->job = job;
        if (run->aborted && job) TerminateJobObject(job, 1);  // 띄우는 사이에 중단됐다
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // stdin 쓰기 — 다 쓰면 닫아 EOF를 알린다
    std::thread writer([inW, stdinText]() mutable {
        size_t off = 0;
        while (off < stdinText.size()) {
            DWORD chunk = (DWORD)std::min<size_t>(stdinText.size() - off, 1 << 16), n = 0;
            if (!WriteFile(inW, stdinText.data() + off, chunk, &n, nullptr) || !n) break;
            off += n;
        }
        CloseHandle(inW);
    });

    // stderr 모으기 (끝부분 16KB만)
    std::string errText;
    std::thread errReader([errR, &errText]() {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(errR, buf, sizeof(buf), &n, nullptr) && n) {
            errText.append(buf, n);
            if (errText.size() > 16384) errText.erase(0, errText.size() - 16384);
        }
    });

    // 감시: 시간 초과면 끝내고, 프로세스가 끝났는데도 파이프를 붙든 손자가 있으면 거둔다
    HANDLE stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool timedOut = false;
    std::thread watchdog([&]() {
        HANDLE waits[2] = {pi.hProcess, stopEvt};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
        if (w == WAIT_TIMEOUT) {
            timedOut = true;
            std::lock_guard<std::mutex> lock(run->mutex);
            if (run->job) TerminateJobObject(run->job, 1);
        } else if (w == WAIT_OBJECT_0) {
            if (WaitForSingleObject(stopEvt, 2000) == WAIT_TIMEOUT) {
                std::lock_guard<std::mutex> lock(run->mutex);
                if (run->job) TerminateJobObject(run->job, 1);
            }
        }
    });

    // stdout 줄 단위 읽기
    std::string pending;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(outR, buf, sizeof(buf), &n, nullptr) && n) {
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) onLine(line);
        }
    }
    if (!pending.empty()) onLine(pending);

    SetEvent(stopEvt);
    watchdog.join();
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) {
        std::lock_guard<std::mutex> lock(run->mutex);
        if (run->job) TerminateJobObject(run->job, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    {
        std::lock_guard<std::mutex> lock(run->mutex);
        run->job = nullptr;
    }
    if (job) CloseHandle(job);  // KILL_ON_JOB_CLOSE — 남은 것이 있으면 여기서 끝난다
    writer.join();
    errReader.join();
    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hProcess);
    CloseHandle(stopEvt);
    r.timedOut = timedOut;
    r.stderrText = errText;
    return r;
}

// stderr·종료 코드에서 오류 문장 하나를 만든다
std::string FailureText(const ExecResult& r, const std::string& fromOutput) {
    if (!fromOutput.empty()) return fromOutput;
    std::string e = Clip(r.stderrText);
    if (!e.empty()) return e;
    return "exit code " + std::to_string(r.exitCode);
}

}  // namespace

void CliAi::Run::Kill() {
    std::lock_guard<std::mutex> lock(mutex);
    aborted = true;
    if (job) TerminateJobObject(job, 1);
}

bool CliAi::ParseKind(const std::string& s, Kind& out) {
    if (s == "claude") { out = Kind::Claude; return true; }
    if (s == "codex") { out = Kind::Codex; return true; }
    return false;
}

std::wstring CliAi::Resolve(Kind kind, const std::string& customPath) {
    if (!customPath.empty()) {
        std::wstring p = Expand(util::Utf8ToWide(customPath));
        if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);
        // 적어 둔 경로가 틀렸으면 다른 것을 몰래 쓰지 않는다 — '찾을 수 없음'으로 알린다
        return FileExists(p) ? p : L"";
    }
    const wchar_t* name = kind == Kind::Claude ? L"claude" : L"codex";
    std::wstring found;
    for (auto& dir : SearchDirs()) {
        for (auto ext : {L".exe", L".cmd"}) {
            std::wstring c = dir + L"\\" + name + ext;
            if (FileExists(c)) { found = c; break; }
        }
        if (!found.empty()) break;
    }
    if (kind == Kind::Codex && EndsWithI(found, L".cmd")) {
        // npm 래퍼는 node를 한 번 더 거친다 — 안에 든 네이티브 exe를 바로 쓴다
        std::wstring native = CodexNativeIn(found.substr(0, found.find_last_of(L'\\')));
        if (!native.empty()) return native;
    }
    if (!found.empty()) return found;

    // PATH에 없어도 흔한 설치 위치는 본다
    std::wstring appData = EnvVar(L"APPDATA"), home = EnvVar(L"USERPROFILE");
    if (kind == Kind::Claude) {
        for (auto& c : {home + L"\\.local\\bin\\claude.exe", appData + L"\\npm\\claude.cmd"})
            if (FileExists(c)) return c;
    } else {
        std::wstring native = CodexNativeIn(appData + L"\\npm");
        if (!native.empty()) return native;
        if (FileExists(appData + L"\\npm\\codex.cmd")) return appData + L"\\npm\\codex.cmd";
    }
    return L"";
}

bool CliAi::ValidModel(const std::string& model) {
    if (model.size() > 80) return false;
    for (unsigned char c : model)
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']' ||
              c == '/'))
            return false;
    return true;
}

void CliAi::Detect(Kind kind, const std::string& customPath, std::function<void(Status)> done) {
    UiPoster poster = uiPoster_;
    std::thread([kind, customPath, done, poster]() {
        Status st;
        std::wstring exe = Resolve(kind, customPath);
        auto finish = [&]() {
            if (poster) poster([done, st]() { done(st); });
        };
        if (exe.empty()) {
            st.error = "cli-missing";
            finish();
            return;
        }
        st.path = util::WideToUtf8(exe);
        std::wstring cwd = WorkDir();

        // 버전
        auto run = std::make_shared<Run>();
        std::string out;
        ExecResult vr = Exec(exe, {L"--version"}, "", cwd, kProbeTimeoutMs, run,
                             [&](const std::string& line) {
                                 if (out.empty()) out = line;
                             });
        if (!vr.launched) {
            st.error = "cli: launch failed";
            finish();
            return;
        }
        st.found = true;
        st.version = Clip(out, 80);

        // 로그인 상태 (토큰을 쓰지 않는 명령)
        std::string all;
        auto collect = [&](const std::string& line) { all += line + "\n"; };
        auto run2 = std::make_shared<Run>();
        if (kind == Kind::Claude) {
            ExecResult ar = Exec(exe, {L"auth", L"status"}, "", cwd, kProbeTimeoutMs, run2, collect);
            json j = json::parse(all, nullptr, false);
            if (ar.launched && j.is_object() && j.contains("loggedIn") && j["loggedIn"].is_boolean()) {
                st.authKnown = true;
                st.loggedIn = j["loggedIn"];
                std::string method = j.value("authMethod", "");
                std::string sub = j.value("subscriptionType", "");
                st.account = method + (sub.empty() ? "" : (method.empty() ? "" : " · ") + sub);
            }
        } else {
            ExecResult ar = Exec(exe, {L"login", L"status"}, "", cwd, kProbeTimeoutMs, run2, collect);
            std::string text = Clip(all + ar.stderrText, 160);
            std::string l = Lower(text);
            if (ar.launched && !text.empty()) {
                st.authKnown = true;
                st.loggedIn = l.find("not logged in") == std::string::npos &&
                              l.find("logged in") != std::string::npos;
                st.account = text.substr(0, text.find('\n'));
            }
        }
        finish();
    }).detach();
}

void CliAi::Chat(const std::string& requestId, Kind kind, const std::string& customPath,
                 const json& messages, const ChatOptions& opts,
                 std::function<void(std::string delta)> onChunk,
                 std::function<void(bool ok, std::string error)> onDone) {
    auto run = std::make_shared<Run>();
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->active[requestId] = run;
    }
    UiPoster poster = uiPoster_;
    std::shared_ptr<Shared> shared = shared_;

    std::thread([=]() {
        auto post = [&](std::function<void()> fn) {
            if (poster) poster(std::move(fn));
        };
        auto finish = [&](bool ok, std::string err) {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->active.erase(requestId);
            }
            post([onDone, ok, err]() { onDone(ok, err); });
        };
        auto emit = [&](const std::string& text) {
            if (text.empty()) return;
            post([onChunk, text]() { onChunk(text); });
        };

        std::wstring exe = Resolve(kind, customPath);
        if (exe.empty()) {
            finish(false, "cli-missing");
            return;
        }
        const bool viaCmd = EndsWithI(exe, L".cmd") || EndsWithI(exe, L".bat");

        // 메시지 → 시스템 지시 + 프롬프트 한 덩어리
        std::string system, prompt;
        std::vector<std::pair<std::string, std::string>> turns;
        if (messages.is_array()) {
            for (auto& m : messages) {
                if (!m.is_object()) continue;
                std::string role = m.value("role", "user");
                std::string text = m.contains("content") ? ContentText(m["content"]) : "";
                if (role == "system") system += (system.empty() ? "" : "\n\n") + text;
                else turns.emplace_back(role, text);
            }
        }
        if (turns.size() == 1) {
            prompt = turns[0].second;
        } else {
            for (auto& [role, text] : turns)
                prompt += std::string(role == "assistant" ? "[Assistant]\n" : "[User]\n") + text + "\n\n";
            prompt += "Reply to the last [User] message.";
        }

        const bool hasSchema = opts.jsonFormat && opts.jsonSchema.is_object();
        const std::string jsonRule = "Respond with a single JSON object only. No code fences, no text before or after it.";

        std::wstring cwd = WorkDir();
        std::wstring tmp = TempPrefix(cwd);
        std::vector<std::wstring> tempFiles;
        std::vector<std::wstring> args;
        std::string model = ValidModel(opts.model) ? opts.model : "";

        if (kind == Kind::Claude) {
            // -p 헤드리스. 도구·MCP를 모두 끄고 기본 시스템 프롬프트(에이전트용)를 우리 것으로
            // 바꾼다 — 파일을 건드리지 않고, 시작이 빠르고, 토큰도 덜 든다.
            if (opts.jsonFormat) {
                // 스키마를 인자로 줄 수 없는 배치 래퍼면 지시문으로 대신한다
                if (!hasSchema || viaCmd) {
                    system += "\n\n" + jsonRule;
                    if (hasSchema) system += "\nJSON schema:\n" + opts.jsonSchema.dump();
                }
            }
            if (system.empty()) system = "You are a helpful assistant.";
            std::wstring sysFile = tmp + L"-system.txt";
            WriteUtf8File(sysFile, system);
            tempFiles.push_back(sysFile);
            args = {L"-p", L"--output-format", L"stream-json", L"--verbose",
                    L"--include-partial-messages", L"--tools", L"", L"--strict-mcp-config",
                    L"--no-session-persistence", L"--system-prompt-file", sysFile};
            if (!model.empty()) args.insert(args.end(), {L"--model", util::Utf8ToWide(model)});
            if (hasSchema && !viaCmd)
                args.insert(args.end(), {L"--json-schema", util::Utf8ToWide(opts.jsonSchema.dump())});
        } else {
            // Codex에는 시스템 프롬프트 플래그가 없다 — 지시를 프롬프트 앞에 붙인다
            if (opts.jsonFormat && !hasSchema) system += (system.empty() ? "" : "\n\n") + jsonRule;
            if (!system.empty())
                prompt = "<instructions>\n" + system + "\n</instructions>\n\n" + prompt;
            std::wstring lastFile = tmp + L"-last.txt";
            tempFiles.push_back(lastFile);
            args = {L"exec", L"--json", L"--skip-git-repo-check", L"--ephemeral",
                    L"--sandbox", L"read-only"};
            if (!model.empty()) args.insert(args.end(), {L"-m", util::Utf8ToWide(model)});
            if (hasSchema) {
                std::wstring schemaFile = tmp + L"-schema.json";
                WriteUtf8File(schemaFile, StrictSchema(opts.jsonSchema).dump());
                tempFiles.push_back(schemaFile);
                args.insert(args.end(), {L"--output-schema", schemaFile});
            }
            args.insert(args.end(), {L"-o", lastFile, L"-"});
        }

        // 출력 해석 상태
        bool emitted = false, gotResult = false, resultError = false, turnFailed = false;
        std::string resultText, errorText, lastMessage;
        json structured;

        auto onLine = [&](const std::string& line) {
            json j = json::parse(line, nullptr, false);
            if (!j.is_object()) return;
            std::string type = j.value("type", "");
            if (kind == Kind::Claude) {
                if (type == "stream_event" && j.contains("event") && j["event"].is_object()) {
                    const json& e = j["event"];
                    if (e.value("type", "") == "content_block_delta" && e.contains("delta") &&
                        e["delta"].is_object() && e["delta"].value("type", "") == "text_delta" &&
                        !opts.jsonFormat) {
                        std::string d = e["delta"].value("text", "");
                        if (!d.empty()) {
                            emitted = true;
                            emit(d);
                        }
                    }
                } else if (type == "result") {
                    gotResult = true;
                    resultError = j.value("is_error", false) || j.value("subtype", "") != "success";
                    if (j.contains("result") && j["result"].is_string()) resultText = j["result"];
                    if (resultText.empty() && resultError) resultText = j.value("subtype", "");
                    if (j.contains("structured_output")) structured = j["structured_output"];
                }
            } else {
                if (type == "item.completed" && j.contains("item") && j["item"].is_object()) {
                    const json& it = j["item"];
                    if (it.value("type", "") == "agent_message" && it.contains("text") &&
                        it["text"].is_string())
                        lastMessage = it["text"];  // 중간 설명이 섞일 수 있다 — 마지막 것만 쓴다
                } else if (type == "error") {
                    errorText = InnerMessage(j.value("message", ""));
                } else if (type == "turn.failed") {
                    turnFailed = true;
                    if (j.contains("error") && j["error"].is_object())
                        errorText = InnerMessage(j["error"].value("message", errorText));
                }
            }
        };

        ExecResult r = Exec(exe, args, prompt, cwd, kChatTimeoutMs, run, onLine);

        std::string codexFileText;
        if (kind == Kind::Codex && lastMessage.empty() && !tempFiles.empty()) {
            // -o 파일이 이벤트를 놓쳤을 때의 보험
            for (auto& f : tempFiles)
                if (EndsWithI(f, L"-last.txt"))
                    if (auto bytes = util::ReadFileBytes(f)) codexFileText = *bytes;
        }
        for (auto& f : tempFiles) DeleteFileW(f.c_str());

        if (run->aborted) return finish(false, "aborted");
        if (!r.launched) return finish(false, "cli: launch failed");
        if (r.timedOut) return finish(false, "cli-timeout");

        if (kind == Kind::Claude) {
            if (gotResult && !resultError) {
                if (opts.jsonFormat)
                    emit(structured.is_object() ? structured.dump() : resultText);
                else if (!emitted)
                    emit(resultText);
                return finish(true, "");
            }
            return finish(false, Classify(FailureText(r, resultText)));
        }
        std::string answer = !lastMessage.empty() ? lastMessage : codexFileText;
        if (!turnFailed && !answer.empty() && r.exitCode == 0) {
            emit(answer);
            return finish(true, "");
        }
        return finish(false, Classify(FailureText(r, errorText)));
    }).detach();
}

void CliAi::Abort(const std::string& requestId) {
    std::shared_ptr<Run> run;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        auto it = shared_->active.find(requestId);
        if (it != shared_->active.end()) run = it->second;
    }
    if (run) run->Kill();
}

void CliAi::AbortAll() {
    std::vector<std::shared_ptr<Run>> runs;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        for (auto& [id, run] : shared_->active) runs.push_back(run);
    }
    for (auto& run : runs) run->Kill();
}
