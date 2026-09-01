#include "LocalAi.h"

#include <winsock2.h>  // PickFreePort — windows.h 뒤에 와도 된다(WIN32_LEAN_AND_MEAN)
#include <ws2tcpip.h>
#include <winhttp.h>

#include <thread>

#include "Utils.h"

namespace {

// llama.cpp 배포본은 하루에도 여러 번 나온다. "latest"를 따라가면 사용자마다 다른 빌드를
// 받게 되므로 검증한 빌드에 고정하고, 올릴 때 sha256도 함께 갱신한다.
// (자산 이름 규칙: llama-<build>-bin-win-<variant>-x64.zip)
const char* kEngineBuild = "b10738";
const char* kEngineUrlPrefix = "https://github.com/ggml-org/llama.cpp/releases/download/";

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// 127.0.0.1에서 비어 있는 포트를 하나 잡는다 (바인드해 보고 바로 닫는다).
// 닫은 뒤 서버가 잡기까지 아주 짧은 틈이 있지만, 실패하면 기동 대기에서 걸러진다.
int PickFreePort() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    int port = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 0 = 커널이 빈 포트를 준다
        if (bind(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            int len = sizeof(addr);
            if (getsockname(s, (sockaddr*)&addr, &len) == 0) port = ntohs(addr.sin_port);
        }
        closesocket(s);
    }
    WSACleanup();
    return port;
}

}  // namespace

// ---------- 카탈로그 ----------

const std::vector<LocalAi::ModelInfo>& LocalAi::Catalog() {
    // 크기 사다리로 고른다. 전부 Hugging Face의 단일 파일 Q4_K_M GGUF이고 크기는 실측값이다.
    // 한국어가 되는 모델만 넣었다 — 앱의 기본 프롬프트가 한국어다.
    static const std::vector<ModelInfo> kModels = {
        {"qwen3-1.7b", "Qwen3 1.7B", "1.7B", "unsloth/Qwen3-1.7B-GGUF",
         "Qwen3-1.7B-Q4_K_M.gguf", "Apache-2.0", 1107409472ull, true, false},
        {"exaone-3.5-2.4b", "EXAONE 3.5 2.4B", "2.4B",
         "LGAI-EXAONE/EXAONE-3.5-2.4B-Instruct-GGUF",
         "EXAONE-3.5-2.4B-Instruct-Q4_K_M.gguf", "EXAONE AI Model License (비상업)",
         1644918272ull, false, false},
        {"qwen3-4b-instruct", "Qwen3 4B Instruct", "4B", "unsloth/Qwen3-4B-Instruct-2507-GGUF",
         "Qwen3-4B-Instruct-2507-Q4_K_M.gguf", "Apache-2.0", 2497281120ull, false, true},
        {"qwen2.5-7b-instruct", "Qwen2.5 7B Instruct", "7B", "bartowski/Qwen2.5-7B-Instruct-GGUF",
         "Qwen2.5-7B-Instruct-Q4_K_M.gguf", "Apache-2.0", 4683074240ull, false, false},
        {"qwen3-8b", "Qwen3 8B", "8B", "unsloth/Qwen3-8B-GGUF", "Qwen3-8B-Q4_K_M.gguf",
         "Apache-2.0", 5027784512ull, true, false},
    };
    return kModels;
}

