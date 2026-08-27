#include "WebViewHost.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>

#include <vector>

#include "App.h"
#include "Utils.h"

using Microsoft::WRL::Callback;
using json = nlohmann::json;

namespace {

wil::com_ptr<ICoreWebView2Environment> g_env;
bool g_envCreating = false;
std::vector<std::function<void(HRESULT)>> g_envWaiters;

bool IsHttpUrl(const std::wstring& uri) {
    return uri.rfind(L"http://", 0) == 0 || uri.rfind(L"https://", 0) == 0;
}

const wchar_t* MimeForPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L"" : path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L"png") return L"image/png";
    if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
    if (ext == L"gif") return L"image/gif";
    if (ext == L"webp") return L"image/webp";
    if (ext == L"svg") return L"image/svg+xml";
    if (ext == L"bmp") return L"image/bmp";
    if (ext == L"pdf") return L"application/pdf";
    if (ext == L"mp4" || ext == L"m4v") return L"video/mp4";
    if (ext == L"webm") return L"video/webm";
    if (ext == L"ogg" || ext == L"ogv") return L"video/ogg";
    if (ext == L"mov") return L"video/quicktime";
    if (ext == L"json" || ext == L"gltf") return L"application/json";
    return L"application/octet-stream";
}

// https://data.sticker/<상대경로> → 데이터 폴더의 파일을 직접 응답한다.
// (가상 호스트 매핑은 교차 출처 하위 리소스(img 등)가 차단되고 커스텀 데이터 폴더도
//  반영되지 않아, 리소스 요청을 가로채 처리한다)
void ServeDataRequest(ICoreWebView2WebResourceRequestedEventArgs* args) {
    wil::com_ptr<ICoreWebView2WebResourceRequest> req;
    if (FAILED(args->get_Request(&req)) || !req) return;
    wil::unique_cotaskmem_string uriRaw;
    if (FAILED(req->get_Uri(&uriRaw)) || !uriRaw) return;

    std::wstring uri = uriRaw.get();
    const std::wstring prefix = L"https://data.sticker/";
    if (uri.rfind(prefix, 0) != 0) return;
    std::wstring rel = uri.substr(prefix.size());
    size_t cut = rel.find_first_of(L"?#");
    if (cut != std::wstring::npos) rel = rel.substr(0, cut);
    rel = util::Utf8ToWide(util::UriDecode(util::WideToUtf8(rel)));
    for (auto& c : rel)
        if (c == L'/') c = L'\\';

    wil::com_ptr<ICoreWebView2WebResourceResponse> resp;
    bool bad = rel.empty() || rel.find(L"..") != std::wstring::npos ||
               rel.find(L':') != std::wstring::npos;
    wil::com_ptr<IStream> stream;
    if (!bad) {
        std::wstring full = App::I().store.AppDir() + L"\\" + rel;
        SHCreateStreamOnFileEx(full.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE,
                               nullptr, &stream);
        if (stream) {
            std::wstring headers = std::wstring(L"Content-Type: ") + MimeForPath(rel) +
                                   L"\r\nAccess-Control-Allow-Origin: *"
                                   L"\r\nCache-Control: no-cache";
            g_env->CreateWebResourceResponse(stream.get(), 200, L"OK", headers.c_str(), &resp);
        }
    }
    if (!resp) {
        g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found",
                                         L"Access-Control-Allow-Origin: *", &resp);
    }
    if (resp) args->put_Response(resp.get());
}

}  // namespace

void WebViewHost::EnsureEnvironment(std::function<void(HRESULT)> done) {
    if (g_env) {
        done(S_OK);
        return;
    }
    g_envWaiters.push_back(std::move(done));
    if (g_envCreating) return;
    g_envCreating = true;

    wchar_t* local = nullptr;
    std::wstring userDataDir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        userDataDir = std::wstring(local) + L"\\SuperSticker\\WebView2";
        CoTaskMemFree(local);
    }

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (SUCCEEDED(hr)) g_env = env;
                g_envCreating = false;
                auto waiters = std::move(g_envWaiters);
                g_envWaiters.clear();
                for (auto& w : waiters) w(hr);
                return S_OK;
            })
            .Get());
}

void WebViewHost::Create(HWND hwnd, const std::wstring& url, const json& initJson,
                         std::function<void()> onReady, Options opts) {
    hostHwnd_ = hwnd;
    url_ = url;
    init_ = initJson;
    onReady_ = std::move(onReady);
    opts_ = opts;
    createAttempts_ = 0;
    CreateInternal();
}

// 컨트롤러가 없으면(생성 실패·프로세스 종료로 비어 버린 창) 다시 만든다
void WebViewHost::EnsureCreated() {
    if (controller_ || !hostHwnd_) return;
    createAttempts_ = 0;
    if (g_env) {
        CreateInternal();
    } else {
        EnsureEnvironment([this](HRESULT hr) {
            if (SUCCEEDED(hr)) CreateInternal();
        });
    }
}

