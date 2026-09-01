#pragma once
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

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
    // 다중 선택 표시 — 그룹창의 드롭 하이라이트와 같은 모양(GDI+ 안티앨리어싱 라운드 테두리)
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
    void UpdateBandBrush();
    int BandPx() const;

    void RegisterTypeBridges();  // 타입별(file/web/pdf) 브리지 메서드 등록

    HWND hwnd_ = nullptr;
    WebViewHost host_;
    WebViewHost siteHost_;  // type=="web" 전용 (그 외에는 미생성)
    UINT dpi_ = 96;
    HBRUSH bandBrush_ = nullptr;
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
