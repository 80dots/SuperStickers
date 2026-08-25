#pragma once
#include <windows.h>

#include <string>

#include "WebViewHost.h"

// 설정 + 스티커 목록 탭을 담는 표준 창 (싱글턴, 닫으면 파괴하여 메모리 회수)
class ManagerWindow {
public:
    static void RegisterWndClass(HINSTANCE hinst);
    static ManagerWindow* Create(HINSTANCE hinst, const std::string& tab);

    HWND hwnd() const { return hwnd_; }
    WebViewHost& host() { return host_; }

    void ShowTab(const std::string& tab);  // 이미 열려 있을 때 탭 전환 + 전면으로
    void OnThemeChanged();                 // 타이틀바 다크 모드 갱신

private:
    ManagerWindow() = default;
    static LRESULT CALLBACK SWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    WebViewHost host_;
};
