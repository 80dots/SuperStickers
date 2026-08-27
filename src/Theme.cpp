#include "Theme.h"

#include <dwmapi.h>

namespace theme {

bool SystemIsDark() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

std::string Effective(const std::string& setting) {
    if (setting == "light") return "light";
    if (setting == "dark") return "dark";
    return SystemIsDark() ? "dark" : "light";
}

void ApplyDarkTitlebar(HWND hwnd, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v));
}

void ApplyRoundCorners(HWND hwnd) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

void SetWindowBorderColor(HWND hwnd, COLORREF color) {
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
}

namespace {

// 레거시 프리셋 이름 → hex (구버전 데이터 호환)
const char* LegacyNameToHex(const std::string& name) {
    if (name == "yellow") return "#FFF4B8";
    if (name == "mint") return "#C8F0DC";
    if (name == "pink") return "#FFD9E3";
    if (name == "blue") return "#CFE5FF";
    if (name == "gray") return "#EAEAEA";
    return nullptr;
}

bool ParseHex(const std::string& hex, int& r, int& g, int& b) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = nib(hex[i + 1]);
        if (v[i] < 0) return false;
    }
    r = v[0] * 16 + v[1];
    g = v[2] * 16 + v[3];
    b = v[4] * 16 + v[5];
    return true;
}

}  // namespace

// 스티커 배경색은 테마와 무관하게 사용자가 고른 색 그대로 사용한다.
// (과거의 다크 변형은 제거 — ui\common\color.js effectiveBg()와 동일하게 유지할 것)
COLORREF StickerColor(const std::string& color, bool /*dark*/) {
    std::string hex = color;
    if (const char* mapped = LegacyNameToHex(color)) hex = mapped;
    int r = 0xFF, g = 0xF4, b = 0xB8;  // 기본 yellow
    ParseHex(hex, r, g, b);
    return RGB(r, g, b);
}

}  // namespace theme
