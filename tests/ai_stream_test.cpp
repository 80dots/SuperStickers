// AiClient의 스트리밍 파서를 실제 서버에 붙여 확인하는 콘솔 테스트.
// 앱 전체를 띄우지 않고 새로 쓴 파싱 경로만 검증한다.
//
//   cmake --build --preset release --target ai_stream_test
//   ai_stream_test <endpoint> [text|json|ollama] [prompt]
//
// 예: 내장 백엔드 검증
//   llama-server --model <gguf> --port 11987 --no-webui
//   ai_stream_test http://127.0.0.1:11987 text "한 문장으로 답해줘"
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "AiClient.h"
#include "Utils.h"

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string endpoint = argc > 1 ? util::WideToUtf8(argv[1]) : "http://127.0.0.1:11987";
    std::string mode = argc > 2 ? util::WideToUtf8(argv[2]) : "text";
    std::string prompt = argc > 3 ? util::WideToUtf8(argv[3]) : "한 문장으로 자기소개 해줘.";

    AiClient client;
    // 이 테스트에는 UI 스레드가 없다 — 콜백을 그 자리에서 실행한다.
    client.SetUiPoster([](std::function<void()> fn) { fn(); });

    // loaded-ollama / loaded-lmstudio: 모델이 메모리에 올라와 있는지 조회만 한다.
    // (세 번째 인자가 모델 이름. 앱은 이 결과로 "모델을 올리는 중" 안내 여부를 정한다)
    if (mode.rfind("loaded", 0) == 0) {
        std::atomic<bool> got{false};
        client.ModelLoaded(endpoint,
                           mode == "loaded-lmstudio" ? AiClient::Protocol::OpenAiSse
                                                     : AiClient::Protocol::OllamaNdjson,
                           prompt, [&](bool known, bool loaded) {
                               printf("known=%d loaded=%d\n", known ? 1 : 0, loaded ? 1 : 0);
                               got = true;
                           });
        for (int i = 0; i < 100 && !got; ++i) Sleep(100);
        if (!got) printf("timeout\n");
        return got ? 0 : 1;
    }

    AiClient::ChatOptions opts;
    // ollama 모드는 리팩터링한 NDJSON 경로가 그대로 도는지 보는 회귀 확인용이다
    opts.protocol = (mode == "ollama") ? AiClient::Protocol::OllamaNdjson
                                       : AiClient::Protocol::OpenAiSse;
    opts.jsonFormat = (mode == "json");
    if (opts.jsonFormat) {
        // 실제 앱(AI Review)과 같은 경로 — json_object가 아니라 스키마로 강제한다
        opts.jsonSchema = {{"type", "object"},
                           {"properties", {{"title", {{"type", "string"}}}}},
                           {"required", {"title"}}};
    }
    opts.disableThinking = true;  // Qwen3 계열에서 본문이 비는 것을 막는다

    nlohmann::json messages = nlohmann::json::array();
    if (opts.jsonFormat) {
        messages.push_back({{"role", "system"},
                            {"content", R"(다음 JSON 형식으로만 답하라: {"title": "제목"})"}});
    }
    messages.push_back({{"role", "user"}, {"content", prompt}});

    std::atomic<bool> finished{false};
    std::string all;
    int chunks = 0;
    bool ok = false;
    std::string error;

    client.Chat(
        "test-1", endpoint, "supersticker", messages, opts,
        [&](std::string delta) {
            all += delta;
            ++chunks;
            fputs(delta.c_str(), stdout);
            fflush(stdout);
        },
        [&](bool success, std::string err) {
            ok = success;
            error = err;
            finished = true;
        });

    for (int i = 0; i < 600 && !finished.load(); ++i) Sleep(100);

    printf("\n----\nok=%d chunks=%d bytes=%zu error=%s\n", ok ? 1 : 0, chunks, all.size(),
           error.c_str());
    if (!finished.load()) {
        printf("TIMEOUT\n");
        return 2;
    }
    // 스트리밍이 실제로 조각조각 왔는지까지 본다 (한 번에 다 오면 파서가 의미 없다)
    if (!ok || chunks < 2 || all.empty()) return 1;
    return 0;
}