void WebViewHost::CreateInternal() {
    if (!g_env || !hostHwnd_) return;
    const std::wstring url = url_;
    const json initJson = init_;
    auto onReady = onReady_;
    Options opts = opts_;

    g_env->CreateCoreWebView2Controller(
        hostHwnd_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, url, initJson, onReady, opts](HRESULT hr,
                                                 ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(hr) || !controller) {
                    // 조용히 빈 창으로 남지 않도록 잠시 후 재시도 (최대 3회)
                    if (++createAttempts_ <= 3) {
                        App::I().RunOnUiDelayed(700, [this]() { CreateInternal(); });
                    }
                    return S_OK;
                }
                createAttempts_ = 0;
                controller_ = controller;
                controller_->get_CoreWebView2(&webview_);
                if (!webview_) return S_OK;

                // 렌더러/브라우저 프로세스가 죽으면 창이 영구히 비어 버리므로 자동 복구
                // (WebGL 3D 렌더링 등에서 GPU·렌더러 크래시가 발생할 수 있음)
                EventRegistrationToken procToken{};
                webview_->add_ProcessFailed(
                    Callback<ICoreWebView2ProcessFailedEventHandler>(
                        [this](ICoreWebView2*,
                               ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                            COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                                COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED;
                            if (args) args->get_ProcessFailedKind(&kind);
                            if (kind ==
                                COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                                // 환경 전체가 종료됨 — 컨트롤러를 버리고 새 환경에서 재생성
                                controller_ = nullptr;
                                webview_ = nullptr;
                                g_env = nullptr;
                                App::I().RunOnUiDelayed(500, [this]() { EnsureCreated(); });
                            } else if (webview_) {
                                webview_->Reload();  // 렌더러만 죽은 경우 페이지 복구
                            }
                            return S_OK;
                        })
                        .Get(),
                    &procToken);

                if (opts.transparentBg) {
                    if (auto c2 = controller_.try_query<ICoreWebView2Controller2>()) {
                        COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
                        c2->put_DefaultBackgroundColor(transparent);
                    }
                }

                wil::com_ptr<ICoreWebView2Settings> settings;
                webview_->get_Settings(&settings);
                if (settings) {
                    // 브라우저 모드(웹 메모)는 일반 웹서핑 UX 유지
                    settings->put_AreDefaultContextMenusEnabled(opts.browserMode ? TRUE : FALSE);
                    settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(opts.browserMode ? TRUE : FALSE);
#ifdef SS_DEBUG
                    settings->put_AreDevToolsEnabled(TRUE);
#else
                    settings->put_AreDevToolsEnabled(FALSE);
#endif
                    if (auto s3 = settings.try_query<ICoreWebView2Settings3>()) {
                        s3->put_AreBrowserAcceleratorKeysEnabled(opts.browserMode ? TRUE : FALSE);
                    }
                }

                if (!opts.browserMode) {
                    // 가상 호스트: UI 자산 / 사용자 데이터(첨부)
                    if (auto wv3 = webview_.try_query<ICoreWebView2_3>()) {
                        wv3->SetVirtualHostNameToFolderMapping(
                            L"app.sticker", util::GetUiDir().c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                    }
                    // 첨부(data.sticker)는 요청을 가로채 직접 응답 — 교차 출처 하위
                    // 리소스 차단을 피하고 커스텀 데이터 폴더도 반영된다
                    webview_->AddWebResourceRequestedFilter(
                        L"https://data.sticker/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    EventRegistrationToken resToken{};
                    webview_->add_WebResourceRequested(
                        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                            [](ICoreWebView2*,
                               ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                ServeDataRequest(args);
                                return S_OK;
                            })
                            .Get(),
                        &resToken);

                    std::wstring init =
                        L"window.__init = " + util::Utf8ToWide(initJson.dump()) + L";";
                    webview_->AddScriptToExecuteOnDocumentCreated(init.c_str(), nullptr);

                    webview_->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [this](ICoreWebView2*,
                                   ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                wil::unique_cotaskmem_string msg;
                                if (FAILED(args->get_WebMessageAsJson(&msg)) || !msg) return S_OK;
                                std::string msgStr = util::WideToUtf8(msg.get());

                                // postMessageWithAdditionalObjects로 전달된 File 객체에서
                                // 전체 경로를 꺼내 params.paths로 합침 (탐색기 드래그앤드롭)
                                std::vector<std::string> extraPaths;
                                if (auto args2 =
                                        wil::com_ptr<ICoreWebView2WebMessageReceivedEventArgs>(
                                            args)
                                            .try_query<
                                                ICoreWebView2WebMessageReceivedEventArgs2>()) {
                                    wil::com_ptr<ICoreWebView2ObjectCollectionView> objs;
                                    args2->get_AdditionalObjects(&objs);
                                    UINT32 count = 0;
                                    if (objs) objs->get_Count(&count);
                                    for (UINT32 i = 0; i < count; i++) {
                                        wil::com_ptr<IUnknown> unk;
                                        if (FAILED(objs->GetValueAtIndex(i, &unk)) || !unk)
                                            continue;
                                        if (auto file =
                                                unk.try_query<ICoreWebView2File>()) {
                                            wil::unique_cotaskmem_string path;
                                            if (SUCCEEDED(file->get_Path(&path)) && path)
                                                extraPaths.push_back(
                                                    util::WideToUtf8(path.get()));
                                        }
                                    }
                                }
                                if (!extraPaths.empty()) {
                                    nlohmann::json req =
                                        nlohmann::json::parse(msgStr, nullptr, false);
                                    if (req.is_object()) {
                                        if (!req.contains("params") || !req["params"].is_object())
                                            req["params"] = nlohmann::json::object();
                                        req["params"]["paths"] = extraPaths;
                                        msgStr = req.dump();
                                    }
                                }

                                std::string resp = bridge_.HandleMessage(msgStr);
                                if (!resp.empty() && webview_) {
                                    webview_->PostWebMessageAsJson(
                                        util::Utf8ToWide(resp).c_str());
                                }
                                return S_OK;
                            })
                            .Get(),
                        nullptr);

                    // 외부 링크는 기본 브라우저로
                    webview_->add_NavigationStarting(
                        Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [](ICoreWebView2*,
                               ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                wil::unique_cotaskmem_string uri;
                                args->get_Uri(&uri);
                                std::wstring u = uri ? uri.get() : L"";
                                if (u.rfind(L"https://app.sticker/", 0) == 0) return S_OK;
                                args->put_Cancel(TRUE);
                                if (IsHttpUrl(u)) {
                                    ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr,
                                                  SW_SHOWNORMAL);
                                }
                                return S_OK;
                            })
                            .Get(),
                        nullptr);

                    webview_->add_NewWindowRequested(
                        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                            [](ICoreWebView2*,
                               ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                args->put_Handled(TRUE);
                                wil::unique_cotaskmem_string uri;
                                args->get_Uri(&uri);
                                if (uri && IsHttpUrl(uri.get())) {
                                    ShellExecuteW(nullptr, L"open", uri.get(), nullptr, nullptr,
                                                  SW_SHOWNORMAL);
                                }
                                return S_OK;
                            })
                            .Get(),
                        nullptr);
                } else {
                    // 브라우저 모드: 새 창 요청은 같은 뷰에서 열고, 페이지 이동을 통지
                    webview_->add_NewWindowRequested(
                        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                            [this](ICoreWebView2* wv,
                                   ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                args->put_Handled(TRUE);
                                wil::unique_cotaskmem_string uri;
                                args->get_Uri(&uri);
                                if (uri) wv->Navigate(uri.get());
                                return S_OK;
                            })
                            .Get(),
                        nullptr);

                    webview_->add_SourceChanged(
                        Callback<ICoreWebView2SourceChangedEventHandler>(
                            [this](ICoreWebView2* wv,
                                   ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                wil::unique_cotaskmem_string uri;
                                wv->get_Source(&uri);
                                if (uri && onSourceChanged) onSourceChanged(uri.get());
                                return S_OK;
                            })
                            .Get(),
                        nullptr);
                }

                if (onReady) onReady();  // 소유 창이 여기서 SetBounds 수행
                if (!url.empty()) webview_->Navigate(url.c_str());
                return S_OK;
            })
            .Get());
}

void WebViewHost::SetVisible(bool visible) {
    if (controller_) controller_->put_IsVisible(visible ? TRUE : FALSE);
}

void WebViewHost::Navigate(const std::wstring& url) {
    if (webview_) webview_->Navigate(url.c_str());
}

void WebViewHost::SetBounds(const RECT& r) {
    if (controller_) controller_->put_Bounds(r);
}

void WebViewHost::SetZoomFactor(double zoom) {
    if (controller_) controller_->put_ZoomFactor(zoom);
}

void WebViewHost::PostEvent(const std::string& event, const json& data) {
    if (!webview_) return;
    json j = {{"event", event}, {"data", data}};
    webview_->PostWebMessageAsJson(util::Utf8ToWide(j.dump()).c_str());
}

void WebViewHost::Focus() {
    if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebViewHost::Close() {
    if (controller_) {
        controller_->Close();
        controller_ = nullptr;
        webview_ = nullptr;
    }
}