const LocalAi::ModelInfo* LocalAi::FindModel(const std::string& id) {
    for (const auto& m : Catalog()) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const std::vector<LocalAi::EngineVariant>& LocalAi::Engines() {
    static const std::vector<EngineVariant> kEngines = {
        {"cpu", std::string("llama-") + kEngineBuild + "-bin-win-cpu-x64.zip",
         "d018e2a8f030682560f83ae92a2a8b6fda455f968bbfff6243f7fe424179796c", 18374025ull},
        {"vulkan", std::string("llama-") + kEngineBuild + "-bin-win-vulkan-x64.zip",
         "286a5c5befca708840495b3e82d6689f6b4341354576d96529b69738f8e69fdb", 35185156ull},
    };
    return kEngines;
}

const LocalAi::EngineVariant* LocalAi::FindEngine(const std::string& id) {
    for (const auto& e : Engines()) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

// ---------- 경로 ----------

void LocalAi::Init(UiPoster poster, const std::wstring& dataDir) {
    uiPoster_ = std::move(poster);
    dataDir_ = dataDir;
}

void LocalAi::SetDataDir(const std::wstring& dataDir) { dataDir_ = dataDir; }

std::wstring LocalAi::AiDir() const { return dataDir_ + L"\\ai"; }

std::wstring LocalAi::EngineDir(const std::string& variant) const {
    return AiDir() + L"\\engine-" + util::Utf8ToWide(variant) + L"-" +
           util::Utf8ToWide(kEngineBuild);
}

std::wstring LocalAi::ServerExe(const std::string& variant) const {
    return EngineDir(variant) + L"\\llama-server.exe";
}

std::wstring LocalAi::ModelPath(const ModelInfo& m) const {
    return AiDir() + L"\\models\\" + util::Utf8ToWide(m.file);
}

bool LocalAi::EngineInstalled(const std::string& variant) const {
    return GetFileAttributesW(ServerExe(variant).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool LocalAi::ModelInstalled(const ModelInfo& m) const {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(ModelPath(m).c_str(), GetFileExInfoStandard, &fad)) return false;
    // 크기가 카탈로그와 크게 다르면 받다 만 파일로 본다
    uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return size > 0 && (m.sizeBytes == 0 || size >= m.sizeBytes - (m.sizeBytes / 20));
}

std::vector<std::string> LocalAi::InstalledModels() const {
    std::vector<std::string> out;
    for (const auto& m : Catalog()) {
        if (ModelInstalled(m)) out.push_back(m.id);
    }
    return out;
}

bool LocalAi::HasVulkanCapableGpu() {
    // 정확한 판별은 Vulkan 로더를 열어야 하지만, 여기서는 힌트만 필요하다.
    // vulkan-1.dll이 있으면 드라이버가 Vulkan을 깔아 둔 것이다.
    HMODULE h = LoadLibraryW(L"vulkan-1.dll");
    if (!h) return false;
    FreeLibrary(h);
    return true;
}

// ---------- 다운로드 ----------

void LocalAi::InstallEngine(const std::string& variant, ProgressFn onProgress, DoneFn onDone) {
    const EngineVariant* ev = FindEngine(variant);
    if (!ev) {
        PostUi([onDone]() { onDone(false, "unknown engine"); });
        return;
    }
    if (busy_.exchange(true)) {
        PostUi([onDone]() { onDone(false, "busy"); });
        return;
    }
    abort_ = false;

    EngineVariant e = *ev;
    std::wstring dir = EngineDir(variant);
    std::wstring aiDir = AiDir();
    std::wstring dataDir = dataDir_;

    std::thread([this, e, dir, aiDir, dataDir, onProgress, onDone]() {
        auto finish = [&](bool ok, const std::string& err) {
            busy_ = false;
            PostUi([onDone, ok, err]() { onDone(ok, err); });
        };
        auto progress = [&](const std::string& stage, uint64_t recv, uint64_t total) {
            PostUi([onProgress, stage, recv, total]() { onProgress(stage, recv, total); });
        };

        util::EnsureDir(dataDir);
        util::EnsureDir(aiDir);

        std::wstring zip = aiDir + L"\\" + util::Utf8ToWide(e.asset);
        std::wstring url = util::Utf8ToWide(std::string(kEngineUrlPrefix) + kEngineBuild + "/" +
                                            e.asset);
        ULONGLONG last = 0;
        bool ok = util::HttpGetToFile(url, zip, nullptr,
                                      [&](uint64_t recv, uint64_t total) {
                                          ULONGLONG now = GetTickCount64();
                                          if (now - last >= 200 || recv == total) {
                                              last = now;
                                              progress("download", recv, total);
                                          }
                                      },
                                      &abort_);
        if (!ok) {
            DeleteFileW(zip.c_str());
            return finish(false, abort_.load() ? "aborted" : "download failed");
        }

        // 받은 파일이 우리가 검증한 그 빌드인지 확인한다 (공급망 사고 방지)
        progress("verify", 0, 0);
        std::string digest = util::Sha256File(zip);
        if (digest != e.sha256) {
            DeleteFileW(zip.c_str());
            return finish(false, "checksum mismatch");
        }

        progress("extract", 0, 0);
        util::EnsureDir(dir);
        bool unzipped = util::UnzipDir(zip, dir);
        DeleteFileW(zip.c_str());
        if (!unzipped || GetFileAttributesW((dir + L"\\llama-server.exe").c_str()) ==
                             INVALID_FILE_ATTRIBUTES) {
            return finish(false, "extract failed");
        }
        finish(true, "");
    }).detach();
}

void LocalAi::DownloadModel(const std::string& modelId, ProgressFn onProgress, DoneFn onDone) {
    const ModelInfo* mi = FindModel(modelId);
    if (!mi) {
        PostUi([onDone]() { onDone(false, "unknown model"); });
        return;
    }
    if (busy_.exchange(true)) {
        PostUi([onDone]() { onDone(false, "busy"); });
        return;
    }
    abort_ = false;

    ModelInfo m = *mi;
    std::wstring dest = ModelPath(m);
    std::wstring modelsDir = AiDir() + L"\\models";
    std::wstring aiDir = AiDir();
    std::wstring dataDir = dataDir_;

    std::thread([this, m, dest, modelsDir, aiDir, dataDir, onProgress, onDone]() {
        auto finish = [&](bool ok, const std::string& err) {
            busy_ = false;
            PostUi([onDone, ok, err]() { onDone(ok, err); });
        };
        auto progress = [&](const std::string& stage, uint64_t recv, uint64_t total) {
            PostUi([onProgress, stage, recv, total]() { onProgress(stage, recv, total); });
        };

        util::EnsureDir(dataDir);
        util::EnsureDir(aiDir);
        util::EnsureDir(modelsDir);

        // 받다 만 파일을 정본 자리에 두면 다음 실행에서 멀쩡한 모델로 오인한다 —
        // .part로 받고 검사를 통과했을 때만 옮긴다.
        std::wstring part = dest + L".part";
        std::string url = "https://huggingface.co/" + m.repo + "/resolve/main/" + m.file +
                          "?download=true";
        ULONGLONG last = 0;
        bool ok = util::HttpGetToFile(util::Utf8ToWide(url), part, nullptr,
                                      [&](uint64_t recv, uint64_t total) {
                                          ULONGLONG now = GetTickCount64();
                                          if (now - last >= 200 || recv == total) {
                                              last = now;
                                              progress("download", recv,
                                                       total ? total : m.sizeBytes);
                                          }
                                      },
                                      &abort_);
        if (!ok) {
            DeleteFileW(part.c_str());
            return finish(false, abort_.load() ? "aborted" : "download failed");
        }

        // GGUF 매직으로 확인한다. 오류 페이지(HTML)를 받았거나 잘린 경우를 여기서 거른다.
        // (모델 파일은 수 GB라 sha256 대신 매직 + 크기로 본다)
        progress("verify", 0, 0);
        bool valid = false;
        {
            HANDLE f = CreateFileW(part.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char magic[4]{};
                DWORD read = 0;
                LARGE_INTEGER size{};
                GetFileSizeEx(f, &size);
                if (ReadFile(f, magic, 4, &read, nullptr) && read == 4) {
                    valid = memcmp(magic, "GGUF", 4) == 0 &&
                            (m.sizeBytes == 0 ||
                             (uint64_t)size.QuadPart >= m.sizeBytes - (m.sizeBytes / 20));
                }
                CloseHandle(f);
            }
        }
        if (!valid) {
            DeleteFileW(part.c_str());
            return finish(false, "invalid model file");
        }

        DeleteFileW(dest.c_str());
        if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(part.c_str());
            return finish(false, "move failed");
        }
        finish(true, "");
    }).detach();
}

void LocalAi::CancelDownloads() { abort_ = true; }

bool LocalAi::DeleteModel(const std::string& modelId) {
    const ModelInfo* m = FindModel(modelId);
    if (!m) return false;
    // 쓰는 중인 모델이면 서버부터 내린다 (파일 핸들을 잡고 있다)
    if (RunningModel() == modelId) StopServer();
    return DeleteFileW(ModelPath(*m).c_str()) != 0;
}

// ---------- 서버 ----------

bool LocalAi::ServerRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!process_) return false;
    return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

std::string LocalAi::Endpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!process_ || port_ == 0) return "";
    return "http://127.0.0.1:" + std::to_string(port_);
}

