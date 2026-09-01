#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <set>
#include <map>
#include <string>
#include <vector>

#include <json.hpp>

#include "AiClient.h"
#include "I18n.h"
#include "LocalAi.h"
#include "Store.h"
#include "TrayIcon.h"

class StickerWindow;
class GroupWindow;
class ManagerWindow;
class WebViewHost;

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_RUNNABLE = WM_APP + 2;

class App {
public:
    static App& I();

    bool Init(HINSTANCE hinst, bool startHidden);
    HINSTANCE hinst() const { return hinst_; }

    Settings settings;
    Store store;
    I18n i18n;
    AiClient ai;      // Ollama·내장 공통 HTTP 클라이언트
    LocalAi localAi;  // 내장 백엔드(llama-server) 관리

    std::string EffectiveTheme() const;  // "light" | "dark"
    // 삭제 확인 네이티브 대화상자. 그룹 콘텐츠 창은 SetWindowRgn으로 잘려 있어
    // 페이지 내 confirm()이 반투명/절단되어 보이므로 항상 네이티브로 띄운다.
    static bool ConfirmYesNo(HWND owner, const std::string& msgKey);
    static bool ConfirmYesNoText(HWND owner, const std::wstring& msg);
    // 삭제 확인 문구: 휴지통 사용 여부에 따라 "휴지통으로 이동"/"완전 삭제"를 구분
    std::string DeleteConfirmKey() const {
        return settings.trashEnabled ? "confirm.deleteToTrash" : "confirm.deleteSticker";
    }
    void RunOnUi(std::function<void()> fn);
    void RunOnUiDelayed(UINT delayMs, std::function<void()> fn);

    // 스티커 관리
    StickerWindow* CreateStickerWindow(const StickerData& d, bool show, bool activate);
    void NewSticker(const std::string& type = "rich");
    void DeleteSticker(const std::string& id);
    StickerWindow* FindSticker(const std::string& id);
    void ShowSticker(const std::string& id);
    bool AnyStickerVisible() const;
    void SetAllVisible(bool visible);
    void ToggleAllVisible();
    void BringAllToFront();  // 보이는 모든 스티커·그룹 창을 맨 앞으로
    void OnStickerDestroyed(StickerWindow* w);

    // 그룹 관리
    GroupWindow* FindGroup(const std::string& id);
    GroupWindow* CreateGroupWindow(const GroupData& g, bool show, bool activate);
    void NewGroup();  // 커서 근처에 새 그룹 생성
    void DeleteGroupReleaseMembers(const std::string& groupId);  // 멤버는 플로팅으로 분리
    void OnGroupDestroyed(GroupWindow* w);
    void AddStickerToGroup(StickerWindow* w, GroupWindow* g);  // 창 파괴 후 그룹에 흡수
    // 그룹에서 분리. (x,y) 물리 좌표가 주어지면 그 지점에 배치하고,
    // 그 지점이 다른 그룹 위이면 해당 그룹으로 이동.
    void PopOutStickerAt(const std::string& stickerId, int x = -1, int y = -1);
    void NewMemoInGroup(const std::string& groupId, const std::string& type = "rich");
    void ReorderGroupMembers(GroupWindow* g, const std::vector<std::string>& order);
    void SaveMemberContent(const nlohmann::json& p);  // 그룹 카드 인라인 편집 저장
    StickerData* FindStickerData(const std::string& id);  // 창 유무 무관 데이터 조회

    // ---------- 메모창 다중 선택 ----------
    // Shift+클릭으로 여러 창을 고르고, 함께 옮기거나(드래그) 한 번에 숨긴다(Delete).
    // 세션 한정 상태 — 저장하지 않는다.
    bool IsSelected(const std::string& id) const { return selected_.count(id) > 0; }
    bool HasMultiSelection() const { return selected_.size() > 1; }
    const std::set<std::string>& Selection() const { return selected_; }
    // 페이지의 클릭 보고: shift면 토글, 아니면 (선택되지 않은 창일 때만) 전체 해제.
    // 이미 선택된 창을 그냥 클릭하는 것은 함께 드래그하려는 동작이므로 선택을 지킨다.
    void OnStickerClicked(const std::string& id, bool shift);
    void ClearSelection();
    void SyncSelectionLook();  // 선택 테두리 갱신 + selection.changed 방송
    void HideSelectedStickers();  // Delete — 선택된 창을 모두 숨김 상태로

    // 자석 정렬: 드래그 중인 메모창을 다른 메모창의 간격·가장자리에 맞춰 붙인다.
    // rect는 WM_MOVING이 준 제안 위치이며, 붙을 자리가 있으면 그 자리로 보정한다.
    void SnapStickerRect(StickerWindow* self, RECT* rect);

