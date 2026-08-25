#pragma once
#include <map>
#include <string>
#include <vector>

#include <json.hpp>

struct OllamaSettings {
    std::string endpoint = "http://localhost:11434";
    std::string model;
};

struct Settings {
    std::string theme = "system";  // "light" | "dark" | "system"
    std::string language;          // "ko" | "en" (빈 값이면 OS 언어로 결정)
    bool autostart = false;
    OllamaSettings ollama;
    bool trashEnabled = true;      // false면 삭제 시 즉시 완전 삭제
    int trashRetentionDays = 30;   // 휴지통 보관 일수, 0 = 자동 삭제하지 않음
    double uiScale = 1.0;          // 전체 UI 배율 (0.3 ~ 2.0, 1.0 = 100%)
};

struct StickerData {
    std::string id;
    std::string type = "rich";     // "rich" | "markdown" | "file" | "web" | "pdf"
    std::string html;              // rich 본문
    std::string markdown;          // markdown 원본 텍스트
    std::string mode = "rich";     // (레거시) type 마이그레이션 소스
    std::vector<std::string> files;  // file 메모: 파일/폴더 전체 경로 목록
    std::string fileView = "list";   // file 메모 보기: "list" | "thumbS" | "thumbL"
    std::string url;               // web 메모: 처음 등록한 URL
    std::string lastUrl;           // web 메모: 마지막으로 본 페이지
    std::string pdfName;           // pdf 메모: attachments\ 파일명
    std::string pdfTitle;          // pdf 메모: 표시용 원본 파일명
    std::string groupId;           // 소속 그룹 id (빈 값 = 플로팅 창)
    std::string color = "yellow";  // 프리셋 이름(레거시) 또는 "#RRGGBB"
    int x = 100, y = 100, w = 340, h = 300;
    bool topmost = false;
    bool hidden = false;
    std::vector<std::string> attachments;  // attachments\ 하위 파일명
    std::string createdAt, updatedAt;
    std::string deletedAt;  // 휴지통 이동 시각 (휴지통 항목에만 존재)
    std::vector<std::string> tags;  // 사용자/AI 태그
    std::string title;              // AI 제목 (한국어)
    std::string summary;            // AI 요약 (한국어)
    std::string titleEn;            // AI 제목 (영어)
    std::string summaryEn;          // AI 요약 (영어)
    std::string transKo;            // 본문 한국어 번역 (원문이 영어일 때)
    std::string transEn;            // 본문 영어 번역 (원문이 한국어일 때)
    std::string srcLang;            // AI가 판별한 원문 언어 "ko" | "en"
    std::string viewLang;           // 사용자가 선택한 표시 언어 (빈 값 = 원문)
    bool needsReview = false;       // 마지막 AI Review 이후 내용이 수정됨
};

struct GroupData {
    std::string id;
    std::string title;
    std::string layout = "grid";   // "grid"(균일) | "masonry"(혼합) | "list"(목록)
    std::string gridSize = "m";    // grid 정렬의 카드 크기: "s" | "m" | "l" (전역)
    std::map<std::string, int> memberHeights;  // 멤버별 카드 높이(CSS px, 없으면 기본)
    std::string color;             // 배경색 hex (빈 값 = 테마 기본)
    double opacity = 1.0;         // 창 투명도 0.3 ~ 1.0
    int x = 160, y = 160, w = 520, h = 420;
    bool hidden = false;
    std::vector<std::string> memberIds;  // 카드 표시 순서
    std::string createdAt, updatedAt;
};

class Store {
public:
    void Init();  // %APPDATA%\SuperSticker 하위 디렉터리 생성

    Settings LoadSettings();
    void SaveSettings(const Settings& s);

    // hadErrors: 파싱 실패로 건너뛴 파일이 있으면 true (첨부 GC 안전장치)
    std::vector<StickerData> LoadAllStickers(bool* hadErrors = nullptr);
    void SaveSticker(const StickerData& d);
    void DeleteSticker(const StickerData& d);  // json + 첨부 삭제

    // base64 데이터를 attachments\에 저장하고 파일명을 반환 (실패 시 빈 문자열)
    std::string SaveAttachment(const std::string& base64, const std::string& ext);
    // 파일 경로를 attachments\로 복사하고 파일명을 반환
    std::string ImportAttachment(const std::wstring& srcPath);

    // 어떤 스티커도 참조하지 않는 첨부 파일 삭제 (loadFailed면 건너뜀)
    // 휴지통 항목이 참조하는 첨부도 보존 대상에 포함한다.
    void GarbageCollectAttachments(const std::vector<StickerData>& stickers, bool loadFailed);

    // 휴지통: json을 trash\로 옮기고 deletedAt을 기록. 첨부는 완전 삭제 때까지 유지.
    void MoveStickerToTrash(StickerData d);
    std::vector<StickerData> LoadTrash(bool* hadErrors = nullptr);
    void PurgeTrashSticker(const StickerData& d);  // trash json + 첨부 완전 삭제
    void RemoveTrashEntry(const std::string& id);  // trash json만 제거 (복원 시 — 첨부 유지)
    int EmptyTrash();                              // 전체 완전 삭제, 삭제된 항목 수 반환
    int CountTrash();

    std::vector<GroupData> LoadAllGroups();
    void SaveGroup(const GroupData& g);
    void DeleteGroup(const std::string& id);

    std::wstring AppDir() const;
    std::wstring StickersDir() const;
    std::wstring GroupsDir() const;
    std::wstring AttachmentsDir() const;
    std::wstring TrashDir() const;

    static nlohmann::json ToJson(const StickerData& d);
    static StickerData FromJson(const nlohmann::json& j);
    static nlohmann::json GroupToJson(const GroupData& g);
    static GroupData GroupFromJson(const nlohmann::json& j);
};
