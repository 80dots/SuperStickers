#include "I18n.h"

#include <windows.h>

#include "Utils.h"

using json = nlohmann::json;

bool I18n::Load(const std::string& lang) {
    auto tryLoad = [this](const std::string& l) -> bool {
        auto bytes = util::ReadFileBytes(util::GetUiDir() + L"\\locales\\" + util::Utf8ToWide(l) +
                                         L".json");
        if (!bytes) return false;
        json j = json::parse(*bytes, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        dict_ = std::move(j);
        lang_ = l;
        return true;
    };
    if (tryLoad(lang)) return true;
    return tryLoad("en");
}

std::wstring I18n::T(const std::string& key) const {
    if (dict_.contains(key) && dict_[key].is_string())
        return util::Utf8ToWide(dict_[key].get<std::string>());
    return util::Utf8ToWide(key);
}

std::string I18n::DetectOsLanguage() {
    LANGID id = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(id) == LANG_KOREAN) ? "ko" : "en";
}
