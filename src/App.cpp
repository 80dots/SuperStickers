#include "App.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <wil/com.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <set>
#include <thread>
#include <vector>

#include "Autostart.h"
#include "GroupWindow.h"
#include "ManagerWindow.h"
#include "WizardWindow.h"
#include "StickerWindow.h"
#include "Theme.h"
#include "LocalAi.h"
#include "Utils.h"
#include "Version.h"  // 빌드 시 CMake가 생성 (원본: src/Version.h.in)
#include "WebViewHost.h"
#include "resource.h"

using json = nlohmann::json;

namespace {
const wchar_t* kAppClassName = L"SuperStickerApp";
constexpr UINT_PTR kQuitTimerId = 1;
constexpr UINT_PTR kTrashTimerId = 2;
constexpr UINT_PTR kTrayClickTimerId = 3;  // 트레이 단일/더블클릭 구분용
constexpr UINT kTrashPurgeIntervalMs = 60 * 60 * 1000;  // 1시간마다 만료 항목 정리
constexpr UINT_PTR kAlarmTimerId = 5;
// 전역 단축키 id (RegisterHotKey). 창 하나에서만 쓰므로 작은 수로 충분하다.
constexpr int kHotkeyToggleId = 1;
constexpr int kHotkeyNewId = 2;
constexpr int kHotkeyListId = 3;
constexpr int kHotkeyArrangeLeftId = 4;
constexpr int kHotkeyArrangeRightId = 5;
constexpr int kHotkeyFirstId = 1;
constexpr int kHotkeyLastId = 5;
constexpr UINT kAlarmIntervalMs = 30 * 1000;  // 캘린더 알람 확인 주기
// 자석이 당기기 시작하는 거리 (논리 px). 민감도가 높을수록 멀리서도 붙는다.
constexpr int kSnapThresholdLowDip = 6;
constexpr int kSnapThresholdMediumDip = 12;
constexpr int kSnapThresholdHighDip = 26;

// "YYYY-MM-DDTHH:MM:SS(.mmm)Z" → FILETIME 100ns 단위. 실패 시 false.
bool IsoToFiletime64(const std::string& iso, ULONGLONG& out) {
    SYSTEMTIME st{};
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(iso.c_str(), "%4u-%2u-%2uT%2u:%2u:%2u", &y, &mo, &d, &h, &mi, &s) != 6)
        return false;
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)s;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    out = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return true;
}
}  // namespace

// 앱 싱글턴 — 창·설정·스토어·AI 클라이언트를 모두 가진 하나의 인스턴스
App& App::I() {
    static App instance;
    return instance;
}

// 앱 초기화: 창 클래스 등록, 숨김 메시지 창·트레이 만들기, 설정 읽기, WebView2 환경 준비
bool App::Init(HINSTANCE hinst, bool startHidden) {
    hinst_ = hinst;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kAppClassName;
    RegisterClassExW(&wc);
    StickerWindow::RegisterWndClass(hinst);
    GroupWindow::RegisterWndClass(hinst);
    ManagerWindow::RegisterWndClass(hinst);
    WizardWindow::RegisterWndClass(hinst);

    // 트레이 콜백·브로드캐스트 수신용 숨김 최상위 창 (표시하지 않음)
    hwnd_ = CreateWindowExW(0, kAppClassName, L"Super Stickers", WS_OVERLAPPED, 0, 0, 0, 0,
                            nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    store.Init();
    settings = store.LoadSettings();
    if (settings.language.empty()) settings.language = I18n::DetectOsLanguage();
    i18n.Load(settings.language);
    settings.autostart = autostart::IsEnabled();  // 레지스트리 실제 상태와 동기화

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    HICON icon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP));
    tray_.Create(hwnd_, WM_APP_TRAY, icon, L"Super Stickers");

    ai.SetUiPoster([this](std::function<void()> fn) { RunOnUi(std::move(fn)); });
    localAi.Init([this](std::function<void()> fn) { RunOnUi(std::move(fn)); },
                 store.AppDir());
    // 로딩 → 준비 전환을 모든 창이 같이 봐야 한다. 메모창은 "올리는 중" 안내를 지우고,
    // 설정 창은 버튼과 상태 문구를 갱신한다.
    localAi.SetStateListener([this]() {
        BroadcastEvent("ai.serverState",
                       {{"state", localAi.ServerStateName()},
                        {"running", localAi.ServerRunning()},
                        {"model", localAi.RunningModel()},
                        {"elapsedMs", localAi.LoadingElapsedMs()}});
    });

    WebViewHost::EnsureEnvironment([this, startHidden](HRESULT hr) {
        if (FAILED(hr)) {
            MessageBoxW(hwnd_, i18n.T("error.webview2Missing").c_str(), L"Super Stickers",
                        MB_ICONERROR | MB_OK);
            return;
        }
        OnEnvironmentReady(startHidden);
    });
    return true;
}

// WebView2 환경이 준비된 뒤: 타이머·전역 단축키 등록, 저장된 메모·그룹 창 복원
void App::OnEnvironmentReady(bool startHidden) {
    PurgeExpiredTrash();  // 보관 기간 지난 휴지통 항목 정리 (GC보다 먼저)
    SetTimer(hwnd_, kTrashTimerId, kTrashPurgeIntervalMs, nullptr);
    SetTimer(hwnd_, kAlarmTimerId, kAlarmIntervalMs, nullptr);
    RegisterHotkeys();
    auto all = store.LoadAllStickers();
    for (auto& d : all) store.GarbageCollectMemoFiles(d);  // 메모 폴더의 미참조 첨부 정리
    auto allGroups = store.LoadAllGroups();

    for (auto& g : allGroups) {
        CreateGroupWindow(g, !g.hidden, false);
    }
    for (auto& d : all) {
        if (!d.groupId.empty() && FindGroup(d.groupId)) {
            groupedStickers_[d.id] = d;  // 그룹 카드로 렌더링 — 개별 창 없음
        } else {
            if (!d.groupId.empty()) {  // 고아 참조 정리
                d.groupId.clear();
                store.SaveSticker(d);
            }
            CreateStickerWindow(d, !d.hidden, false);
        }
    }
    if (all.empty() && allGroups.empty() && !startHidden) NewSticker();

    MaybeAutoLoadModel();
}

// 자체 모델을 쓰고 자동 로드가 켜져 있으면 시작할 때 미리 올려 둔다.
// 엔진이나 모델이 아직 없으면 조용히 넘어간다 — 시작할 때 오류 팝업을 띄우지 않는다.
void App::MaybeAutoLoadModel() {
    if (!Settings::kBuiltinBackendEnabled) return;
    if (settings.aiProvider != "builtin" || !settings.builtin.autoLoad) return;
    if (settings.builtin.modelId.empty()) return;
    const LocalAi::ModelInfo* m = LocalAi::FindModel(settings.builtin.modelId);
    if (!m || !localAi.ModelInstalled(*m)) return;
    std::string variant = ResolvedEngineVariant();
    if (!localAi.EngineInstalled(variant)) return;

    localAi.EnsureServer(settings.builtin.modelId, variant, settings.builtin.contextSize,
                         [this](bool ok, const std::string& endpoint, const std::string& err) {
                             // 설정 화면이 열려 있으면 상태를 갱신해 준다
                             SendEventToManager("ai.serverState",
                                                {{"running", ok},
                                                 {"endpoint", endpoint},
                                                 {"error", err}});
                         });
}

std::string App::EffectiveTheme() const { return theme::Effective(settings.theme); }

// 예/아니오 네이티브 확인 대화상자 (로케일 키로 문구 지정)
bool App::ConfirmYesNo(HWND owner, const std::string& msgKey) {
    return ConfirmYesNoText(owner, I().i18n.T(msgKey));
}

// 예/아니오 네이티브 확인 대화상자 (문구를 직접 준다)
bool App::ConfirmYesNoText(HWND owner, const std::wstring& msg) {
    // TDF_POSITION_RELATIVE_TO_WINDOW: 소유자 창 중앙에 표시 (MessageBox는 소유자 기준
    // 배치가 보장되지 않음). 네이티브 대화상자라 그룹창 region 클리핑의 영향을 받지 않는다.
    TASKDIALOGCONFIG tdc{};
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = owner;
    tdc.hInstance = I().hinst();
    tdc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION |
                  TDF_SIZE_TO_CONTENT;
    tdc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    tdc.pszWindowTitle = L"Super Stickers";
    tdc.pszMainIcon = TD_WARNING_ICON;
    tdc.pszContent = msg.c_str();
    tdc.nDefaultButton = IDNO;
    int btn = 0;
    if (FAILED(TaskDialogIndirect(&tdc, &btn, nullptr, nullptr))) {
        return MessageBoxW(owner, msg.c_str(), L"Super Stickers",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST |
                               MB_SETFOREGROUND) == IDYES;
    }
    return btn == IDYES;
}

// 다른 스레드에서 UI 스레드로 작업을 넘긴다 (메시지 큐 경유)
void App::RunOnUi(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(hwnd_, WM_APP_RUNNABLE, 0, (LPARAM)p)) delete p;
}

// delayMs 뒤에 UI 스레드에서 실행 (타이머 하나에 작업 하나)
void App::RunOnUiDelayed(UINT delayMs, std::function<void()> fn) {
    UINT_PTR id = nextTimerId_++;
    delayedTasks_[id] = std::move(fn);
    SetTimer(hwnd_, id, delayMs, nullptr);
}

// ---------- 스티커 관리 ----------

StickerWindow* App::CreateStickerWindow(const StickerData& d, bool show, bool activate,
                                        bool focusEditor) {
    auto* w = StickerWindow::Create(hinst_, d, show, activate, focusEditor);
    if (w) stickers_.push_back(w);
    return w;
}

// 새 메모 만들기: 종류별 기본 크기·계단식 위치로 저장한 뒤 창을 띄우고 본문에 커서를 둔다
void App::NewSticker(const std::string& type) {
    StickerData d;
    d.id = util::WideToUtf8(util::NewGuid());
    d.type = type;
    d.mode = (type == "markdown") ? "markdown" : "rich";  // 레거시 필드 동기화
    d.createdAt = d.updatedAt = util::WideToUtf8(util::NowIso8601());
    // 타입별 기본 크기 (UI 배율 반영). rich/markdown은 StickerData 기본값(510x450).
    if (type == "file") { d.w = 570; d.h = 660; }
    else if (type == "web") { d.w = 750; d.h = 690; }
    else if (type == "pdf") { d.w = 750; d.h = 900; }
    d.w = (int)(d.w * settings.uiScale + 0.5);
    d.h = (int)(d.h * settings.uiScale + 0.5);
    int n = (int)stickers_.size();
    d.x = 120 + (n % 8) * 44;
    d.y = 120 + (n % 8) * 44;
    store.SaveSticker(d);
    // 새로 만든 메모는 바로 쓸 수 있게 본문에 커서를 놓는다
    CreateStickerWindow(d, true, true, true);
}

// id로 떠 있는 메모창 찾기 (그룹 안의 메모는 대상이 아니다)
StickerWindow* App::FindSticker(const std::string& id) {
    for (auto* w : stickers_)
        if (w->data.id == id) return w;
    return nullptr;
}

// 메모 삭제: 휴지통이 켜져 있으면 trash\로 옮기고, 아니면 폴더째 지운다
void App::DeleteSticker(const std::string& id) {
    // 휴지통 사용 시 완전 삭제 대신 trash\로 이동
    auto dispose = [this](const StickerData& d) {
        if (settings.trashEnabled) {
            store.MoveStickerToTrash(d);
            BroadcastEvent("trash.changed", {{"count", store.CountTrash()}});
        } else {
            store.DeleteSticker(d);
        }
    };
    if (auto* w = FindSticker(id)) {
        StickerData d = w->data;
        w->Destroy();
        dispose(d);
        return;
    }
    // 그룹 소속 메모 (창 없음): 그룹 멤버 목록에서도 제거
    auto it = groupedStickers_.find(id);
    if (it == groupedStickers_.end()) return;
    StickerData d = it->second;
    groupedStickers_.erase(it);
    if (auto* g = FindGroup(d.groupId)) {
        auto& m = g->data.memberIds;
        m.erase(std::remove(m.begin(), m.end(), id), m.end());
        g->data.memberHeights.erase(id);
        g->SaveData();
        BroadcastEvent("group.membersChanged", {{"groupId", g->data.id}});
    }
    d.groupId.clear();  // 휴지통 항목은 그룹 참조를 갖지 않음
    dispose(d);
}

