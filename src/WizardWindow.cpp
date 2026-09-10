#include "WizardWindow.h"

#include "App.h"
#include "Theme.h"
#include "Utils.h"

using json = nlohmann::json;

namespace {
const wchar_t* kClassName = L"SuperStickerWizard";
}

// 마법사 창 클래스 등록
void WizardWindow::RegisterWndClass(HINSTANCE hinst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hinst, L"#101");
    RegisterClassExW(&wc);
}

// 마법사 창 생성 — 부른 메모창 가운데(없으면 화면 가운데), 작업 영역 안으로
WizardWindow* WizardWindow::Create(HINSTANCE hinst, bool force, HWND ownerHwnd) {
    auto* self = new WizardWindow();

    UINT dpi = ownerHwnd ? GetDpiForWindow(ownerHwnd) : GetDpiForSystem();
    int w = MulDiv(500, dpi, 96), h = MulDiv(600, dpi, 96);
    int x, y;
    RECT o{};
    if (ownerHwnd && GetWindowRect(ownerHwnd, &o)) {
        x = o.left + ((o.right - o.left) - w) / 2;
        y = o.top + ((o.bottom - o.top) - h) / 2;
    } else {
        x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    }
    util::ClampRectToWorkArea(x, y, w, h);

    // 최대화는 뜻이 없고, 크기 조절은 허용한다
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    std::wstring title = App::I().i18n.T("wizard.title");
    HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, x, y, w, h, nullptr, nullptr,
                                hinst, self);
    if (!hwnd) {
        delete self;
        return nullptr;
    }
    theme::ApplyDarkTitlebar(hwnd, App::I().EffectiveTheme() == "dark");

    App::I().SetupCommonBridge(self->host_);

    std::wstring url = std::wstring(L"https://app.sticker/wizard.html") + (force ? L"?force=1" : L"");
    self->host_.Create(hwnd, url, App::I().MakeInitJson("wizard", ""), [self]() {
        self->host_.SetZoomFactor(App::I().settings.uiScale);
        RECT rc{};
        GetClientRect(self->hwnd_, &rc);
        self->host_.SetBounds(rc);
    });

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return self;
}

// 이미 열려 있을 때 앞으로
void WizardWindow::Focus() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

// 테마 변경 시 타이틀바 색
void WizardWindow::OnThemeChanged() {
    theme::ApplyDarkTitlebar(hwnd_, App::I().EffectiveTheme() == "dark");
}

// 정적 프로시저
LRESULT CALLBACK WizardWindow::SWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WizardWindow* self;
    if (msg == WM_NCCREATE) {
        self = (WizardWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (WizardWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

// 마법사 창 메시지 처리 — 닫기(X)는 언제나 중단할지 묻는다 (진행 중이면 그것이 끊긴다고)
LRESULT WizardWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            host_.SetBounds(rc);
            return 0;
        }
        case WM_CLOSE: {
            bool busy = App::I().HasActiveOllamaTasks();
            if (!App::ConfirmYesNo(hwnd_, busy ? "wizard.confirmCloseBusy" : "wizard.confirmClose"))
                return 0;
            if (busy) App::I().AbortOllamaTasks();
            App::I().FinishWizard(false);  // 결과를 돌려주고 창을 부순다
            return 0;
        }
        case WM_DESTROY:
            host_.Close();
            App::I().OnWizardDestroyed();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete this;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
