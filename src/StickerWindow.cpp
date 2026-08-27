#include "StickerWindow.h"

#include <cmath>

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <windowsx.h>

#include <algorithm>
#include <thread>

using std::max;
using std::min;
#include <gdiplus.h>

#include "App.h"
#include "Theme.h"
#include "Utils.h"

using json = nlohmann::json;

namespace {
const wchar_t* kClassName = L"SuperStickerNote";
constexpr int kBandDip = 6;    // 네이티브 리사이즈 밴드 폭 (DIP)
constexpr int kMinWDip = 220;
constexpr int kMinHDip = 160;

// ---------- 파일 메모 헬퍼 ----------

// 파일/폴더 선택 대화상자 (다중 선택)
std::vector<std::wstring> PickFilesOrFolders(HWND owner, bool folders) {
    std::vector<std::wstring> out;
    wil::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM;
    if (folders) opts |= FOS_PICKFOLDERS;
    dlg->SetOptions(opts);
    if (FAILED(dlg->Show(owner))) return out;
    wil::com_ptr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; i++) {
        wil::com_ptr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        wil::unique_cotaskmem_string path;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            out.push_back(path.get());
    }
    return out;
}

std::wstring BackslashPath(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

// CF_HDROP으로 클립보드에 파일 복사 (탐색기에 붙여넣기 가능)
void CopyFilesToClipboard(HWND owner, std::vector<std::wstring> paths) {
    for (auto& p : paths) p = BackslashPath(p);
    if (paths.empty()) return;
    size_t chars = 0;
    for (auto& p : paths) chars += p.size() + 1;
    chars += 1;  // 이중 널 종료
    SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return;
    auto* df = (DROPFILES*)GlobalLock(mem);
    ZeroMemory(df, bytes);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    auto* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    for (auto& p : paths) {
        wcscpy_s(dst, p.size() + 1, p.c_str());
        dst += p.size() + 1;
    }
    GlobalUnlock(mem);
    if (OpenClipboard(owner)) {
        EmptyClipboard();
        SetClipboardData(CF_HDROP, mem);
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

CLSID PngEncoderClsid() {
    static CLSID clsid = []() {
        CLSID result{};
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size == 0) return result;
        std::vector<BYTE> buf(size);
        auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(num, size, codecs);
        for (UINT i = 0; i < num; i++) {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                result = codecs[i].Clsid;
                break;
            }
        }
        return result;
    }();
    return clsid;
}

// 셸 썸네일/아이콘 → PNG data URL (워커 스레드에서 호출)
std::string ThumbDataUrl(const std::wstring& path, int size) {
    // 셸 파싱 이름은 백슬래시만 허용 — 구분자 정규화
    std::wstring norm = path;
    for (auto& c : norm)
        if (c == L'/') c = L'\\';
    wil::com_ptr<IShellItemImageFactory> factory;
    HRESULT hr0 = SHCreateItemFromParsingName(norm.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr0)) return {};
    HBITMAP hbmp = nullptr;
    SIZE sz{size, size};
    HRESULT hr1 = factory->GetImage(sz, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp);
    if (FAILED(hr1) || !hbmp) {
        hbmp = nullptr;  // 썸네일 실패 시 아이콘으로 폴백
        HRESULT hr2 = factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
        if (FAILED(hr2) || !hbmp) return {};
    }

    std::string result;
    BITMAP bm{};
    GetObjectW(hbmp, sizeof(bm), &bm);
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bm.bmWidth;
        bmi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> bits((size_t)bm.bmWidth * bm.bmHeight * 4);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, hbmp, 0, bm.bmHeight, bits.data(), &bmi, DIB_RGB_COLORS)) {
            Gdiplus::Bitmap gb(bm.bmWidth, bm.bmHeight, bm.bmWidth * 4, PixelFormat32bppARGB,
                               bits.data());
            wil::com_ptr<IStream> stream;
            CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            CLSID png = PngEncoderClsid();
            if (stream && gb.Save(stream.get(), &png, nullptr) == Gdiplus::Ok) {
                HGLOBAL hg = nullptr;
                GetHGlobalFromStream(stream.get(), &hg);
                SIZE_T len = GlobalSize(hg);
                BYTE* data = (BYTE*)GlobalLock(hg);
                if (data && len > 0) {
                    DWORD b64len = 0;
                    CryptBinaryToStringA(data, (DWORD)len,
                                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                                         &b64len);
                    std::string b64(b64len, 0);
                    if (CryptBinaryToStringA(data, (DWORD)len,
                                             CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                             b64.data(), &b64len)) {
                        b64.resize(b64len);
                        result = "data:image/png;base64," + b64;
                    }
                }
                if (data) GlobalUnlock(hg);
            }
        }
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(hbmp);
    return result;
}