std::string LocalAi::RunningModel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!process_ || WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) return "";
    return runningModel_;
}

void LocalAi::KillServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (job_) {
        CloseHandle(job_);  // KILL_ON_JOB_CLOSE — 자식이 함께 종료된다
        job_ = nullptr;
    }
    if (process_) {
        TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 3000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    port_ = 0;
    runningModel_.clear();
    runningVariant_.clear();
}

void LocalAi::StopServer() { KillServer(); }

bool LocalAi::StartServerBlocking(const ModelInfo& m, const std::string& variant, int ctxSize,
                                  std::string& error) {
    std::wstring exe = ServerExe(variant);
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "engine not installed";
        return false;
    }
    std::wstring model = ModelPath(m);
    if (GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "model not installed";
        return false;
    }

    int port = PickFreePort();
    if (port == 0) {
        error = "no free port";
        return false;
    }

    // --no-webui: 우리는 API만 쓴다. --jinja는 기본값이라 모델의 채팅 템플릿이 그대로 쓰인다.
    // -ngl 999: vulkan 변형일 때만 GPU에 최대한 올린다 (cpu 빌드는 무시된다).
    std::wstring cmd = Quote(exe) + L" --model " + Quote(model) + L" --host 127.0.0.1 --port " +
                       std::to_wstring(port) + L" --ctx-size " + std::to_wstring(ctxSize) +
                       L" --no-webui --no-warmup --alias supersticker" + L" --log-file " +
                       Quote(AiDir() + L"\\server.log");
    if (variant == "vulkan") cmd += L" --n-gpu-layers 999";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, EngineDir(variant).c_str(),
                        &si, &pi)) {
        if (job) CloseHandle(job);
        error = "launch failed";
        return false;
    }
    // 잡에 넣은 뒤 실행 — 앱이 강제 종료돼도 서버가 남지 않는다
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = pi.hProcess;
        job_ = job;
        port_ = port;
        runningModel_ = m.id;
        runningVariant_ = variant;
    }

    // 준비될 때까지 /health를 두드린다. 로딩 중에는 503, 다 되면 200이다.
    // 큰 모델은 첫 로딩이 오래 걸려 넉넉히 기다린다.
    std::wstring health = L"http://127.0.0.1:" + std::to_wstring(port) + L"/health";
    for (int i = 0; i < 180; ++i) {
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_TIMEOUT) {
            error = "server exited";  // 모델 로딩 실패 등 — server.log에 이유가 남는다
            KillServer();
            return false;
        }
        std::string body;
        if (util::HttpGetToFile(health, L"", &body)) return true;
        Sleep(500);
    }
    error = "server timeout";
    KillServer();
    return false;
}

