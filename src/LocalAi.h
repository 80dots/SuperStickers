#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 내장 AI 백엔드: llama.cpp의 llama-server를 자식 프로세스로 띄우고 로컬 HTTP로 쓴다.
// Ollama 없이 동작하며, 엔진과 모델은 필요할 때 내려받아 %APPDATA%\SuperSticker\ai 아래 둔다.
//
// 왜 자식 프로세스인가: 추론 라이브러리를 앱에 정적 링크하면 GPU 백엔드까지 우리 빌드로
// 떠안게 된다. 서버를 띄우면 공식 배포본을 그대로 쓰고, 모델 로딩 중 크래시도 앱과 분리된다.
// 스트리밍 응답 형식은 OpenAI 호환(/v1/chat/completions, SSE)이라 AiClient가 그대로 받는다.
class LocalAi {
public:
    // 카탈로그 항목. 전부 단일 파일 Q4_K_M GGUF이고, 라이선스는 UI에 그대로 노출한다.
    struct ModelInfo {
        std::string id;       // 설정에 저장되는 식별자
        std::string name;     // 표시 이름
        std::string params;   // "1.7B"
        std::string repo;     // Hugging Face 저장소
        std::string file;     // 저장소 안의 파일명 (그대로 로컬 파일명이 된다)
        std::string license;  // 표시용 라이선스 이름
        uint64_t sizeBytes = 0;
        bool disableThinking = false;  // Qwen3 계열: 켜 두면 reasoning만 내고 본문이 빈다
        bool recommended = false;
    };

    // 엔진 변형. cpu는 어디서나 돌고, vulkan은 NVIDIA·AMD·Intel 내장까지 한 빌드로 덮는다.
    struct EngineVariant {
        std::string id;   // "cpu" | "vulkan"
        std::string asset;
        std::string sha256;
        uint64_t sizeBytes = 0;
    };

    using UiPoster = std::function<void(std::function<void()>)>;
    using ProgressFn = std::function<void(const std::string& stage, uint64_t received,
                                          uint64_t total)>;
    using DoneFn = std::function<void(bool ok, const std::string& error)>;

    static const std::vector<ModelInfo>& Catalog();
    static const ModelInfo* FindModel(const std::string& id);
    static const std::vector<EngineVariant>& Engines();
    static const EngineVariant* FindEngine(const std::string& id);

    // dataDir = %APPDATA%\SuperSticker (저장 경로를 옮기면 함께 따라간다)
    void Init(UiPoster poster, const std::wstring& dataDir);
    void SetDataDir(const std::wstring& dataDir);

    std::wstring AiDir() const;
    std::wstring EngineDir(const std::string& variant) const;
    std::wstring ServerExe(const std::string& variant) const;
    std::wstring ModelPath(const ModelInfo& m) const;

    bool EngineInstalled(const std::string& variant) const;
    bool ModelInstalled(const ModelInfo& m) const;
    // 설치된 모델 id 목록 (파일이 실제로 있는 것만)
    std::vector<std::string> InstalledModels() const;
    // GPU가 있어 vulkan 빌드를 권할 만한지 (없으면 cpu를 권한다)
    static bool HasVulkanCapableGpu();

    // ---- 다운로드 (워커 스레드에서 돌고 콜백은 UI 스레드로 마샬링된다) ----
    void InstallEngine(const std::string& variant, ProgressFn onProgress, DoneFn onDone);
    void DownloadModel(const std::string& modelId, ProgressFn onProgress, DoneFn onDone);
    void CancelDownloads();
    bool Busy() const { return busy_.load(); }
    bool DeleteModel(const std::string& modelId);

    // ---- 서버 ----
    // 요청한 모델로 서버가 떠 있게 만든다. 이미 같은 모델이면 즉시 성공.
    // 모델이 다르면 기존 서버를 내리고 다시 띄운다.
    void EnsureServer(const std::string& modelId, const std::string& variant, int ctxSize,
                      std::function<void(bool ok, const std::string& endpoint,
                                         const std::string& error)> done);
    void StopServer();
    bool ServerRunning() const;
    std::string Endpoint() const;      // "http://127.0.0.1:<port>"
    std::string RunningModel() const;  // 떠 있는 모델 id (없으면 빈 문자열)

private:
    void PostUi(std::function<void()> fn) const {
        if (uiPoster_) uiPoster_(std::move(fn));
    }
    bool StartServerBlocking(const ModelInfo& m, const std::string& variant, int ctxSize,
                             std::string& error);
    void KillServer();

    UiPoster uiPoster_;
    std::wstring dataDir_;

    mutable std::mutex mutex_;
    std::atomic<bool> busy_{false};     // 엔진/모델 다운로드 진행 중
    std::atomic<bool> abort_{false};    // 다운로드 중단 요청
    std::atomic<bool> starting_{false}; // 서버 기동 중 (중복 기동 방지)

    // 자식 프로세스. 잡 오브젝트에 넣어 앱이 죽어도 남지 않게 한다.
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    int port_ = 0;
    std::string runningModel_;
    std::string runningVariant_;
};
