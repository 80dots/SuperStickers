#pragma once
#include <windows.h>

#include <string>

#include "WebViewHost.h"

// AI 설정 마법사 창 — 메모창 안의 모달이 아니라 별도의 표준 창이다 (싱글턴, 닫으면 파괴).
// 메모창이나 설정 창이 `wizard.open`으로 띄우고, 결과(마침/중단)는 App이 `wizard.result`로
// 부른 창에 돌려준다. 페이지는 ui/wizard.html(aiwizard.js의 standalone 모드).
class WizardWindow {
public:
    static void RegisterWndClass(HINSTANCE hinst);
    // ownerHwnd가 있으면 그 창 가운데에, 없으면 화면 가운데에 띄운다
    static WizardWindow* Create(HINSTANCE hinst, bool force, HWND ownerHwnd);

    HWND hwnd() const { return hwnd_; }
    WebViewHost& host() { return host_; }

    void Focus();            // 이미 열려 있을 때 앞으로
    void OnThemeChanged();   // 타이틀바 다크 모드 갱신

private:
    WizardWindow() = default;
    static LRESULT CALLBACK SWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    WebViewHost host_;
};