// 캘린더 알람. 페이지가 메모 메타(calAlarms)에 넣어 둔 "언제·무엇"만 보고 트레이 알림을
// 띄운다. 본문 HTML을 파싱하지 않는 이유: 창이 없는 그룹 소속 메모도 알람이 울려야 한다.
//
// 시각이 막 지난 것만 띄운다(최근 kAlarmWindowMin분). 앱이 꺼져 있던 동안의 지난 알람이
// 켜자마자 우르르 뜨면 안 되기 때문이다. 이미 띄운 것은 실행 중 다시 띄우지 않는다.
void App::CheckCalendarAlarms() {
    constexpr int kAlarmWindowMin = 5;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    // 현지 시각을 분 단위 정수로 (같은 날 안에서만 비교하면 자정을 넘길 때 어긋난다)
    auto toMinutes = [](int y, int mo, int d, int h, int mi) {
        // 대략적인 통일 척도면 충분하다 — 같은 척도끼리만 뺀다
        std::tm tm{};
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_isdst = -1;
        std::time_t t = std::mktime(&tm);
        return t == (std::time_t)-1 ? (long long)0 : (long long)(t / 60);
    };
    const long long now = toMinutes(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

    auto scan = [&](const StickerData& d) {
        if (d.calAlarms.empty()) return;
        json list = json::parse(d.calAlarms, nullptr, false);
        if (!list.is_array()) return;
        for (auto& a : list) {
            if (!a.is_object() || !a.contains("at") || !a["at"].is_string()) continue;
            std::string at = a["at"].get<std::string>();          // "YYYY-MM-DDTHH:MM"
            if (at.size() < 16) continue;
            int y = atoi(at.substr(0, 4).c_str()), mo = atoi(at.substr(5, 2).c_str());
            int dd = atoi(at.substr(8, 2).c_str()), hh = atoi(at.substr(11, 2).c_str());
            int mi = atoi(at.substr(14, 2).c_str());
            long long when = toMinutes(y, mo, dd, hh, mi);
            if (when > now || now - when > kAlarmWindowMin) continue;
            std::string key = d.id + "|" + at + "|" + a.value("id", "");
            if (!firedAlarms_.insert(key).second) continue;       // 이미 띄웠다
            std::string title = a.value("title", "");
            std::string when_s = a.value("when", "");
            std::wstring head = title.empty() ? i18n.T("cal.title") : util::Utf8ToWide(title);
            tray_.ShowBalloon(head, util::Utf8ToWide(when_s));
        }
    };

    for (auto* w : stickers_) scan(w->data);
    for (auto& kv : groupedStickers_) scan(kv.second);
}

// 보관 기간이 지난 휴지통 메모를 완전히 지운다 (주기 타이머와 시작 시)
void App::PurgeExpiredTrash() {
    if (settings.trashRetentionDays <= 0) return;  // 자동 삭제하지 않음
    ULONGLONG now = 0;
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    const ULONGLONG limit = (ULONGLONG)settings.trashRetentionDays * 24ULL * 3600ULL *
                            10000000ULL;  // 일 → 100ns
    for (auto& d : store.LoadTrash()) {
        ULONGLONG deleted = 0;
        if (!IsoToFiletime64(d.deletedAt, deleted)) continue;  // 시각 불명 → 보존
        if (now > deleted && now - deleted > limit) store.PurgeTrashSticker(d);
    }
}

// 휴지통의 메모를 되살려 창으로 띄운다
void App::RestoreTrashSticker(const std::string& id) {
    for (auto& d : store.LoadTrash()) {
        if (d.id != id) continue;
        d.deletedAt.clear();
        d.hidden = false;  // 복원 즉시 보이도록
        util::ClampRectToWorkArea(d.x, d.y, d.w, d.h);  // 삭제 후 해상도가 바뀌었을 수 있음
        if (!store.RestoreTrashEntry(id)) return;  // 폴더를 stickers\로 되돌린 뒤 저장
        store.SaveSticker(d);
        CreateStickerWindow(d, true, true);
        return;
    }
}

// 휴지통 비우기 — 개수를 보여 주고 확인을 받은 뒤 지운다
void App::EmptyTrashInteractive(HWND owner) {
    int count = store.CountTrash();
    if (count == 0) {
        MessageBoxW(owner, i18n.T("trash.emptyNone").c_str(), L"Super Stickers",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    std::wstring msg = i18n.T("confirm.emptyTrash");
    size_t pos = msg.find(L"{n}");
    if (pos != std::wstring::npos) msg.replace(pos, 3, std::to_wstring(count));
    if (!ConfirmYesNoText(owner, msg)) return;
    store.EmptyTrash();
    BroadcastEvent("trash.changed", {{"count", 0}});
}

// .ssticker 목록을 가져와 각각 새 메모로 만든다. 성공한 개수를 반환.
int App::ImportStickerFiles(const std::vector<std::wstring>& paths,
                            std::vector<std::string>* errors) {
    int count = 0;
    for (auto& path : paths) {
        // 확장자 검사 (드래그앤드롭은 아무 파일이나 올 수 있음)
        std::wstring lower = path;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        if (lower.size() < 9 || lower.substr(lower.size() - 9) != L".ssticker") {
            if (errors) errors->push_back("ext");
            continue;
        }
        std::string err;
        StickerData d = store.ImportSticker(path, &err);
        if (d.id.empty()) {
            if (errors) errors->push_back(err.empty() ? "unknown" : err);
            continue;
        }
        ++count;
        // 창 생성은 UI 스레드에서 (브리지 콜백 재진입 회피)
        StickerData copy = d;
        RunOnUi([this, copy]() {
            StickerData d2 = copy;
            util::ClampRectToWorkArea(d2.x, d2.y, d2.w, d2.h);  // 내보낸 머신과 해상도가 다를 수 있음
            // 여러 개를 한꺼번에 가져와도 겹치지 않게 살짝 어긋난 위치에 배치
            d2.x += 24 * (int)(stickers_.size() % 6);
            d2.y += 24 * (int)(stickers_.size() % 6);
            store.SaveSticker(d2);
            CreateStickerWindow(d2, true, true);
        });
    }
    if (count > 0) BroadcastEvent("stickers.changed", json::object());
    return count;
}

// 모든 데이터 삭제 (확인 포함). 실제로 지웠으면 true
bool App::DeleteAllDataInteractive(HWND owner) {
    int count = store.CountAllData();
    if (count == 0) {
        MessageBoxW(owner, i18n.T("data.deleteAllNone").c_str(), L"Super Stickers",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return false;
    }
    // 되돌릴 수 없는 작업이라 두 번 확인한다
    std::wstring msg = i18n.T("confirm.deleteAllData");
    size_t pos = msg.find(L"{n}");
    if (pos != std::wstring::npos) msg.replace(pos, 3, std::to_wstring(count));
    if (!ConfirmYesNoText(owner, msg)) return false;
    if (!ConfirmYesNo(owner, "confirm.deleteAllDataFinal")) return false;

    // 모든 창을 닫고 메모리 상태를 비운 뒤 파일 삭제 (설정은 보존)
    while (!stickers_.empty()) stickers_.back()->Destroy();
    while (!groups_.empty()) groups_.back()->Destroy();
    groupedStickers_.clear();
    store.DeleteAllData();

    BroadcastEvent("data.cleared", json::object());
    BroadcastEvent("trash.changed", {{"count", 0}});
    MessageBoxW(owner, i18n.T("data.deleteAllDone").c_str(), L"Super Stickers",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return true;
}

// 이 경로가 어느 메모의 파일 항목이나 본문 링크로 등록되어 있는지 (드롭 중복 판정용)
bool App::IsRegisteredFilePath(const std::wstring& path) const {
    if (path.empty()) return false;
    auto norm = [](std::wstring p) {
        for (auto& c : p)
            if (c == L'/') c = L'\\';
        return p;
    };
    std::wstring want = norm(path);
    // 일반 메모는 본문에 <div class="mfile" data-path="..."> 로 들어 있다.
    // HTML을 파싱하지 않고 그 속성 문자열이 있는지만 본다 (마크업은 우리가 만든다).
    const std::string needle = "data-path=\"" + util::WideToUtf8(want) + "\"";
    auto has = [&](const StickerData& d) {
        for (auto& f : d.files)
            if (norm(util::Utf8ToWide(f)) == want) return true;
        return d.html.find(needle) != std::string::npos;
    };
    for (auto* w : stickers_)
        if (has(w->data)) return true;
    for (auto& kv : groupedStickers_)
        if (has(kv.second)) return true;
    return false;
}

// 메모창 표시: 닫혀 있던 메모도 열고, 그룹 소속이면 그 그룹창을 띄운다
void App::ShowSticker(const std::string& id) {
    if (auto* w = FindSticker(id)) {
        w->data.hidden = false;
        w->SaveData();
        w->ShowWin(true, true);
        SetForegroundWindow(w->hwnd());
        return;
    }
    // 그룹 소속 메모 → 그룹 창을 표시
    auto it = groupedStickers_.find(id);
    if (it == groupedStickers_.end()) return;
    if (auto* g = FindGroup(it->second.groupId)) {
        g->data.hidden = false;
        g->SaveData();
        g->ShowWin(true, true);
        SetForegroundWindow(g->hwnd());
    }
}

// 화면에 보이는 메모창·그룹창이 하나라도 있는지
bool App::AnyStickerVisible() const {
    for (auto* w : stickers_)
        if (w->VisibleNow()) return true;
    for (auto* g : groups_)
        if (g->VisibleNow()) return true;
    return false;
}

// 모든 메모·그룹창 보이기/감추기 (트레이 메뉴, 두 번째 인스턴스 실행 시)
void App::SetAllVisible(bool visible) {
    hotkeyHidden_.clear();  // 명시적인 전체 보이기/감추기가 우선한다
    // 상태가 실제로 바뀐 창만 저장한다 — 저장은 본문 전체 직렬화 + .bak 복사 + 원자적 쓰기라
    // 메모가 많으면 트레이 토글 한 번에 파일 연산이 3N번 일어났다.
    for (auto* w : stickers_) {
        if (w->data.hidden != !visible) {
            w->data.hidden = !visible;
            w->SaveData();
        }
        w->ShowWin(visible, false);
    }
    for (auto* g : groups_) {
        if (g->data.hidden != !visible) {
            g->data.hidden = !visible;
            g->SaveData();
        }
        g->ShowWin(visible, false);
    }
}

void App::ToggleAllVisible() { SetAllVisible(!AnyStickerVisible()); }

// 보이는 창을 모두 맨 앞으로 올린다
void App::BringAllToFront() {
    // 전경이 아닌 프로세스의 HWND_TOP 요청은 무시되므로 먼저 전경으로 전환
    // (트레이 클릭은 사용자 입력이라 SetForegroundWindow가 허용됨)
    HWND first = nullptr;
    for (auto* w : stickers_)
        if (w->VisibleNow()) { first = w->hwnd(); break; }
    if (!first)
        for (auto* g : groups_)
            if (g->VisibleNow()) { first = g->hwnd(); break; }
    if (!first) return;
    SetForegroundWindow(first);

    // topmost 토글 트릭: 잠시 TOPMOST로 올렸다 해제하면 확실히 z 최상위로 온다
    auto raise = [](HWND h, bool keepTopmost) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!keepTopmost)
            SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    for (auto* w : stickers_) {
        if (w->VisibleNow()) raise(w->hwnd(), w->data.topmost);
    }
    for (auto* g : groups_) {
        if (!g->VisibleNow()) continue;
        raise(g->hwnd(), g->data.topmost);
        if (g->contentHwnd()) raise(g->contentHwnd(), g->data.topmost);
    }
}

// 메모창이 파괴될 때: 목록에서 빼고 그 창이 시작한 AI 요청을 끊는다
void App::OnStickerDestroyed(StickerWindow* w) {
    AbortOllamaByOwner(w->data.id);  // 진행 중인 AI 요청(리뷰·AI 패널) 취소
    stickers_.erase(std::remove(stickers_.begin(), stickers_.end(), w), stickers_.end());
}

namespace {

// Ollama 데스크톱 앱 실행 파일 경로 (%LOCALAPPDATA%\Programs\Ollama)
std::wstring OllamaAppExePath() {
    wchar_t* local = nullptr;
    std::wstring exe;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        exe = std::wstring(local) + L"\\Programs\\Ollama\\ollama app.exe";
        CoTaskMemFree(local);
    }
    return exe;
}

// 폴더 선택 대화상자 (취소 시 빈 문자열)
std::wstring PickFolder(HWND owner) {
    wil::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return L"";
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(owner))) return L"";
    wil::com_ptr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return L"";
    wil::unique_cotaskmem_string path;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return L"";
    return path.get();
}

}  // namespace

// 자체 엔진 변형(cpu/vulkan) 결정 — auto면 GPU 힌트로 고른다
std::string App::ResolvedEngineVariant() const {
    if (settings.builtin.engine == "cpu" || settings.builtin.engine == "vulkan")
        return settings.builtin.engine;
    // auto: Vulkan 드라이버가 있고 그 엔진이 설치돼 있으면 GPU 쪽을 쓴다.
    // 설치돼 있지 않으면 cpu로 떨어져야 한다 — 없는 엔진으로 띄우면 그냥 실패한다.
    if (LocalAi::HasVulkanCapableGpu() && localAi.EngineInstalled("vulkan")) return "vulkan";
    if (localAi.EngineInstalled("cpu")) return "cpu";
    return LocalAi::HasVulkanCapableGpu() ? "vulkan" : "cpu";
}

// Ollama가 설치되어 있는지 (앱 exe 또는 PATH의 ollama.exe)
bool App::IsOllamaInstalled() {
    std::wstring exe = OllamaAppExePath();
    if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    wchar_t found[MAX_PATH]{};
    return SearchPathW(nullptr, L"ollama.exe", nullptr, MAX_PATH, found, nullptr) > 0;
}

// Ollama 설치·모델 내려받기·자체 엔진 작업이 진행 중인지
bool App::HasActiveOllamaTasks() const {
    return installingOllama_.load() || !activePulls_.empty() || localAi.Busy();
}

// 진행 중인 Ollama 관련 작업을 모두 중단
void App::AbortOllamaTasks() {
    installAbort_ = true;  // 설치 다운로드 단계 중단 (설치 프로그램 실행 중이면 완주)
    for (auto& id : activePulls_) ai.Abort(id);
    localAi.CancelDownloads();
}

// Ollama 설치(내려받기 → 무인 설치 → 서버 시작)를 워커 스레드에서. 진행률은 ownerId 창으로
void App::InstallOllama(const std::string& ownerId) {
    if (installingOllama_.exchange(true)) return;  // 중복 실행 방지
    installAbort_ = false;
    std::thread([this, ownerId]() {
        auto progress = [this, ownerId](const std::string& stage, uint64_t received,
                                        uint64_t total) {
            RunOnUi([this, ownerId, stage, received, total]() {
                SendEventToOwner(ownerId, "ollama.installProgress",
                               {{"stage", stage}, {"received", received}, {"total", total}});
            });
        };
        auto finish = [this, ownerId](bool ok, const std::string& err, bool already,
                                      bool exposeSet) {
            RunOnUi([this, ownerId, ok, err, already, exposeSet]() {
                installingOllama_ = false;
                SendEventToOwner(ownerId, "ollama.installDone", {{"ok", ok},
                                                      {"error", err},
                                                      {"already", already},
                                                      {"exposeSet", exposeSet}});
            });
        };
        auto startApp = []() {
            std::wstring exe = OllamaAppExePath();
            if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr,
                              SW_SHOWMINNOACTIVE);
                Sleep(3000);  // 서버 기동 대기
            }
        };

        if (IsOllamaInstalled()) {
            // 이미 설치됨 — 서버만 시작해 주고 종료 (기존 설정은 건드리지 않음)
            startApp();
            return finish(true, "", true, false);
        }

        // 1) 공식 설치 프로그램 다운로드 (진행률 %)
        wchar_t tmpDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmpDir);
        std::wstring setupPath = std::wstring(tmpDir) + L"OllamaSetup.exe";
        ULONGLONG lastPost = 0;
        bool downloaded = util::HttpGetToFile(
            L"https://ollama.com/download/OllamaSetup.exe", setupPath, nullptr,
            [&](uint64_t received, uint64_t total) {
                ULONGLONG now = GetTickCount64();
                if (now - lastPost >= 250 || received == total) {
                    lastPost = now;
                    progress("download", received, total);
                }
            },
            &installAbort_);
        if (!downloaded) {
            DeleteFileW(setupPath.c_str());
            return finish(false, installAbort_.load() ? "aborted" : "download failed", false,
                          false);
        }

        // 2) 무인 설치 (Inno Setup)
        progress("install", 0, 0);
        std::wstring cmd = L"\"" + setupPath + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            return finish(false, "installer launch failed", false, false);
        }
        DWORD wait = WaitForSingleObject(pi.hProcess, 20 * 60 * 1000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DeleteFileW(setupPath.c_str());
        if (wait != WAIT_OBJECT_0 || code != 0)
            return finish(false, "installer exit " + std::to_string(code), false, false);

        // 3) Expose Ollama to the network: OLLAMA_HOST=0.0.0.0 (사용자 환경변수)
        bool exposeSet =
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Environment", L"OLLAMA_HOST", REG_SZ,
                            L"0.0.0.0", sizeof(L"0.0.0.0")) == ERROR_SUCCESS;
        if (exposeSet) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                                SMTO_ABORTIFHUNG, 3000, &res);
        }

        // 4) Ollama 앱(서버) 시작 — 새 환경변수를 읽도록 설치 후 시작
        progress("starting", 0, 0);
        startApp();
        finish(true, "", false, exposeSet);
    }).detach();
}


