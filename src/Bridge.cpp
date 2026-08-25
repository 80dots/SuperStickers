#include "Bridge.h"

using json = nlohmann::json;

std::string Bridge::HandleMessage(const std::string& msg) {
    json req = json::parse(msg, nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("id")) return {};

    json resp = {{"id", req["id"]}};
    std::string method = req.value("method", "");
    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        resp["ok"] = false;
        resp["error"] = "unknown method: " + method;
        return resp.dump();
    }
    try {
        resp["ok"] = true;
        resp["result"] = it->second(req.contains("params") ? req["params"] : json::object());
    } catch (const std::exception& e) {
        resp["ok"] = false;
        resp["error"] = e.what();
    }
    return resp.dump();
}
