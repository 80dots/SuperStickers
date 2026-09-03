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
    HWND hwnd = CreateWindowExW(exStyle, kClassName, L"Super Stickers",
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
        setStr("calAlarms", self->data.calAlarms);  // 캘린더 알람 (네이티브 타이머가 본다)
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

    // 모달 대화상자가 떠 있는 동안에도 타이머·RunOnUi 작업은 계속 돈다(그룹 드롭 250ms 타이머,
    // 설정 창의 삭제, 종료). 그 사이 이 창이 파괴될 수 있으므로 대화상자에서 돌아온 뒤에는
    // self를 만지기 전에 살아 있는지 다시 확인한다.
    const std::string selfId = self->data.id;
    auto stillAlive = [self, selfId]() { return App::I().FindSticker(selfId) == self; };
    b.Register("sticker.delete", [self, selfId](const json&) {
        if (!App::ConfirmYesNo(self->hwnd_, App::I().DeleteConfirmKey()))
            return json{{"deleted", false}};
        App::I().RunOnUi([selfId]() { App::I().DeleteSticker(selfId); });
        return json{{"deleted", true}};
    });

    b.Register("window.startDrag", [self](const json&) {
        ReleaseCapture();
        SendMessageW(self->hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return json::object();
    });

    // UI 자동 숨김 상태 보고 (web 메모 전용 — 사이트 뷰가 타이틀바 자리를 채우도록)
    b.Register("window.setUiHidden", [self](const json& p) {
        bool hidden = p.value("hidden", false);
        App::I().RunOnUi([self, hidden]() {
            if (self->webUiHidden_ == hidden) return;
            self->webUiHidden_ = hidden;
            self->LayoutWebView();
        });
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

    b.Register("attachment.pickVideo", [self, stillAlive](const json&) {
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
        if (!stillAlive()) return json{{"cancelled", true}};
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
    // 컨트롤러 가시성 동기화 — 숨긴 창의 WebView 렌더링(rAF 포함)이 확실히 멈춘다.
    // (표시 시 web 메모의 사이트 뷰도 복구되는데, 팝오버용 web.suspendSite와 겹치는
    //  경우는 '팝오버를 연 채 창을 숨겼다 다시 표시'뿐이라 무시할 수 있는 엣지 케이스)
    host_.SetVisible(show);
    if (data.type == "web") siteHost_.SetVisible(show);
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

void StickerWindow::SetSelectedLook(bool on) {
    if (selected_ == on) return;
    selected_ = on;
    // 창 가장자리 1px(DWM 보더)까지 액센트/배경색으로 맞춰 일체감 유지 (그룹창과 동일)
    if (on) {
        theme::SetWindowBorderColor(hwnd_, RGB(0x63, 0x55, 0xE0));
    } else {
        UpdateBandBrush();  // 원래 메모 색으로 보더 복구
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

int StickerWindow::BandPx() const { return MulDiv(kBandDip, dpi_, 96); }

int StickerWindow::CssPx(int cssPx) const {
    return (int)llround(cssPx * App::I().settings.uiScale * dpi_ / 96.0);
}

void StickerWindow::StoreGeometryFromWindow() {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    data.x = r.left;
    data.y = r.top;
    data.w = r.right - r.left;
    data.h = r.bottom - r.top;
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
    if (data.type == "web") {
        // 상단 스트립(타이틀바 32 + URL바 32 CSS px) = 메인 페이지, 나머지 = 사이트 뷰.
        // UI 숨김 중에는 타이틀바가 레이아웃에서 빠져 URL바(32)만 남는다.
        double stripCss = webUiHidden_ ? 32.0 : 64.0;
        int strip = (int)(stripCss * App::I().settings.uiScale * dpi_ / 96.0 + 0.5);
        RECT top{rc.left + band, rc.top + band, rc.right - band,
                 min(rc.top + band + strip, rc.bottom - band)};
        RECT bottom{rc.left + band, top.bottom, rc.right - band, rc.bottom - band};
        host_.SetBounds(top);
        siteHost_.SetBounds(bottom);
        return;
    }
    RECT bounds{rc.left + band, rc.top + band, rc.right - band, rc.bottom - band};
    host_.SetBounds(bounds);
}

// 타입별(file/web/pdf) 브리지 메서드
void StickerWindow::RegisterTypeBridges() {
    Bridge& b = host_.bridge();
    StickerWindow* self = this;
    // 대화상자에서 돌아온 뒤 self가 아직 살아 있는지 (Create의 같은 이름 람다와 같은 이유)
    const std::string selfId = self->data.id;
    auto stillAlive = [self, selfId]() { return App::I().FindSticker(selfId) == self; };

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

    b.Register("files.addDialog", [self, fileList, addPaths, stillAlive](const json& p) {
        bool folders = p.value("folders", false);
        auto picked = PickFilesOrFolders(self->hwnd_, folders);
        if (!stillAlive()) return json::object();
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
        // 파일 메모에 등록된 경로만 연다 — 페이지가 준 임의 경로를 ShellExecute하면 본문에
        // 섞여 들어온 스크립트가 실행 파일을 띄우는 통로가 된다
        if (!App::I().IsRegisteredFilePath(path)) throw std::runtime_error("not a memo file");
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

    b.Register("pdf.pick", [self, importPdf, stillAlive](const json&) -> json {
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
        if (!stillAlive()) return json{{"cancelled", true}};
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

    b.Register("model.pick", [self, normalizeModelPath, stillAlive](const json&) -> json {
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
        if (!stillAlive()) return json{{"cancelled", true}};
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
        {
            // 3D 모델과 그 부속(gltf의 .bin·텍스처)만. 페이지 스크립트가 임의 파일을 읽어 가는
            // 통로가 되지 않도록 확장자를 제한한다.
            static const wchar_t* kAllowed[] = {L".glb", L".gltf", L".obj", L".stl", L".mtl",
                                                L".bin", L".png", L".jpg", L".jpeg", L".webp",
                                                L".ktx2", L".hdr", L".exr", L".basis"};
            std::wstring ext = PathFindExtensionW(path.c_str());
            for (auto& c : ext) c = (wchar_t)towlower(c);
            bool ok = false;
            for (auto* a : kAllowed) ok = ok || ext == a;
            if (!ok) throw std::runtime_error("unsupported file type");
        }
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
    // ---------- 일반 메모에 넣는 파일·폴더 ----------
    // 파일 메모의 기능을 리치 본문에서도 쓴다. 본문에는 링크(원본 경로)나
    // 복사본(메모 폴더 안 File/…)이 <div class="mfile">로 들어간다.
    b.Register("memofile.pick", [self](const json& p) {
        auto picked = PickFilesOrFolders(self->hwnd_, p.value("folders", false));
        json arr = json::array();
        for (auto& w : picked) arr.push_back(util::WideToUtf8(w));
        return json{{"paths", arr}};
    });

    // 드롭·붙여넣기로 들어온 File 객체의 전체 경로 (WebViewHost가 params.paths로 합쳐 준다)
    b.Register("memofile.dropPaths", [](const json& p) {
        json arr = json::array();
        if (p.contains("paths") && p["paths"].is_array())
            for (auto& x : p["paths"])
                if (x.is_string()) arr.push_back(x);
        return json{{"paths", arr}};
    });

    // 클립보드의 파일 목록 (탐색기에서 복사한 것). File 객체 경로가 막힌 경로를 우회한다.
    b.Register("memofile.clipboardPaths", [self](const json&) {
        json arr = json::array();
        if (!OpenClipboard(self->hwnd_)) return json{{"paths", arr}};
        HANDLE h = GetClipboardData(CF_HDROP);
        if (h) {
            auto drop = (HDROP)h;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH * 2]{};
                if (DragQueryFileW(drop, i, buf, MAX_PATH * 2)) arr.push_back(util::WideToUtf8(buf));
            }
        }
        CloseClipboard();
        return json{{"paths", arr}};
    });

    // 링크로 넣을지 복사본으로 넣을지 묻는다. 폴더는 복사본을 만들지 않는다(통째로 복제해야 한다).
    b.Register("memofile.askKind", [self](const json& p) {
        bool isDir = p.value("isDir", false);
        int count = p.value("count", 1);
        if (isDir) return json{{"kind", "link"}};
        TASKDIALOGCONFIG tdc{};
        tdc.cbSize = sizeof(tdc);
        tdc.hwndParent = self->hwnd_;
        tdc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_POSITION_RELATIVE_TO_WINDOW |
                      TDF_ALLOW_DIALOG_CANCELLATION;
        tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        std::wstring title = App::I().i18n.T("mf.askTitle");
        std::wstring head = App::I().i18n.T("mf.askHead");
        if (count > 1) head += L" (" + std::to_wstring(count) + L")";
        tdc.pszWindowTitle = L"Super Stickers";
        tdc.pszMainInstruction = head.c_str();
        std::wstring body = App::I().i18n.T("mf.askBody");
        tdc.pszContent = body.c_str();
        std::wstring linkTxt = App::I().i18n.T("mf.askLink");
        std::wstring copyTxt = App::I().i18n.T("mf.askCopy");
        TASKDIALOG_BUTTON btns[2] = {{101, linkTxt.c_str()}, {102, copyTxt.c_str()}};
        tdc.pButtons = btns;
        tdc.cButtons = 2;
        int pressed = 0;
        if (FAILED(TaskDialogIndirect(&tdc, &pressed, nullptr, nullptr)))
            return json{{"kind", "link"}};
        return json{{"kind", pressed == 102 ? "copy" : pressed == 101 ? "link" : "cancel"}};
    });

    // 원본을 메모 폴더로 복사한다 (File/<guid>.<ext>)
    b.Register("memofile.copyIn", [self](const json& p) {
        std::wstring src = BackslashPath(util::Utf8ToWide(p.value("path", "")));
        if (src.empty()) throw std::runtime_error("no path");
        std::string rel = App::I().store.ImportAttachment(self->data.id, src, "file");
        if (rel.empty()) throw std::runtime_error("copy failed");
        return json{{"rel", rel},
                    {"name", util::WideToUtf8(PathFindFileNameW(src.c_str()))}};
    });

    // 링크가 살아 있는지 (없어진 링크는 페이지가 깨진 표시를 한다)
    b.Register("memofile.exists", [](const json& p) {
        json out = json::object();
        if (p.contains("paths") && p["paths"].is_array()) {
            for (auto& x : p["paths"]) {
                if (!x.is_string()) continue;
                std::wstring w = BackslashPath(util::Utf8ToWide(x.get<std::string>()));
                out[x.get<std::string>()] =
                    GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
            }
        }
        return out;
    });

    // 메모 폴더 안의 복사본을 연결된 앱으로 연다
    b.Register("memofile.openCopy", [self](const json& p) {
        std::string rel = p.value("rel", "");
        if (rel.empty()) throw std::runtime_error("no rel");
        for (auto& c : rel) if (c == '/') c = '\\';
        std::wstring path = App::I().store.StickerDir(self->data.id) + L"\\" +
                            util::Utf8ToWide(rel);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("file not found");
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return json::object();
    });

    b.Register("memofile.reveal", [](const json& p) {
        std::wstring path = BackslashPath(util::Utf8ToWide(p.value("path", "")));
        if (path.empty()) throw std::runtime_error("no path");
        if (!App::I().IsRegisteredFilePath(path)) throw std::runtime_error("not a memo file");
        std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        return json::object();
    });

    // 없어진 링크를 열려고 할 때
    b.Register("memofile.notFound", [self](const json& p) {
        std::wstring path = util::Utf8ToWide(p.value("path", ""));
        std::wstring msg = App::I().i18n.T("mf.notFound") + L"\n\n" + path;
        MessageBoxW(self->hwnd_, msg.c_str(), L"Super Stickers",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return json::object();
    });

    // 링크를 지울 때의 안내 ("원본은 지워지지 않습니다"). 다시 보지 않기를 체크하면 설정에 남는다.
    b.Register("memofile.linkDeleteNotice", [self](const json&) {
        if (App::I().settings.hideLinkDeleteNotice) return json{{"shown", false}};
        TASKDIALOGCONFIG tdc{};
        tdc.cbSize = sizeof(tdc);
        tdc.hwndParent = self->hwnd_;
        tdc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
        tdc.dwCommonButtons = TDCBF_OK_BUTTON;
        tdc.pszWindowTitle = L"Super Stickers";
        std::wstring head = App::I().i18n.T("mf.linkDelHead");
        std::wstring body = App::I().i18n.T("mf.linkDelBody");
        std::wstring verify = App::I().i18n.T("mf.dontShowAgain");
        tdc.pszMainInstruction = head.c_str();
        tdc.pszContent = body.c_str();
        tdc.pszVerificationText = verify.c_str();
        int pressed = 0;
        BOOL checked = FALSE;
        TaskDialogIndirect(&tdc, &pressed, nullptr, &checked);
        if (checked) {
            App::I().settings.hideLinkDeleteNotice = true;
            App::I().store.SaveSettings(App::I().settings);
        }
        return json{{"shown", true}};
    });

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
            HDC dc = (HDC)wp;
            FillRect(dc, &rc, bandBrush_);
            if (selected_) {
                // 그룹창에 메모를 드롭할 때의 하이라이트와 동일 (색·두께·모서리 반지름)
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

        // DWM NC 렌더링을 끄면(DWMNCRP_DISABLED) 클래식 프레임이 대신 그려져
        // 활성(회색)/비활성(흰색) 아웃라인이 나타난다 — NC 페인트를 완전히 억제.
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, (LPARAM)-1);  // -1: NC 다시 그리지 않음

        case WM_SIZE:
            LayoutWebView();
            // 선택 테두리는 창 가장자리를 따라 그려지므로, 크기가 바뀌면 새로 드러난
            // 부분만 다시 그려져 테두리가 점선처럼 끊긴다. 선택 중일 때는 클라이언트
            // 전체를 다시 칠해 테두리를 이어 준다.
            if (selected_) InvalidateRect(hwnd, nullptr, TRUE);
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

        case WM_ENTERSIZEMOVE:
            GetWindowRect(hwnd, &dragStartRect_);
            GetCursorPos(&dragStartCursor_);
            inSizeMove_ = true;
            // 다중 선택 상태에서 선택된 창을 잡으면 나머지 선택 창도 같은 delta로 따라온다
            dragPeers_.clear();
            if (App::I().IsSelected(data.id) && App::I().HasMultiSelection()) {
                for (auto& id : App::I().Selection()) {
                    if (id == data.id) continue;
                    auto* peer = App::I().FindSticker(id);
                    if (!peer || !peer->VisibleNow()) continue;
                    RECT pr{};
                    GetWindowRect(peer->hwnd(), &pr);
                    dragPeers_.push_back({peer, POINT{pr.left, pr.top}});
                }
            }
            return 0;

        case WM_MOVING: {
            RECT* pr = (RECT*)lp;
            // 자석이 창을 옮기면 이동 루프의 기준 사각형도 그 위치로 바뀐다.
            // 그대로 두면 다음 제안이 "붙은 위치 + 직전 메시지의 작은 이동량"이 되어
            // 항상 임계값 안에 머물고, 아무리 멀리 끌어도 떨어지지 않는다(실측).
            // 그래서 커서만 따라가는 자유 위치를 매번 새로 계산해 자석의 입력으로 준다.
            POINT c{};
            GetCursorPos(&c);
            // 커서가 움직였으면 마우스 드래그, 그대로면 키보드 이동(Alt+Space → 이동)이라
            // 제안 좌표를 그대로 존중한다.
            if (inSizeMove_ && (c.x != dragStartCursor_.x || c.y != dragStartCursor_.y)) {
                int w = pr->right - pr->left, h = pr->bottom - pr->top;
                pr->left = dragStartRect_.left + (c.x - dragStartCursor_.x);
                pr->top = dragStartRect_.top + (c.y - dragStartCursor_.y);
                pr->right = pr->left + w;
                pr->bottom = pr->top + h;
            }
            App::I().SnapStickerRect(this, pr);  // 다른 메모창에 자석처럼 붙이기
            // 선택된 다른 창들을 같은 이동량만큼 옮긴다 (상대 위치 = 레이아웃 유지)
            if (!dragPeers_.empty()) {
                int dx = pr->left - dragStartRect_.left;
                int dy = pr->top - dragStartRect_.top;
                for (auto& [peer, start] : dragPeers_) {
                    SetWindowPos(peer->hwnd(), nullptr, start.x + dx, start.y + dy, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            App::I().UpdateDragHover(this);      // 그룹 위 드래그 하이라이트
            return TRUE;  // 보정한 사각형을 적용
        }

        case WM_SIZING: {
            RECT* pr = (RECT*)lp;
            const int edge = (int)wp;
            // WM_MOVING과 같은 이유로(위 주석 참고) 자석이 보정한 좌표가 다음 제안의
            // 기준이 되면 변이 그 자리에 고착된다. 잡고 있는 변은 커서만 따라가는
            // 자유 좌표로 매번 다시 계산해 자석의 입력으로 준다.
            POINT c{};
            GetCursorPos(&c);
            if (inSizeMove_ && (c.x != dragStartCursor_.x || c.y != dragStartCursor_.y)) {
                const int dx = c.x - dragStartCursor_.x, dy = c.y - dragStartCursor_.y;
                if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
                    pr->left = dragStartRect_.left + dx;
                if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT)
                    pr->right = dragStartRect_.right + dx;
                if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
                    pr->top = dragStartRect_.top + dy;
                if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT)
                    pr->bottom = dragStartRect_.bottom + dy;
            }
            App::I().SnapStickerResize(this, pr, edge);  // 인접 창의 변에 자석처럼 맞추기
            // 자유 좌표를 직접 넣으면 DefWindowProc의 최소 크기 제한을 지나치게 되므로
            // 여기서 다시 지킨다 (WM_GETMINMAXINFO와 같은 계산).
            const double sc = App::I().settings.uiScale;
            const int minW = (int)(kMinWDip * sc * dpi_ / 96.0);
            const int minH = (int)(kMinHDip * sc * dpi_ / 96.0);
            if (pr->right - pr->left < minW) {
                if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
                    pr->left = pr->right - minW;
                else
                    pr->right = pr->left + minW;
            }
            if (pr->bottom - pr->top < minH) {
                if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
                    pr->top = pr->bottom - minH;
                else
                    pr->bottom = pr->top + minH;
            }
            return TRUE;  // 보정한 사각형을 적용
        }

        case WM_EXITSIZEMOVE: {
            inSizeMove_ = false;
            RECT r{};
            GetWindowRect(hwnd, &r);
            StoreGeometryFromWindow();
            SaveData();
            for (auto& [peer, start] : dragPeers_) {  // 함께 움직인 창들도 저장
                peer->StoreGeometryFromWindow();
                peer->SaveData();
            }
            dragPeers_.clear();
            // 크기 변화 없이 위치만 바뀐 순수 이동이면 그룹 드롭 검사
            bool moved = (r.left != dragStartRect_.left || r.top != dragStartRect_.top);
            bool resized = (r.right - r.left != dragStartRect_.right - dragStartRect_.left) ||
                           (r.bottom - r.top != dragStartRect_.bottom - dragStartRect_.top);
            if (moved && !resized) App::I().HandleStickerMoveEnd(this);
            return 0;
        }

        case WM_ACTIVATEAPP:
            // 다른 앱이나 바탕화면을 클릭해 우리 앱을 벗어나면 선택을 푼다
            if (wp == FALSE) App::I().ClearSelection();
            return 0;

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
