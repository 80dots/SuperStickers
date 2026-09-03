#pragma once
#include <windows.h>
#include <shellapi.h>

#include <string>

class TrayIcon {
public:
    void Create(HWND owner, UINT callbackMsg, HICON icon, const std::wstring& tip);
    void Recreate();  // explorer 재시작(TaskbarCreated) 시 복구
    void UpdateTip(const std::wstring& tip);
    // 풍선(토스트) 알림. 캘린더 알람이 쓴다 — 창이 숨어 있어도 보인다.
    void ShowBalloon(const std::wstring& title, const std::wstring& text);
    void Destroy();

private:
    NOTIFYICONDATAW nid_{};
    bool added_ = false;
};
