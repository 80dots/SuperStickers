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

}  // namespace

void AiClient::ListModels(
    const std::string& endpoint,
    std::function<void(bool, std::vector<std::string>, std::string)> done) {
    Url u = ParseEndpoint(endpoint);
    std::thread([this, u, done]() {
        auto fail = [&](const std::string& err) {
            PostUi([done, err]() { done(false, {}, err); });
        };
        if (!u.valid) return fail("invalid endpoint");

        Session s;
        OpenRequest(s, u, L"GET", L"/api/tags");
        if (!s.request || !WinHttpSendRequest(s.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(s.request, nullptr)) {
            return fail("connection failed");
        }
        if (StatusCode(s.request) != 200) return fail("http " + std::to_string(StatusCode(s.request)));

        json j = json::parse(ReadAll(s.request), nullptr, false);
        if (j.is_discarded() || !j.contains("models")) return fail("unexpected response");
        std::vector<std::string> models;
        for (auto& m : j["models"]) {
            if (m.contains("name")) models.push_back(m["name"].get<std::string>());
        }
        PostUi([done, models]() { done(true, models, ""); });
    }).detach();
}

void AiClient::Chat(const std::string& requestId, const std::string& endpoint,
                    const std::string& model, const json& messages, const ChatOptions& opts,
                    std::function<void(std::string)> onChunk,
                    std::function<void(bool, std::string)> onDone) {
    Url u = ParseEndpoint(endpoint);
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeRequests_[requestId] = aborted;
    }
    std::thread([this, requestId, u, model, messages, onChunk, onDone, aborted, opts]() {
        auto finish = [&](bool ok, const std::string& err) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeRequests_.erase(requestId);
            }
            PostUi([onDone, ok, err]() { onDone(ok, err); });
        };
        if (!u.valid) return finish(false, "invalid endpoint");

        const bool sse = opts.protocol == Protocol::OpenAiSse;
        Session s;
        OpenRequest(s, u, L"POST", sse ? L"/v1/chat/completions" : L"/api/chat");
        if (!s.request) return finish(false, "connection failed");

        json bodyJson = {{"model", model}, {"messages", messages}, {"stream", true}};
        if (sse) {
            // llama-server는 OpenAI 규격을 따른다
            if (opts.jsonFormat) bodyJson["response_format"] = {{"type", "json_object"}};
            // Qwen3 같은 하이브리드 추론 모델은 이걸 끄지 않으면 reasoning_content만 내보내고
            // content가 비어서 결과가 통째로 사라진다 (실측). 템플릿이 모르는 키는 무시된다.
            if (opts.disableThinking) {
                bodyJson["chat_template_kwargs"] = {{"enable_thinking", false}};
            }
        } else if (opts.jsonFormat) {
            bodyJson["format"] = "json";
        }
        std::string body = bodyJson.dump();
        bool sent = WinHttpSendRequest(s.request, L"Content-Type: application/json\r\n", (DWORD)-1,
                                       (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(),
                                       0) &&
                    WinHttpReceiveResponse(s.request, nullptr);
        if (!sent) return finish(false, "connection failed");
        if (StatusCode(s.request) != 200) {
            return finish(false, "http " + std::to_string(StatusCode(s.request)));
        }

        // 두 프로토콜 모두 라인 단위다 (NDJSON / SSE). 청크 사이마다 중단 플래그를 본다.
        std::string pending;
        bool doneFlag = false;
        std::string errMsg;
        while (!aborted->load()) {
            // ReadData만 쓰면 버퍼가 찰 때까지 블록되어 스트리밍이 안 됨 —
            // QueryDataAvailable로 도착한 만큼만 즉시 읽는다.
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(s.request, &avail)) {
                errMsg = "read error";
                break;
            }
            if (avail == 0) break;  // 스트림 종료
            char buf[8192];
            DWORD toRead = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD read = 0;
            if (!WinHttpReadData(s.request, buf, toRead, &read)) {
                errMsg = "read error";
                break;
            }
            if (read == 0) break;
            pending.append(buf, read);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();  // SSE는 CRLF
                if (line.empty()) continue;

                std::string payload = line;
                if (sse) {
                    if (line.rfind("data:", 0) != 0) continue;  // 주석·이벤트 줄은 버린다
                    payload = line.substr(5);
                    if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                    if (payload == "[DONE]") {
                        doneFlag = true;
                        break;
                    }
                }

                json j = json::parse(payload, nullptr, false);
                if (j.is_discarded()) continue;
                if (j.contains("error")) {
                    const json& e = j["error"];
                    if (e.is_string()) errMsg = e.get<std::string>();
                    else if (e.is_object() && e.contains("message") && e["message"].is_string())
                        errMsg = e["message"].get<std::string>();
                    else errMsg = "error";
                    doneFlag = true;
                    break;
                }

                std::string delta;
                if (sse) {
                    // reasoning_content(추론 과정)는 본문이 아니므로 버린다
                    if (j.contains("choices") && j["choices"].is_array() &&
                        !j["choices"].empty()) {
                        const json& ch = j["choices"][0];
                        if (ch.contains("delta") && ch["delta"].contains("content") &&
                            ch["delta"]["content"].is_string()) {
                            delta = ch["delta"]["content"].get<std::string>();
                        }
                    }
                } else {
                    if (j.contains("message") && j["message"].contains("content")) {
                        delta = j["message"]["content"].get<std::string>();
                    }
                    if (j.value("done", false)) doneFlag = true;
                }
                if (!delta.empty() && !aborted->load()) {
                    PostUi([onChunk, delta]() { onChunk(delta); });
                }
            }
            if (doneFlag) break;
        }
        if (aborted->load()) return finish(false, "aborted");
        finish(errMsg.empty() && doneFlag, errMsg.empty() ? "" : errMsg);
    }).detach();
}

