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

// 다크 테마에서는 HSL로 변환해 어둡고 채도 낮은 변형을 자동 생성한다.
// ui\common\color.js의 effectiveBg()와 반드시 동일한 알고리즘을 유지할 것.
COLORREF StickerColor(const std::string& color, bool dark) {
    std::string hex = color;
    if (const char* mapped = LegacyNameToHex(color)) hex = mapped;
    int r = 0xFF, g = 0xF4, b = 0xB8;  // 기본 yellow
    ParseHex(hex, r, g, b);
    if (!dark) return RGB(r, g, b);

    // RGB → HSL
    double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
    double mx = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    double mn = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    double l = (mx + mn) / 2.0, h = 0, s = 0;
    if (mx != mn) {
        double d = mx - mn;
        s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == rf) h = (gf - bf) / d + (gf < bf ? 6 : 0);
        else if (mx == gf) h = (bf - rf) / d + 2;
        else h = (rf - gf) / d + 4;
        h /= 6.0;
    }
    // 다크 변형: 밝기·채도 축소
    l = 0.18 + 0.12 * l;
    s = s * 0.40;
    // HSL → RGB
    auto hue2rgb = [](double p, double q, double t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 1.0 / 2) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    double rr, gg, bb;
    if (s == 0) {
        rr = gg = bb = l;
    } else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        rr = hue2rgb(p, q, h + 1.0 / 3);
        gg = hue2rgb(p, q, h);
        bb = hue2rgb(p, q, h - 1.0 / 3);
    }
    return RGB((BYTE)(rr * 255 + 0.5), (BYTE)(gg * 255 + 0.5), (BYTE)(bb * 255 + 0.5));
}

}  // namespace theme
