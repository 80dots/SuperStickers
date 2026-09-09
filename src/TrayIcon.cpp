#include "TrayIcon.h"

// 트레이 아이콘 추가
void TrayIcon::Create(HWND owner, UINT callbackMsg, HICON icon, const std::wstring& tip) {
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = owner;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = callbackMsg;
    nid_.hIcon = icon;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != 0;
}

// 탐색기가 다시 시작된 뒤 아이콘을 다시 추가
void TrayIcon::Recreate() {
    if (nid_.hWnd) {
        Shell_NotifyIconW(NIM_ADD, &nid_);
        added_ = true;
    }
}

// 툴팁 갱신
void TrayIcon::UpdateTip(const std::wstring& tip) {
    if (!added_) return;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

// 풍선 알림 (캘린더 알람)
void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// 아이콘 제거
void TrayIcon::Destroy() {
    if (added_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        added_ = false;
    }
}
