#pragma once
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

// Ollama HTTP 클라이언트. 워커 스레드에서 WinHTTP 동기 API로 통신하고,
// 콜백은 uiPoster를 통해 UI 스레드로 마샬링되어 호출된다.
class OllamaClient {
public:
    using UiPoster = std::function<void(std::function<void()>)>;

    void SetUiPoster(UiPoster p) { uiPoster_ = std::move(p); }

    // GET /api/tags → 모델 이름 목록 (연결 테스트 겸용)
    void ListModels(const std::string& endpoint,
                    std::function<void(bool ok, std::vector<std::string> models,
                                       std::string error)> done);

    // POST /api/chat (stream=true). 델타마다 onChunk, 종료 시 onDone.
    // jsonFormat=true면 Ollama가 유효한 JSON만 생성하도록 강제 (format:"json").
    void Chat(const std::string& requestId, const std::string& endpoint, const std::string& model,
              const nlohmann::json& messages, std::function<void(std::string delta)> onChunk,
              std::function<void(bool ok, std::string error)> onDone, bool jsonFormat = false);

    // POST /api/pull (stream=true). 진행 라인마다 onProgress, 종료 시 onDone.
    // total/completed는 현재 레이어의 바이트 수 (없으면 0).
    void Pull(const std::string& requestId, const std::string& endpoint, const std::string& model,
              std::function<void(std::string status, uint64_t total, uint64_t completed)> onProgress,
              std::function<void(bool ok, std::string error)> onDone);

    void Abort(const std::string& requestId);

    struct Url {
        std::wstring host;
        INTERNET_PORT port = 11434;
        bool https = false;
        bool valid = false;
    };

private:
    static Url ParseEndpoint(const std::string& endpoint);

    void PostUi(std::function<void()> fn) {
        if (uiPoster_) uiPoster_(std::move(fn));
    }

    UiPoster uiPoster_;
    std::mutex mutex_;
    // requestId → 중단 플래그. 워커가 청크 사이마다 확인한다.
    // (WinHTTP 동기 모드에서 타 스레드의 핸들 닫기는 진행 중인 호출을 취소하지 못함)
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> activeRequests_;
};