// URL 정규화: 스킴이 없으면 https:// 를 붙인다
std::string NormalizeUrl(std::string url) {
    while (!url.empty() && (url.front() == ' ' || url.back() == ' ')) {
        if (url.front() == ' ') url.erase(url.begin());
        if (!url.empty() && url.back() == ' ') url.pop_back();
    }
    if (url.empty()) return url;
    if (url.find("://") == std::string::npos) url = "https://" + url;
    return url;
}

}  // namespace

void StickerWindow::RegisterWndClass(HINSTANCE hinst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SWndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

StickerWindow* StickerWindow::Create(HINSTANCE hinst, const StickerData& d, bool show,
                                     bool activate) {
    auto* self = new StickerWindow();
    self->data = d;

    int x = d.x, y = d.y, w = d.w, h = d.h;
    util::ClampRectToWorkArea(x, y, w, h);

    DWORD exStyle = WS_EX_TOOLWINDOW | (d.topmost ? WS_EX_TOPMOST : 0);
    HWND hwnd = CreateWindowExW(exStyle, kClassName, L"Super Sticker",
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, w, h,
                                nullptr, nullptr, hinst, self);
    if (!hwnd) {
        delete self;
        return nullptr;
    }
    self->dpi_ = GetDpiForWindow(hwnd);
    theme::ApplyRoundCorners(hwnd);  // DWM 라운드 (안티앨리어싱)
    self->UpdateBandBrush();         // 밴드 브러시 + DWM 보더 색(배경색과 동일 → 안 보임)

    // --- 브리지: 스티커 전용 메서드 ---
    Bridge& b = self->host_.bridge();
    b.Register("sticker.load", [self](const json&) { return Store::ToJson(self->data); });

    b.Register("sticker.saveContent", [self](const json& p) {
        self->data.html = p.value("html", self->data.html);
        self->data.markdown = p.value("markdown", self->data.markdown);
        self->data.mode = p.value("mode", self->data.mode);
        if (p.contains("attachments") && p["attachments"].is_array()) {
            self->data.attachments.clear();
            for (auto& a : p["attachments"])
                if (a.is_string()) self->data.attachments.push_back(a.get<std::string>());
        }
        self->data.needsReview = true;  // 새 내용 → AI Review 버튼 활성화
        self->SaveData();
        return json::object();
    });

    // 태그/제목/요약/리뷰 상태 부분 갱신 (본문은 건드리지 않음)
    b.Register("sticker.setMeta", [self](const json& p) {
        if (p.contains("tags") && p["tags"].is_array()) {
            self->data.tags.clear();
            for (auto& t : p["tags"])
                if (t.is_string()) self->data.tags.push_back(t.get<std::string>());
        }
        if (p.contains("aiTags") && p["aiTags"].is_array()) {
            self->data.aiTags.clear();
            for (auto& t : p["aiTags"])
                if (t.is_string()) self->data.aiTags.push_back(t.get<std::string>());
        }
        auto setStr = [&](const char* key, std::string& dst) {
            if (p.contains(key) && p[key].is_string()) dst = p[key];
        };
        setStr("title", self->data.title);
        setStr("summary", self->data.summary);
        setStr("titleEn", self->data.titleEn);
        setStr("summaryEn", self->data.summaryEn);
        setStr("transKo", self->data.transKo);
        setStr("transEn", self->data.transEn);
        setStr("srcLang", self->data.srcLang);
        setStr("viewLang", self->data.viewLang);
        if (p.contains("needsReview") && p["needsReview"].is_boolean())
            self->data.needsReview = p["needsReview"];
        self->SaveData();
        return json::object();
    });

    b.Register("sticker.setColor", [self](const json& p) {
        self->SetColor(p.value("color", "yellow"));
        return json::object();
    });

    b.Register("sticker.setTopmost", [self](const json& p) {
        self->SetTopmost(p.value("topmost", false));
        return json::object();
    });

    b.Register("sticker.hide", [self](const json&) {
        self->data.hidden = true;
        self->SaveData();
        self->ShowWin(false, false);
        return json::object();
    });

    b.Register("sticker.delete", [self](const json&) {
        if (!App::ConfirmYesNo(self->hwnd_, App::I().DeleteConfirmKey()))
            return json{{"deleted", false}};
        std::string id = self->data.id;
        App::I().RunOnUi([id]() { App::I().DeleteSticker(id); });
        return json{{"deleted", true}};
    });

    b.Register("window.startDrag", [self](const json&) {
        ReleaseCapture();
        SendMessageW(self->hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return json::object();
    });

    // 페이지가 측정한 헤더/툴바 높이 보고 (자석 정렬 기준 계산에 쓰인다)
    b.Register("window.setUiExtents", [self](const json& p) {
        self->SetUiExtents(p.value("top", 0), p.value("bottom", 0));
        return json::object();
    });

    // UI 자동 숨김: 페이지가 측정한 헤더/툴바 높이(CSS px)만큼 창을 접거나 편다
    b.Register("window.setCollapse", [self](const json& p) {
        int top = p.value("top", 0), bottom = p.value("bottom", 0);
        App::I().RunOnUi([self, top, bottom]() { self->ApplyCollapse(top, bottom); });
        return json::object();
    });

    // kind: "image" | "video" | "3d"(썸네일) — 메모 폴더의 해당 하위 폴더에 저장
    b.Register("attachment.save", [self](const json& p) {
        std::string name = App::I().store.SaveAttachment(
            self->data.id, p.value("dataBase64", ""), p.value("ext", "bin"), p.value("kind", ""));
        if (name.empty()) throw std::runtime_error("attachment save failed");
        self->data.attachments.push_back(name);
        self->SaveData();
        return json{{"name", name}, {"url", AttachmentUrl(self->data.id, name)}};
    });

    b.Register("attachment.pickVideo", [self](const json&) {
        wil::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))))
            throw std::runtime_error("dialog failed");
        COMDLG_FILTERSPEC filters[] = {{L"Video", L"*.mp4;*.webm;*.ogg;*.mov;*.m4v"}};
        dlg->SetFileTypes(1, filters);
        if (FAILED(dlg->Show(self->hwnd_))) return json{{"cancelled", true}};
        wil::com_ptr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return json{{"cancelled", true}};
        wil::unique_cotaskmem_string path;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            throw std::runtime_error("path failed");
        std::string name =
            App::I().store.ImportAttachment(self->data.id, path.get(), "video");
        if (name.empty()) throw std::runtime_error("copy failed");
        self->data.attachments.push_back(name);
        self->SaveData();
        return json{{"name", name}, {"url", AttachmentUrl(self->data.id, name)}};
    });

    self->RegisterTypeBridges();
    App::I().SetupCommonBridge(self->host_);

    self->host_.Create(hwnd, L"https://app.sticker/sticker.html",
                       App::I().MakeInitJson("sticker", d.id),
                       [self]() { self->ApplyUiScale(); });

    // 웹 메모: 상단 스트립 아래를 채우는 자유 탐색 브라우저 뷰
    if (d.type == "web") {
        WebViewHost::Options browserOpts;
        browserOpts.browserMode = true;
        self->siteHost_.onSourceChanged = [self](const std::wstring& uri) {
            std::string u = util::WideToUtf8(uri);
            if (u == "about:blank" || u == self->data.lastUrl) return;
            self->data.lastUrl = u;
            self->SaveData();
            self->host_.PostEvent("web.urlChanged", json{{"url", u}});
        };
        std::string start = !d.lastUrl.empty() ? d.lastUrl : d.url;
        self->siteHost_.Create(hwnd, start.empty() ? L"about:blank" : util::Utf8ToWide(start),
                               json::object(), [self]() { self->ApplyUiScale(); }, browserOpts);
    }

    if (show) self->ShowWin(true, activate);
    return self;
}

