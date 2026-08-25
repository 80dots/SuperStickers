#include "Store.h"

#include <windows.h>

#include <set>

#include "Utils.h"

using json = nlohmann::json;
using util::ReadFileBytes;
using util::Utf8ToWide;
using util::WideToUtf8;
using util::WriteFileAtomic;

void Store::Init() {
    util::EnsureDir(AppDir());
    util::EnsureDir(StickersDir());
    util::EnsureDir(GroupsDir());
    util::EnsureDir(AttachmentsDir());
    util::EnsureDir(TrashDir());
}

std::wstring Store::AppDir() const { return util::GetAppDataDir(); }
std::wstring Store::StickersDir() const { return AppDir() + L"\\stickers"; }
std::wstring Store::GroupsDir() const { return AppDir() + L"\\groups"; }
std::wstring Store::AttachmentsDir() const { return AppDir() + L"\\attachments"; }
std::wstring Store::TrashDir() const { return AppDir() + L"\\trash"; }

static json ParseOr(const std::optional<std::string>& bytes) {
    if (!bytes) return nullptr;
    return json::parse(*bytes, nullptr, false);  // 예외 대신 discarded 반환
}

Settings Store::LoadSettings() {
    Settings s;
    std::wstring path = AppDir() + L"\\settings.json";
    json j = ParseOr(ReadFileBytes(path));
    if (j.is_discarded() || !j.is_object()) {
        j = ParseOr(ReadFileBytes(path + L".bak"));
    }
    if (j.is_object()) {
        s.theme = j.value("theme", s.theme);
        s.language = j.value("language", s.language);
        s.autostart = j.value("autostart", s.autostart);
        if (j.contains("ollama") && j["ollama"].is_object()) {
            s.ollama.endpoint = j["ollama"].value("endpoint", s.ollama.endpoint);
            s.ollama.model = j["ollama"].value("model", s.ollama.model);
        }
        if (j.contains("trash") && j["trash"].is_object()) {
            s.trashEnabled = j["trash"].value("enabled", s.trashEnabled);
            s.trashRetentionDays = j["trash"].value("retentionDays", s.trashRetentionDays);
            if (s.trashRetentionDays < 0) s.trashRetentionDays = 0;
        }
        s.uiScale = j.value("uiScale", s.uiScale);
        if (s.uiScale < 0.3) s.uiScale = 0.3;
        if (s.uiScale > 2.0) s.uiScale = 2.0;
    }
    return s;
}

void Store::SaveSettings(const Settings& s) {
    json j = {
        {"version", 1},
        {"theme", s.theme},
        {"language", s.language},
        {"autostart", s.autostart},
        {"ollama", {{"endpoint", s.ollama.endpoint}, {"model", s.ollama.model}}},
        {"trash", {{"enabled", s.trashEnabled}, {"retentionDays", s.trashRetentionDays}}},
        {"uiScale", s.uiScale},
    };
    WriteFileAtomic(AppDir() + L"\\settings.json", j.dump(2));
}

json Store::ToJson(const StickerData& d) {
    return json{
        {"version", 1},   {"id", d.id},           {"type", d.type},
        {"html", d.html},
        {"markdown", d.markdown}, {"mode", d.mode}, {"groupId", d.groupId},
        {"files", d.files}, {"fileView", d.fileView},
        {"url", d.url}, {"lastUrl", d.lastUrl},
        {"pdfName", d.pdfName}, {"pdfTitle", d.pdfTitle},
        {"color", d.color}, {"x", d.x},           {"y", d.y},
        {"w", d.w},       {"h", d.h},             {"topmost", d.topmost},
        {"hidden", d.hidden}, {"attachments", d.attachments},
        {"createdAt", d.createdAt}, {"updatedAt", d.updatedAt},
        {"deletedAt", d.deletedAt},
        {"tags", d.tags}, {"title", d.title}, {"summary", d.summary},
        {"titleEn", d.titleEn}, {"summaryEn", d.summaryEn},
        {"transKo", d.transKo}, {"transEn", d.transEn},
        {"srcLang", d.srcLang}, {"viewLang", d.viewLang},
        {"needsReview", d.needsReview},
    };
}

