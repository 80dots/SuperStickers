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
    util::EnsureDir(util::GetAppDataDir());
    // datadir.txt가 있으면 그 폴더를 데이터 저장 위치로 사용
    auto txt = ReadFileBytes(util::GetAppDataDir() + L"\\datadir.txt");
    if (txt) {
        std::wstring dir = Utf8ToWide(*txt);
        while (!dir.empty() &&
               (dir.back() == L'\r' || dir.back() == L'\n' || dir.back() == L' '))
            dir.pop_back();
        DWORD attr = dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            customDir_ = dir;
    }
    util::EnsureDir(AppDir());
    util::EnsureDir(StickersDir());
    util::EnsureDir(GroupsDir());
    util::EnsureDir(TrashDir());
    CleanupLegacyLayout();
}

bool Store::SetCustomDataDir(const std::wstring& dir) {
    std::wstring pointer = util::GetAppDataDir() + L"\\datadir.txt";
    if (dir.empty()) {
        DeleteFileW(pointer.c_str());
        DeleteFileW((pointer + L".bak").c_str());
        customDir_.clear();
        return true;
    }
    if (!WriteFileAtomic(pointer, WideToUtf8(dir))) return false;
    customDir_ = dir;
    return true;
}

std::wstring Store::AppDir() const {
    return customDir_.empty() ? util::GetAppDataDir() : customDir_;
}
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
        s.autoHideUi = j.value("autoHideUi", s.autoHideUi);
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
        {"autoHideUi", s.autoHideUi},
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
        {"tags", d.tags}, {"aiTags", d.aiTags},
        {"title", d.title}, {"summary", d.summary},
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
    if (j.contains("aiTags") && j["aiTags"].is_array()) {
        for (auto& t : j["aiTags"])
            if (t.is_string()) d.aiTags.push_back(t.get<std::string>());
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

// 메모 폴더 구조: stickers/<id>/memo.json + Image, Video, PDF, 3D 하위 폴더
static const wchar_t* kMemoFile = L"\\memo.json";

std::wstring Store::StickerDir(const std::string& id) const {
    return StickersDir() + L"\\" + Utf8ToWide(id);
}
std::wstring Store::TrashStickerDir(const std::string& id) const {
    return TrashDir() + L"\\" + Utf8ToWide(id);
}

// 폴더 하위의 각 <id> 폴더에서 memo.json을 읽는다.
static std::vector<StickerData> LoadStickersFromDir(const std::wstring& dir, bool* hadErrors) {
    if (hadErrors) *hadErrors = false;
    std::vector<StickerData> out;
    for (auto& [name, isDir] : util::ListDirEntries(dir)) {
        if (!isDir) continue;
        std::wstring path = dir + L"\\" + name + kMemoFile;
        json j = ParseOr(ReadFileBytes(path));
        if (j.is_discarded() || !j.is_object()) j = ParseOr(ReadFileBytes(path + L".bak"));
        if (j.is_object()) {
            StickerData d = Store::FromJson(j);
            // 폴더명을 정본 id로 사용 (가져오기·수동 복사에도 어긋나지 않도록)
            d.id = WideToUtf8(name);
            out.push_back(std::move(d));
        } else if (hadErrors) {
            *hadErrors = true;
        }
    }
    return out;
}

std::vector<StickerData> Store::LoadAllStickers(bool* hadErrors) {
    return LoadStickersFromDir(StickersDir(), hadErrors);
}

void Store::SaveSticker(const StickerData& d) {
    if (d.id.empty()) return;
    std::wstring dir = StickerDir(d.id);
    util::EnsureDir(dir);
    WriteFileAtomic(dir + kMemoFile, ToJson(d).dump());
}

void Store::DeleteSticker(const StickerData& d) {
    if (d.id.empty()) return;
    util::RemoveDirRecursive(StickerDir(d.id));
}

void Store::MoveStickerToTrash(StickerData d) {
    if (d.id.empty()) return;
    d.deletedAt = WideToUtf8(util::NowIso8601());
    SaveSticker(d);  // deletedAt 기록 후 폴더째 이동
    std::wstring dst = TrashStickerDir(d.id);
    util::RemoveDirRecursive(dst);  // 같은 id가 이미 있으면 대체
    if (!util::MoveDirTo(StickerDir(d.id), dst)) util::RemoveDirRecursive(StickerDir(d.id));
}

std::vector<StickerData> Store::LoadTrash(bool* hadErrors) {
    return LoadStickersFromDir(TrashDir(), hadErrors);
}

bool Store::RestoreTrashEntry(const std::string& id) {
    if (id.empty()) return false;
    std::wstring src = TrashStickerDir(id);
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return util::MoveDirTo(src, StickerDir(id));
}

void Store::PurgeTrashSticker(const StickerData& d) {
    if (d.id.empty()) return;
    util::RemoveDirRecursive(TrashStickerDir(d.id));
}

int Store::EmptyTrash() {
    int count = 0;
    for (auto& [name, isDir] : util::ListDirEntries(TrashDir())) {
        std::wstring path = TrashDir() + L"\\" + name;
        if (isDir) {
            util::RemoveDirRecursive(path);
            ++count;
        } else {
            DeleteFileW(path.c_str());  // 잔여 구버전 파일
        }
    }
    return count;
}

// 폴더 안의 파일을 모두 지운다 (하위 폴더는 건드리지 않음). 지운 .json 수 반환.
static int PurgeDirFiles(const std::wstring& dir) {
    int jsonCount = 0;
    for (auto& [name, isDir] : util::ListDirEntries(dir)) {
        if (isDir) continue;
        if (name.size() > 5 && name.substr(name.size() - 5) == L".json") ++jsonCount;
        DeleteFileW((dir + L"\\" + name).c_str());
    }
    return jsonCount;
}

// 폴더 하위의 <id> 폴더를 모두 삭제하고 개수를 반환
static int PurgeDirEntries(const std::wstring& dir) {
    int count = 0;
    for (auto& [name, isDir] : util::ListDirEntries(dir)) {
        std::wstring path = dir + L"\\" + name;
        if (isDir) {
            util::RemoveDirRecursive(path);
            ++count;
        } else {
            DeleteFileW(path.c_str());
        }
    }
    return count;
}

int Store::DeleteAllData() {
    int count = 0;
    count += PurgeDirEntries(StickersDir());
    count += PurgeDirFiles(GroupsDir());
    count += PurgeDirEntries(TrashDir());
    util::RemoveDirRecursive(AttachmentsDir());
    return count;
}

int Store::CountAllData() {
    bool err = false;
    int n = (int)LoadAllStickers(&err).size();
    n += (int)LoadAllGroups().size();
    n += CountTrash();
    return n;
}

int Store::CountTrash() {
    int count = 0;
    for (auto& [name, isDir] : util::ListDirEntries(TrashDir()))
        if (isDir) ++count;
    return count;
}

// 첨부 종류별 하위 폴더. kind가 비어 있으면 확장자로 추론한다.
static std::wstring SubdirFor(std::string kind, std::wstring ext) {
    for (auto& c : kind) c = (char)tolower((unsigned char)c);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (kind == "3d") return L"3D";
    if (kind == "pdf") return L"PDF";
    if (kind == "video") return L"Video";
    if (kind == "image") return L"Image";
    if (ext == L"pdf") return L"PDF";
    if (ext == L"mp4" || ext == L"webm" || ext == L"mov" || ext == L"mkv" || ext == L"avi" ||
        ext == L"m4v" || ext == L"ogv")
        return L"Video";
    return L"Image";
}

std::string Store::SaveAttachment(const std::string& stickerId, const std::string& base64,
                                  const std::string& ext, const std::string& kind) {
    if (stickerId.empty()) return {};
    auto bytes = util::Base64Decode(base64);
    if (bytes.empty()) return {};
    std::wstring wext = Utf8ToWide(ext);
    std::wstring sub = SubdirFor(kind, wext);
    std::wstring dir = StickerDir(stickerId);
    util::EnsureDir(dir);
    util::EnsureDir(dir + L"\\" + sub);
    std::wstring file = util::NewGuid() + L"." + wext;
    std::wstring path = dir + L"\\" + sub + L"\\" + file;
    if (!WriteFileAtomic(path, std::string((char*)bytes.data(), bytes.size()))) return {};
    return WideToUtf8(sub + L"/" + file);  // 메모 폴더 기준 상대 경로 (URL용 슬래시)
}

std::string Store::ImportAttachment(const std::string& stickerId, const std::wstring& srcPath,
                                    const std::string& kind) {
    if (stickerId.empty()) return {};
    size_t dot = srcPath.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L"bin" : srcPath.substr(dot + 1);
    std::wstring sub = SubdirFor(kind, ext);
    std::wstring dir = StickerDir(stickerId);
    util::EnsureDir(dir);
    util::EnsureDir(dir + L"\\" + sub);
    std::wstring file = util::NewGuid() + L"." + ext;
    if (!CopyFileW(srcPath.c_str(), (dir + L"\\" + sub + L"\\" + file).c_str(), TRUE)) return {};
    return WideToUtf8(sub + L"/" + file);
}

void Store::GarbageCollectMemoFiles(const StickerData& d) {
    if (d.id.empty()) return;
    // 참조 판정은 attachments 목록 + 본문에 등장하는 파일명 (목록 누락 시에도 지워지지 않게)
    std::set<std::wstring> refs;
    auto addRef = [&refs](const std::string& rel) {
        if (rel.empty()) return;
        std::wstring w = Utf8ToWide(rel);
        size_t slash = w.find_last_of(L"/\\");
        refs.insert(slash == std::wstring::npos ? w : w.substr(slash + 1));
    };
    for (auto& a : d.attachments) addRef(a);
    addRef(d.pdfName);
    static const wchar_t* kSubs[] = {L"Image", L"Video", L"PDF", L"3D"};
    for (auto* sub : kSubs) {
        std::wstring dir = StickerDir(d.id) + L"\\" + sub;
        for (auto& [name, isDir] : util::ListDirEntries(dir)) {
            if (isDir || refs.count(name)) continue;
            std::string u8 = WideToUtf8(name);
            if (d.html.find(u8) != std::string::npos) continue;
            if (d.markdown.find(u8) != std::string::npos) continue;
            DeleteFileW((dir + L"\\" + name).c_str());
        }
    }
}

void Store::CleanupLegacyLayout() {
    // 구버전 레이아웃: stickers\<id>.json / trash\<id>.json / 공용 attachments 폴더
    for (auto dir : {StickersDir(), TrashDir()}) {
        for (auto& [name, isDir] : util::ListDirEntries(dir)) {
            if (isDir) continue;
            if (name.find(L".json") == std::wstring::npos) continue;
            DeleteFileW((dir + L"\\" + name).c_str());
        }
    }
    util::RemoveDirRecursive(AttachmentsDir());
}

// ---------- .ssticker 내보내기 / 가져오기 ----------

static std::wstring TempWorkDir() {
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"SuperSticker-" + util::NewGuid();
    util::EnsureDir(dir);
    return dir;
}

bool Store::ExportSticker(const std::string& id, const std::wstring& destFile) {
    std::wstring src = StickerDir(id);
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring work = TempWorkDir();
    std::wstring zip = work + L"\\pack.zip";
    // 백업본(.bak)은 빼고 압축하기 위해 임시 폴더로 복사한 뒤 정리한다
    std::wstring stage = work + L"\\memo";
    if (!util::CopyDirRecursive(src, stage)) {
        util::RemoveDirRecursive(work);
        return false;
    }
    for (auto& [name, isDir] : util::ListDirEntries(stage)) {
        if (isDir) continue;
        if (name.size() > 4 && name.substr(name.size() - 4) == L".bak")
            DeleteFileW((stage + L"\\" + name).c_str());
    }
    // Compress-Archive는 .zip 확장자를 요구하므로 임시 zip으로 만든 뒤 .ssticker로 옮긴다
    bool ok = util::ZipDir(stage, zip);
    if (ok) {
        DeleteFileW(destFile.c_str());
        ok = MoveFileExW(zip.c_str(), destFile.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
    }
    util::RemoveDirRecursive(work);
    return ok;
}

StickerData Store::ImportSticker(const std::wstring& srcFile, std::string* err) {
    auto fail = [err](const char* stage) { if (err) *err = stage; };
    StickerData d;
    d.id.clear();
    std::wstring work = TempWorkDir();
    std::wstring zip = work + L"\\pack.zip", out = work + L"\\out";
    // Expand-Archive가 .ssticker 확장자를 거부하므로 .zip으로 복사해서 해제한다
    if (!CopyFileW(srcFile.c_str(), zip.c_str(), FALSE)) {
        fail("copy");
        util::RemoveDirRecursive(work);
        return d;
    }
    json j = nullptr;
    if (!util::UnzipDir(zip, out)) {
        fail("expand");
    } else {
        j = ParseOr(ReadFileBytes(out + kMemoFile));
        // 폴더가 한 겹 더 감싸인 zip도 허용
        if (j.is_discarded() || !j.is_object()) {
            for (auto& [name, isDir] : util::ListDirEntries(out)) {
                if (!isDir) continue;
                json inner = ParseOr(ReadFileBytes(out + L"\\" + name + kMemoFile));
                if (inner.is_object()) {
                    j = inner;
                    out = out + L"\\" + name;
                    break;
                }
            }
        }
    }
    if (!j.is_object()) {
        if (err && err->empty()) fail("parse");
        util::RemoveDirRecursive(work);
        return d;
    }
    d = FromJson(j);
    std::string oldId = d.id;
    d.id = WideToUtf8(util::NewGuid());
    d.groupId.clear();
    d.hidden = false;
    d.deletedAt.clear();
    d.updatedAt = WideToUtf8(util::NowIso8601());
    if (d.createdAt.empty()) d.createdAt = d.updatedAt;
    // 본문에 박혀 있는 data.sticker URL의 메모 id 교체
    if (!oldId.empty()) {
        std::string from = "stickers/" + oldId + "/", to = "stickers/" + d.id + "/";
        for (std::string* t : {&d.html, &d.markdown}) {
            size_t pos = 0;
            while ((pos = t->find(from, pos)) != std::string::npos) {
                t->replace(pos, from.size(), to);
                pos += to.size();
            }
        }
    }
    std::wstring dst = StickerDir(d.id);
    util::RemoveDirRecursive(dst);
    bool ok = util::MoveDirTo(out, dst);
    util::RemoveDirRecursive(work);
    if (!ok) {
        fail("move");
        d.id.clear();
        return d;
    }
    SaveSticker(d);
    return d;
}

json Store::GroupToJson(const GroupData& g) {
    return json{
        {"version", 1}, {"id", g.id},     {"title", g.title},   {"layout", g.layout},
        {"gridSize", g.gridSize}, {"memberHeights", g.memberHeights},
        {"color", g.color}, {"opacity", g.opacity},
        {"x", g.x},     {"y", g.y},       {"w", g.w},           {"h", g.h},
        {"topmost", g.topmost},
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
    g.topmost = j.value("topmost", false);
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
