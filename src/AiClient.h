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

// 로컬 AI HTTP 클라이언트. 워커 스레드에서 WinHTTP 동기 API로 통신하고,
// 콜백은 uiPoster를 통해 UI 스레드로 마샬링되어 호출된다.
//
// 두 백엔드를 같은 코드로 다룬다:
//  - Ollama       : POST /api/chat, NDJSON 한 줄에 message.content 델타
//  - 내장(llama-server): POST /v1/chat/completions, SSE `data: {...}` 프레임에
//                   choices[0].delta.content 델타 (마지막은 `data: [DONE]`)
class AiClient {
public:
    // 채팅 프로토콜. 스트리밍 파싱 방식이 갈리는 유일한 지점이다.
    enum class Protocol { OllamaNdjson, OpenAiSse };

    struct ChatOptions {
        Protocol protocol = Protocol::OllamaNdjson;
        bool jsonFormat = false;      // 응답을 유효한 JSON으로 강제 (AI Review)
        bool disableThinking = false; // Qwen3 계열: 끄지 않으면 reasoning만 내고 본문이 빈다
    };

    using UiPoster = std::function<void(std::function<void()>)>;

    void SetUiPoster(UiPoster p) { uiPoster_ = std::move(p); }

    // GET /api/tags → 모델 이름 목록 (Ollama 전용. 연결 테스트 겸용)
    void ListModels(const std::string& endpoint,
                    std::function<void(bool ok, std::vector<std::string> models,
                                       std::string error)> done);

    // 스트리밍 채팅. 델타마다 onChunk, 종료 시 onDone. 경로와 파싱은 opts.protocol이 정한다.
    void Chat(const std::string& requestId, const std::string& endpoint, const std::string& model,
              const nlohmann::json& messages, const ChatOptions& opts,
              std::function<void(std::string delta)> onChunk,
              std::function<void(bool ok, std::string error)> onDone);

    // POST /api/pull (stream=true). Ollama 전용. 진행 라인마다 onProgress, 종료 시 onDone.
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