void StickerWindow::ShowWin(bool show, bool activate) {
    if (show) {
        // 생성 실패·프로세스 크래시로 비어 있으면 표시 시점에 복구
        host_.EnsureCreated();
        if (data.type == "web") siteHost_.EnsureCreated();
    }
    ShowWindow(hwnd_, show ? (activate ? SW_SHOW : SW_SHOWNA) : SW_HIDE);
    if (show && activate) host_.Focus();
}

void StickerWindow::SetTopmost(bool on) {
    data.topmost = on;
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SaveData();
}

void StickerWindow::SetColor(const std::string& color) {
    data.color = color;
    UpdateBandBrush();
    InvalidateRect(hwnd_, nullptr, TRUE);
    SaveData();
}

void StickerWindow::OnThemeChanged() {
    UpdateBandBrush();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void StickerWindow::SaveData() {
    data.updatedAt = util::WideToUtf8(util::NowIso8601());
    App::I().store.SaveSticker(data);
}

void StickerWindow::Destroy() { DestroyWindow(hwnd_); }

void StickerWindow::UpdateBandBrush() {
    if (bandBrush_) DeleteObject(bandBrush_);
    bool dark = App::I().EffectiveTheme() == "dark";
    COLORREF c = theme::StickerColor(data.color, dark);
    bandBrush_ = CreateSolidBrush(c);
    // DWM 보더를 배경색과 같게 — 아웃라인이 티 나지 않음 (색/테마 변경 시 함께 갱신)
    if (hwnd_) theme::SetWindowBorderColor(hwnd_, c);
}

int StickerWindow::BandPx() const { return MulDiv(kBandDip, dpi_, 96); }

// 접기/펼치기: 부모 창 rect만 200ms 선형으로 애니메이션한다.
// 매 프레임 WM_SIZE → LayoutWebView가 자식 뷰를 화면에 고정하므로 본문은 정지 상태.
void StickerWindow::ApplyCollapse(int topCss, int bottomCss) {
    collapseTopCss_ = topCss;
    collapseBottomCss_ = bottomCss;
    double k = App::I().settings.uiScale * dpi_ / 96.0;  // CSS px → 물리 px
    double toTop = topCss * k, toBottom = bottomCss * k;
    if (fabs(toTop - animCurTop_) < 0.5 && fabs(toBottom - animCurBottom_) < 0.5) return;
    // 현재 rect에서 "펼친 기준" rect를 복원한다.
    // 현재 적용값(animCur*)으로 되돌리므로 애니메이션 중간에 재요청돼도 이어서 자연스럽다.
    RECT r{};
    GetWindowRect(hwnd_, &r);
    animBase_ = r;
    animBase_.top -= (LONG)llround(animCurTop_);
    animBase_.bottom += (LONG)llround(animCurBottom_);
    animFromTop_ = animCurTop_;
    animFromBottom_ = animCurBottom_;
    animToTop_ = toTop;
    animToBottom_ = toBottom;
    animStartTick_ = GetTickCount();
    SetTimer(hwnd_, kCollapseTimerId, 10, nullptr);
    StepCollapseAnim();  // 첫 프레임 즉시 — 페이지 트랜지션과 시작 시점을 맞춘다
}

void StickerWindow::StepCollapseAnim() {
    double p = (GetTickCount() - animStartTick_) / (double)kCollapseAnimMs;
    if (p >= 1.0) p = 1.0;
    // 선형 보간으로 부모 rect만 이동/축소 — SetWindowPos가 동기 발생시키는 WM_SIZE에서
    // LayoutWebView가 같은 틱에 자식 뷰를 화면 고정 위치로 되돌린다 (본문 정지)
    animCurTop_ = animFromTop_ + (animToTop_ - animFromTop_) * p;
    animCurBottom_ = animFromBottom_ + (animToBottom_ - animFromBottom_) * p;
    int top = animBase_.top + (int)llround(animCurTop_);
    int h = (animBase_.bottom - animBase_.top) - (int)llround(animCurTop_) -
            (int)llround(animCurBottom_);
    SetWindowPos(hwnd_, nullptr, animBase_.left, top, animBase_.right - animBase_.left, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (p >= 1.0) KillTimer(hwnd_, kCollapseTimerId);
}

int StickerWindow::CssPx(int cssPx) const {
    return (int)llround(cssPx * App::I().settings.uiScale * dpi_ / 96.0);
}

void StickerWindow::SetUiExtents(int topCss, int bottomCss) {
    uiExtentTopCss_ = topCss;
    uiExtentBottomCss_ = bottomCss;
}

RECT StickerWindow::AlignBasis(const RECT& actual) const {
    RECT r = actual;
    if (!App::I().settings.autoHideUi) return r;  // 숨김을 안 쓰면 지금 모양이 곧 기준
    // 아직 접히지 않은 만큼만 덜어낸다: 이미 접힌 창은 그대로, 펼친 창은 접혔을 때의 모양으로.
    r.top += CssPx(uiExtentTopCss_ - collapseTopCss_);
    r.bottom -= CssPx(uiExtentBottomCss_ - collapseBottomCss_);
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

RECT StickerWindow::AlignRectNow() const {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    return AlignBasis(r);
}

void StickerWindow::StoreGeometryFromWindow() {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    // 애니메이션 중이어도 현재 적용값 기준으로 복원하므로 항상 "펼친 상태"가 저장된다
    data.x = r.left;
    data.y = r.top - (int)llround(animCurTop_);
    data.w = r.right - r.left;
    data.h = (r.bottom - r.top) + (int)llround(animCurTop_) + (int)llround(animCurBottom_);
}

void StickerWindow::ApplyUiScale() {
    double s = App::I().settings.uiScale;
    host_.SetZoomFactor(s);
    if (data.type == "web") siteHost_.SetZoomFactor(s);
    LayoutWebView();  // 웹 스트립 높이가 배율에 따라 달라짐
}

void StickerWindow::LayoutWebView() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int band = BandPx();
    // UI 자동 숨김: 페이지 레이아웃은 절대 바뀌지 않는다. 대신 WebView 자식 창을
    // 항상 "펼친 기준" 위치·크기에 고정(핀)한다 — 부모 창이 위/아래로 줄어도
    // 자식 뷰는 화면에 정지해 있으므로 본문이 1px도 움직일 수 없고,
    // 부모 밖으로 벗어난 부분(페이드 아웃된 헤더/툴바 영역)만 잘려나간다.
    int cTop = (int)llround(animCurTop_);
    int cBottom = (int)llround(animCurBottom_);
    int y0 = rc.top + band - cTop;              // 펼친 기준 상단 (접힘만큼 위로)
    int innerBottom = rc.bottom + cBottom - band;  // 펼친 기준 하단 (접힘만큼 아래로)
    if (data.type == "web") {
        // 상단 스트립(타이틀바 32 + URL바 32 CSS px) = 메인 페이지, 나머지 = 사이트 뷰.
        // 레이아웃이 불변이므로 스트립도 항상 64 — 타이틀바는 부모 밖으로 잘린다.
        int strip = (int)(64.0 * App::I().settings.uiScale * dpi_ / 96.0 + 0.5);
        RECT top{rc.left + band, y0, rc.right - band, min(y0 + strip, innerBottom)};
        RECT bottom{rc.left + band, top.bottom, rc.right - band, innerBottom};
        host_.SetBounds(top);
        siteHost_.SetBounds(bottom);
        return;
    }
    RECT bounds{rc.left + band, y0, rc.right - band, innerBottom};
    host_.SetBounds(bounds);
}

// 타입별(file/web/pdf) 브리지 메서드
void StickerWindow::RegisterTypeBridges() {
    Bridge& b = host_.bridge();
    StickerWindow* self = this;

    // ---------- 파일 메모 ----------
    auto fileList = [self]() {
        json arr = json::array();
        for (auto& f : self->data.files) {
            std::wstring wp = util::Utf8ToWide(f);
            DWORD attrs = GetFileAttributesW(wp.c_str());
            bool exists = attrs != INVALID_FILE_ATTRIBUTES;
            bool isDir = exists && (attrs & FILE_ATTRIBUTE_DIRECTORY);
            const wchar_t* name = PathFindFileNameW(wp.c_str());
            arr.push_back({{"path", f},
                           {"name", util::WideToUtf8(name)},
                           {"isDir", isDir},
                           {"exists", exists}});
        }
        return arr;
    };
    auto addPaths = [self](const std::vector<std::string>& paths) {
        for (auto& p : paths) {
            if (p.empty()) continue;
            if (std::find(self->data.files.begin(), self->data.files.end(), p) ==
                self->data.files.end())
                self->data.files.push_back(p);
        }
        self->SaveData();
    };

    b.Register("files.list", [fileList](const json&) { return fileList(); });

    b.Register("files.setView", [self](const json& p) {
        self->data.fileView = p.value("view", "list");
        self->SaveData();
        return json::object();
    });

    b.Register("files.addDialog", [self, fileList, addPaths](const json& p) {
        bool folders = p.value("folders", false);
        auto picked = PickFilesOrFolders(self->hwnd_, folders);
        std::vector<std::string> utf8;
        for (auto& w : picked) utf8.push_back(util::WideToUtf8(w));
        addPaths(utf8);
        return fileList();
    });

    b.Register("files.addPaths", [fileList, addPaths](const json& p) {
        std::vector<std::string> paths;
        if (p.contains("paths") && p["paths"].is_array()) {
            for (auto& x : p["paths"])
                if (x.is_string()) paths.push_back(x.get<std::string>());
        }
        addPaths(paths);
        return fileList();
    });

    b.Register("files.remove", [self, fileList](const json& p) {
        std::string path = p.value("path", "");
        auto& v = self->data.files;
        v.erase(std::remove(v.begin(), v.end(), path), v.end());
        self->SaveData();
        return fileList();
    });

    b.Register("files.open", [](const json& p) {
        std::wstring path = BackslashPath(util::Utf8ToWide(p.value("path", "")));
        if (!path.empty())
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return json::object();
    });

    b.Register("files.copyClipboard", [self](const json& p) {
        std::vector<std::wstring> paths;
        if (p.contains("paths") && p["paths"].is_array()) {
            for (auto& x : p["paths"])
                if (x.is_string()) paths.push_back(util::Utf8ToWide(x.get<std::string>()));
        }
        CopyFilesToClipboard(self->hwnd_, paths);
        return json{{"count", paths.size()}};
    });

    // 썸네일: 워커 스레드에서 추출해 files.thumb 이벤트로 하나씩 전달
    b.Register("files.requestThumbs", [self](const json& p) {
        std::string requestId = p.value("requestId", "");
        int size = p.value("size", 96);
        std::vector<std::string> paths;
        if (p.contains("paths") && p["paths"].is_array()) {
            for (auto& x : p["paths"])
                if (x.is_string()) paths.push_back(x.get<std::string>());
        }
        std::string stickerId = self->data.id;
        std::thread([requestId, size, paths, stickerId]() {
            // 셸 썸네일 추출은 STA에서 안정적
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (auto& path : paths) {
                std::string dataUrl = ThumbDataUrl(util::Utf8ToWide(path), size);
                App::I().RunOnUi([stickerId, requestId, path, dataUrl]() {
                    if (auto* w = App::I().FindSticker(stickerId)) {
                        w->host().PostEvent("files.thumb", json{{"requestId", requestId},
                                                                {"path", path},
                                                                {"dataUrl", dataUrl}});
                    }
                });
            }
            CoUninitialize();
        }).detach();
        return json::object();
    });

    // ---------- 웹 메모 ----------
    b.Register("web.getState", [self](const json&) {
        return json{{"url", self->data.url}, {"lastUrl", self->data.lastUrl}};
    });

    b.Register("web.navigate", [self](const json& p) {
        std::string url = NormalizeUrl(p.value("url", ""));
        if (!url.empty()) self->siteHost_.Navigate(util::Utf8ToWide(url));
        return json{{"url", url}};
    });

    b.Register("web.setHome", [self](const json& p) {
        std::string url = NormalizeUrl(p.value("url", ""));
        if (!url.empty()) {
            self->data.url = url;
            self->SaveData();
            self->siteHost_.Navigate(util::Utf8ToWide(url));
        }
        return json{{"url", url}};
    });

    b.Register("web.goHome", [self](const json&) {
        if (!self->data.url.empty())
            self->siteHost_.Navigate(util::Utf8ToWide(self->data.url));
        return json::object();
    });

    // 팝오버가 열릴 때 사이트 뷰를 잠시 숨겨 팝오버가 가려지지 않게 함
    b.Register("web.suspendSite", [self](const json& p) {
        self->siteHost_.SetVisible(!p.value("on", false));
        return json::object();
    });

    // ---------- PDF 메모 ----------
    b.Register("pdf.get", [self](const json&) {
        json r = {{"title", self->data.pdfTitle}};
        r["url"] = self->data.pdfName.empty()
                       ? ""
                       : AttachmentUrl(self->data.id, self->data.pdfName);
        return r;
    });

    auto importPdf = [self](const std::wstring& path) -> json {
        std::string name = App::I().store.ImportAttachment(self->data.id, path, "pdf");
        if (name.empty()) throw std::runtime_error("import failed");
        // 이전 PDF 첨부는 교체
        if (!self->data.pdfName.empty()) {
            auto& at = self->data.attachments;
            at.erase(std::remove(at.begin(), at.end(), self->data.pdfName), at.end());
            DeleteFileW((App::I().store.StickerDir(self->data.id) + L"\\" +
                         util::Utf8ToWide(self->data.pdfName))
                            .c_str());
        }
        self->data.pdfName = name;
        self->data.pdfTitle = util::WideToUtf8(PathFindFileNameW(path.c_str()));
        self->data.attachments.push_back(name);
        self->SaveData();
        return json{{"url", AttachmentUrl(self->data.id, name)},
                    {"title", self->data.pdfTitle}};
    };

    b.Register("pdf.pick", [self, importPdf](const json&) -> json {
        wil::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))))
            throw std::runtime_error("dialog failed");
        COMDLG_FILTERSPEC filters[] = {{L"PDF", L"*.pdf"}};
        dlg->SetFileTypes(1, filters);
        if (FAILED(dlg->Show(self->hwnd_))) return json{{"cancelled", true}};
        wil::com_ptr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return json{{"cancelled", true}};
        wil::unique_cotaskmem_string path;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            throw std::runtime_error("path failed");
        return importPdf(path.get());
    });

    b.Register("pdf.setPath", [importPdf](const json& p) -> json {
        std::string path = p.value("path", "");
        if (path.empty() && p.contains("paths") && p["paths"].is_array() &&
            !p["paths"].empty() && p["paths"][0].is_string())
            path = p["paths"][0].get<std::string>();  // 드래그앤드롭 경로
        if (path.empty()) throw std::runtime_error("no path");
        return importPdf(util::Utf8ToWide(path));
    });

    // ---------- 3D 모델 임베드 (rich 메모) ----------
    // 파일을 복사하지 않고 원본 경로를 저장한다. 내용은 model.readFile로 읽어 렌더.
    auto normalizeModelPath = [](std::wstring path) {
        for (auto& c : path)
            if (c == L'/') c = L'\\';
        return path;
    };

    b.Register("model.pick", [self, normalizeModelPath](const json&) -> json {
        wil::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))))
            throw std::runtime_error("dialog failed");
        COMDLG_FILTERSPEC filters[] = {{L"3D Model", L"*.glb;*.gltf;*.obj;*.stl"}};
        dlg->SetFileTypes(1, filters);
        if (FAILED(dlg->Show(self->hwnd_))) return json{{"cancelled", true}};
        wil::com_ptr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) return json{{"cancelled", true}};
        wil::unique_cotaskmem_string path;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            throw std::runtime_error("path failed");
        return json{{"path", util::WideToUtf8(normalizeModelPath(path.get()))}};
    });

    b.Register("model.importPath", [normalizeModelPath](const json& p) -> json {
        std::string path = p.value("path", "");
        if (path.empty() && p.contains("paths") && p["paths"].is_array() &&
            !p["paths"].empty() && p["paths"][0].is_string())
            path = p["paths"][0].get<std::string>();  // 드래그앤드롭 경로
        if (path.empty()) throw std::runtime_error("no path");
        return json{{"path", util::WideToUtf8(normalizeModelPath(util::Utf8ToWide(path)))}};
    });

    // 원본 파일 내용을 base64로 읽어 반환 (뷰어가 three.js 로더에 직접 전달).
    // relative가 오면 base 파일과 같은 폴더 기준으로 해석 — gltf의 .bin·텍스처 로드용.
    b.Register("model.readFile", [normalizeModelPath](const json& p) -> json {
        std::wstring path = normalizeModelPath(util::Utf8ToWide(p.value("path", "")));
        std::string rel = p.value("relative", "");
        if (!rel.empty()) {
            // URI 디코드 후 상대 경로를 base 폴더에 결합 (상위 경로 이탈 방지)
            std::wstring relW = util::Utf8ToWide(util::UriDecode(rel));
            for (auto& c : relW)
                if (c == L'/') c = L'\\';
            if (relW.find(L"..") != std::wstring::npos || relW.find(L':') != std::wstring::npos)
                throw std::runtime_error("invalid relative path");
            size_t slash = path.find_last_of(L'\\');
            std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash);
            path = dir.empty() ? relW : (dir + L"\\" + relW);
        }
        if (path.empty()) throw std::runtime_error("no path");
        auto bytes = util::ReadFileBytes(path);
        if (!bytes) throw std::runtime_error("file not found");
        if (bytes->size() > 256ull * 1024 * 1024) throw std::runtime_error("file too large");
        DWORD b64len = 0;
        CryptBinaryToStringA((const BYTE*)bytes->data(), (DWORD)bytes->size(),
                             CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64len);
        std::string b64(b64len, 0);
        if (!CryptBinaryToStringA((const BYTE*)bytes->data(), (DWORD)bytes->size(),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(),
                                  &b64len))
            throw std::runtime_error("encode failed");
        b64.resize(b64len);
        return json{{"dataBase64", b64}};
    });

    // 원본 파일 위치를 탐색기에서 열고 파일 선택
    b.Register("model.reveal", [normalizeModelPath](const json& p) {
        std::wstring path = normalizeModelPath(util::Utf8ToWide(p.value("path", "")));
        if (path.empty()) throw std::runtime_error("no path");
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        return json::object();
    });
}

