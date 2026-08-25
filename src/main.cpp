#include <windows.h>

#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <string>

using std::max;
using std::min;
#include <gdiplus.h>

#include "App.h"

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine,
                    _In_ int) {
    bool startHidden = wcsstr(lpCmdLine, L"--hidden") != nullptr;

    // 단일 인스턴스: 이미 실행 중이면 기존 인스턴스에 "표시" 신호 후 종료
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SuperSticker.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"SuperStickerApp", nullptr);
        if (existing) {
            COPYDATASTRUCT cds{};
            cds.dwData = 1;  // show
            SendMessageW(existing, WM_COPYDATA, 0, (LPARAM)&cds);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // 파일 메모 썸네일 PNG 인코딩용
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    int ret = 0;
    if (App::I().Init(hInstance, startHidden)) {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ret = (int)msg.wParam;
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return ret;
}
