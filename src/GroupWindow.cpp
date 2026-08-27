#include "GroupWindow.h"

#include <cmath>

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

using std::max;
using std::min;
#include <gdiplus.h>

#include "App.h"
#include "Theme.h"
#include "Utils.h"

using json = nlohmann::json;

namespace {
const wchar_t* kBackClass = L"SuperStickerGroup";
const wchar_t* kContentClass = L"SuperStickerGroupContent";
constexpr int kBandDip = 6;
constexpr int kMinWDip = 280;
constexpr int kMinHDip = 220;

COLORREF GroupBaseColor(const GroupData& d, bool /*dark*/) {
    if (!d.color.empty()) return theme::StickerColor(d.color, false);
    // group 기본 배경 — 앱 테마와 무관하게 라이트 --bg 값으로 고정
    return RGB(0xF7, 0xF7, 0xF8);
}
}  // namespace

void GroupWindow::RegisterWndClass(HINSTANCE hinst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SBackProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBackClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW wc2{sizeof(wc2)};
    wc2.lpfnWndProc = SContentProc;
    wc2.hInstance = hinst;
    wc2.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc2.lpszClassName = kContentClass;
    RegisterClassExW(&wc2);
}

GroupWindow* GroupWindow::Create(HINSTANCE hinst, const GroupData& g, bool show, bool activate) {
    auto* self = new GroupWindow();
    self->data = g;

    int x = g.x, y = g.y, w = g.w, h = g.h;
    util::ClampRectToWorkArea(x, y, w, h);

    // backdrop: 반투명 배경 (LWA_ALPHA는 자식 없는 단순 창에서 데스크톱 투과가 안정적)
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kBackClass,
                                L"Super Sticker Group", WS_POPUP | WS_THICKFRAME, x, y, w, h,
                                nullptr, nullptr, hinst, self);
    if (!hwnd) {
        delete self;
        return nullptr;
    }
    self->dpi_ = GetDpiForWindow(hwnd);
    theme::ApplyRoundCorners(hwnd);  // DWM 라운드 (안티앨리어싱)
    self->ApplyAppearance();         // 배경 + DWM 보더 색(배경색과 동일 → 안 보임)

    // content: backdrop이 소유한 팝업 — 항상 backdrop 위에 유지됨.
    // 페이지가 setShape로 보내기 전까지는 빈 region(완전히 잘림)으로 시작한다.
    int band = self->BandPx();
    HWND content = CreateWindowExW(WS_EX_TOOLWINDOW, kContentClass, L"", WS_POPUP, x + band,
                                   y + band, w - band * 2, h - band * 2, hwnd, nullptr, hinst,
                                   self);
    self->contentHwnd_ = content;
    if (content) SetWindowRgn(content, CreateRectRgn(0, 0, 0, 0), FALSE);

    // --- 브리지 ---
    Bridge& b = self->host_.bridge();

    b.Register("group.load", [self](const json&) {
        json members = json::array();
        for (auto& id : self->data.memberIds) {
            if (auto* d = App::I().FindStickerData(id)) members.push_back(Store::ToJson(*d));
        }
        return json{{"group", Store::GroupToJson(self->data)}, {"members", members}};
    });

    b.Register("group.setTitle", [self](const json& p) {
        self->data.title = p.value("title", "");
        self->SaveData();
        return json::object();
    });

    b.Register("group.setLayout", [self](const json& p) {
        std::string layout = p.value("layout", "grid");
        self->data.layout =
            (layout == "masonry" || layout == "list") ? layout : "grid";
        self->SaveData();
        return json::object();
    });

    b.Register("group.setGridSize", [self](const json& p) {
        std::string s = p.value("size", "m");
        self->data.gridSize = (s == "s" || s == "l") ? s : "m";
        self->SaveData();
        return json::object();
    });

    b.Register("group.setMemberHeight", [self](const json& p) {
        std::string id = p.value("id", "");
        int h = p.value("height", 0);
        if (id.empty()) return json::object();
        if (h > 0)
            self->data.memberHeights[id] = h;
        else
            self->data.memberHeights.erase(id);  // 0 이하 = 기본 높이로 복원
        self->SaveData();
        return json::object();
    });

    b.Register("group.reorder", [self](const json& p) {
        if (p.contains("memberIds") && p["memberIds"].is_array()) {
            std::vector<std::string> order;
            for (auto& m : p["memberIds"])
                if (m.is_string()) order.push_back(m.get<std::string>());
            App::I().ReorderGroupMembers(self, order);
        }
        return json::object();
    });

    b.Register("group.setTopmost", [self](const json& p) {
        self->SetTopmost(p.value("topmost", false));
        return json::object();
    });

    b.Register("group.hide", [self](const json&) {
        self->data.hidden = true;
        self->SaveData();
        self->ShowWin(false, false);
        return json::object();
    });

    b.Register("group.delete", [self](const json&) {
        if (!App::ConfirmYesNo(self->contentHwnd_, "group.deleteConfirm"))
            return json{{"deleted", false}};
        std::string id = self->data.id;
        App::I().RunOnUi([id]() { App::I().DeleteGroupReleaseMembers(id); });
        return json{{"deleted", true}};
    });

    b.Register("group.removeMember", [self](const json& p) {
        std::string id = p.value("id", "");
        int px = p.value("x", -1);
        int py = p.value("y", -1);
        App::I().RunOnUi([id, px, py]() { App::I().PopOutStickerAt(id, px, py); });
        return json::object();
    });

    b.Register("group.newMemberMemo", [self](const json& p) {
        std::string groupId = self->data.id;
        std::string type = p.value("type", "rich");
        App::I().RunOnUi([groupId, type]() { App::I().NewMemoInGroup(groupId, type); });
        return json::object();
    });

    b.Register("group.setAppearance", [self](const json& p) {
        if (p.contains("color") && p["color"].is_string()) self->data.color = p["color"];
        if (p.contains("opacity") && p["opacity"].is_number()) {
            double v = p["opacity"];
            self->data.opacity = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        }
        self->ApplyAppearance();
        self->SaveData();
        return json::object();
    });

    // 페이지의 불투명 영역(헤더/카드/팝오버) → content 창 region
    b.Register("group.setShape", [self](const json& p) {
        self->ApplyShape(p);
        return json::object();
    });

    b.Register("member.save", [self](const json& p) {
        App::I().SaveMemberContent(p);
        return json::object();
    });

    b.Register("member.openPath", [](const json& p) {
        std::wstring path = util::Utf8ToWide(p.value("path", ""));
        for (auto& c : path)
            if (c == L'/') c = L'\\';
        if (!path.empty())
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return json::object();
    });

    b.Register("member.delete", [self](const json& p) {
        if (!App::ConfirmYesNo(self->contentHwnd_, App::I().DeleteConfirmKey()))
            return json{{"deleted", false}};
        std::string id = p.value("id", "");
        App::I().RunOnUi([id]() { App::I().DeleteSticker(id); });
        return json{{"deleted", true}};
    });

    b.Register("window.startDrag", [self](const json&) {
        ReleaseCapture();
        SendMessageW(self->hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return json::object();
    });

    App::I().SetupCommonBridge(self->host_);

    self->host_.Create(content, L"https://app.sticker/group.html",
                       App::I().MakeInitJson("group", g.id), [self]() {
                           self->host_.SetZoomFactor(App::I().settings.uiScale);
                           self->LayoutWebView();
                       });

    if (g.topmost) self->SetTopmost(true);
    if (show) self->ShowWin(true, activate);
    return self;
}