// 특정 창이 시작한 AI 요청을 모두 중단 (그 창이 닫힐 때)
void App::AbortOllamaByOwner(const std::string& ownerId) {
    if (ownerId.empty()) return;
    for (auto it = ollamaOwners_.begin(); it != ollamaOwners_.end();) {
        if (it->second == ownerId) {
            ai.Abort(it->first);
            it = ollamaOwners_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------- 그룹 관리 ----------

GroupWindow* App::FindGroup(const std::string& id) {
    for (auto* g : groups_)
        if (g->data.id == id) return g;
    return nullptr;
}

// 그룹창을 만들고 목록에 등록
GroupWindow* App::CreateGroupWindow(const GroupData& g, bool show, bool activate) {
    auto* w = GroupWindow::Create(hinst_, g, show, activate);
    if (w) groups_.push_back(w);
    return w;
}

// 새 그룹창을 커서 근처에 만든다
void App::NewGroup() {
    GroupData g;
    g.id = util::WideToUtf8(util::NewGuid());
    g.createdAt = g.updatedAt = util::WideToUtf8(util::NowIso8601());
    POINT pt{};
    GetCursorPos(&pt);
    g.x = pt.x + 24;
    g.y = pt.y + 24;
    g.w = (int)(g.w * settings.uiScale + 0.5);  // 기본 크기에 UI 배율 반영
    g.h = (int)(g.h * settings.uiScale + 0.5);
    store.SaveGroup(g);
    CreateGroupWindow(g, true, true);
}

// 그룹창 파괴 시 목록에서 제거
void App::OnGroupDestroyed(GroupWindow* w) {
    groups_.erase(std::remove(groups_.begin(), groups_.end(), w), groups_.end());
}

// 그룹 삭제: 소속 메모를 개별 창으로 풀어 준 뒤 그룹을 지운다
void App::DeleteGroupReleaseMembers(const std::string& groupId) {
    GroupWindow* g = FindGroup(groupId);
    if (!g) return;
    // 멤버 메모는 삭제하지 않고 그룹 근처에 플로팅 창으로 분리
    int i = 0;
    for (auto& memberId : g->data.memberIds) {
        auto it = groupedStickers_.find(memberId);
        if (it == groupedStickers_.end()) continue;
        StickerData d = it->second;
        groupedStickers_.erase(it);
        d.groupId.clear();
        d.hidden = false;
        d.x = g->data.x + 30 + (i % 6) * 40;
        d.y = g->data.y + 30 + (i % 6) * 40;
        i++;
        store.SaveSticker(d);
        CreateStickerWindow(d, true, false);
    }
    g->Destroy();
    store.DeleteGroup(groupId);
}

// 메모창을 그룹에 넣는다 (창을 닫고 그룹 멤버 데이터로 옮긴다)
void App::AddStickerToGroup(StickerWindow* w, GroupWindow* g) {
    StickerData d = w->data;
    w->Destroy();
    d.groupId = g->data.id;
    d.hidden = false;
    groupedStickers_[d.id] = d;
    store.SaveSticker(d);
    g->data.memberIds.push_back(d.id);
    g->SaveData();
    BroadcastEvent("group.membersChanged", {{"groupId", g->data.id}});
}

// 그룹 멤버를 지정 위치의 개별 메모창으로 꺼낸다
void App::PopOutStickerAt(const std::string& stickerId, int x, int y) {
    auto it = groupedStickers_.find(stickerId);
    if (it == groupedStickers_.end()) return;
    StickerData d = it->second;
    GroupWindow* src = FindGroup(d.groupId);

    // 드롭 지점이 다른 그룹 위라면 그 그룹으로 이동
    if (x >= 0 && y >= 0) {
        GroupWindow* target = GroupAtPoint(POINT{x, y});
        if (target && target != src) {
            if (src) {
                auto& m = src->data.memberIds;
                m.erase(std::remove(m.begin(), m.end(), stickerId), m.end());
                src->data.memberHeights.erase(stickerId);
                src->SaveData();
            }
            it->second.groupId = target->data.id;
            store.SaveSticker(it->second);
            target->data.memberIds.push_back(stickerId);
            target->SaveData();
            // 그룹창은 자기 groupId의 방송만 반영한다 — 빠져나간 쪽에도 알려야 카드가 사라진다
            if (src) BroadcastEvent("group.membersChanged", {{"groupId", src->data.id}});
            BroadcastEvent("group.membersChanged", {{"groupId", target->data.id}});
            return;
        }
    }

    if (src) {
        auto& m = src->data.memberIds;
        m.erase(std::remove(m.begin(), m.end(), stickerId), m.end());
        src->data.memberHeights.erase(stickerId);
        src->SaveData();
        d.x = src->data.x + src->data.w + 12;  // 기본: 그룹 오른쪽 옆
        d.y = src->data.y;
    }
    if (x >= 0 && y >= 0) {  // 드롭 지점에 배치 (커서가 창 좌상단 근처에 오도록 보정)
        d.x = x - 40;
        d.y = y - 16;
    }
    groupedStickers_.erase(it);
    d.groupId.clear();
    d.hidden = false;
    store.SaveSticker(d);
    CreateStickerWindow(d, true, true);
    BroadcastEvent("group.membersChanged", {{"groupId", src ? src->data.id : ""}});
}

// 그룹 안에 새 메모 멤버를 만든다
void App::NewMemoInGroup(const std::string& groupId, const std::string& type) {
    GroupWindow* g = FindGroup(groupId);
    if (!g) return;
    StickerData d;
    d.id = util::WideToUtf8(util::NewGuid());
    d.type = type;
    d.mode = (type == "markdown") ? "markdown" : "rich";
    d.groupId = groupId;
    d.createdAt = d.updatedAt = util::WideToUtf8(util::NowIso8601());
    store.SaveSticker(d);
    groupedStickers_[d.id] = d;
    g->data.memberIds.push_back(d.id);
    g->SaveData();
    BroadcastEvent("group.membersChanged", {{"groupId", groupId}});
}

// 그룹 멤버 순서 변경 (페이지의 드래그 정렬 결과)
void App::ReorderGroupMembers(GroupWindow* g, const std::vector<std::string>& order) {
    // 안전장치: 기존 멤버 집합과 동일한 항목만 반영
    std::vector<std::string> next;
    for (auto& id : order) {
        if (std::find(g->data.memberIds.begin(), g->data.memberIds.end(), id) !=
            g->data.memberIds.end())
            next.push_back(id);
    }
    for (auto& id : g->data.memberIds) {
        if (std::find(next.begin(), next.end(), id) == next.end()) next.push_back(id);
    }
    g->data.memberIds = std::move(next);
    g->SaveData();
}

// 그룹 카드에서 편집한 멤버 본문을 저장
void App::SaveMemberContent(const nlohmann::json& p) {
    std::string id = p.value("id", "");
    auto it = groupedStickers_.find(id);
    if (it == groupedStickers_.end()) return;
    StickerData& d = it->second;
    d.html = p.value("html", d.html);
    d.markdown = p.value("markdown", d.markdown);
    d.mode = p.value("mode", d.mode);
    if (p.contains("attachments") && p["attachments"].is_array()) {
        d.attachments.clear();
        for (auto& a : p["attachments"])
            if (a.is_string()) d.attachments.push_back(a.get<std::string>());
    }
    d.updatedAt = util::WideToUtf8(util::NowIso8601());
    store.SaveSticker(d);
}

// id로 메모 데이터 찾기 — 떠 있는 창이든 그룹 멤버든
StickerData* App::FindStickerData(const std::string& id) {
    if (auto* w = FindSticker(id)) return &w->data;
    auto it = groupedStickers_.find(id);
    return it != groupedStickers_.end() ? &it->second : nullptr;
}

// 화면 좌표 아래의 그룹창 (겹치면 z 순서상 위의 것)
GroupWindow* App::GroupAtPoint(POINT pt) {
    // z-order 상단부터 검사해 겹친 그룹 중 위에 있는 것을 선택
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        for (auto* g : groups_) {
            if (g->hwnd() != h || !g->VisibleNow()) continue;
            RECT r{};
            GetWindowRect(g->hwnd(), &r);
            if (PtInRect(&r, pt)) return g;
        }
    }
    return nullptr;
}

// 커서 아래의 그룹창
GroupWindow* App::GroupUnderCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    return GroupAtPoint(pt);
}

// 메모창 드래그가 끝났을 때: 그룹 위에 놓였으면 잠시 뒤 그 그룹에 넣는다
void App::HandleStickerMoveEnd(StickerWindow* w) {
    // 드래그 하이라이트 정리
    if (!lastDragHoverGroup_.empty()) {
        if (auto* prev = FindGroup(lastDragHoverGroup_)) prev->SetDropHover(false);
        lastDragHoverGroup_.clear();
    }
    GroupWindow* g = GroupUnderCursor();
    if (!g) return;
    // 웹 측 자동 저장 디바운스를 플러시할 시간을 준 뒤 흡수
    w->host().PostEvent("app.flush", nlohmann::json::object());
    std::string stickerId = w->data.id;
    std::string groupId = g->data.id;
    RunOnUiDelayed(250, [this, stickerId, groupId]() {
        StickerWindow* sw = FindSticker(stickerId);
        GroupWindow* gw = FindGroup(groupId);
        if (sw && gw) AddStickerToGroup(sw, gw);
    });
}

// 스타일 설정을 페이지용 JSON으로
json App::StyleJson() const {
    return json{{"background", settings.style.background},
                {"font", settings.style.font},
                {"fontSize", settings.style.fontSize}};
}

// ---------- 전역 단축키 ----------
namespace {

// "Ctrl+Shift+S" -> 수정자 비트와 가상 키. 해석할 수 없으면 false.
bool ParseHotkey(const std::string& text, UINT* mods, UINT* vk) {
    static const std::map<std::string, UINT> kNamed = {
        {"SPACE", VK_SPACE},   {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"HOME", VK_HOME},     {"END", VK_END},       {"PAGEUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT}, {"LEFT", VK_LEFT},     {"RIGHT", VK_RIGHT},
        {"UP", VK_UP},         {"DOWN", VK_DOWN},     {"TAB", VK_TAB},
        {"ENTER", VK_RETURN},  {"ESC", VK_ESCAPE},
    };
    *mods = 0;
    *vk = 0;
    std::string cur;
    auto flush = [&]() -> bool {
        if (cur.empty()) return true;
        std::string k;
        for (char c : cur) k += (char)toupper((unsigned char)c);
        cur.clear();
        if (k == "CTRL" || k == "CONTROL") { *mods |= MOD_CONTROL; return true; }
        if (k == "SHIFT") { *mods |= MOD_SHIFT; return true; }
        if (k == "ALT") { *mods |= MOD_ALT; return true; }
        if (k == "WIN") { *mods |= MOD_WIN; return true; }
        if (*vk) return false;  // 키는 하나만
        if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) {
            *vk = (UINT)(unsigned char)k[0];
            return true;
        }
        if (k.size() >= 2 && k[0] == 'F' && isdigit((unsigned char)k[1])) {
            int n = atoi(k.c_str() + 1);
            if (n >= 1 && n <= 24) { *vk = VK_F1 + (UINT)(n - 1); return true; }
        }
        auto it = kNamed.find(k);
        if (it == kNamed.end()) return false;
        *vk = it->second;
        return true;
    };
    for (char c : text) {
        if (c == '+') { if (!flush()) return false; }
        else if (!isspace((unsigned char)c)) cur += c;
    }
    if (!flush()) return false;
    // 수정자 없는 단일 키를 전역으로 잡으면 다른 앱의 타자를 먹는다 - 거부한다
    return *vk != 0 && *mods != 0;
}

}  // namespace

// 등록해 둔 전역 단축키를 모두 해제
void App::UnregisterHotkeys() {
    if (hwnd_)
        for (int id = kHotkeyFirstId; id <= kHotkeyLastId; ++id) UnregisterHotKey(hwnd_, id);
    failedHotkeys_.clear();
}

// 설정의 단축키 문자열을 해석해 RegisterHotKey. 실패한 항목은 failedHotkeys_에 남긴다
void App::RegisterHotkeys() {
    UnregisterHotkeys();
    if (!hwnd_ || !settings.hotkeys.enabled) return;
    struct Item { int id; const char* name; const std::string* text; };
    const Item items[] = {
        {kHotkeyToggleId, "toggleAll", &settings.hotkeys.toggleAll},
        {kHotkeyNewId, "newMemo", &settings.hotkeys.newMemo},
        {kHotkeyListId, "list", &settings.hotkeys.list},
        {kHotkeyArrangeLeftId, "arrangeLeft", &settings.hotkeys.arrangeLeft},
        {kHotkeyArrangeRightId, "arrangeRight", &settings.hotkeys.arrangeRight},
    };
    for (const auto& it : items) {
        if (it.text->empty()) continue;  // 빈 값 = 쓰지 않음
        UINT mods = 0, vk = 0;
        if (!ParseHotkey(*it.text, &mods, &vk)) { failedHotkeys_.insert(it.name); continue; }
        // MOD_NOREPEAT: 키를 누르고 있어도 한 번만 (아니면 창이 깜빡인다)
        if (!RegisterHotKey(hwnd_, it.id, mods | MOD_NOREPEAT, vk)) failedHotkeys_.insert(it.name);
    }
}

// WM_HOTKEY 분기 — 어느 단축키인지에 따라 동작
void App::OnHotkey(int id) {
    if (id == kHotkeyToggleId) ToggleShowAllFront();
    else if (id == kHotkeyNewId) NewSticker("rich");
    else if (id == kHotkeyListId) OpenManager("list");
    else if (id == kHotkeyArrangeLeftId) ArrangeToEdge(false);
    else if (id == kHotkeyArrangeRightId) ArrangeToEdge(true);
}

// 화면에 보이는 메모를 모두 최소화해 한쪽 가장자리에 위에서부터 같은 간격으로 세운다.
// 한 줄이 작업 영역 아래에 닿으면 안쪽으로 한 줄 더 세운다. 그룹창은 대상이 아니다.
void App::ArrangeToEdge(bool right) {
    std::vector<StickerWindow*> list;
    for (auto* w : stickers_)
        if (w->VisibleNow()) list.push_back(w);
    if (list.empty()) return;
    // 정렬 순서는 지금 화면의 위→아래, 같은 높이면 왼쪽→오른쪽 (눈에 보이는 순서를 지킨다)
    std::stable_sort(list.begin(), list.end(), [](StickerWindow* a, StickerWindow* b) {
        RECT ra{}, rb{};
        GetWindowRect(a->hwnd(), &ra);
        GetWindowRect(b->hwnd(), &rb);
        return ra.top != rb.top ? ra.top < rb.top : ra.left < rb.left;
    });

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int gap = (int)(8 * settings.uiScale + 0.5);
    int y = wa.top + gap;
    int columnEdge = right ? wa.right - gap : wa.left + gap;  // 이 줄이 붙는 가장자리
    int columnMaxW = 0;
    for (auto* w : list) {
        w->SetMinimized(true);
        RECT r{};
        GetWindowRect(w->hwnd(), &r);
        int cw = r.right - r.left, ch = r.bottom - r.top;
        if (y + ch > wa.bottom && y != wa.top + gap) {  // 이 줄이 찼다 — 안쪽으로 한 줄
            y = wa.top + gap;
            columnEdge += right ? -(columnMaxW + gap) : (columnMaxW + gap);
            columnMaxW = 0;
        }
        int x = right ? columnEdge - cw : columnEdge;
        SetWindowPos(w->hwnd(), nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        w->StoreGeometryFromWindow();
        w->SaveData();
        y += ch + gap;
        if (cw > columnMaxW) columnMaxW = cw;
    }
    BringAllToFront();
}

// 보기/감추기 단축키 동작 (규칙은 App.h 선언의 설명 참고)
void App::ToggleShowAllFront() {
    // 이 단축키가 감춘 것이 있으면 그것만 되돌린다. ×로 닫아 둔 메모(data.hidden)는
    // 화면에 없는 것이 사용자의 뜻이므로 꺼내지 않는다.
    if (!hotkeyHidden_.empty()) {
        for (auto* w : stickers_)
            if (hotkeyHidden_.count(w->data.id)) w->ShowWin(true, false);
        for (auto* g : groups_)
            if (hotkeyHidden_.count("g:" + g->data.id)) g->ShowWin(true, false);
        hotkeyHidden_.clear();
        RaiseAllAndRecord();
        return;
    }
    if (!AnyStickerVisible()) return;  // 화면에 있는 메모가 없다 — 꺼낼 것도 없다
    // 전부 보이는 중이다. 다른 앱이 앞에 있으면 그 아래 깔린 것이므로 위로 올리고,
    // 우리 창이 이미 앞이면 그때 감춘다.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    // 우리 창이 앞이면 감추고, 아니면 앞으로 올린다. 다만 직전에 올리려다 전경 전환이
    // 거부됐다면(RaiseAllAndRecord 참고) 아무리 눌러도 감춰지지 않으므로 감추기로 넘어간다.
    if (pid == GetCurrentProcessId() || raiseFailed_) {
        // 지금 화면에 나와 있는 것만 감춘다. data.hidden은 그대로 두어 다음에 이 단축키를
        // 누르면 되돌아오고, 앱을 다시 켜도 원래대로 열린다 (닫은 것과 구분된다).
        for (auto* w : stickers_)
            if (w->VisibleNow()) { hotkeyHidden_.insert(w->data.id); w->ShowWin(false, false); }
        for (auto* g : groups_)
            if (g->VisibleNow()) {
                hotkeyHidden_.insert("g:" + g->data.id);
                g->ShowWin(false, false);
            }
        raiseFailed_ = false;
    } else {
        RaiseAllAndRecord();
    }
}

// 앞으로 올린 뒤 정말 전경이 되었는지 남겨 둔다. 다른 앱이 방금 입력을 받았으면 윈도우가
// SetForegroundWindow를 조용히 거부한다 — z 순서는 올라가지만 전경은 그대로다.
void App::RaiseAllAndRecord() {
    BringAllToFront();
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    raiseFailed_ = pid != GetCurrentProcessId();
}

// 모든 창을 작업 영역 안으로 들인다 (해상도·모니터 구성이 바뀐 뒤)
void App::ClampAllWindowsToScreen() {
    auto clampWindow = [](HWND hwnd) -> bool {
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return false;
        int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
        if (!util::ClampRectToWorkArea(x, y, w, h)) return false;
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    };
    for (auto* w : stickers_) {
        if (clampWindow(w->hwnd())) {
            w->StoreGeometryFromWindow();  // 자동 숨김으로 접힌 상태면 펼친 기준으로 저장
            w->SaveData();
        }
    }
    for (auto* g : groups_) {
        if (clampWindow(g->hwnd())) {
            g->StoreGeometryFromWindow();  // 자동 숨김으로 접힌 상태면 펼친 기준으로 저장
            g->SaveData();
        }
    }
}

// ---------- 다중 선택 ----------

// 선택 표시(네이티브 테두리)를 모든 창에 반영하고 페이지에도 알린다
void App::SyncSelectionLook() {
    for (auto* w : stickers_) w->SetSelectedLook(selected_.count(w->data.id) > 0);
    json ids = json::array();
    for (auto& id : selected_) ids.push_back(id);
    BroadcastEvent("selection.changed", {{"ids", ids}});
}

// 메모창 다중 선택 해제
void App::ClearSelection() {
    if (selected_.empty()) return;
    selected_.clear();
    SyncSelectionLook();
}

// 메모창 클릭: Ctrl이면 선택 토글, 아니면 선택 해제
void App::OnStickerClicked(const std::string& id, bool toggle) {
    if (toggle) {
        if (!selected_.insert(id).second) selected_.erase(id);  // 이미 있으면 해제
    } else if (!selected_.count(id)) {
        // 선택에 없는 창을 그냥 클릭 = 다른 작업으로 넘어감 → 전체 해제.
        // (선택된 창을 그냥 클릭한 경우는 함께 드래그하려는 것이므로 그대로 둔다)
        if (selected_.empty()) return;
        selected_.clear();
    } else {
        return;  // 선택된 창의 평범한 클릭 — 상태 유지
    }
    SyncSelectionLook();
}

// 선택된 메모창들을 감춘다 (Delete 키)
void App::HideSelectedStickers() {
    if (selected_.empty()) return;
    std::vector<std::string> ids(selected_.begin(), selected_.end());
    selected_.clear();
    SyncSelectionLook();
    for (auto& id : ids) {
        if (auto* w = FindSticker(id)) {
            w->data.hidden = true;
            w->SaveData();
            w->ShowWin(false, false);
        }
    }
}

// 이동 중 자석: WM_MOVING의 제안 사각형을 이웃 창에 맞춘다
void App::SnapStickerRect(StickerWindow* self, RECT* rect) {
    if (!settings.magnetEnabled || !rect) return;
    const RECT& me = *rect;
    const int w = me.right - me.left, h = me.bottom - me.top;
    const int gap = self->CssPx(settings.magnetGap);
    const int thrDip = settings.magnetSensitivity == "high"  ? kSnapThresholdHighDip
                       : settings.magnetSensitivity == "low" ? kSnapThresholdLowDip
                                                             : kSnapThresholdMediumDip;
    const int thr = self->CssPx(thrDip);  // 자석이 당기기 시작하는 거리

    int bestDx = 0, bestDy = 0;
    int bestX = thr + 1, bestY = thr + 1;  // 현재까지 가장 가까운 후보와의 거리
    auto tryX = [&](int target) {
        int d = target - me.left;
        if (abs(d) <= thr && abs(d) < bestX) { bestX = abs(d); bestDx = d; }
    };
    auto tryY = [&](int target) {
        int d = target - me.top;
        if (abs(d) <= thr && abs(d) < bestY) { bestY = abs(d); bestDy = d; }
    };

    const bool multi = IsSelected(self->data.id) && HasMultiSelection();
    for (auto* other : stickers_) {
        if (other == self || !other->VisibleNow()) continue;
        // 함께 끌려오는 창에는 붙지 않는다 (서로 당겨 레이아웃이 무너진다)
        if (multi && IsSelected(other->data.id)) continue;
        RECT o{};
        GetWindowRect(other->hwnd(), &o);
        // 세로로 겹치거나 가까울 때만 좌우로 붙인다 (엉뚱하게 멀리 있는 창에 끌리지 않도록)
        if (me.top <= o.bottom + gap + thr && me.bottom >= o.top - gap - thr) {
            tryX(o.right + gap);          // 오른쪽에 간격 두고 붙이기
            tryX(o.left - gap - w);       // 왼쪽에 간격 두고 붙이기
            tryX(o.left);                 // 왼쪽 가장자리 정렬
            tryX(o.right - w);            // 오른쪽 가장자리 정렬
        }
        // 가로로 겹치거나 가까울 때만 위아래로 붙인다
        if (me.left <= o.right + gap + thr && me.right >= o.left - gap - thr) {
            tryY(o.bottom + gap);         // 아래에 간격 두고 붙이기
            tryY(o.top - gap - h);        // 위에 간격 두고 붙이기
            tryY(o.top);                  // 위쪽 가장자리 정렬
            tryY(o.bottom - h);           // 아래쪽 가장자리 정렬
        }
    }
    if (bestDx == 0 && bestDy == 0) return;
    OffsetRect(rect, bestDx, bestDy);
}

// 크기 변경 중 자석: WM_SIZING에서 잡고 있는 변만 이웃 창의 변에 맞춘다
void App::SnapStickerResize(StickerWindow* self, RECT* rect, int edge) {
    if (!settings.magnetEnabled || !rect) return;
    // 잡고 있는 변 (모서리는 가로·세로를 한 변씩 동시에 끈다)
    const bool dragL = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    const bool dragR = edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT;
    const bool dragT = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    const bool dragB = edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
    if (!dragL && !dragR && !dragT && !dragB) return;

    const RECT& me = *rect;
    const int gap = self->CssPx(settings.magnetGap);
    const int thrDip = settings.magnetSensitivity == "high"  ? kSnapThresholdHighDip
                       : settings.magnetSensitivity == "low" ? kSnapThresholdLowDip
                                                             : kSnapThresholdMediumDip;
    const int thr = self->CssPx(thrDip);

    int bestDx = 0, bestDy = 0;
    int bestX = thr + 1, bestY = thr + 1;
    // cur = 지금 끌고 있는 변의 좌표, target = 붙을 후보 좌표
    auto tryX = [&](int cur, int target) {
        int d = target - cur;
        if (abs(d) <= thr && abs(d) < bestX) { bestX = abs(d); bestDx = d; }
    };
    auto tryY = [&](int cur, int target) {
        int d = target - cur;
        if (abs(d) <= thr && abs(d) < bestY) { bestY = abs(d); bestDy = d; }
    };

    for (auto* other : stickers_) {
        if (other == self || !other->VisibleNow()) continue;
        RECT o{};
        GetWindowRect(other->hwnd(), &o);
        // 이동 자석과 같은 근접 조건: 세로로 가까울 때만 좌우로, 가로로 가까울 때만 위아래로
        if (me.top <= o.bottom + gap + thr && me.bottom >= o.top - gap - thr) {
            if (dragL) {
                tryX(me.left, o.left);         // 왼쪽 변끼리 같은 X
                tryX(me.left, o.right);        // 상대 오른쪽 변과 같은 X
                tryX(me.left, o.right + gap);  // 상대 오른쪽에 간격 두고 맞대기
            }
            if (dragR) {
                tryX(me.right, o.right);       // 오른쪽 변끼리 같은 X
                tryX(me.right, o.left);        // 상대 왼쪽 변과 같은 X
                tryX(me.right, o.left - gap);  // 상대 왼쪽에 간격 두고 맞대기
            }
        }
        if (me.left <= o.right + gap + thr && me.right >= o.left - gap - thr) {
            if (dragT) {
                tryY(me.top, o.top);           // 윗변끼리 같은 Y
                tryY(me.top, o.bottom);        // 상대 아랫변과 같은 Y
                tryY(me.top, o.bottom + gap);  // 상대 아래에 간격 두고 맞대기
            }
            if (dragB) {
                tryY(me.bottom, o.bottom);     // 아랫변끼리 같은 Y
                tryY(me.bottom, o.top);        // 상대 윗변과 같은 Y
                tryY(me.bottom, o.top - gap);  // 상대 위에 간격 두고 맞대기
            }
        }
    }
    if (dragL) rect->left += bestDx;
    if (dragR) rect->right += bestDx;
    if (dragT) rect->top += bestDy;
    if (dragB) rect->bottom += bestDy;
}

// 드래그 중 커서 아래 그룹의 드롭 하이라이트를 갱신
void App::UpdateDragHover(StickerWindow*) {
    GroupWindow* g = GroupUnderCursor();
    std::string id = g ? g->data.id : "";
    if (id == lastDragHoverGroup_) return;
    if (!lastDragHoverGroup_.empty())
        if (auto* prev = FindGroup(lastDragHoverGroup_)) prev->SetDropHover(false);
    if (g) g->SetDropHover(true);
    lastDragHoverGroup_ = id;
}

// ---------- Manager 창 ----------

void App::OpenManager(const std::string& tab) {
    if (manager_) {
        manager_->ShowTab(tab);
        return;
    }
    manager_ = ManagerWindow::Create(hinst_, tab);
}

void App::OnManagerDestroyed() { manager_ = nullptr; }

// 마법사 창 열기 — 부른 메모창 가운데에. 이미 떠 있으면 앞으로만 (owner는 바꾸지 않는다)
void App::OpenWizard(const std::string& ownerId, bool force) {
    if (wizard_) {
        wizard_->Focus();
        return;
    }
    wizardOwner_ = ownerId;
    HWND ownerHwnd = nullptr;
    if (auto* w = FindSticker(ownerId)) ownerHwnd = w->hwnd();
    wizard_ = WizardWindow::Create(hinst_, force, ownerHwnd);
}

// 마법사 끝: 결과를 부른 창에 돌려주고 창을 닫는다 (두 번 불려도 한 번만 보낸다)
void App::FinishWizard(bool ok) {
    if (!wizard_) return;
    std::string owner = wizardOwner_;
    wizardOwner_.clear();
    json data{{"ok", ok}};
    if (!owner.empty()) SendEventToSticker(owner, "wizard.result", data);
    else SendEventToManager("wizard.result", data);
    HWND h = wizard_->hwnd();
    wizard_ = nullptr;  // WM_DESTROY의 OnWizardDestroyed와 겹치지 않게 먼저 비운다
    DestroyWindow(h);
}

void App::OnWizardDestroyed() {
    wizard_ = nullptr;
    wizardOwner_.clear();
}

// ---------- 설정 반영 ----------

void App::ApplySettingsPatch(const json& patch) {
    bool themeChanged = false, langChanged = false;

    if (patch.contains("aiProvider") && patch["aiProvider"].is_string()) {
        std::string v = patch["aiProvider"];
        const bool allowed = v == "ollama" || v == "lmstudio" ||
                             (v == "builtin" && Settings::kBuiltinBackendEnabled);
        if (allowed) {
            if (v != settings.aiProvider) {
                settings.aiProvider = v;
                // 백엔드를 바꾸면 떠 있는 서버는 쓸모가 없다 — 메모리를 바로 돌려준다
                if (v != "builtin") localAi.StopServer();
            }
        }
    }
    if (patch.contains("lmstudio") && patch["lmstudio"].is_object()) {
        const json& lm = patch["lmstudio"];
        if (lm.contains("endpoint") && lm["endpoint"].is_string())
            settings.lmstudio.endpoint = lm["endpoint"];
        if (lm.contains("model") && lm["model"].is_string())
            settings.lmstudio.model = lm["model"];
    }
    if (patch.contains("builtin") && patch["builtin"].is_object()) {
        const json& bi = patch["builtin"];
        if (bi.contains("modelId") && bi["modelId"].is_string()) {
            std::string v = bi["modelId"];
            if (v != settings.builtin.modelId) {
                settings.builtin.modelId = v;
                localAi.StopServer();  // 다른 모델이 떠 있으면 내린다
            }
        }
        if (bi.contains("engine") && bi["engine"].is_string()) {
            std::string v = bi["engine"];
            if (v == "auto" || v == "cpu" || v == "vulkan") {
                if (v != settings.builtin.engine) {
                    settings.builtin.engine = v;
                    localAi.StopServer();
                }
            }
        }
        if (bi.contains("autoLoad") && bi["autoLoad"].is_boolean()) {
            settings.builtin.autoLoad = bi["autoLoad"];
        }
        if (bi.contains("contextSize") && bi["contextSize"].is_number_integer()) {
            int v = bi["contextSize"];
            v = v < 1024 ? 1024 : (v > 32768 ? 32768 : v);
            if (v != settings.builtin.contextSize) {
                settings.builtin.contextSize = v;
                localAi.StopServer();
            }
        }
    }
    if (patch.contains("theme") && patch["theme"].is_string()) {
        std::string v = patch["theme"];
        if (v != settings.theme) {
            settings.theme = v;
            themeChanged = true;
        }
    }
    if (patch.contains("language") && patch["language"].is_string()) {
        std::string v = patch["language"];
        if (v != settings.language) {
            settings.language = v;
            langChanged = true;
        }
    }
    if (patch.contains("autostart") && patch["autostart"].is_boolean()) {
        settings.autostart = patch["autostart"];
        autostart::SetEnabled(settings.autostart);
    }
    if (patch.contains("ollama") && patch["ollama"].is_object()) {
        auto& o = patch["ollama"];
        if (o.contains("endpoint") && o["endpoint"].is_string())
            settings.ollama.endpoint = o["endpoint"];
        if (o.contains("model") && o["model"].is_string()) settings.ollama.model = o["model"];
    }
    if (patch.contains("uiScale") && patch["uiScale"].is_number()) {
        double v = patch["uiScale"];
        v = v < 0.3 ? 0.3 : (v > 2.0 ? 2.0 : v);
        double old = settings.uiScale > 0.0 ? settings.uiScale : 1.0;
        settings.uiScale = v;
        double ratio = v / old;
        // DPI 변경처럼 창의 물리 크기도 배율에 맞춰 조정 (좌상단 고정, 작업 영역 클램프)
        auto resizeWindow = [ratio](HWND h, int* px, int* py, int* pw, int* ph) {
            RECT r{};
            GetWindowRect(h, &r);
            int x = r.left, y = r.top;
            int w = (int)((r.right - r.left) * ratio + 0.5);
            int hh = (int)((r.bottom - r.top) * ratio + 0.5);
            util::ClampRectToWorkArea(x, y, w, hh);
            SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
            if (px) { *px = x; *py = y; *pw = w; *ph = hh; }
        };
        bool resize = ratio > 1.0001 || ratio < 0.9999;
        for (auto* w : stickers_) {
            w->ApplyUiScale();
            if (resize) {
                resizeWindow(w->hwnd(), nullptr, nullptr, nullptr, nullptr);
                w->StoreGeometryFromWindow();  // 자동 숨김으로 접힌 상태면 펼친 기준으로 저장
                w->SaveData();
            }
        }
        for (auto* g : groups_) {
            g->host().SetZoomFactor(v);
            if (resize) {
                resizeWindow(g->hwnd(), nullptr, nullptr, nullptr, nullptr);
                g->StoreGeometryFromWindow();
                g->SaveData();
            }
        }
        if (manager_) {
            manager_->host().SetZoomFactor(v);
            if (resize) resizeWindow(manager_->hwnd(), nullptr, nullptr, nullptr, nullptr);
        }
    }
    if (patch.contains("autoHideUi") && patch["autoHideUi"].is_boolean()) {
        bool v = patch["autoHideUi"];
        if (v != settings.autoHideUi) {
            settings.autoHideUi = v;
            // 각 스티커 페이지가 즉시 접기/펼치기를 갱신한다 (그룹창은 대상 아님)
            BroadcastEvent("ui.autoHideChanged", {{"on", v}});
        }
    }
    if (patch.contains("uiRevealOnClick") && patch["uiRevealOnClick"].is_boolean()) {
        bool v = patch["uiRevealOnClick"];
        if (v != settings.uiRevealOnClick) {
            settings.uiRevealOnClick = v;
            // 각 스티커 페이지가 표시 조건(호버 vs 클릭)을 즉시 바꾼다
            BroadcastEvent("ui.revealModeChanged", {{"clickOnly", v}});
        }
    }
    if (patch.contains("tts") && patch["tts"].is_object()) {
        auto& t = patch["tts"];
        if (t.contains("voice") && t["voice"].is_string()) settings.tts.voice = t["voice"];
        if (t.contains("rate") && t["rate"].is_number()) {
            int v = t["rate"];
            settings.tts.rate = v < -10 ? -10 : (v > 10 ? 10 : v);
        }
    }
    if (patch.contains("style") && patch["style"].is_object()) {
        auto& st = patch["style"];
        bool changed = false;
        if (st.contains("background") && st["background"].is_string()) {
            std::string v = st["background"];
            // 프리셋 id와 파일명만 받는다 — 경로·URL은 페이지가 만들지 않는다
            bool ok = v.empty() || v.rfind("preset:", 0) == 0 ||
                      (v.rfind("file:", 0) == 0 && v.find('\\') == std::string::npos &&
                       v.find('/') == std::string::npos && v.find("..") == std::string::npos);
            if (ok && v != settings.style.background) { settings.style.background = v; changed = true; }
        }
        if (st.contains("font") && st["font"].is_string()) {
            std::string v = st["font"];
            if (v.size() <= 40 && v != settings.style.font) { settings.style.font = v; changed = true; }
        }
        if (st.contains("fontSize") && st["fontSize"].is_number()) {
            int v = st["fontSize"];
            if (v != 0 && (v < 11 || v > 24)) v = 0;
            if (v != settings.style.fontSize) { settings.style.fontSize = v; changed = true; }
        }
        if (changed) BroadcastEvent("style.changed", StyleJson());  // 모든 창이 즉시 갈아입는다
    }
    if (patch.contains("hotkeys") && patch["hotkeys"].is_object()) {
        auto& h = patch["hotkeys"];
        if (h.contains("enabled") && h["enabled"].is_boolean())
            settings.hotkeys.enabled = h["enabled"];
        // 값은 그대로 받아 두고 등록에서 걸러낸다 (해석 실패는 FailedHotkeys로 알린다)
        auto take = [&](const char* key, std::string& dst) {
            if (h.contains(key) && h[key].is_string()) {
                std::string v = h[key];
                if (v.size() <= 64) dst = v;
            }
        };
        take("toggleAll", settings.hotkeys.toggleAll);
        take("newMemo", settings.hotkeys.newMemo);
        take("list", settings.hotkeys.list);
        take("arrangeLeft", settings.hotkeys.arrangeLeft);
        take("arrangeRight", settings.hotkeys.arrangeRight);
        RegisterHotkeys();
        SendEventToManager("hotkeys.changed", {{"failed", failedHotkeys_}});
    }
    if (patch.contains("magnet") && patch["magnet"].is_object()) {
        auto& m = patch["magnet"];
        if (m.contains("enabled") && m["enabled"].is_boolean())
            settings.magnetEnabled = m["enabled"];
        if (m.contains("gap") && m["gap"].is_number()) {
            int v = m["gap"];
            settings.magnetGap = v < 0 ? 0 : (v > 200 ? 200 : v);
        }
        if (m.contains("sensitivity") && m["sensitivity"].is_string()) {
            std::string v = m["sensitivity"];
            if (v == "low" || v == "medium" || v == "high") settings.magnetSensitivity = v;
        }
    }
    if (patch.contains("prompts") && patch["prompts"].is_object()) {
        // AI 프롬프트 재정의: 준 키만 갱신하고, 빈 문자열이면 기본값으로 되돌린다는 뜻이라 지운다
        for (auto& [k, v] : patch["prompts"].items()) {
            if (!v.is_string()) continue;
            std::string val = v.get<std::string>();
            if (val.size() > 8000) val.resize(8000);
            if (val.empty()) settings.prompts.erase(k);
            else settings.prompts[k] = val;
        }
        BroadcastEvent("prompts.changed", {{"prompts", settings.prompts}});
    }
    if (patch.contains("highlightColors") && patch["highlightColors"].is_array()) {
        // 형광펜 사용자 색: 배열을 통째로 교체하고 모든 메모창에 즉시 방송한다
        std::vector<std::string> v;
        for (auto& c : patch["highlightColors"]) {
            if (!c.is_string() || v.size() >= 24) continue;
            std::string h = c.get<std::string>();
            bool ok = h.size() == 7 && h[0] == '#';
            for (size_t i = 1; ok && i < 7; i++)
                if (!isxdigit((unsigned char)h[i])) ok = false;
            if (ok) v.push_back(h);
        }
        if (v != settings.highlightColors) {
            settings.highlightColors = v;
            BroadcastEvent("highlight.colorsChanged", {{"colors", v}});
        }
    }
    if (patch.contains("trash") && patch["trash"].is_object()) {
        auto& t = patch["trash"];
        if (t.contains("enabled") && t["enabled"].is_boolean())
            settings.trashEnabled = t["enabled"];
        if (t.contains("retentionDays") && t["retentionDays"].is_number()) {
            int v = t["retentionDays"];
            settings.trashRetentionDays = v < 0 ? 0 : (v > 36500 ? 36500 : v);
            PurgeExpiredTrash();  // 기간 단축 시 즉시 반영
        }
    }
    store.SaveSettings(settings);

    if (themeChanged) {
        for (auto* w : stickers_) w->OnThemeChanged();
        for (auto* g : groups_) g->OnThemeChanged();
        if (manager_) manager_->OnThemeChanged();
        BroadcastEvent("theme.changed", {{"effective", EffectiveTheme()}});
    }
    if (langChanged) {
        i18n.Load(settings.language);
        BroadcastEvent("locale.changed", {{"lang", i18n.Lang()}});
    }
}

// 모든 창(메모·그룹·설정)에 이벤트를 방송
void App::BroadcastEvent(const std::string& ev, const json& data) {
    // 창마다 dump()를 반복하지 않도록 한 번만 직렬화해서 그대로 보낸다
    std::wstring payload = util::Utf8ToWide(json{{"event", ev}, {"data", data}}.dump());
    for (auto* w : stickers_) w->host().PostEventRaw(payload);
    for (auto* g : groups_) g->host().PostEventRaw(payload);
    if (manager_) manager_->host().PostEventRaw(payload);
    if (wizard_) wizard_->host().PostEventRaw(payload);
}

// Ollama 스트리밍은 청크가 초당 수십 개씩 오므로 모든 창에 뿌리면
// (창 수 x 청크 수)만큼 IPC·JS 콜백이 낭비된다. 구독자가 정해진 이벤트는 그 창에만 보낸다.
void App::SendEventToSticker(const std::string& stickerId, const std::string& ev,
                             const json& data) {
    if (auto* w = FindSticker(stickerId)) {
        w->host().PostEvent(ev, data);
        return;
    }
    BroadcastEvent(ev, data);  // 창을 못 찾으면(직후 파괴 등) 기존 동작으로 폴백
}

// AI 설정 마법사는 설정 창이 아니라 메모창 안에서도 돈다. 진행률·결과를 누가 받을지
// 요청이 ownerId로 정해 주면 그 창에, 없으면 지금까지처럼 설정 창에 보낸다.
void App::SendEventToOwner(const std::string& ownerId, const std::string& ev,
                           const json& data) {
    if (ownerId.empty()) {
        SendEventToManager(ev, data);
        return;
    }
    if (ownerId == "wizard") {  // 마법사 창 자신이 부른 설치·내려받기
        if (wizard_) wizard_->host().PostEvent(ev, data);
        else BroadcastEvent(ev, data);
        return;
    }
    SendEventToSticker(ownerId, ev, data);
}

// 설정(관리자) 창에 이벤트 — 창이 없으면 방송으로 폴백
void App::SendEventToManager(const std::string& ev, const json& data) {
    if (manager_) {
        manager_->host().PostEvent(ev, data);
        return;
    }
    BroadcastEvent(ev, data);
}

// ---------- 공통 브리지 ----------

namespace {

// 레지스트리의 문자열/DWORD 값 (없으면 기본값)
std::wstring RegStr(const wchar_t* subkey, const wchar_t* name) {
    wchar_t buf[256]{};
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, nullptr, buf, &cb) !=
        ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

// HKLM 레지스트리 DWORD 값 읽기 (없으면 0)
DWORD RegDword(const wchar_t* subkey, const wchar_t* name) {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) !=
        ERROR_SUCCESS) {
        return 0;
    }
    return v;
}

// "Windows 11 24H2 (빌드 26200.1234)" 형태.
// GetVersionEx는 매니페스트에 따라 거짓말을 하므로 레지스트리를 읽는다. ProductName도
// Windows 11에서 "Windows 10 …"으로 남아 있어(마이크로소프트가 고치지 않았다) 쓰지 않고,
// 빌드 번호로 10/11을 가른다(11 = 22000 이상).
std::string OsVersionString() {
    const wchar_t* kKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring buildStr = RegStr(kKey, L"CurrentBuildNumber");
    std::wstring display = RegStr(kKey, L"DisplayVersion");
    DWORD ubr = RegDword(kKey, L"UBR");
    long build = buildStr.empty() ? 0 : wcstol(buildStr.c_str(), nullptr, 10);

    std::wstring s = build >= 22000 ? L"Windows 11" : L"Windows 10";
    if (!display.empty()) s += L" " + display;
    if (build > 0) {
        s += L" (" + buildStr;
        if (ubr) s += L"." + std::to_wstring(ubr);
        s += L")";
    }
    return util::WideToUtf8(s);
}

// CPU 아키텍처 이름 (정보 탭 표시용)
std::string CpuArchString() {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        default: return "unknown";
    }
}

}  // namespace

