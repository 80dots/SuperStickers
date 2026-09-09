#pragma once
#include <windows.h>
#include <sapi.h>
#include <wil/com.h>

#include <string>
#include <vector>

// 읽어주기(TTS) — 윈도우에 설치된 음성으로 글을 읽는다 (SAPI 5, 무료·오프라인).
// WebView2의 Web Speech API는 음성 목록이 비어 있어(실측 getVoices()=0) 네이티브로 한다.
// SAPI 목록에는 예전 'Desktop' 음성만 보이므로 최신 OneCore 음성(윈도우 설정의 음성)도
// 레지스트리 범주를 직접 열어 함께 보여 준다.
class Tts {
public:
    // 음성 하나
    struct Voice {
        std::string id;    // 토큰 id (설정에 저장되는 값)
        std::string name;  // "Microsoft Heami Desktop" 등
        std::string lang;  // "ko-KR" 등 (모르면 빈 값)
    };

    // 설치된 음성. 한국어를 앞에, 그 다음 UI 언어 순으로 정렬한다.
    std::vector<Voice> Voices();
    // 읽기 시작 (이전 읽기는 끊는다). voiceId가 비었거나 못 찾으면 기본 음성.
    // rate는 -10(느림) ~ 10(빠름), 0이 보통.
    bool Speak(const std::wstring& text, const std::string& voiceId, int rate);
    void Stop();
    bool Speaking();
    // COM 객체를 미리 놓는다. App은 정적 싱글턴이라 소멸이 main의 CoUninitialize() 뒤에 오는데,
    // 그때 ISpVoice를 Release하면 이미 내려간 SAPI를 건드려 종료 중에 죽을 수 있다.
    void Shutdown();

private:
    bool EnsureVoice();
    wil::com_ptr<ISpVoice> voice_;
};