void AiClient::Pull(const std::string& requestId, const std::string& endpoint,
                        const std::string& model,
                        std::function<void(std::string, uint64_t, uint64_t)> onProgress,
                        std::function<void(bool, std::string)> onDone) {
    Url u = ParseEndpoint(endpoint);
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeRequests_[requestId] = aborted;
    }
    std::thread([this, requestId, u, model, onProgress, onDone, aborted]() {
        auto finish = [&](bool ok, const std::string& err) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeRequests_.erase(requestId);
            }
            PostUi([onDone, ok, err]() { onDone(ok, err); });
        };
        if (!u.valid) return finish(false, "invalid endpoint");

        Session s;
        OpenRequest(s, u, L"POST", L"/api/pull");
        if (!s.request) return finish(false, "connection failed");
        // 대형 모델 다운로드는 오래 걸림 — 수신 타임아웃 무제한 (request 핸들에 직접 적용)
        WinHttpSetTimeouts(s.request, 10000, 10000, 0, 0);

        // 신버전은 "model", 구버전은 "name" — 둘 다 전달해 호환
        std::string body =
            json{{"model", model}, {"name", model}, {"stream", true}}.dump();
        bool sent = WinHttpSendRequest(s.request, L"Content-Type: application/json\r\n", (DWORD)-1,
                                       (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(),
                                       0) &&
                    WinHttpReceiveResponse(s.request, nullptr);
        if (!sent) return finish(false, "connection failed");
        if (StatusCode(s.request) != 200) {
            return finish(false, "http " + std::to_string(StatusCode(s.request)));
        }

        std::string pending;
        bool success = false;
        std::string errMsg;
        std::string lastStatus;
        ULONGLONG lastPost = 0;  // 진행 이벤트 쓰로틀 (다운로드 중 라인이 매우 잦음)
        while (!aborted->load()) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(s.request, &avail)) {
                errMsg = "read error";
                break;
            }
            if (avail == 0) break;
            char buf[8192];
            DWORD toRead = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            DWORD read = 0;
            if (!WinHttpReadData(s.request, buf, toRead, &read)) {
                errMsg = "read error";
                break;
            }
            if (read == 0) break;
            pending.append(buf, read);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (line.empty()) continue;
                json j = json::parse(line, nullptr, false);
                if (j.is_discarded()) continue;
                if (j.contains("error")) {
                    errMsg = j["error"].is_string() ? j["error"].get<std::string>() : "error";
                    break;
                }
                std::string status = j.value("status", "");
                uint64_t total = j.value("total", (uint64_t)0);
                uint64_t completed = j.value("completed", (uint64_t)0);
                if (status == "success") success = true;
                ULONGLONG now = GetTickCount64();
                bool important = success || status != lastStatus;
                if (!aborted->load() && (important || now - lastPost >= 250)) {
                    lastStatus = status;
                    lastPost = now;
                    PostUi([onProgress, status, total, completed]() {
                        onProgress(status, total, completed);
                    });
                }
            }
            if (!errMsg.empty() || success) break;
        }
        if (aborted->load()) return finish(false, "aborted");
        finish(errMsg.empty() && success, errMsg);
    }).detach();
}

void AiClient::Abort(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeRequests_.find(requestId);
    if (it != activeRequests_.end()) it->second->store(true);
}
