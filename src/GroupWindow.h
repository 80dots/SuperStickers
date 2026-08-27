#pragma once
#include <windows.h>

#include <string>

#include "Store.h"
#include "WebViewHost.h"

// 메모 그룹 창 — 이중 창 구조로 "배경만 반투명"을 구현한다.
//  - backdrop(hwnd_): 주 창. 배경색을 칠하고 LWA_ALPHA로 반투명(데스크톱 투과).
//    리사이즈 밴드·이동·드롭 대상·트레이 토글의 기준 창.
//  - content(contentHwnd_): backdrop 위에 소유(owned)로 떠 있는 WebView2 창.
//    페이지가 보낸 헤더/카드 사각형만 SetWindowRgn으로 남겨 불투명하게 표시하고
//    나머지 영역은 잘려나가 뒤의 backdrop(반투명 배경)이 보인다.
class GroupWindow {
public:
    static void RegisterWndClass(HINSTANCE hinst);
    static GroupWindow* Create(HINSTANCE hinst, const GroupData& g, bool show, bool activate);

    HWND hwnd() const { return hwnd_; }  // backdrop (위치·크기·드롭 판정 기준)
    HWND contentHwnd() const { return contentHwnd_; }
    WebViewHost& host() { return host_; }

    GroupData data;

    void ShowWin(bool show, bool activate);
    bool VisibleNow() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void OnThemeChanged();
    void ApplyAppearance();        // 배경 브러시 + 알파 갱신
    void SetDropHover(bool on);    // 스티커 드래그 중 하이라이트 테두리
    void SetTopmost(bool on);      // 항상 위 고정 (backdrop + content)
    // 현재 창 rect를 data.x/y/w/h에 기록 (UI 자동 숨김은 메모창 전용이라 보정 없음)
    void StoreGeometryFromWindow();
    void SaveData();
    void Destroy();

private:
    GroupWindow() = default;
    static LRESULT CALLBACK SBackProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SContentProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT BackProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT ContentProc(HWND, UINT, WPARAM, LPARAM);
    void SyncContent();  // content 창을 backdrop 클라이언트(밴드 안쪽)에 정렬
    void LayoutWebView();
    void ApplyShape(const nlohmann::json& p);  // 페이지가 보낸 불투명 영역 적용
    int BandPx() const;

    HWND hwnd_ = nullptr;
    HWND contentHwnd_ = nullptr;
    WebViewHost host_;
    UINT dpi_ = 96;
    HBRUSH bgBrush_ = nullptr;
    bool dropHover_ = false;
};
