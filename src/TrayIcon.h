#pragma once
#include <windows.h>
#include <shellapi.h>

#include <string>

class TrayIcon {
public:
    void Create(HWND owner, UINT callbackMsg, HICON icon, const std::wstring& tip);
    void Recreate();  // explorer 재시작(TaskbarCreated) 시 복구
    void UpdateTip(const std::wstring& tip);
    void Destroy();

private:
    NOTIFYICONDATAW nid_{};
    bool added_ = false;
};
