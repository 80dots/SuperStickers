#pragma once
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "Store.h"
#include "WebViewHost.h"

// 프레임리스 스티커 창. WebView2가 클라이언트 전체를 덮는다 — 창 안의 모든 픽셀을 페이지 한
// 표면이 그려야 네이티브와 WebView가 서로 다른 순간에 화면에 올라가 테두리처럼 보이는 일이
// 없다. 사방 6px(밴드)는 페이지의 여백이고, 거기서 누르면 window.startResize로 네이티브
// 크기 조절을 시작한다(sticker-frame.js).
class StickerWindow {
public:
    static void RegisterWndClass(HINSTANCE hinst);
    // clip: 클립보드로 만든 새 메모가 첫 그림에 넣을 내용 (App::NewStickerFromClipboard)
    static StickerWindow* Create(HINSTANCE hinst, const StickerData& d, bool show,
                                 bool activate, bool focusEditor = false,
                                 const nlohmann::json& clip = nlohmann::json());

    HWND hwnd() const { return hwnd_; }
    WebViewHost& host() { return host_; }
    WebViewHost& siteHost() { return siteHost_; }  // web 메모의 사이트 브라우저 뷰

    StickerData data;

    void ShowWin(bool show, bool activate);
    bool VisibleNow() const { return hwnd_ && IsWindowVisible(hwnd_); }
    // 최소화: 타이틀바 한 줄만 남긴다 (그룹창의 목록 보기와 같은 높이 감각). 저장된다.
    void SetMinimized(bool on);
    int MinimizedHeightPx() const;
    // 창 크기를 창의 실제 DPI에 맞춘다 (배율 변경을 놓친 크기·최소화 높이). 바뀌었으면 true.
    bool FitToCurrentDpi();
    void SetTopmost(bool on);
    void SetColor(const std::string& color);
    void OnThemeChanged();  // 배경색 갱신
    // 다중 선택 표시 — 테두리는 페이지가 그리고(selection.changed), 여기서는 DWM 보더 색만 맞춘다
    void SetSelectedLook(bool on);
    void ApplyUiScale();    // 설정의 UI 배율을 WebView 줌으로 반영
    // 현재 창 rect를 data.x/y/w/h에 기록한다.
    void StoreGeometryFromWindow();

    int CssPx(int cssPx) const;  // CSS px → 물리 px (UI 배율·DPI 반영)

    void SaveData();        // updatedAt 갱신 후 저장
    void Destroy();

private:
    StickerWindow() = default;
    static LRESULT CALLBACK SWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void LayoutWebView();
    void RaiseSiteView();       // web 메모: 사이트 뷰를 메인 페이지 위로
    void UpdateBackground();    // 배경 브러시·WebView 기본 배경·DWM 보더를 메모 색으로
    nlohmann::json WindowMetricsJson() const;  // 페이지가 그리는 테두리 치수 (물리 px)
    void SyncWindowMetrics();   // DPI가 바뀌었으면 페이지에 새 치수를 보낸다
    int BandPx() const;

    void RegisterTypeBridges();  // 타입별(file/web/pdf) 브리지 메서드 등록

    HWND hwnd_ = nullptr;
    WebViewHost host_;
    WebViewHost siteHost_;  // type=="web" 전용 (그 외에는 미생성)
    UINT dpi_ = 96;
    UINT metricsDpi_ = 0;  // 페이지에 마지막으로 알린 테두리 치수의 DPI
    HBRUSH bgBrush_ = nullptr;  // WebView가 붙기 전·커지는 순간 드러나는 자리를 칠한다
    bool selected_ = false;  // 다중 선택 표시 여부
    RECT dragStartRect_{};    // 이동 vs 리사이즈 구분용 (그룹 드롭 감지)
    POINT dragStartCursor_{};  // 드래그 시작 시 커서 — 자석과 무관한 "자유 위치" 계산 기준
    bool inSizeMove_ = false;  // 이동/리사이즈 모달 루프 안인가 (위 두 기준값의 유효 구간)
    // web 타입 전용: UI 자동 숨김으로 타이틀바가 빠지면 상단 스트립이 URL바만 남는다.
    // 사이트 뷰는 네이티브 자식 창이라 페이지 CSS 리플로우가 닿지 않아 여기서 맞춘다.
    bool webUiHidden_ = false;
    // 다중 선택 드래그: 함께 움직일 창들의 시작 위치 (레이아웃 유지를 위해 같은 delta 적용)
    std::vector<std::pair<StickerWindow*, POINT>> dragPeers_;
};