void LocalAi::EnsureServer(const std::string& modelId, const std::string& variant, int ctxSize,
                           std::function<void(bool, const std::string&, const std::string&)> done) {
    const ModelInfo* mi = FindModel(modelId);
    if (!mi) {
        PostUi([done]() { done(false, "", "unknown model"); });
        return;
    }
    // 이미 같은 모델·변형으로 떠 있으면 그대로 쓴다
    if (RunningModel() == modelId) {
        std::string ep = Endpoint();
        std::lock_guard<std::mutex> lock(mutex_);
        if (runningVariant_ == variant && !ep.empty()) {
            PostUi([done, ep]() { done(true, ep, ""); });
            return;
        }
    }
    if (starting_.exchange(true)) {
        PostUi([done]() { done(false, "", "starting"); });
        return;
    }

    ModelInfo m = *mi;
    std::thread([this, m, variant, ctxSize, done]() {
        KillServer();  // 다른 모델이 떠 있으면 내린다 (모델 하나당 수 GB라 동시에 못 띄운다)
        std::string error;
        bool ok = StartServerBlocking(m, variant, ctxSize, error);
        std::string ep = ok ? Endpoint() : "";
        starting_ = false;
        PostUi([done, ok, ep, error]() { done(ok, ep, error); });
    }).detach();
}
