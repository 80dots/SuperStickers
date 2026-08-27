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
    bool autoHideUi = true;        // 메모창 전용: 마우스가 벗어나면 헤더·서식 툴바 자동 숨김
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
    std::string pdfName;           // pdf 메모: 메모 폴더 기준 경로 ("PDF/xxx.pdf")
    std::string pdfTitle;          // pdf 메모: 표시용 원본 파일명
    std::string groupId;           // 소속 그룹 id (빈 값 = 플로팅 창)
    std::string color = "yellow";  // 프리셋 이름(레거시) 또는 "#RRGGBB"
    int x = 100, y = 100, w = 340, h = 300;
    bool topmost = false;
    bool hidden = false;
    std::vector<std::string> attachments;  // 메모 폴더 기준 경로 ("Image/xxx.png")
    std::string createdAt, updatedAt;
    std::string deletedAt;  // 휴지통 이동 시각 (휴지통 항목에만 존재)
    std::vector<std::string> tags;    // 사용자가 직접 추가한 태그 (AI Review가 건드리지 않음)
    std::vector<std::string> aiTags;  // AI Review가 생성한 태그 (리뷰마다 새로 대체)
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
    bool topmost = false;
    bool hidden = false;
    std::vector<std::string> memberIds;  // 카드 표시 순서
    std::string createdAt, updatedAt;
};

// 메모 폴더 안의 첨부(상대 경로)를 가리키는 페이지용 URL
inline std::string AttachmentUrl(const std::string& stickerId, const std::string& rel) {
    return "https://data.sticker/stickers/" + stickerId + "/" + rel;
}

class Store {
public:
    void Init();  // %APPDATA%\SuperSticker 하위 디렉터리 생성

    Settings LoadSettings();
    void SaveSettings(const Settings& s);

    // hadErrors: 파싱 실패로 건너뛴 파일이 있으면 true (첨부 GC 안전장치)
    std::vector<StickerData> LoadAllStickers(bool* hadErrors = nullptr);
    void SaveSticker(const StickerData& d);
    void DeleteSticker(const StickerData& d);  // json + 첨부 삭제

    // 메모 폴더 안에 첨부를 저장한다. kind: "image" | "video" | "pdf" | "3d" | "" (확장자 추론)
    // 반환값은 메모 폴더 기준 상대 경로 (예: "Image/xxx.png"). 실패 시 빈 문자열.
    std::string SaveAttachment(const std::string& stickerId, const std::string& base64,
                               const std::string& ext, const std::string& kind = "");
    std::string ImportAttachment(const std::string& stickerId, const std::wstring& srcPath,
                                 const std::string& kind = "");

    // 메모 폴더에서 참조되지 않는 첨부 파일 정리 (썸네일 교체 등으로 남은 파일)
    void GarbageCollectMemoFiles(const StickerData& d);

    // 구버전 레이아웃(스티커별 json 파일 + 공용 attachments 폴더) 정리
    void CleanupLegacyLayout();

    // 메모 폴더 (스티커별 데이터·첨부가 모두 이 안에 있음)
    std::wstring StickerDir(const std::string& id) const;
    std::wstring TrashStickerDir(const std::string& id) const;

    // 내보내기: 메모 폴더를 zip으로 압축해 destFile(.ssticker)로 저장 (.bak 제외)
    bool ExportSticker(const std::string& id, const std::wstring& destFile);
    // 가져오기: .ssticker를 풀어 새 id의 메모로 저장하고 데이터를 반환.
    // 실패 시 반환값의 id가 빈 값이며 err에 실패 단계가 기록된다 (copy/expand/parse/move).
    StickerData ImportSticker(const std::wstring& srcFile, std::string* err = nullptr);

    // 휴지통: 메모 폴더를 통째로 trash\<id>로 옮기고 deletedAt을 기록한다.
    void MoveStickerToTrash(StickerData d);
    std::vector<StickerData> LoadTrash(bool* hadErrors = nullptr);
    void PurgeTrashSticker(const StickerData& d);  // trash json + 첨부 완전 삭제
    bool RestoreTrashEntry(const std::string& id);  // 휴지통 폴더를 stickers\로 되돌림
    int EmptyTrash();                              // 전체 완전 삭제, 삭제된 항목 수 반환
    int CountTrash();

    // 메모·그룹·휴지통·첨부를 모두 완전 삭제 (설정은 보존). 삭제한 항목 수 반환.
    int DeleteAllData();
    int CountAllData();  // 메모(그룹 소속 포함) + 그룹 + 휴지통 항목 수

    std::vector<GroupData> LoadAllGroups();
    void SaveGroup(const GroupData& g);
    void DeleteGroup(const std::string& id);

    std::wstring AppDir() const;  // 커스텀 경로가 설정돼 있으면 그 폴더
    std::wstring StickersDir() const;
    std::wstring GroupsDir() const;
    std::wstring AttachmentsDir() const;
    std::wstring TrashDir() const;

    // 데이터 저장 경로 변경: 기본 %APPDATA% 폴더의 datadir.txt가 실제 폴더를 가리킨다
    // (설정 파일이 데이터 폴더 안에 있어 포인터는 항상 기본 위치에 둠)
    bool SetCustomDataDir(const std::wstring& dir);  // 빈 문자열 = 기본 경로로 복귀

    static nlohmann::json ToJson(const StickerData& d);
    static StickerData FromJson(const nlohmann::json& j);
    static nlohmann::json GroupToJson(const GroupData& g);
    static GroupData GroupFromJson(const nlohmann::json& j);

private:
    std::wstring customDir_;
};
