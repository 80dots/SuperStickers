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
    // UI 자동 숨김: 헤더/툴바가 차지하던 CSS px 만큼 창을 위/아래에서 접거나 편다.
    // 내부 컨텐츠의 화면 위치는 변하지 않는다 (위쪽은 y를 함께 이동).
    // 크기 변화는 페이지의 height 트랜지션(linear 0.2s)과 같은 선형 타임라인으로
    // 애니메이션된다 — 양쪽이 같은 속도로 움직여 컨텐츠가 흔들리지 않는다.
    void ApplyCollapse(int topCss, int bottomCss);
    // 현재 창 rect를 data.x/y/w/h에 기록하되, 접힌 오프셋을 되돌려
    // 항상 "펼친 상태" 기준으로 저장한다 (재시작 시 위치·크기가 어긋나지 않도록).
    void StoreGeometryFromWindow();

    // 페이지가 알려주는 헤더·서식 툴바의 높이 (CSS px). 자동 숨김으로 접힐 수 있는 양이며,
    // 펼쳐져 있어도 값이 유지되므로 "숨겨졌을 때"의 사각형을 언제든 계산할 수 있다.
    void SetUiExtents(int topCss, int bottomCss);
    // 자석 정렬 기준 사각형: UI 자동 숨김이 켜져 있으면 "UI가 숨겨진 상태"의 모양으로 환산한다.
    // (드래그 중인 창은 헤더 위에 마우스가 있어 펼쳐져 있으므로 그대로 쓰면 어긋난다)
    RECT AlignBasis(const RECT& actual) const;
    RECT AlignRectNow() const;
    int CssPx(int cssPx) const;  // CSS px → 물리 px (UI 배율·DPI 반영)

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
    void StepCollapseAnim();  // 접기/펼치기 애니메이션 한 프레임 진행 (WM_TIMER)

    // UI 자동 숨김으로 접힌 높이 (CSS px — DPI/배율 변경에도 안전하게 CSS 단위로 보관)
    int collapseTopCss_ = 0;
    int collapseBottomCss_ = 0;
    // 접힐 수 있는 최대치(헤더·툴바 높이). 접힘 여부와 무관하게 유지된다.
    int uiExtentTopCss_ = 0;
    int uiExtentBottomCss_ = 0;
    // 애니메이션 상태: 현재 적용된 접힘(물리 px, 중간값)과 시작·목표·기준 rect.
    // "펼친 기준" rect를 시작 시 복원해 두므로 중간에 재요청돼도 이어서 자연스럽다.
    double animCurTop_ = 0, animCurBottom_ = 0;
    double animFromTop_ = 0, animFromBottom_ = 0;
    double animToTop_ = 0, animToBottom_ = 0;
    RECT animBase_{};
    DWORD animStartTick_ = 0;
    static constexpr UINT_PTR kCollapseTimerId = 7;
    static constexpr DWORD kCollapseAnimMs = 200;  // 페이지 CSS 트랜지션과 반드시 일치
};
