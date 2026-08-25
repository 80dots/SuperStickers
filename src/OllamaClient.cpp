#include "OllamaClient.h"

#include <thread>
#include <vector>

#include "Utils.h"

using json = nlohmann::json;

OllamaClient::Url OllamaClient::ParseEndpoint(const std::string& endpoint) {
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
void OpenRequest(Session& s, const OllamaClient::Url& u, const wchar_t* verb,
                 const wchar_t* path) {
    s.session = WinHttpOpen(L"SuperSticker/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
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

void OllamaClient::ListModels(
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

void OllamaClient::Chat(const std::string& requestId, const std::string& endpoint,
                        const std::string& model, const json& messages,
                        std::function<void(std::string)> onChunk,
                        std::function<void(bool, std::string)> onDone) {
    Url u = ParseEndpoint(endpoint);
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeRequests_[requestId] = aborted;
    }
    std::thread([this, requestId, u, model, messages, onChunk, onDone, aborted]() {
        auto finish = [&](bool ok, const std::string& err) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeRequests_.erase(requestId);
            }
            PostUi([onDone, ok, err]() { onDone(ok, err); });
        };
        if (!u.valid) return finish(false, "invalid endpoint");

        Session s;
        OpenRequest(s, u, L"POST", L"/api/chat");
        if (!s.request) return finish(false, "connection failed");

        std::string body = json{{"model", model}, {"messages", messages}, {"stream", true}}.dump();
        bool sent = WinHttpSendRequest(s.request, L"Content-Type: application/json\r\n", (DWORD)-1,
                                       (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(),
                                       0) &&
                    WinHttpReceiveResponse(s.request, nullptr);
        if (!sent) return finish(false, "connection failed");
        if (StatusCode(s.request) != 200) {
            return finish(false, "http " + std::to_string(StatusCode(s.request)));
        }

        // NDJSON 스트림: 라인 단위 파싱. 청크 사이마다 중단 플래그 확인.
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
                if (line.empty()) continue;
                json j = json::parse(line, nullptr, false);
                if (j.is_discarded()) continue;
                if (j.contains("error")) {
                    errMsg = j["error"].is_string() ? j["error"].get<std::string>() : "error";
                    doneFlag = true;
                    break;
                }
                if (j.contains("message") && j["message"].contains("content")) {
                    std::string delta = j["message"]["content"].get<std::string>();
                    if (!delta.empty() && !aborted->load()) {
                        PostUi([onChunk, delta]() { onChunk(delta); });
                    }
                }
                if (j.value("done", false)) doneFlag = true;
            }
            if (doneFlag) break;
        }
        if (aborted->load()) return finish(false, "aborted");
        finish(errMsg.empty() && doneFlag, errMsg.empty() ? "" : errMsg);
    }).detach();
}

void OllamaClient::Pull(const std::string& requestId, const std::string& endpoint,
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

void OllamaClient::Abort(const std::string& requestId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = activeRequests_.find(requestId);
    if (it != activeRequests_.end()) it->second->store(true);
}
