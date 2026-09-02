#include "AiClient.h"

#include <thread>
#include <vector>

#include "Utils.h"

using json = nlohmann::json;

AiClient::Url AiClient::ParseEndpoint(const std::string& endpoint) {
    Url u;
    std::wstring w = util::Utf8ToWide(endpoint);
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof(c);
    wchar_t host[256]{};
    c.lpszHostName = host;
    c.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &c)) return u;
    u.host = host;
    u.port = c.nPort;
    u.https = (c.nScheme == INTERNET_SCHEME_HTTPS);
    u.valid = !u.host.empty();
    return u;
}

namespace {

struct Session {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    ~Session() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

// 요청 핸들 열기까지 공통 처리. 실패 시 request == nullptr.
void OpenRequest(Session& s, const AiClient::Url& u, const wchar_t* verb,
                 const wchar_t* path) {
    s.session = WinHttpOpen(L"SuperStickers/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s.session) return;
    // 로컬 LLM 응답은 느릴 수 있으므로 수신 타임아웃을 넉넉히 (10분)
    WinHttpSetTimeouts(s.session, 10000, 10000, 600000, 600000);
    s.connect = WinHttpConnect(s.session, u.host.c_str(), u.port, 0);
    if (!s.connect) return;
    s.request = WinHttpOpenRequest(s.connect, verb, path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   u.https ? WINHTTP_FLAG_SECURE : 0);
}

std::string ReadAll(HINTERNET request) {
    std::string body;
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0) break;
        body.append(buf.data(), read);
    } while (avail > 0);
    return body;
}

DWORD StatusCode(HINTERNET request) {
    DWORD code = 0, size = sizeof(code);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
    return code;
}

// 서버가 보낸 error 필드에서 사람이 읽을 메시지를 뽑는다 (문자열 또는 {message})
std::string ErrorMessage(const json& e) {
    if (e.is_string()) return e.get<std::string>();
    if (e.is_object() && e.contains("message") && e["message"].is_string())
        return e["message"].get<std::string>();
    return "error";
}

// POST 요청을 열고 본문을 보내 응답 헤더까지 받는다. 실패 원인은 err.
bool PostJson(Session& s, const AiClient::Url& u, const wchar_t* path, const std::string& body,
              std::string& err) {
    OpenRequest(s, u, L"POST", path);
    if (!s.request) {
        err = "connection failed";
        return false;
    }
    bool sent = WinHttpSendRequest(s.request, L"Content-Type: application/json\r\n", (DWORD)-1,
                                   (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(),
                                   0) &&
                WinHttpReceiveResponse(s.request, nullptr);
    if (!sent) {
        err = "connection failed";
        return false;
    }
    if (StatusCode(s.request) != 200) {
        err = "http " + std::to_string(StatusCode(s.request));
        return false;
    }
    return true;
}

// 응답 본문을 라인 단위로 onLine에 넘긴다 (NDJSON·SSE 공용). onLine이 false를 돌려주면 멈춘다.
// 청크 사이마다 중단 플래그를 본다. 읽기 오류면 false(err 설정).
// ReadData만 쓰면 버퍼가 찰 때까지 블록되어 스트리밍이 안 됨 —
// QueryDataAvailable로 도착한 만큼만 즉시 읽는다.
bool StreamLines(HINTERNET request, const std::atomic<bool>& aborted,
                 const std::function<bool(const std::string&)>& onLine, std::string& err) {
    std::string pending;
    while (!aborted.load()) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            err = "read error";
            return false;
        }
        if (avail == 0) return true;  // 스트림 종료
        char buf[8192];
        DWORD toRead = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf, toRead, &read)) {
            err = "read error";
            return false;
        }
        if (read == 0) return true;
        pending.append(buf, read);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();  // SSE는 CRLF
            if (line.empty()) continue;
            if (!onLine(line)) return true;
        }
    }
    return true;
}

}  // namespace

