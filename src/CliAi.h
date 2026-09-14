#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <json.hpp>

// Claude Code·Codex CLI를 헤드리스 모드로 실행하는 AI 백엔드.
//
// HTTP 서버가 아니라 이 PC에 설치된 CLI를 요청마다 자식 프로세스로 띄운다. 로그인·구독·
// API 키는 CLI가 이미 가진 것을 그대로 쓴다 — 앱은 자격 증명을 만지지 않는다.
//  - Claude : claude -p --output-format stream-json (토큰 단위 스트리밍)
//  - Codex  : codex exec --json (메시지 단위 — 답이 한 번에 온다)
// 프롬프트는 표준 입력으로 넘긴다. 명령줄 길이 제한(32K)과 따옴표 해석을 피하려는 것이다.
// 워커 스레드에서 돌리고, 콜백은 uiPoster로 UI 스레드에서 부른다 (AiClient와 같은 규약).
class CliAi {
public:
    enum class Kind { Claude, Codex };

    // "claude" / "codex" → Kind. 둘 다 아니면 false
    static bool ParseKind(const std::string& s, Kind& out);

    using UiPoster = std::function<void(std::function<void()>)>;
    void SetUiPoster(UiPoster p) { uiPoster_ = std::move(p); }

    // 실행 파일 찾기: 사용자가 적은 경로 > PATH(레지스트리의 최신 값 포함) > 알려진 설치 위치.
    // 못 찾으면 빈 문자열. Codex는 npm 래퍼(.cmd) 대신 그 안의 네이티브 exe를 고른다.
    static std::wstring Resolve(Kind kind, const std::string& customPath);

    // 모델 이름으로 받을 수 있는 글자인지 (명령줄 인자로 그대로 들어간다)
    static bool ValidModel(const std::string& model);

    // 설치·로그인 확인 결과
    struct Status {
        bool found = false;
        std::string path;
        std::string version;
        bool authKnown = false;  // 로그인 여부를 알아냈는지
        bool loggedIn = false;
        std::string account;     // 로그인 방식·구독 등 짧은 설명
        std::string error;       // "cli-missing" 등
    };
    // --version과 로그인 상태 명령을 돌려 본다 (토큰을 쓰지 않는다)
    void Detect(Kind kind, const std::string& customPath, std::function<void(Status)> done);

    struct ChatOptions {
        std::string model;         // 빈 값이면 CLI의 기본 모델
        bool jsonFormat = false;   // 응답을 JSON 하나로 (AI Review)
        nlohmann::json jsonSchema; // 있으면 CLI의 구조화 출력으로 강제한다
    };
    // 채팅. 받는 대로 onChunk, 끝나면 onDone. 오류 문자열은 페이지가 알아보도록
    // "cli-missing" / "cli-auth: …" / "cli-timeout" / "cli: …" / "aborted" 형태로 준다.
    void Chat(const std::string& requestId, Kind kind, const std::string& customPath,
              const nlohmann::json& messages, const ChatOptions& opts,
              std::function<void(std::string delta)> onChunk,
              std::function<void(bool ok, std::string error)> onDone);

    void Abort(const std::string& requestId);
    void AbortAll();  // 앱 종료 시 — 떠 있는 CLI 프로세스를 모두 끝낸다

    // 실행 중인 요청 하나. 중단은 잡 오브젝트를 통째로 끝내 손자 프로세스(node 등)까지 거둔다.
    struct Run {
        std::mutex mutex;
        HANDLE job = nullptr;  // 프로세스가 살아 있는 동안만 유효 (mutex로 보호)
        std::atomic<bool> aborted{false};
        void Kill();
    };

private:
    // 워커와 나눠 갖는 상태 — 워커는 this를 만지지 않는다 (AiClient::Shared와 같은 이유)
    struct Shared {
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<Run>> active;
    };

    void PostUi(std::function<void()> fn) {
        if (uiPoster_) uiPoster_(std::move(fn));
    }

    UiPoster uiPoster_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};
