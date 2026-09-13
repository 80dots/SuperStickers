#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include <json.hpp>
#include <wil/com.h>
#include <WebView2.h>

#include "Bridge.h"

// WebView2 컨트롤러 래퍼. 환경(env)은 프로세스에서 1개를 공유한다.
// 주의: 모든 WebView2 API는 UI 스레드에서만 호출할 것.
class WebViewHost {
public:
    // 공유 환경 준비 (이미 준비됐으면 즉시 콜백)
    static void EnsureEnvironment(std::function<void(HRESULT)> done);

    // 웹뷰 생성 옵션
    struct Options {
        bool transparentBg = false;  // 기본 배경 투명 (페이지 알파 픽셀이 창 뒤를 보이게)
        bool browserMode = false;    // 자유 탐색 브라우저 (내비게이션 제한·브리지 없음)
        // 창을 숨긴 채로 만들 때는 WebView를 만들지 않는다. WebView 하나마다 렌더러 프로세스가
        // 하나씩(수십 MB) 붙으므로, 바탕화면에 없는 메모는 그 비용을 내지 않는다.
        // 표시할 때 ShowWin이 부르는 EnsureCreated()가 그때 만든다.
        bool deferUntilShown = false;
    };

    // hwnd 클라이언트에 WebView2 생성 후 url 로드.
    // initJson은 window.__init 으로 문서 생성 시 주입된다.
    void Create(HWND hwnd, const std::wstring& url, const nlohmann::json& initJson,
                std::function<void()> onReady, Options opts = {});

    // 생성 실패·프로세스 크래시로 비어 버린 창 복구 (창을 표시할 때 확인)
    void EnsureCreated();

    // 지연 생성·재생성 시 init JSON을 그 시점에 다시 만든다. Create()에 넘긴 값은 만들 때
    // 굳어 버리는데, 그 사이 테마·언어·서체가 바뀔 수 있다 (숨은 메모는 몇 시간 뒤에 열린다).
    void SetInitProvider(std::function<nlohmann::json()> fn) { initProvider_ = std::move(fn); }

    void SetBounds(const RECT& r);
    void SetVisible(bool visible);
    // UI Scale: 브라우저 줌으로 페이지 전체(CSS px)를 DPI 변경처럼 일괄 스케일
    void SetZoomFactor(double zoom);
    void Navigate(const std::wstring& url);
    void PostEvent(const std::string& event, const nlohmann::json& data);
    // 이미 직렬화한 {"event",...} JSON을 그대로 전송 — 브로드캐스트 시 창마다
    // 다시 직렬화하지 않도록 App::BroadcastEvent가 사용한다
    void PostEventRaw(const std::wstring& payload);
    void Focus();
    void Close();
    ~WebViewHost() { Close(); }
    bool Ready() const { return webview_ != nullptr; }

    // 브라우저 모드: 페이지 이동 시 호출 (마지막 페이지 저장용)
    std::function<void(const std::wstring&)> onSourceChanged;

    Bridge& bridge() { return bridge_; }

private:
    void CreateInternal();  // 실제 컨트롤러 생성 (재시도·재생성 공용)

    wil::com_ptr<ICoreWebView2Controller> controller_;
    wil::com_ptr<ICoreWebView2> webview_;
    Bridge bridge_;

    // 재생성을 위해 보관하는 생성 파라미터
    HWND hostHwnd_ = nullptr;
    std::wstring url_;
    nlohmann::json init_;
    std::function<nlohmann::json()> initProvider_;  // 있으면 init_ 대신 생성 시점에 호출
    std::function<void()> onReady_;
    Options opts_{};
    int createAttempts_ = 0;
    // 비동기 콜백(컨트롤러 생성·재시도·환경 준비)이 도착했을 때 이 객체가 아직 살아 있는지.
    // 창은 생성 완료 전에도 파괴될 수 있다(그룹에 드롭, 삭제, 종료) — raw this를 잡은 콜백이
    // 해제된 메모리에 쓰지 않도록 토큰으로 확인한다. Close()가 무효화한다.
    std::shared_ptr<bool> alive_;
};
