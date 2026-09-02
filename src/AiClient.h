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
        // JSON 스키마(선택). 있으면 문법 제약으로 넘긴다 — llama-server는 json_object만으로는
        // 출력을 묶지 않아(실측) 스키마가 있어야 실제로 강제된다. Ollama는 format에 그대로 준다.
        nlohmann::json jsonSchema;
        bool disableThinking = false; // Qwen3 계열: 끄지 않으면 reasoning만 내고 본문이 빈다
    };

    using UiPoster = std::function<void(std::function<void()>)>;

    void SetUiPoster(UiPoster p) { uiPoster_ = std::move(p); }

    // 모델 이름 목록 (연결 테스트 겸용). 프로토콜에 따라 경로와 응답 형태가 다르다:
    //   Ollama     : GET /api/tags   → models[].name
    //   OpenAI 호환: GET /v1/models  → data[].id   (LM Studio·llama-server)
    void ListModels(const std::string& endpoint, Protocol protocol,
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
    // 진행 중인 모든 요청 중단 (앱 종료 시). 워커는 다음 청크 경계에서 빠져나온다.
    void AbortAll();

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

    // 워커 스레드와 나눠 갖는 상태. **워커는 this를 만지지 않는다** — detached 스레드가
    // 앱 종료(정적 소멸) 뒤까지 살아남아도 파괴된 멤버를 건드리지 않도록 shared_ptr로 소유한다.
    struct Shared {
        std::mutex mutex;
        // requestId → 중단 플래그. 워커가 청크 사이마다 확인한다.
        // (WinHTTP 동기 모드에서 타 스레드의 핸들 닫기는 진행 중인 호출을 취소하지 못함)
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> active;
    };
    std::shared_ptr<std::atomic<bool>> Track(const std::string& requestId);

    UiPoster uiPoster_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};