json App::MakeInitJson(const std::string& page, const std::string& stickerId,
                       bool focusEditor) {
    return json{{"page", page},
                {"stickerId", stickerId},
                {"focusEditor", focusEditor},  // 새 메모: 페이지가 본문에 커서를 놓는다
                {"style", StyleJson()},         // 배경·서체·글자 크기 (깜빡임 없이 첫 그림부터)
                {"theme", EffectiveTheme()},
                {"lang", i18n.Lang()},
                {"country", util::UserCountry()},  // 캘린더의 국경일 표시에 쓴다
                {"autoHideUi", settings.autoHideUi},
                {"uiRevealOnClick", settings.uiRevealOnClick},
                {"highlightColors", settings.highlightColors},
                {"prompts", settings.prompts}};
}

// 모든 페이지가 함께 쓰는 브리지 메서드 등록 (설정·메모·그룹·AI·데이터·스타일·TTS)
void App::SetupCommonBridge(WebViewHost& host) {
    Bridge& b = host.bridge();

    // '정보' 탭이 쓰는 실행 시점의 사실들. 버전·릴리스 날짜는 Version.h(=CMakeLists)에서 온다.
    // 서드파티 고지 목록은 표시용 데이터라 manager.js가 들고 있다.
    b.Register("app.info", [this](const json&) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);

        // WebView2 런타임 버전 (설치돼 있지 않으면 빈 문자열 — UI가 안내 문구로 바꾼다)
        std::string wv2;
        wil::unique_cotaskmem_string wv2Ver;
        if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &wv2Ver)) && wv2Ver) {
            wv2 = util::WideToUtf8(wv2Ver.get());
        }

        return json{{"name", SS_APP_NAME},
                    {"version", SS_VERSION},
                    {"releaseDate", SS_RELEASE_DATE},
                    {"copyright", SS_COPYRIGHT},
                    {"developer", SS_DEVELOPER},
                    {"license", SS_LICENSE},
                    {"homepage", SS_HOMEPAGE},
                    {"exePath", util::WideToUtf8(exe)},
                    {"dataDir", util::WideToUtf8(store.AppDir())},
                    {"os", OsVersionString()},
                    {"arch", CpuArchString()},
                    {"webview2", wv2},
                    {"ollamaEndpoint", settings.ollama.endpoint}};
    });

    b.Register("app.getState", [this](const json&) {
        return json{{"settings",
                     {{"theme", settings.theme},
                      {"language", settings.language},
                      {"autostart", settings.autostart},
                      {"aiProvider", settings.aiProvider},
                      {"lmstudio",
                       {{"endpoint", settings.lmstudio.endpoint},
                        {"model", settings.lmstudio.model}}},
                      {"ollama",
                       {{"endpoint", settings.ollama.endpoint},
                        {"model", settings.ollama.model}}},
                      {"builtin",
                       {{"modelId", settings.builtin.modelId},
                        {"engine", settings.builtin.engine},
                        {"contextSize", settings.builtin.contextSize},
                        {"autoLoad", settings.builtin.autoLoad}}},
                      {"trash",
                       {{"enabled", settings.trashEnabled},
                        {"retentionDays", settings.trashRetentionDays}}},
                      {"uiScale", settings.uiScale},
                      {"autoHideUi", settings.autoHideUi},
                      {"uiRevealOnClick", settings.uiRevealOnClick},
                      {"style", StyleJson()},
                      {"tts", {{"voice", settings.tts.voice}, {"rate", settings.tts.rate}}},
                      {"hotkeys",
                       {{"enabled", settings.hotkeys.enabled},
                        {"toggleAll", settings.hotkeys.toggleAll},
                        {"newMemo", settings.hotkeys.newMemo},
                        {"list", settings.hotkeys.list},
                        {"arrangeLeft", settings.hotkeys.arrangeLeft},
                        {"arrangeRight", settings.hotkeys.arrangeRight},
                        {"failed", failedHotkeys_}}},
                      {"magnet",
                       {{"enabled", settings.magnetEnabled},
                        {"gap", settings.magnetGap},
                        {"sensitivity", settings.magnetSensitivity}}},
                      {"highlightColors", settings.highlightColors},
                      {"prompts", settings.prompts}}},
                    {"effectiveTheme", EffectiveTheme()},
                    {"lang", i18n.Lang()}};
    });

    b.Register("trash.count", [this](const json&) {
        return json{{"count", store.CountTrash()}};
    });

    b.Register("trash.list", [this](const json&) {
        json arr = json::array();
        for (auto& d : store.LoadTrash()) arr.push_back(Store::ToJson(d));
        return json{{"items", arr}};
    });

    b.Register("trash.restore", [this](const json& p) {
        std::string id = p.value("id", "");
        RunOnUi([this, id]() { RestoreTrashSticker(id); });
        return json{{"restored", true}};
    });

    b.Register("trash.purge", [this](const json& p) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        if (!ConfirmYesNo(owner, "confirm.purgeSticker")) return json{{"purged", false}};
        std::string id = p.value("id", "");
        for (auto& d : store.LoadTrash()) {
            if (d.id == id) {
                store.PurgeTrashSticker(d);
                break;
            }
        }
        return json{{"purged", true}, {"count", store.CountTrash()}};
    });

    b.Register("trash.empty", [this](const json&) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        EmptyTrashInteractive(owner);
        return json{{"count", store.CountTrash()}};
    });

    b.Register("settings.set", [this](const json& p) {
        ApplySettingsPatch(p);
        return json{{"effectiveTheme", EffectiveTheme()}};
    });

    b.Register("app.openManager", [this](const json& p) {
        std::string tab = p.value("tab", "list");
        RunOnUi([this, tab]() { OpenManager(tab); });
        return json::object();
    });

    b.Register("stickers.list", [this](const json&) {
        json arr = json::array();
        for (auto* w : stickers_) arr.push_back(Store::ToJson(w->data));
        for (auto& [id, d] : groupedStickers_) arr.push_back(Store::ToJson(d));
        return arr;
    });

    b.Register("groups.new", [this](const json&) {
        RunOnUi([this]() { NewGroup(); });
        return json::object();
    });

    b.Register("groups.list", [this](const json&) {
        json arr = json::array();
        for (auto* g : groups_) arr.push_back(Store::GroupToJson(g->data));
        return arr;
    });

    b.Register("groups.show", [this](const json& p) {
        std::string id = p.value("id", "");
        RunOnUi([this, id]() {
            if (auto* g = FindGroup(id)) {
                g->data.hidden = false;
                g->SaveData();
                g->ShowWin(true, true);
                SetForegroundWindow(g->hwnd());
            }
        });
        return json::object();
    });

    // 관리자 목록에서 그룹 삭제 (멤버 메모는 개별 스티커로 분리)
    b.Register("groups.delete", [this](const json& p) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        if (!ConfirmYesNo(owner, "group.deleteConfirm")) return json{{"deleted", false}};
        std::string id = p.value("id", "");
        RunOnUi([this, id]() { DeleteGroupReleaseMembers(id); });
        return json{{"deleted", true}};
    });

    // ---------- 다중 선택 (Ctrl+클릭 / Delete) ----------
    b.Register("selection.click", [this](const json& p) {
        std::string id = p.value("id", "");
        bool toggle = p.value("toggle", false);
        if (!id.empty()) OnStickerClicked(id, toggle);
        return json::object();
    });

    b.Register("selection.clear", [this](const json&) {
        ClearSelection();
        return json::object();
    });

    b.Register("selection.hide", [this](const json&) {
        RunOnUi([this]() { HideSelectedStickers(); });  // 창 파괴/숨김은 UI 스레드에서
        return json::object();
    });

    b.Register("stickers.show", [this](const json& p) {
        std::string id = p.value("id", "");
        // 메모 링크가 지워진 메모를 가리킬 수 있다 — 있는지 먼저 알려 준다
        bool known = FindSticker(id) != nullptr || groupedStickers_.count(id) > 0;
        if (known) RunOnUi([this, id]() { ShowSticker(id); });
        return json{{"shown", known}};
    });

    b.Register("stickers.delete", [this](const json& p) {
        if (!ConfirmYesNo(nullptr, DeleteConfirmKey())) return json{{"deleted", false}};
        std::string id = p.value("id", "");
        RunOnUi([this, id]() { DeleteSticker(id); });
        return json{{"deleted", true}};
    });

    // 스티커 폴더를 .ssticker(zip)로 내보내기 — 저장 위치는 파일 대화상자로 지정
    b.Register("sticker.export", [this](const json& p) {
        std::string id = p.value("id", "");
        StickerData* d = FindStickerData(id);
        if (!d) return json{{"started", false}};
        // 파일명 기본값: 제목(없으면 id) — 파일명 금지 문자는 '_'로 치환
        std::wstring name = util::Utf8ToWide(!d->title.empty() ? d->title : d->id);
        for (auto& c : name)
            if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
        if (name.size() > 60) name = name.substr(0, 60);
        // params.dest가 오면 대화상자 생략 (테스트용)
        std::wstring destOverride = p.contains("dest") && p["dest"].is_string()
                                        ? util::Utf8ToWide(p["dest"].get<std::string>())
                                        : L"";
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        wil::com_ptr<IFileSaveDialog> dlg;
        if (destOverride.empty() &&
            FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))))
            return json{{"started", false}};
        std::wstring dest = destOverride;
        if (dest.empty()) {
            COMDLG_FILTERSPEC filters[] = {{L"Super Stickers", L"*.ssticker"}};
            dlg->SetFileTypes(1, filters);
            dlg->SetDefaultExtension(L"ssticker");
            dlg->SetFileName((name + L".ssticker").c_str());
            if (FAILED(dlg->Show(owner))) return json{{"started", false}};
            wil::com_ptr<IShellItem> item;
            if (FAILED(dlg->GetResult(&item))) return json{{"started", false}};
            wil::unique_cotaskmem_string path;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                return json{{"started", false}};
            dest = path.get();
        }
        BroadcastEvent("app.flush", json::object());  // 편집 중 내용 먼저 저장
        RunOnUiDelayed(600, [this, id, dest]() {
            std::thread([this, id, dest]() {
                bool ok = store.ExportSticker(id, dest);
                std::string pathUtf8 = util::WideToUtf8(dest);
                RunOnUi([this, ok, pathUtf8]() {
                    BroadcastEvent("sticker.exportDone", {{"ok", ok}, {"path", pathUtf8}});
                });
            }).detach();
        });
        return json{{"started", true}};
    });

    // .ssticker 가져오기 — 파일 대화상자(다중 선택)
    b.Register("stickers.import", [this](const json&) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        wil::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))))
            return json{{"count", 0}};
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
        COMDLG_FILTERSPEC filters[] = {{L"Super Stickers", L"*.ssticker"}};
        dlg->SetFileTypes(1, filters);
        if (FAILED(dlg->Show(owner))) return json{{"count", 0}};
        wil::com_ptr<IShellItemArray> items;
        if (FAILED(dlg->GetResults(&items))) return json{{"count", 0}};
        DWORD n = 0;
        items->GetCount(&n);
        std::vector<std::wstring> paths;
        for (DWORD i = 0; i < n; ++i) {
            wil::com_ptr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item))) continue;
            wil::unique_cotaskmem_string path;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                paths.push_back(path.get());
        }
        std::vector<std::string> errors;
        int imported = ImportStickerFiles(paths, &errors);
        return json{{"count", imported}, {"errors", errors}};
    });

    // .ssticker 드래그앤드롭 — 네이티브가 File 객체에서 뽑은 전체 경로 목록
    b.Register("stickers.importPaths", [this](const json& p) {
        std::vector<std::wstring> paths;
        if (p.contains("paths") && p["paths"].is_array()) {
            for (auto& v : p["paths"])
                if (v.is_string()) paths.push_back(util::Utf8ToWide(v.get<std::string>()));
        }
        std::vector<std::string> errors;
        int n = ImportStickerFiles(paths, &errors);
        return json{{"count", n}, {"errors", errors}};
    });

    b.Register("stickers.new", [this](const json& p) {
        std::string type = p.value("type", "rich");
        RunOnUi([this, type]() { NewSticker(type); });
        return json::object();
    });

    b.Register("stickers.toggleAll", [this](const json&) {
        RunOnUi([this]() { ToggleAllVisible(); });
        return json::object();
    });

    b.Register("ai.listModels", [this](const json& p) {
        std::string requestId = p.value("requestId", "");
        std::string ownerId = p.value("ownerId", "");  // 설정 마법사가 메모창에서 부를 때
        std::string provider = p.value("provider", settings.aiProvider);
        bool openai = provider == "lmstudio";
        std::string endpoint = p.value(
            "endpoint", openai ? settings.lmstudio.endpoint : settings.ollama.endpoint);
        ai.ListModels(endpoint,
                      openai ? AiClient::Protocol::OpenAiSse : AiClient::Protocol::OllamaNdjson,
                      [this, requestId, provider, ownerId](bool ok,
                                                          std::vector<std::string> models,
                                                          std::string error) {
                          SendEventToOwner(ownerId, "ai.models", {{"requestId", requestId},
                                                           {"provider", provider},
                                                           {"ok", ok},
                                                           {"models", models},
                                                           {"error", error}});
                      });
        return json::object();
    });

    b.Register("ai.getConfig", [this](const json&) {
        json models = json::array();
        for (const auto& m : LocalAi::Catalog()) {
            models.push_back({{"id", m.id},
                              {"name", m.name},
                              {"params", m.params},
                              {"repo", m.repo},
                              {"license", m.license},
                              {"sizeBytes", m.sizeBytes},
                              {"recommended", m.recommended},
                              {"installed", localAi.ModelInstalled(m)}});
        }
        json engines = json::array();
        for (const auto& e : LocalAi::Engines()) {
            engines.push_back({{"id", e.id},
                               {"sizeBytes", e.sizeBytes},
                               {"installed", localAi.EngineInstalled(e.id)}});
        }
        return json{{"provider", settings.aiProvider},
                    {"builtinEnabled", Settings::kBuiltinBackendEnabled},
                    {"lmstudio",
                     {{"endpoint", settings.lmstudio.endpoint}, {"model", settings.lmstudio.model}}},
                    {"builtin",
                     {{"modelId", settings.builtin.modelId},
                      {"engine", settings.builtin.engine},
                      {"resolvedEngine", ResolvedEngineVariant()},
                      {"contextSize", settings.builtin.contextSize},
                      {"autoLoad", settings.builtin.autoLoad},
                      {"hasGpu", LocalAi::HasVulkanCapableGpu()},
                      {"serverRunning", localAi.ServerRunning()},
                      {"serverState", localAi.ServerStateName()},
                      {"loadingMs", localAi.LoadingElapsedMs()},
                      {"runningModel", localAi.RunningModel()},
                      {"busy", localAi.Busy()},
                      {"dir", util::WideToUtf8(localAi.AiDir())}}},
                    {"models", models},
                    {"engines", engines}};
    });

    b.Register("ai.installEngine", [this](const json& p) {
        std::string variant = p.value("variant", ResolvedEngineVariant());
        localAi.InstallEngine(
            variant,
            [this, variant](const std::string& stage, uint64_t recv, uint64_t total) {
                SendEventToManager("ai.engineProgress", {{"variant", variant},
                                                         {"stage", stage},
                                                         {"received", recv},
                                                         {"total", total}});
            },
            [this, variant](bool ok, const std::string& err) {
                SendEventToManager("ai.engineDone",
                                   {{"variant", variant}, {"ok", ok}, {"error", err}});
            });
        return json{{"started", true}};
    });

    b.Register("ai.downloadModel", [this](const json& p) {
        std::string id = p.value("id", "");
        localAi.DownloadModel(
            id,
            [this, id](const std::string& stage, uint64_t recv, uint64_t total) {
                SendEventToManager("ai.modelProgress", {{"id", id},
                                                        {"stage", stage},
                                                        {"received", recv},
                                                        {"total", total}});
            },
            [this, id](bool ok, const std::string& err) {
                SendEventToManager("ai.modelDone", {{"id", id}, {"ok", ok}, {"error", err}});
            });
        return json{{"started", true}};
    });

    b.Register("ai.openModelFolder", [this](const json& p) {
        const LocalAi::ModelInfo* m = LocalAi::FindModel(p.value("id", ""));
        if (!m) throw std::runtime_error("unknown model");
        std::wstring path = localAi.ModelPath(*m);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // 파일이 있으면 그 파일을 선택한 채로 연다
            std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                          SW_SHOWNORMAL);
        } else {
            std::wstring dir = localAi.AiDir() + L"\\models";
            util::EnsureDir(localAi.AiDir());
            util::EnsureDir(dir);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return json::object();
    });

    b.Register("ai.cancelDownload", [this](const json&) {
        localAi.CancelDownloads();
        return json::object();
    });

    b.Register("ai.deleteModel", [this](const json& p) {
        std::string id = p.value("id", "");
        bool ok = localAi.DeleteModel(id);
        if (ok && settings.builtin.modelId == id) {
            settings.builtin.modelId.clear();
            store.SaveSettings(settings);
        }
        return json{{"ok", ok}};
    });

    // 서버를 미리 띄워 둔다 (설정 화면의 '지금 시작'). 첫 응답 지연을 사용자가 통제할 수 있다.
    b.Register("ai.startServer", [this](const json&) {
        if (settings.builtin.modelId.empty()) throw std::runtime_error("no model selected");
        localAi.EnsureServer(settings.builtin.modelId, ResolvedEngineVariant(),
                             settings.builtin.contextSize,
                             [this](bool ok, const std::string& endpoint, const std::string& err) {
                                 SendEventToManager("ai.serverState",
                                                    {{"running", ok},
                                                     {"endpoint", endpoint},
                                                     {"error", err}});
                             });
        return json{{"starting", true}};
    });

    b.Register("ai.stopServer", [this](const json&) {
        localAi.StopServer();
        SendEventToManager("ai.serverState", {{"running", false}, {"endpoint", ""}, {"error", ""}});
        return json::object();
    });

    // 채팅은 백엔드를 감춘다 — 페이지는 provider를 모른 채 ai.chat만 부른다.
    // 내장 백엔드면 요청 시점에 llama-server를 띄우고(이미 떠 있으면 그대로) 이어서 보낸다.
    b.Register("ai.chat", [this](const json& p) {
        std::string requestId = p.value("requestId", "");
        std::string ownerId = p.value("ownerId", "");
        bool jsonFormat = p.value("jsonFormat", false);
        json jsonSchema = p.value("jsonSchema", json());
        // const operator[]는 키가 없으면 미정의 동작이다 — 검증해서 예외로 돌려준다
        json messages = p.value("messages", json());
        if (!messages.is_array() || messages.empty()) throw std::runtime_error("messages required");

        auto send = [this, requestId, ownerId](const std::string& endpoint,
                                               const std::string& model,
                                               AiClient::ChatOptions opts, const json& messages) {
            // 소유 창이 닫히면 AbortOllamaByOwner가 이 맵으로 요청을 찾아 중단한다
            if (!ownerId.empty()) ollamaOwners_[requestId] = ownerId;
            ai.Chat(
                requestId, endpoint, model, messages, opts,
                [this, requestId, ownerId](std::string delta) {
                    SendEventToSticker(ownerId, "ai.chunk",
                                       {{"requestId", requestId}, {"delta", delta}});
                },
                [this, requestId, ownerId](bool ok, std::string err) {
                    ollamaOwners_.erase(requestId);  // UI 스레드 콜백
                    SendEventToSticker(ownerId, "ai.done",
                                       {{"requestId", requestId}, {"ok", ok}, {"error", err}});
                });
        };

        if (settings.aiProvider == "builtin") {
            const LocalAi::ModelInfo* m = LocalAi::FindModel(settings.builtin.modelId);
            if (!m) throw std::runtime_error("no model selected");
            AiClient::ChatOptions opts;
            opts.protocol = AiClient::Protocol::OpenAiSse;
            opts.jsonFormat = jsonFormat;
            opts.jsonSchema = jsonSchema;
            opts.disableThinking = m->disableThinking;
            std::string variant = ResolvedEngineVariant();
            // 모델 로딩은 수십 초가 걸릴 수 있다. 준비되면 그때 요청을 보낸다.
            // 이미 같은 모델이 떠 있으면(설정에서 미리 올려 둔 경우) 로딩 안내를 보내지 않는다 —
            // 보내면 메모창이 첫 토큰이 올 때까지 "올리는 중"을 띄워 사용자를 헷갈리게 한다.
            const bool ready = localAi.ServerRunning() &&
                               localAi.RunningModel() == settings.builtin.modelId;
            if (!ready) {
                SendEventToSticker(ownerId, "ai.status",
                                   {{"requestId", requestId}, {"state", "loading"}});
            }
            localAi.EnsureServer(
                settings.builtin.modelId, variant, settings.builtin.contextSize,
                [this, requestId, ownerId, opts, messages, send](
                    bool ok, const std::string& endpoint, const std::string& err) {
                    if (!ok) {
                        SendEventToSticker(ownerId, "ai.done",
                                           {{"requestId", requestId},
                                            {"ok", false},
                                            {"error", err.empty() ? "server failed" : err}});
                        return;
                    }
                    send(endpoint, "supersticker", opts, messages);
                });
            return json::object();
        }

        // Ollama·LM Studio도 모델이 메모리에 없으면 첫 응답까지 수십 초가 걸린다(요청을 받고
        // 그때 올린다). 조회는 요청과 나란히 돌려 요청을 늦추지 않고, 아직 올라와 있지
        // 않을 때만 "모델을 올리는 중"을 보낸다. 응답이 이미 끝났으면 보내지 않는다.
        auto notifyIfLoading = [this, requestId, ownerId](const std::string& endpoint,
                                                          AiClient::Protocol protocol,
                                                          const std::string& model) {
            if (ownerId.empty()) return;
            ai.ModelLoaded(endpoint, protocol, model,
                           [this, requestId, ownerId](bool known, bool loaded) {
                               if (!known || loaded) return;
                               if (!ollamaOwners_.count(requestId)) return;  // 이미 끝난 요청
                               SendEventToSticker(
                                   ownerId, "ai.status",
                                   {{"requestId", requestId}, {"state", "loading"}});
                           });
        };

        AiClient::ChatOptions opts;
        opts.jsonFormat = jsonFormat;
        opts.jsonSchema = jsonSchema;
        if (settings.aiProvider == "lmstudio") {
            // LM Studio는 OpenAI 호환 서버다 — 내장 백엔드와 같은 경로·파싱을 쓴다
            opts.protocol = AiClient::Protocol::OpenAiSse;
            std::string model = p.value("model", settings.lmstudio.model);
            if (model.empty()) throw std::runtime_error("no model selected");
            send(settings.lmstudio.endpoint, model, opts, messages);
            notifyIfLoading(settings.lmstudio.endpoint, opts.protocol, model);
            return json::object();
        }
        opts.protocol = AiClient::Protocol::OllamaNdjson;
        std::string model = p.value("model", settings.ollama.model);
        if (model.empty()) throw std::runtime_error("no model selected");
        send(settings.ollama.endpoint, model, opts, messages);
        notifyIfLoading(settings.ollama.endpoint, opts.protocol, model);
        return json::object();
    });

    b.Register("ai.abort", [this](const json& p) {
        ai.Abort(p.value("requestId", ""));
        return json::object();
    });

    b.Register("ollama.pull", [this](const json& p) {
        std::string requestId = p.value("requestId", "");
        std::string ownerId = p.value("ownerId", "");  // 설정 마법사가 메모창에서 부를 때
        std::string name = p.value("name", "");
        if (name.empty()) throw std::runtime_error("model name required");
        activePulls_.insert(requestId);  // 설정 창 닫기 시 중단 대상
        ai.Pull(
            requestId, settings.ollama.endpoint, name,
            [this, requestId, ownerId](std::string status, uint64_t total,
                                       uint64_t completed) {
                SendEventToOwner(ownerId, "ollama.pullProgress",
                               {{"requestId", requestId},
                                {"status", status},
                                {"total", total},
                                {"completed", completed}});
            },
            [this, requestId, ownerId](bool ok, std::string err) {
                activePulls_.erase(requestId);  // UI 스레드 콜백
                SendEventToOwner(ownerId, "ollama.pullDone",
                               {{"requestId", requestId}, {"ok", ok}, {"error", err}});
            });
        return json::object();
    });

    b.Register("ollama.installOllama", [this](const json& p) {
        InstallOllama(p.value("ownerId", ""));  // 설정 마법사가 메모창에서 부를 때
        return json{{"started", true}};
    });

    // 마법사가 닫힐 때 진행 중인 설치를 접는다 (내려받기 단계만 접을 수 있고,
    // 설치 프로그램이 이미 돌고 있으면 그것은 끝까지 간다 — AbortOllamaTasks와 같은 규칙)
    b.Register("ollama.cancelInstall", [this](const json&) {
        installAbort_ = true;
        return json::object();
    });

    // ---------- AI 설정 마법사 창 ----------
    b.Register("wizard.open", [this](const json& p) {
        std::string ownerId = p.value("ownerId", "");
        bool force = p.value("force", false);
        RunOnUi([this, ownerId, force]() { OpenWizard(ownerId, force); });
        return json::object();
    });
    b.Register("wizard.close", [this](const json& p) {
        bool ok = p.value("ok", false);
        RunOnUi([this, ok]() { FinishWizard(ok); });
        return json::object();
    });

    // ---------- 읽어주기 (TTS) ----------
    b.Register("tts.voices", [this](const json&) {
        json arr = json::array();
        for (auto& v : tts.Voices())
            arr.push_back({{"id", v.id}, {"name", v.name}, {"lang", v.lang}});
        return json{{"voices", arr}};
    });
    // 읽기 시작. voice/rate를 주지 않으면 설정값을 쓴다 (설정 화면의 '시험 듣기'가 준다).
    b.Register("tts.speak", [this](const json& p) {
        std::string text = p.value("text", "");
        if (text.empty()) return json{{"ok", false}};
        std::string voice = p.value("voice", settings.tts.voice);
        int rate = p.value("rate", settings.tts.rate);
        return json{{"ok", tts.Speak(util::Utf8ToWide(text), voice, rate)}};
    });
    b.Register("tts.stop", [this](const json&) {
        tts.Stop();
        return json::object();
    });
    b.Register("tts.speaking", [this](const json&) {
        return json{{"speaking", tts.Speaking()}};
    });

    // 배경 이미지 파일 고르기. 고른 파일은 데이터 폴더의 style\ 아래로 복사해 두고
    // (원본이 옮겨져도 배경이 깨지지 않게) "file:<이름>"으로 저장한다. 이전 파일은 지운다.
    b.Register("style.pickBackground", [this](const json& p) {
        HWND owner = nullptr;
        if (p.contains("hwnd") && p["hwnd"].is_number()) owner = (HWND)(intptr_t)p["hwnd"].get<int64_t>();
        wil::com_ptr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg))))
            return json{{"changed", false}};
        COMDLG_FILTERSPEC filters[] = {
            {L"이미지 (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.svg)", L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.svg"},
            {L"모든 파일", L"*.*"}};
        dlg->SetFileTypes(2, filters);
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        if (FAILED(dlg->Show(owner))) return json{{"changed", false}};
        wil::com_ptr<IShellItem> item;
        wil::unique_cotaskmem_string path;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            return json{{"changed", false}};
        std::wstring src = path.get();
        std::wstring ext = src.substr(src.find_last_of(L'.') == std::wstring::npos ? src.size()
                                                                                  : src.find_last_of(L'.'));
        for (auto& c : ext) c = (wchar_t)towlower(c);
        std::wstring dir = store.AppDir() + L"\\style";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring name = L"bg-" + std::to_wstring(GetTickCount64()) + ext;
        if (!CopyFileW(src.c_str(), (dir + L"\\" + name).c_str(), FALSE))
            return json{{"changed", false}};
        // 이전 사용자 파일 정리
        if (settings.style.background.rfind("file:", 0) == 0) {
            std::wstring old = dir + L"\\" + util::Utf8ToWide(settings.style.background.substr(5));
            DeleteFileW(old.c_str());
        }
        settings.style.background = "file:" + util::WideToUtf8(name);
        store.SaveSettings(settings);
        BroadcastEvent("style.changed", StyleJson());
        return json{{"changed", true}, {"background", settings.style.background}};
    });

    b.Register("ollama.checkInstalled", [](const json&) {
        return json{{"installed", IsOllamaInstalled()}};
    });

    // ---------- 데이터 탭 ----------
    b.Register("data.getPath", [this](const json&) {
        return json{{"path", util::WideToUtf8(store.AppDir())}};
    });

    b.Register("data.openFolder", [this](const json&) {
        ShellExecuteW(nullptr, L"open", store.AppDir().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return json::object();
    });

    // 저장 경로 변경: 새 폴더에 데이터를 복사한 뒤 datadir.txt 포인터를 바꾸고 재시작.
    // params.path가 오면 폴더 선택 대화상자를 생략한다 (테스트용).
    b.Register("data.changeLocation", [this](const json& p) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        std::wstring dir = p.contains("path") && p["path"].is_string()
                               ? util::Utf8ToWide(p["path"].get<std::string>())
                               : PickFolder(owner);
        if (dir.empty()) return json{{"changed", false}};
        std::wstring cur = store.AppDir();
        if (_wcsicmp(dir.c_str(), cur.c_str()) == 0) return json{{"changed", false}};
        // 현재 데이터 폴더의 하위 폴더는 무한 복사가 되므로 거부
        std::wstring curPrefix = cur + L"\\";
        if (dir.size() > curPrefix.size() &&
            _wcsnicmp(dir.c_str(), curPrefix.c_str(), curPrefix.size()) == 0) {
            MessageBoxW(owner, i18n.T("data.changeFailed").c_str(), L"Super Stickers",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return json{{"changed", false}};
        }
        if (!ConfirmYesNo(owner, "confirm.changeDataDir")) return json{{"changed", false}};

        BroadcastEvent("app.flush", json::object());  // 편집 중 내용 저장
        RunOnUiDelayed(600, [this, dir, cur, owner]() {
            bool ok = util::CopyDirRecursive(cur, dir) && store.SetCustomDataDir(dir);
            if (!ok) {
                MessageBoxW(owner, i18n.T("data.changeFailed").c_str(), L"Super Stickers",
                            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                return;
            }
            MessageBoxW(owner, i18n.T("data.changeDone").c_str(), L"Super Stickers",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            // 단일 인스턴스 뮤텍스와 겹치지 않게 2초 지연 후 재시작
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & start \"\" \"" +
                               std::wstring(exe) + L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(0);
            if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            Quit();
        });
        return json{{"changed", true}};
    });

    // 백업 ZIP 생성. params.dir이 오면 대화상자 생략 (테스트용).
    b.Register("data.backup", [this](const json& p) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        std::wstring dir = p.contains("dir") && p["dir"].is_string()
                               ? util::Utf8ToWide(p["dir"].get<std::string>())
                               : PickFolder(owner);
        if (dir.empty()) return json{{"started", false}};
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"SuperStickers-Backup-%04u%02u%02u-%02u%02u%02u.zip", st.wYear,
                   st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::wstring zipPath = dir + L"\\" + name;
        std::wstring dataDir = store.AppDir();
        BroadcastEvent("app.flush", json::object());  // 편집 중 내용 저장
        // 압축은 오래 걸릴 수 있어 워커 스레드에서 실행하고 완료 이벤트로 알린다
        RunOnUiDelayed(600, [this, dataDir, zipPath]() {
            std::thread([this, dataDir, zipPath]() {
                bool ok = util::ZipDir(dataDir, zipPath);
                std::string zipUtf8 = util::WideToUtf8(zipPath);
                RunOnUi([this, ok, zipUtf8]() {
                    BroadcastEvent("data.backupDone", {{"ok", ok}, {"path", zipUtf8}});
                });
            }).detach();
        });
        return json{{"started", true}};
    });

    // 모든 스티커 목록(관리자) 창 열기 — 스티커/그룹 타이틀바 버튼용
    b.Register("data.deleteAll", [this](const json&) {
        HWND owner = manager_ ? manager_->hwnd() : nullptr;
        // 개수 세기는 모든 memo.json을 통째로 파싱한다 — 함수 안에서 한 번이면 충분하다
        return json{{"deleted", DeleteAllDataInteractive(owner)}};
    });

    b.Register("app.openExternal", [](const json& p) {
        std::string url = p.value("url", "");
        if (url.rfind("https://", 0) != 0) throw std::runtime_error("https only");
        ShellExecuteW(nullptr, L"open", util::Utf8ToWide(url).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return json::object();
    });
}

// ---------- 트레이 ----------

void App::ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    HMENU newMenu = CreatePopupMenu();  // TrackPopupMenu 후 부모와 함께 파괴됨
    AppendMenuW(newMenu, MF_STRING, IDM_TRAY_NEW_RICH, i18n.T("menu.newRich").c_str());
    AppendMenuW(newMenu, MF_STRING, IDM_TRAY_NEW_MD, i18n.T("menu.newMarkdown").c_str());
    AppendMenuW(newMenu, MF_STRING, IDM_TRAY_NEW_FILE, i18n.T("menu.newFile").c_str());
    AppendMenuW(newMenu, MF_STRING, IDM_TRAY_NEW_WEB, i18n.T("menu.newWeb").c_str());
    AppendMenuW(newMenu, MF_STRING, IDM_TRAY_NEW_PDF, i18n.T("menu.newPdf").c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)newMenu, i18n.T("tray.newSticker").c_str());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_NEW_GROUP, i18n.T("tray.newGroup").c_str());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW_ALL, i18n.T("tray.showAll").c_str());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_HIDE_ALL, i18n.T("tray.hideAll").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_LIST, i18n.T("tray.list").c_str());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, i18n.T("tray.settings").c_str());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, i18n.T("tray.about").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EMPTY_TRASH, i18n.T("tray.emptyTrash").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, i18n.T("tray.quit").c_str());

    SetForegroundWindow(hwnd_);  // 메뉴 밖 클릭 시 닫히도록 하는 관례
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