LRESULT CALLBACK StickerWindow::SWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    StickerWindow* self;
    if (msg == WM_NCCREATE) {
        self = (StickerWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (StickerWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

LRESULT StickerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp == TRUE) return 0;  // 비클라이언트 프레임 제거 (프레임리스)
            break;

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
            return HTCLIENT;
        }

        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, bandBrush_);
            return 1;
        }

        // DWM NC 렌더링을 끄면(DWMNCRP_DISABLED) 클래식 프레임이 대신 그려져
        // 활성(회색)/비활성(흰색) 아웃라인이 나타난다 — NC 페인트를 완전히 억제.
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, (LPARAM)-1);  // -1: NC 다시 그리지 않음

        case WM_SIZE:
            LayoutWebView();
            return 0;

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

        case WM_TIMER:
            if (wp == kCollapseTimerId) {
                StepCollapseAnim();
                return 0;
            }
            break;

        case WM_ENTERSIZEMOVE:
            // 접기 애니메이션 중 드래그가 시작되면 즉시 목표 상태로 마무리
            // (모달 이동 루프 중의 SetWindowPos가 드래그와 충돌하지 않도록)
            if (animCurTop_ != animToTop_ || animCurBottom_ != animToBottom_) {
                animStartTick_ = GetTickCount() - kCollapseAnimMs;
                StepCollapseAnim();
            }
            GetWindowRect(hwnd, &dragStartRect_);
            return 0;

        case WM_MOVING:
            App::I().SnapStickerRect(this, (RECT*)lp);  // 다른 메모창에 자석처럼 붙이기
            App::I().UpdateDragHover(this);             // 그룹 위 드래그 하이라이트
            return TRUE;  // 보정한 사각형을 적용

        case WM_EXITSIZEMOVE: {
            RECT r{};
            GetWindowRect(hwnd, &r);
            StoreGeometryFromWindow();  // 접힌 상태면 펼친 기준으로 보정해 저장
            SaveData();
            // 크기 변화 없이 위치만 바뀐 순수 이동이면 그룹 드롭 검사
            bool moved = (r.left != dragStartRect_.left || r.top != dragStartRect_.top);
            bool resized = (r.right - r.left != dragStartRect_.right - dragStartRect_.left) ||
                           (r.bottom - r.top != dragStartRect_.bottom - dragStartRect_.top);
            if (moved && !resized) App::I().HandleStickerMoveEnd(this);
            return 0;
        }

        case WM_CLOSE:  // X = 종료가 아니라 숨김
            data.hidden = true;
            SaveData();
            ShowWin(false, false);
            return 0;

        case WM_DESTROY:
            host_.Close();
            siteHost_.Close();
            if (bandBrush_) {
                DeleteObject(bandBrush_);
                bandBrush_ = nullptr;
            }
            App::I().OnStickerDestroyed(this);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete this;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