void GroupWindow::ShowWin(bool show, bool activate) {
    if (show) host_.EnsureCreated();  // 비어 버린 창 복구
    ShowWindow(hwnd_, show ? (activate ? SW_SHOWNA : SW_SHOWNA) : SW_HIDE);
    if (contentHwnd_) ShowWindow(contentHwnd_, show ? SW_SHOWNA : SW_HIDE);
    // 컨트롤러 가시성을 창 상태와 일치시킨다. 숨긴 창의 WebView 렌더링이 멈추고,
    // 페이지 visibilityState가 정확해져 shape 폴링 가드가 올바르게 동작한다
    host_.SetVisible(show);
    if (show && data.topmost) {
        // 숨김 상태에서 설정한 항상 위가 표시 과정에서 풀리는 경우 방어
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (contentHwnd_)
            SetWindowPos(contentHwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (show && activate && contentHwnd_) SetForegroundWindow(contentHwnd_);
}

void GroupWindow::OnThemeChanged() { ApplyAppearance(); }

void GroupWindow::ApplyAppearance() {
    if (bgBrush_) DeleteObject(bgBrush_);
    bool dark = App::I().EffectiveTheme() == "dark";
    COLORREF base = GroupBaseColor(data, dark);
    bgBrush_ = CreateSolidBrush(base);
    // DWM 보더를 배경색과 같게 (드롭 호버 중에는 액센트 색 유지)
    theme::SetWindowBorderColor(hwnd_, dropHover_ ? RGB(0x63, 0x55, 0xE0) : base);
    // 알파 0이면 클릭이 통과해 이동·리사이즈가 불가능해지므로 최소 1 유지
    int alpha = (int)(data.opacity * 255.0 + 0.5);
    if (alpha < 1) alpha = 1;
    if (alpha > 255) alpha = 255;
    SetLayeredWindowAttributes(hwnd_, 0, (BYTE)alpha, LWA_ALPHA);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void GroupWindow::SetDropHover(bool on) {
    if (dropHover_ == on) return;
    dropHover_ = on;
    // 창 가장자리 1px(DWM 보더)까지 액센트/배경색으로 맞춰 일체감 유지
    bool dark = App::I().EffectiveTheme() == "dark";
    theme::SetWindowBorderColor(hwnd_,
                                on ? RGB(0x63, 0x55, 0xE0) : GroupBaseColor(data, dark));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void GroupWindow::SetTopmost(bool on) {
    data.topmost = on;
    HWND ins = on ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(hwnd_, ins, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (contentHwnd_)
        SetWindowPos(contentHwnd_, ins, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SaveData();
}

void GroupWindow::SaveData() {
    data.updatedAt = util::WideToUtf8(util::NowIso8601());
    App::I().store.SaveGroup(data);
}

void GroupWindow::Destroy() { DestroyWindow(hwnd_); }

int GroupWindow::BandPx() const { return MulDiv(kBandDip, dpi_, 96); }

void GroupWindow::StoreGeometryFromWindow() {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    data.x = r.left;
    data.y = r.top;
    data.w = r.right - r.left;
    data.h = r.bottom - r.top;
}

void GroupWindow::SyncContent() {
    if (!contentHwnd_) return;
    RECT r{};
    GetWindowRect(hwnd_, &r);
    int band = BandPx();
    SetWindowPos(contentHwnd_, nullptr, r.left + band, r.top + band,
                 (r.right - r.left) - band * 2, (r.bottom - r.top) - band * 2,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void GroupWindow::LayoutWebView() {
    if (!contentHwnd_) return;
    RECT rc{};
    GetClientRect(contentHwnd_, &rc);
    host_.SetBounds(rc);
}

void GroupWindow::ApplyShape(const json& p) {
    if (!contentHwnd_) return;
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (p.contains("rects") && p["rects"].is_array()) {
        for (auto& r : p["rects"]) {
            int x = r.value("x", 0), y = r.value("y", 0);
            int w = r.value("w", 0), h = r.value("h", 0);
            int rad = r.value("r", 0);
            if (w <= 0 || h <= 0) continue;
            HRGN piece = rad > 0
                             ? CreateRoundRectRgn(x, y, x + w + 1, y + h + 1, rad * 2, rad * 2)
                             : CreateRectRgn(x, y, x + w, y + h);
            CombineRgn(rgn, rgn, piece, RGN_OR);
            DeleteObject(piece);
        }
    }
    SetWindowRgn(contentHwnd_, rgn, TRUE);  // 소유권은 시스템으로 이전됨
}

LRESULT CALLBACK GroupWindow::SBackProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GroupWindow* self;
    if (msg == WM_NCCREATE) {
        self = (GroupWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (GroupWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->BackProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK GroupWindow::SContentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    GroupWindow* self;
    if (msg == WM_NCCREATE) {
        self = (GroupWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (GroupWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->ContentProc(hwnd, msg, wp, lp);
}

LRESULT GroupWindow::BackProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp == TRUE) return 0;
            break;

        // 클래식 NC 프레임(흰/회색 아웃라인) 억제 — StickerWindow와 동일
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, (LPARAM)-1);

        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT r{};
            GetWindowRect(hwnd, &r);
            int band = BandPx();
            bool left = pt.x < r.left + band, right = pt.x >= r.right - band;
            bool top = pt.y < r.top + band, bottom = pt.y >= r.bottom - band;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            return HTCAPTION;  // 배경 빈 영역 드래그로 그룹 이동
        }

        case WM_NCLBUTTONDBLCLK:
            return 0;  // 캡션 더블클릭 최대화 방지

        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC dc = (HDC)wp;
            FillRect(dc, &rc, bgBrush_);
            if (dropHover_) {  // 스티커 드롭 하이라이트 — GDI+ 안티앨리어싱 라운드 테두리
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                float thick = (float)MulDiv(3, dpi_, 96);
                Gdiplus::Pen pen(Gdiplus::Color(255, 0x63, 0x55, 0xE0), thick);
                float in = thick / 2.0f;
                float x = rc.left + in, y = rc.top + in;
                float w = (rc.right - rc.left) - thick, h = (rc.bottom - rc.top) - thick;
                float d = (float)MulDiv(16, dpi_, 96);  // 모서리 호 지름 (DWM 라운드와 유사)
                Gdiplus::GraphicsPath path;
                path.AddArc(x, y, d, d, 180.0f, 90.0f);
                path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
                path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
                path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
                path.CloseFigure();
                g.DrawPath(&pen, &path);
            }
            return 1;
        }

        case WM_WINDOWPOSCHANGED:
            SyncContent();
            break;  // DefWindowProc가 WM_SIZE/WM_MOVE 생성하도록 계속 진행

        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            double s = App::I().settings.uiScale;  // 최소 크기도 UI 배율을 따름
            mmi->ptMinTrackSize.x = (LONG)(kMinWDip * s * dpi_ / 96.0);
            mmi->ptMinTrackSize.y = (LONG)(kMinHDip * s * dpi_ / 96.0);
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_EXITSIZEMOVE: {
            StoreGeometryFromWindow();
            SaveData();
            return 0;
        }

        case WM_CLOSE:
            data.hidden = true;
            SaveData();
            ShowWin(false, false);
            return 0;

        case WM_DESTROY:
            if (contentHwnd_) {
                DestroyWindow(contentHwnd_);
                contentHwnd_ = nullptr;
            }
            if (bgBrush_) {
                DeleteObject(bgBrush_);
                bgBrush_ = nullptr;
            }
            App::I().OnGroupDestroyed(this);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete this;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT GroupWindow::ContentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCPAINT:
            return 0;  // 클래식 NC 아웃라인 억제
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, (LPARAM)-1);
        case WM_SIZE:
            LayoutWebView();
            return 0;
        case WM_CLOSE:
            return 0;  // content 단독으로는 닫지 않음
        case WM_DESTROY:
            host_.Close();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
