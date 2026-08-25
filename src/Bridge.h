#pragma once
#include <functional>
#include <map>
#include <string>

#include <json.hpp>

// 웹 → 네이티브 JSON-RPC 유사 디스패처.
// 요청: {id, method, params}  응답: {id, ok, result|error}
class Bridge {
public:
    // 핸들러는 result를 반환하거나 std::runtime_error를 던져 오류를 전달한다.
    using Handler = std::function<nlohmann::json(const nlohmann::json& params)>;

    void Register(const std::string& method, Handler h) { handlers_[method] = std::move(h); }

    // 요청 JSON 문자열 처리 → 응답 JSON 문자열 (id 없는 요청은 빈 문자열 반환)
    std::string HandleMessage(const std::string& msg);

private:
    std::map<std::string, Handler> handlers_;
};