StickerData Store::FromJson(const json& j) {
    StickerData d;
    d.id = j.value("id", "");
    d.html = j.value("html", "");
    d.markdown = j.value("markdown", "");
    d.mode = j.value("mode", "rich");
    // 타입 마이그레이션: 구버전 데이터는 mode("rich"|"markdown")가 타입 역할을 했음
    d.type = j.value("type", "");
    if (d.type.empty()) d.type = (d.mode == "markdown") ? "markdown" : "rich";
    if (j.contains("files") && j["files"].is_array()) {
        for (auto& f : j["files"])
            if (f.is_string()) d.files.push_back(f.get<std::string>());
    }
    d.fileView = j.value("fileView", "list");
    d.url = j.value("url", "");
    d.lastUrl = j.value("lastUrl", "");
    d.pdfName = j.value("pdfName", "");
    d.pdfTitle = j.value("pdfTitle", "");
    d.groupId = j.value("groupId", "");
    d.color = j.value("color", d.color);
    d.x = j.value("x", d.x);
    d.y = j.value("y", d.y);
    d.w = j.value("w", d.w);
    d.h = j.value("h", d.h);
    d.topmost = j.value("topmost", false);
    d.hidden = j.value("hidden", false);
    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (auto& a : j["attachments"])
            if (a.is_string()) d.attachments.push_back(a.get<std::string>());
    }
    d.createdAt = j.value("createdAt", "");
    d.updatedAt = j.value("updatedAt", "");
    d.deletedAt = j.value("deletedAt", "");
    if (j.contains("tags") && j["tags"].is_array()) {
        for (auto& t : j["tags"])
            if (t.is_string()) d.tags.push_back(t.get<std::string>());
    }
    d.title = j.value("title", "");
    d.summary = j.value("summary", "");
    d.titleEn = j.value("titleEn", "");
    d.summaryEn = j.value("summaryEn", "");
    d.transKo = j.value("transKo", "");
    d.transEn = j.value("transEn", "");
    d.srcLang = j.value("srcLang", "");
    d.viewLang = j.value("viewLang", "");
    d.needsReview = j.value("needsReview", false);
    return d;
}

