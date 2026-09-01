#include "ManagerWindow.h"

#include "App.h"
#include "Theme.h"
#include "Utils.h"

using json = nlohmann::json;

namespace {
const wchar_t* kClassName = L"SuperStickerManager";
}

void ManagerWindow::RegisterWndClass(HINSTANCE hinst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hinst, L"#101");
    RegisterClassExW(&wc);
}

ManagerWindow* ManagerWindow::Create(HINSTANCE hinst, const std::string& tab) {
    auto* self = new ManagerWindow();

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(780, dpi, 96), h = MulDiv(560, dpi, 96);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(0, kClassName, L"Super Stickers", WS_OVERLAPPEDWINDOW, x, y, w, h,
                                nullptr, nullptr, hinst, self);
    if (!hwnd) {
        delete self;
        return nullptr;
    }
    theme::ApplyDarkTitlebar(hwnd, App::I().EffectiveTheme() == "dark");

    App::I().SetupCommonBridge(self->host_);

    std::wstring url = L"https://app.sticker/manager.html?tab=" + util::Utf8ToWide(tab);
    self->host_.Create(hwnd, url, App::I().MakeInitJson("manager", ""), [self]() {
        self->host_.SetZoomFactor(App::I().settings.uiScale);
        RECT rc{};
        GetClientRect(self->hwnd_, &rc);
        self->host_.SetBounds(rc);
    });

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return self;
}

void ManagerWindow::ShowTab(const std::string& tab) {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    host_.PostEvent("manager.showTab", json{{"tab", tab}});
}

void ManagerWindow::OnThemeChanged() {
    theme::ApplyDarkTitlebar(hwnd_, App::I().EffectiveTheme() == "dark");
}

LRESULT CALLBACK ManagerWindow::SWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ManagerWindow* self;
    if (msg == WM_NCCREATE) {
        self = (ManagerWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (ManagerWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

LRESULT ManagerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            host_.SetBounds(rc);
            return 0;
        }
        case WM_CLOSE:
            // Ollama 설치/모델 다운로드가 진행 중이면 중단 경고 후 확인 시에만 닫기
            if (App::I().HasActiveOllamaTasks()) {
                if (!App::ConfirmYesNo(hwnd_, "confirm.closeCancelsDownloads")) return 0;
                App::I().AbortOllamaTasks();
            }
            break;  // DefWindowProc → DestroyWindow

        case WM_DESTROY:
            host_.Close();
            App::I().OnManagerDestroyed();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete this;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