// 앱 종료: 단축키·TTS·AI 작업을 정리하고, 페이지가 저장을 마칠 시간을 준 뒤 창을 부순다
void App::Quit() {
    if (quitting_) return;
    quitting_ = true;
    UnregisterHotkeys();
    tts.Shutdown();  // CoUninitialize 전에 SAPI 객체를 놓는다
    // 수 GB를 물고 있는 자식 프로세스를 남기지 않는다 (잡 오브젝트는 비정상 종료용 보험)
    localAi.CancelDownloads();
    localAi.StopServer();
    ai.AbortAll();  // 스트리밍 중인 워커가 종료 뒤까지 서버를 붙들고 있지 않도록
    // 웹 측 자동 저장 디바운스를 플러시할 시간을 준 뒤 종료
    BroadcastEvent("app.flush", json::object());
    SetTimer(hwnd_, kQuitTimerId, 350, nullptr);
}

// 정적 윈도우 프로시저 → 인스턴스 메서드로
LRESULT CALLBACK App::SWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return App::I().WndProc(hwnd, msg, wp, lp);
}

// 숨김 앱 창의 메시지 처리 (트레이·타이머·전역 단축키·표시 변경·종료)
LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        tray_.Recreate();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    break;
                case WM_LBUTTONUP:
                    // 더블클릭은 UP → DBLCLK → UP 순으로 오므로, 더블클릭 뒤에 따라오는
                    // UP은 무시해야 단일 클릭 동작이 함께 실행되지 않는다
                    if (trayIgnoreNextUp_) {
                        trayIgnoreNextUp_ = false;
                        break;
                    }
                    // 더블클릭 여부를 기다렸다가(더블클릭 대기 시간) 단일 클릭 처리
                    SetTimer(hwnd_, kTrayClickTimerId, GetDoubleClickTime(), nullptr);
                    break;
                case WM_LBUTTONDBLCLK:
                    KillTimer(hwnd_, kTrayClickTimerId);  // 단일 클릭 동작 취소
                    trayIgnoreNextUp_ = true;              // 뒤따르는 UP 무시
                    OpenManager("list");                   // 더블클릭: 모든 스티커 목록
                    break;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_TRAY_NEW:
                case IDM_TRAY_NEW_RICH:
                    NewSticker("rich");
                    break;
                case IDM_TRAY_NEW_MD:
                    NewSticker("markdown");
                    break;
                case IDM_TRAY_NEW_FILE:
                    NewSticker("file");
                    break;
                case IDM_TRAY_NEW_WEB:
                    NewSticker("web");
                    break;
                case IDM_TRAY_NEW_PDF:
                    NewSticker("pdf");
                    break;
                case IDM_TRAY_NEW_GROUP:
                    NewGroup();
                    break;
                case IDM_TRAY_SHOW_ALL:
                    SetAllVisible(true);
                    break;
                case IDM_TRAY_HIDE_ALL:
                    SetAllVisible(false);
                    break;
                case IDM_TRAY_LIST:
                    OpenManager("list");
                    break;
                case IDM_TRAY_SETTINGS:
                    OpenManager("settings");
                    break;
                case IDM_TRAY_ABOUT:
                    OpenManager("about");
                    break;
                case IDM_TRAY_EMPTY_TRASH:
                    EmptyTrashInteractive(nullptr);
                    break;
                case IDM_TRAY_QUIT:
                    // Ollama 설치·모델 내려받기가 도는 중이면 확인 후에만 끝낸다
                    // (설정 창의 닫기와 같은 규칙 — ManagerWindow의 WM_CLOSE 참고)
                    if (HasActiveOllamaTasks()) {
                        if (!ConfirmYesNo(hwnd_, "confirm.closeCancelsDownloads")) break;
                        AbortOllamaTasks();
                    }
                    Quit();
                    break;
            }
            return 0;

        case WM_HOTKEY:
            OnHotkey((int)wp);
            return 0;

        case WM_APP_RUNNABLE: {
            auto* fn = (std::function<void()>*)lp;
            (*fn)();
            delete fn;
            return 0;
        }

        case WM_COPYDATA:  // 두 번째 인스턴스 실행 → 전체 표시
            SetAllVisible(true);
            return TRUE;

        case WM_DISPLAYCHANGE:
            // 해상도/모니터 구성 변경: 안정화 후 전체 창을 작업 영역 안으로 보정
            RunOnUiDelayed(400, [this]() { ClampAllWindowsToScreen(); });
            return 0;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) {
                // 작업 표시줄 위치/크기 변경
                RunOnUiDelayed(400, [this]() { ClampAllWindowsToScreen(); });
                return 0;
            }
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0 &&
                settings.theme == "system") {
                for (auto* w : stickers_) w->OnThemeChanged();
                if (manager_) manager_->OnThemeChanged();
                if (wizard_) wizard_->OnThemeChanged();
                BroadcastEvent("theme.changed", {{"effective", EffectiveTheme()}});
            }
            return 0;

        case WM_TIMER:
            if (wp == kQuitTimerId) {
                KillTimer(hwnd, kQuitTimerId);
                tray_.Destroy();
                while (!stickers_.empty()) stickers_.back()->Destroy();
                while (!groups_.empty()) groups_.back()->Destroy();
                if (manager_) DestroyWindow(manager_->hwnd());
                if (wizard_) DestroyWindow(wizard_->hwnd());
                DestroyWindow(hwnd_);
            } else if (wp == kTrashTimerId) {
                PurgeExpiredTrash();
            } else if (wp == kAlarmTimerId) {
                CheckCalendarAlarms();
            } else if (wp == kTrayClickTimerId) {
                KillTimer(hwnd, kTrayClickTimerId);
                BringAllToFront();  // 트레이 단일 클릭: 보이는 스티커 모두 맨 앞으로
            } else if (auto it = delayedTasks_.find(wp); it != delayedTasks_.end()) {
                KillTimer(hwnd, wp);
                auto fn = std::move(it->second);
                delayedTasks_.erase(it);
                fn();
            }
            return 0;

        // 바깥에서 온 종료 요청(설치 프로그램, 작업 관리자의 '작업 끝내기', 로그오프).
        // 그냥 창을 부수면 웹 쪽 자동 저장 디바운스가 남은 채 끝나므로 Quit로 보낸다 —
        // app.flush를 방송해 편집 중이던 내용을 저장할 시간을 준 뒤 종료한다.
        case WM_CLOSE:
            Quit();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
