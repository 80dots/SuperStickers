#include "TrayIcon.h"

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

void TrayIcon::Recreate() {
    if (nid_.hWnd) {
        Shell_NotifyIconW(NIM_ADD, &nid_);
        added_ = true;
    }
}

void TrayIcon::UpdateTip(const std::wstring& tip) {
    if (!added_) return;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::Destroy() {
    if (added_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        added_ = false;
    }
}
