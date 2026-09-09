#include "Tts.h"

#include <algorithm>

#include "Utils.h"

// sphelper.h는 ATL(CComPtr)을 끌어오므로 쓰지 않는다 — 필요한 것은 토큰 열거와 id→토큰뿐이라
// SAPI COM 인터페이스로 직접 한다.
namespace {

constexpr const wchar_t* kSapiVoices = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices";
constexpr const wchar_t* kOneCoreVoices =
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices";

// 토큰 하나를 Voice로. 이름은 토큰의 기본 문자열, 언어는 Attributes\Language(LCID 16진수).
bool VoiceFromToken(ISpObjectToken* tok, Tts::Voice* out) {
    wil::unique_cotaskmem_string id, name;
    if (FAILED(tok->GetId(&id)) || !id) return false;
    if (FAILED(tok->GetStringValue(nullptr, &name)) || !name) return false;
    out->id = util::WideToUtf8(id.get());
    out->name = util::WideToUtf8(name.get());
    wil::com_ptr<ISpDataKey> attrs;
    if (SUCCEEDED(tok->OpenKey(L"Attributes", &attrs)) && attrs) {
        wil::unique_cotaskmem_string lang;
        if (SUCCEEDED(attrs->GetStringValue(L"Language", &lang)) && lang) {
            // "412" → LCID 0x412 → "ko-KR" (여러 개면 첫 것)
            std::wstring l = lang.get();
            size_t semi = l.find(L';');
            if (semi != std::wstring::npos) l = l.substr(0, semi);
            LCID lcid = (LCID)wcstoul(l.c_str(), nullptr, 16);
            wchar_t tag[LOCALE_NAME_MAX_LENGTH]{};
            if (lcid && LCIDToLocaleName(lcid, tag, LOCALE_NAME_MAX_LENGTH, 0))
                out->lang = util::WideToUtf8(tag);
        }
    }
    return true;
}

// 범주(레지스트리 경로) 아래의 음성 토큰을 모두 모은다
void CollectVoices(const wchar_t* categoryId, std::vector<Tts::Voice>* out) {
    wil::com_ptr<ISpObjectTokenCategory> cat;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&cat))))
        return;
    if (FAILED(cat->SetId(categoryId, FALSE))) return;
    wil::com_ptr<IEnumSpObjectTokens> en;
    if (FAILED(cat->EnumTokens(nullptr, nullptr, &en)) || !en) return;
    ULONG n = 0;
    en->GetCount(&n);
    for (ULONG i = 0; i < n; i++) {
        wil::com_ptr<ISpObjectToken> tok;
        if (FAILED(en->Next(1, &tok, nullptr)) || !tok) break;
        Tts::Voice v;
        if (!VoiceFromToken(tok.get(), &v)) continue;
        // 같은 음성이 두 범주에 다 있으면 하나만 (이름 기준)
        bool dup = std::any_of(out->begin(), out->end(),
                               [&](const Tts::Voice& o) { return o.name == v.name; });
        if (!dup) out->push_back(std::move(v));
    }
}

// 토큰 id("HKEY_LOCAL_MACHINE\...\TokenName") → 토큰 객체
wil::com_ptr<ISpObjectToken> TokenFromId(const std::wstring& id) {
    wil::com_ptr<ISpObjectToken> tok;
    if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&tok))))
        return nullptr;
    if (FAILED(tok->SetId(nullptr, id.c_str(), FALSE))) return nullptr;
    return tok;
}

}  // namespace

// 설치된 음성 목록 (Tts.h 참고)
std::vector<Tts::Voice> Tts::Voices() {
    std::vector<Voice> out;
    CollectVoices(kSapiVoices, &out);
    CollectVoices(kOneCoreVoices, &out);
    // 한국어 → 그 밖의 순으로, 같은 언어 안에서는 이름순
    std::stable_sort(out.begin(), out.end(), [](const Voice& a, const Voice& b) {
        bool ka = a.lang.rfind("ko", 0) == 0, kb = b.lang.rfind("ko", 0) == 0;
        if (ka != kb) return ka;
        if (a.lang != b.lang) return a.lang < b.lang;
        return a.name < b.name;
    });
    return out;
}

// SpVoice COM 객체를 한 번만 만든다
bool Tts::EnsureVoice() {
    if (voice_) return true;
    return SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice_)));
}

// 읽기 시작 (Tts.h 참고)
bool Tts::Speak(const std::wstring& text, const std::string& voiceId, int rate) {
    if (!EnsureVoice()) return false;
    wil::com_ptr<ISpObjectToken> tok;
    if (!voiceId.empty()) tok = TokenFromId(util::Utf8ToWide(voiceId));
    voice_->SetVoice(tok.get());  // nullptr이면 시스템 기본 음성
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    voice_->SetRate(rate);
    // 비동기 + 직전 읽기 끊기. 텍스트를 XML로 해석하지 않게 한다 (메모의 < > 가 태그로 읽히지 않도록)
    return SUCCEEDED(voice_->Speak(text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML,
                                   nullptr));
}

// 읽기 중단
void Tts::Stop() {
    if (!voice_) return;
    voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
}

// 종료 전에 COM 객체를 놓는다 (Tts.h 참고)
void Tts::Shutdown() {
    Stop();
    voice_.reset();
}

// 지금 읽는 중인지
bool Tts::Speaking() {
    if (!voice_) return false;
    SPVOICESTATUS st{};
    if (FAILED(voice_->GetStatus(&st, nullptr))) return false;
    return st.dwRunningState == SPRS_IS_SPEAKING;
}