void AiClient::ListModels(
    const std::string& endpoint, Protocol protocol,
    std::function<void(bool, std::vector<std::string>, std::string)> done) {
    Url u = ParseEndpoint(endpoint);
    const bool openai = protocol == Protocol::OpenAiSse;
    std::thread([this, u, done, openai]() {
        auto fail = [&](const std::string& err) {
            PostUi([done, err]() { done(false, {}, err); });
        };
        if (!u.valid) return fail("invalid endpoint");

        Session s;
        OpenRequest(s, u, L"GET", openai ? L"/v1/models" : L"/api/tags");
        if (!s.request || !WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(s.request, nullptr)) {
            return fail("connection failed");
        }
        if (StatusCode(s.request) != 200) return fail("http " + std::to_string(StatusCode(s.request)));

        json j = json::parse(ReadAll(s.request), nullptr, false);
        std::vector<std::string> models;
        if (j.is_discarded()) return fail("unexpected response");
        if (openai) {
            if (!j.contains("data") || !j["data"].is_array()) return fail("unexpected response");
            for (auto& m : j["data"]) {
                if (m.is_object() && m.contains("id") && m["id"].is_string())
                    models.push_back(m["id"].get<std::string>());
            }
        } else {
            if (!j.contains("models")) return fail("unexpected response");
            if (!j["models"].is_array()) return fail("unexpected response");
            for (auto& m : j["models"]) {
                if (m.is_object() && m.contains("name") && m["name"].is_string())
                    models.push_back(m["name"].get<std::string>());
            }
        }
        PostUi([done, models]() { done(true, models, ""); });
    }).detach();
}

std::shared_ptr<std::atomic<bool>> AiClient::Track(const std::string& requestId) {
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->active[requestId] = aborted;
    return aborted;
}

void AiClient::Chat(const std::string& requestId, const std::string& endpoint,
                    const std::string& model, const json& messages, const ChatOptions& opts,
                    std::function<void(std::string)> onChunk,
                    std::function<void(bool, std::string)> onDone) {
    Url u = ParseEndpoint(endpoint);
    auto aborted = Track(requestId);
    auto shared = shared_;
    UiPoster post = uiPoster_;
    std::thread([shared, post, requestId, u, model, messages, onChunk, onDone, aborted, opts]() {
        auto finish = [&](bool ok, const std::string& err) {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->active.erase(requestId);
            }
            if (post) post([onDone, ok, err]() { onDone(ok, err); });
        };
        // 워커에서 새는 예외는 std::terminate — 서버가 예상 밖의 JSON을 보내도 앱이 죽어선 안 된다
        try {
            if (!u.valid) return finish(false, "invalid endpoint");

            const bool sse = opts.protocol == Protocol::OpenAiSse;
            json bodyJson = {{"model", model}, {"messages", messages}, {"stream", true}};
            const bool hasSchema = opts.jsonSchema.is_object() && !opts.jsonSchema.empty();
            if (sse) {
                // llama-server·LM Studio는 OpenAI 규격을 따른다. json_object는 llama-server가
                // 문법으로 강제하지 않으므로(실측) 스키마가 있으면 json_schema로 보낸다.
                if (hasSchema) {
                    bodyJson["response_format"] = {
                        {"type", "json_schema"},
                        {"json_schema", {{"name", "response"}, {"schema", opts.jsonSchema}}}};
                } else if (opts.jsonFormat) {
                    bodyJson["response_format"] = {{"type", "json_object"}};
                }
                // Qwen3 같은 하이브리드 추론 모델은 이걸 끄지 않으면 reasoning_content만 내보내고
                // content가 비어서 결과가 통째로 사라진다 (실측). 템플릿이 모르는 키는 무시된다.
                if (opts.disableThinking) {
                    bodyJson["chat_template_kwargs"] = {{"enable_thinking", false}};
                }
            } else if (hasSchema) {
                bodyJson["format"] = opts.jsonSchema;  // Ollama 구조화 출력: 스키마를 그대로 받는다
            } else if (opts.jsonFormat) {
                bodyJson["format"] = "json";
            }

            Session s;
            std::string err;
            if (!PostJson(s, u, sse ? L"/v1/chat/completions" : L"/api/chat", bodyJson.dump(),
                          err)) {
                return finish(false, err);
            }

            bool doneFlag = false;
            std::string errMsg;
            bool readOk = StreamLines(
                s.request, *aborted,
                [&](const std::string& line) {
                    std::string payload = line;
                    if (sse) {
                        if (line.rfind("data:", 0) != 0) return true;  // 주석·이벤트 줄은 버린다
                        payload = line.substr(5);
                        if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                        if (payload == "[DONE]") {
                            doneFlag = true;
                            return false;
                        }
                    }
                    json j = json::parse(payload, nullptr, false);
                    if (!j.is_object()) return true;  // 깨진 줄·예상 밖 형태는 건너뛴다
                    if (j.contains("error")) {
                        errMsg = ErrorMessage(j["error"]);
                        doneFlag = true;
                        return false;
                    }
                    std::string delta;
                    if (sse) {
                        // reasoning_content(추론 과정)는 본문이 아니므로 버린다
                        const json* ch = (j.contains("choices") && j["choices"].is_array() &&
                                          !j["choices"].empty())
                                             ? &j["choices"][0]
                                             : nullptr;
                        if (ch && ch->is_object() && ch->contains("delta") &&
                            (*ch)["delta"].is_object()) {
                            const json& d = (*ch)["delta"];
                            if (d.contains("content") && d["content"].is_string())
                                delta = d["content"].get<std::string>();
                        }
                    } else {
                        if (j.contains("message") && j["message"].is_object()) {
                            const json& m = j["message"];
                            if (m.contains("content") && m["content"].is_string())
                                delta = m["content"].get<std::string>();
                        }
                        if (j.contains("done") && j["done"].is_boolean() && j["done"].get<bool>())
                            doneFlag = true;
                    }
                    if (!delta.empty() && !aborted->load() && post) {
                        post([onChunk, delta]() { onChunk(delta); });
                    }
                    return true;
                },
                err);
            if (aborted->load()) return finish(false, "aborted");
            if (!readOk) return finish(false, err);
            if (!errMsg.empty()) return finish(false, errMsg);
            // 종료 표식([DONE] / done:true) 없이 연결이 끊기면 응답이 잘린 것이다
            finish(doneFlag, doneFlag ? "" : "incomplete response");
        } catch (const std::exception& e) {
            finish(false, e.what());
        }
    }).detach();
}

