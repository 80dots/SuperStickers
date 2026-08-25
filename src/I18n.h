#pragma once
#include <map>
#include <string>

#include <json.hpp>

// ui\locales\<lang>.json을 네이티브·웹이 공유하는 단일 소스로 사용한다.
class I18n {
public:
    // lang: "ko" | "en". 실패 시 en 폴백.
    bool Load(const std::string& lang);

    std::wstring T(const std::string& key) const;   // 트레이 메뉴 등 네이티브 문자열
    const nlohmann::json& Dict() const { return dict_; }  // 웹으로 그대로 전달
    const std::string& Lang() const { return lang_; }

    static std::string DetectOsLanguage();  // "ko" | "en"

private:
    std::string lang_ = "en";
    nlohmann::json dict_;
};