    // 리사이즈 중 자석. 이동과 달리 잡고 있는 변만 움직여야 하므로 사각형을 통째로
    // 옮기지 않고 그 변의 좌표만 당긴다. edge는 WM_SIZING의 wParam(WMSZ_*).
    void SnapStickerResize(StickerWindow* self, RECT* rect, int edge);

    // 플로팅 스티커 드래그 → 그룹 드롭 감지
    void HandleStickerMoveEnd(StickerWindow* w);
    void UpdateDragHover(StickerWindow* w);  // WM_MOVING 중 하이라이트

    // 해상도/모니터 변경 시 모든 창을 작업 영역 안으로 보정
    void ClampAllWindowsToScreen();

    // 진행 중인 Ollama 요청을 소유 창(스티커 id) 기준으로 중단
    // — 스티커 삭제/창 파괴 시 낭비되는 생성 요청을 취소
    void AbortOllamaByOwner(const std::string& ownerId);

    // Ollama 공식 설치 프로그램을 내려받아 무인 설치 (워커 스레드).
    // 진행: ollama.installProgress {stage, total, received} / 완료: ollama.installDone.
    void InstallOllama();
    // 설정의 "auto"를 실제 엔진 변형으로 푼다 ("cpu" | "vulkan")
    std::string ResolvedEngineVariant() const;
    // 시작 시 자동 로드 (설정이 켜져 있고 엔진·모델이 준비된 경우에만)
    void MaybeAutoLoadModel();
    static bool IsOllamaInstalled();
    // 설정 창을 닫을 때: 진행 중인 설치/모델 다운로드 확인·중단
    bool HasActiveOllamaTasks() const;
    void AbortOllamaTasks();

    // 휴지통
    void PurgeExpiredTrash();                 // 보관 기간 지난 항목 완전 삭제
    void EmptyTrashInteractive(HWND owner);   // 확인 팝업 → 휴지통 비우기
    void RestoreTrashSticker(const std::string& id);  // 개별 스티커로 복원·표시

    // .ssticker 가져오기 (파일 선택/드래그앤드롭 공용). 가져온 개수 반환.
    // errors에는 실패한 파일별 원인이 순서대로 쌓인다 (ext/copy/expand/parse/move).
    int ImportStickerFiles(const std::vector<std::wstring>& paths,
                           std::vector<std::string>* errors = nullptr);

    // 데이터 탭: 모든 메모·그룹·휴지통 완전 삭제 (확인 팝업 포함, 설정은 보존)
    void DeleteAllDataInteractive(HWND owner);

    // Manager(설정/목록) 창
    void OpenManager(const std::string& tab);
    void OnManagerDestroyed();

    // 설정 반영 + 웹 브로드캐스트
    void ApplySettingsPatch(const nlohmann::json& patch);
    void BroadcastEvent(const std::string& ev, const nlohmann::json& data);
    // 구독 창이 정해진 이벤트의 타깃 전달 (창이 없으면 브로드캐스트 폴백)
    void SendEventToSticker(const std::string& stickerId, const std::string& ev,
                            const nlohmann::json& data);
    void SendEventToManager(const std::string& ev, const nlohmann::json& data);

    // 브리지: 모든 창에 공통으로 등록되는 메서드 (settings/ollama/stickers/app)
    void SetupCommonBridge(WebViewHost& host);
    nlohmann::json MakeInitJson(const std::string& page, const std::string& stickerId);

    void Quit();

private:
    static LRESULT CALLBACK SWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void ShowTrayMenu();
    void OnEnvironmentReady(bool startHidden);

    GroupWindow* GroupUnderCursor();          // 커서 아래 표시 중인 그룹 (없으면 nullptr)
    GroupWindow* GroupAtPoint(POINT pt);      // 특정 지점의 표시 중인 그룹 (z-order 상단 우선)

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;  // 숨김 최상위 창 (트레이/브로드캐스트 수신)
    TrayIcon tray_;
    std::vector<StickerWindow*> stickers_;
    std::vector<GroupWindow*> groups_;
    std::map<std::string, StickerData> groupedStickers_;  // 창이 없는(그룹 소속) 메모 데이터
    ManagerWindow* manager_ = nullptr;
    std::string lastDragHoverGroup_;
    std::map<UINT_PTR, std::function<void()>> delayedTasks_;
    std::set<std::string> selected_;  // 다중 선택된 스티커 id (세션 한정)
    std::map<std::string, std::string> ollamaOwners_;  // requestId → 스티커 id
    std::atomic<bool> installingOllama_{false};
    std::atomic<bool> installAbort_{false};       // 설치 다운로드 중단 플래그
    std::set<std::string> activePulls_;           // 진행 중인 모델 다운로드 requestId
    UINT_PTR nextTimerId_ = 100;
    UINT taskbarCreatedMsg_ = 0;
    bool quitting_ = false;
    bool trayIgnoreNextUp_ = false;  // 트레이 더블클릭 뒤 따라오는 UP 무시
};
