#pragma once
#include <windows.h>

#include <string>

namespace theme {

// OS 앱 테마가 다크인지 (HKCU ...\Personalize\AppsUseLightTheme)
bool SystemIsDark();

// 설정값("light"|"dark"|"system")으로부터 유효 테마 계산 → "light"|"dark"
std::string Effective(const std::string& setting);

// 타이틀바 다크 모드 (DWMWA_USE_IMMERSIVE_DARK_MODE)
void ApplyDarkTitlebar(HWND hwnd, bool dark);

// Windows 11 라운드 코너
void ApplyRoundCorners(HWND hwnd);

// DWM 창 보더 색 지정. 이 Windows 빌드(26200)는 DWMWA_COLOR_NONE(보더 제거)을
// 무시하므로, 창 배경색과 같은 색을 지정해 보더가 보이지 않게 한다.
// (DWM 렌더링을 유지해 둥근 모서리가 안티앨리어싱됨)
void SetWindowBorderColor(HWND hwnd, COLORREF color);

// 스티커 색상 이름 → 배경 COLORREF (다크/라이트 변형). ui\common\theme.css와 동기화 유지.
COLORREF StickerColor(const std::string& name, bool dark);

}  // namespace theme