static std::vector<StickerData> LoadStickersFromDir(const std::wstring& dir, bool* hadErrors) {
    if (hadErrors) *hadErrors = false;
    std::vector<StickerData> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name.size() > 4 && name.substr(name.size() - 4) == L".bak") continue;
        std::wstring path = dir + L"\\" + name;
        json j = ParseOr(ReadFileBytes(path));
        if (j.is_discarded() || !j.is_object()) {
            j = ParseOr(ReadFileBytes(path + L".bak"));
        }
        if (j.is_object()) {
            StickerData d = Store::FromJson(j);
            if (!d.id.empty())
                out.push_back(std::move(d));
            else if (hadErrors)
                *hadErrors = true;
        } else if (hadErrors) {
            *hadErrors = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<StickerData> Store::LoadAllStickers(bool* hadErrors) {
    return LoadStickersFromDir(StickersDir(), hadErrors);
}

void Store::SaveSticker(const StickerData& d) {
    WriteFileAtomic(StickersDir() + L"\\" + Utf8ToWide(d.id) + L".json", ToJson(d).dump());
}

void Store::DeleteSticker(const StickerData& d) {
    std::wstring base = StickersDir() + L"\\" + Utf8ToWide(d.id) + L".json";
    DeleteFileW(base.c_str());
    DeleteFileW((base + L".bak").c_str());
    for (auto& a : d.attachments) {
        DeleteFileW((AttachmentsDir() + L"\\" + Utf8ToWide(a)).c_str());
    }
}

void Store::MoveStickerToTrash(StickerData d) {
    d.deletedAt = WideToUtf8(util::NowIso8601());
    WriteFileAtomic(TrashDir() + L"\\" + Utf8ToWide(d.id) + L".json", ToJson(d).dump());
    std::wstring base = StickersDir() + L"\\" + Utf8ToWide(d.id) + L".json";
    DeleteFileW(base.c_str());
    DeleteFileW((base + L".bak").c_str());
}

std::vector<StickerData> Store::LoadTrash(bool* hadErrors) {
    return LoadStickersFromDir(TrashDir(), hadErrors);
}

void Store::RemoveTrashEntry(const std::string& id) {
    std::wstring base = TrashDir() + L"\\" + Utf8ToWide(id) + L".json";
    DeleteFileW(base.c_str());
    DeleteFileW((base + L".bak").c_str());
}

void Store::PurgeTrashSticker(const StickerData& d) {
    std::wstring base = TrashDir() + L"\\" + Utf8ToWide(d.id) + L".json";
    DeleteFileW(base.c_str());
    DeleteFileW((base + L".bak").c_str());
    for (auto& a : d.attachments) {
        DeleteFileW((AttachmentsDir() + L"\\" + Utf8ToWide(a)).c_str());
    }
}

int Store::EmptyTrash() {
    int count = 0;
    for (auto& d : LoadTrash()) {
        PurgeTrashSticker(d);
        ++count;
    }
    // 파싱 실패로 LoadTrash에 잡히지 않은 잔여 파일까지 정리
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((TrashDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DeleteFileW((TrashDir() + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return count;
}

int Store::CountTrash() {
    int count = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((TrashDir() + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        if (name.size() > 4 && name.substr(name.size() - 4) == L".bak") continue;
        ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

std::string Store::SaveAttachment(const std::string& base64, const std::string& ext) {
    auto bytes = util::Base64Decode(base64);
    if (bytes.empty()) return {};
    std::string name = WideToUtf8(util::NewGuid()) + "." + ext;
    std::wstring path = AttachmentsDir() + L"\\" + Utf8ToWide(name);
    if (!WriteFileAtomic(path, std::string((char*)bytes.data(), bytes.size()))) return {};
    return name;
}

std::string Store::ImportAttachment(const std::wstring& srcPath) {
    size_t dot = srcPath.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L"bin" : srcPath.substr(dot + 1);
    std::string name = WideToUtf8(util::NewGuid() + L"." + ext);
    std::wstring dst = AttachmentsDir() + L"\\" + Utf8ToWide(name);
    if (!CopyFileW(srcPath.c_str(), dst.c_str(), TRUE)) return {};
    return name;
}

json Store::GroupToJson(const GroupData& g) {
    return json{
        {"version", 1}, {"id", g.id},     {"title", g.title},   {"layout", g.layout},
        {"gridSize", g.gridSize}, {"memberHeights", g.memberHeights},
        {"color", g.color}, {"opacity", g.opacity},
        {"x", g.x},     {"y", g.y},       {"w", g.w},           {"h", g.h},
        {"hidden", g.hidden}, {"memberIds", g.memberIds},
        {"createdAt", g.createdAt}, {"updatedAt", g.updatedAt},
    };
}

GroupData Store::GroupFromJson(const json& j) {
    GroupData g;
    g.id = j.value("id", "");
    g.title = j.value("title", "");
    g.layout = j.value("layout", "grid");
    g.gridSize = j.value("gridSize", "m");
    if (j.contains("memberHeights") && j["memberHeights"].is_object()) {
        for (auto& [k, v] : j["memberHeights"].items())
            if (v.is_number()) g.memberHeights[k] = v.get<int>();
    }
    g.color = j.value("color", "");
    g.opacity = j.value("opacity", 1.0);
    if (g.opacity < 0.0) g.opacity = 0.0;
    if (g.opacity > 1.0) g.opacity = 1.0;
    g.x = j.value("x", g.x);
    g.y = j.value("y", g.y);
    g.w = j.value("w", g.w);
    g.h = j.value("h", g.h);
    g.hidden = j.value("hidden", false);
    if (j.contains("memberIds") && j["memberIds"].is_array()) {
        for (auto& m : j["memberIds"])
            if (m.is_string()) g.memberIds.push_back(m.get<std::string>());
    }
    g.createdAt = j.value("createdAt", "");
    g.updatedAt = j.value("updatedAt", "");
    return g;
}

std::vector<GroupData> Store::LoadAllGroups() {
    std::vector<GroupData> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((GroupsDir() + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name.size() > 4 && name.substr(name.size() - 4) == L".bak") continue;
        std::wstring path = GroupsDir() + L"\\" + name;
        json j = ParseOr(ReadFileBytes(path));
        if (j.is_discarded() || !j.is_object()) j = ParseOr(ReadFileBytes(path + L".bak"));
        if (j.is_object()) {
            GroupData g = GroupFromJson(j);
            if (!g.id.empty()) out.push_back(std::move(g));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void Store::SaveGroup(const GroupData& g) {
    WriteFileAtomic(GroupsDir() + L"\\" + Utf8ToWide(g.id) + L".json", GroupToJson(g).dump());
}

void Store::DeleteGroup(const std::string& id) {
    std::wstring base = GroupsDir() + L"\\" + Utf8ToWide(id) + L".json";
    DeleteFileW(base.c_str());
    DeleteFileW((base + L".bak").c_str());
}

void Store::GarbageCollectAttachments(const std::vector<StickerData>& stickers, bool loadFailed) {
    if (loadFailed) return;  // 일부 스티커 파싱 실패 시 삭제 판단 불가 → 건너뜀
    bool trashErrors = false;
    auto trashed = LoadTrash(&trashErrors);
    if (trashErrors) return;  // 휴지통 파싱 실패 시에도 삭제 판단 불가
    std::set<std::wstring> referenced;
    for (auto& s : stickers)
        for (auto& a : s.attachments) referenced.insert(Utf8ToWide(a));
    for (auto& s : trashed)
        for (auto& a : s.attachments) referenced.insert(Utf8ToWide(a));

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((AttachmentsDir() + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!referenced.count(fd.cFileName)) {
            DeleteFileW((AttachmentsDir() + L"\\" + fd.cFileName).c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
