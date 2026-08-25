#pragma once
#include <windows.h>

#include <string>

#include "Store.h"
#include "WebViewHost.h"

// 프레임리스 스티커 창. WebView2가 사방 6px(밴드) 안쪽을 덮고,
// 밴드 영역은 WM_NCHITTEST로 네이티브 리사이즈를 처리한다.
class StickerWindow {
public:
    static void RegisterWndClass(HINSTANCE hinst);
    static StickerWindow* Create(HINSTANCE hinst, const StickerData& d, bool show, bool activate);

    HWND hwnd() const { return hwnd_; }
    WebViewHost& host() { return host_; }
    WebViewHost& siteHost() { return siteHost_; }  // web 메모의 사이트 브라우저 뷰

    StickerData data;

    void ShowWin(bool show, bool activate);
    bool VisibleNow() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void SetTopmost(bool on);
    void SetColor(const std::string& color);
    void OnThemeChanged();  // 밴드 색 갱신
    void ApplyUiScale();    // 설정의 UI 배율을 WebView 줌으로 반영
    void SaveData();        // updatedAt 갱신 후 저장
    void Destroy();

private:
    StickerWindow() = default;
    static LRESULT CALLBACK SWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void LayoutWebView();
    void UpdateBandBrush();
    int BandPx() const;

    void RegisterTypeBridges();  // 타입별(file/web/pdf) 브리지 메서드 등록

    HWND hwnd_ = nullptr;
    WebViewHost host_;
    WebViewHost siteHost_;  // type=="web" 전용 (그 외에는 미생성)
    UINT dpi_ = 96;
    HBRUSH bandBrush_ = nullptr;
    RECT dragStartRect_{};  // 이동 vs 리사이즈 구분용 (그룹 드롭 감지)
};