void AiClient::Pull(const std::string& requestId, const std::string& endpoint,
                    const std::string& model,
                    std::function<void(std::string, uint64_t, uint64_t)> onProgress,
                    std::function<void(bool, std::string)> onDone) {
    Url u = ParseEndpoint(endpoint);
    auto aborted = Track(requestId);
    auto shared = shared_;
    UiPoster post = uiPoster_;
    std::thread([shared, post, requestId, u, model, onProgress, onDone, aborted]() {
        auto finish = [&](bool ok, const std::string& err) {
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->active.erase(requestId);
            }
            if (post) post([onDone, ok, err]() { onDone(ok, err); });
        };
        try {
            if (!u.valid) return finish(false, "invalid endpoint");

            Session s;
            // 신버전은 "model", 구버전은 "name" — 둘 다 전달해 호환
            std::string body = json{{"model", model}, {"name", model}, {"stream", true}}.dump();
            std::string err;
            // 대형 모델 다운로드는 오래 걸림 — 세션 타임아웃(10분)보다 길 수 있어 요청 핸들에
            // 무제한을 건다. OpenRequest 뒤·SendRequest 전에 걸어야 하므로 PostJson을 쓰지 않는다.
            OpenRequest(s, u, L"POST", L"/api/pull");
            if (!s.request) return finish(false, "connection failed");
            WinHttpSetTimeouts(s.request, 10000, 10000, 0, 0);
            bool sent = WinHttpSendRequest(s.request, L"Content-Type: application/json\r\n",
                                           (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(),
                                           (DWORD)body.size(), 0) &&
                        WinHttpReceiveResponse(s.request, nullptr);
            if (!sent) return finish(false, "connection failed");
            if (StatusCode(s.request) != 200) {
                return finish(false, "http " + std::to_string(StatusCode(s.request)));
            }

            bool success = false;
            std::string errMsg;
            std::string lastStatus;
            ULONGLONG lastPost = 0;  // 진행 이벤트 쓰로틀 (다운로드 중 라인이 매우 잦음)
            bool readOk = StreamLines(
                s.request, *aborted,
                [&](const std::string& line) {
                    json j = json::parse(line, nullptr, false);
                    if (!j.is_object()) return true;
                    if (j.contains("error")) {
                        errMsg = ErrorMessage(j["error"]);
                        return false;
                    }
                    std::string status =
                        j.contains("status") && j["status"].is_string() ? j["status"] : "";
                    auto num = [&](const char* key) -> uint64_t {
                        return j.contains(key) && j[key].is_number_unsigned()
                                   ? j[key].get<uint64_t>()
                                   : 0;
                    };
                    uint64_t total = num("total"), completed = num("completed");
                    if (status == "success") success = true;
                    ULONGLONG now = GetTickCount64();
                    bool important = success || status != lastStatus;
                    if (!aborted->load() && post && (important || now - lastPost >= 250)) {
                        lastStatus = status;
                        lastPost = now;
                        post([onProgress, status, total, completed]() {
                            onProgress(status, total, completed);
                        });
                    }
                    return !success;
                },
                err);
            if (aborted->load()) return finish(false, "aborted");
            if (!readOk) return finish(false, err);
            finish(errMsg.empty() && success, errMsg);
        } catch (const std::exception& e) {
            finish(false, e.what());
        }
    }).detach();
}

void AiClient::Abort(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    auto it = shared_->active.find(requestId);
    if (it != shared_->active.end()) it->second->store(true);
}

void AiClient::AbortAll() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (auto& kv : shared_->active) kv.second->store(true);
}
