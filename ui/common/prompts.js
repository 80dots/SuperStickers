// AI 작업별 프롬프트 (Ollama /api/chat messages 배열 생성)
// uiLang: 결과 안내 언어 ("ko" | "en")
const prompts = (() => {
  const sys = {
    ko: {
      summarize:
        '당신은 메모 요약 도우미입니다. 사용자가 준 메모를 핵심만 남겨 1~3문장으로 요약하세요. ' +
        '다른 설명 없이 요약문만 한국어로 출력하세요. 단, 메모가 다른 언어로 쓰였으면 그 언어로 요약하세요.',
      polish:
        '당신은 글 교정 도우미입니다. 사용자가 준 텍스트의 맞춤법과 문법을 교정하고 자연스럽게 다듬되 ' +
        '의미와 언어는 그대로 유지하세요. 다른 설명 없이 교정된 텍스트만 출력하세요.',
      koToEn:
        '당신은 번역가입니다. 사용자가 준 텍스트를 자연스러운 영어로 번역하세요. ' +
        '다른 설명 없이 번역문만 출력하세요.',
      enToKo:
        '당신은 번역가입니다. 사용자가 준 텍스트를 자연스러운 한국어로 번역하세요. ' +
        '다른 설명 없이 번역문만 출력하세요.',
      ask:
        '당신은 스티커 메모 앱의 AI 도우미입니다. 아래 메모 내용을 참고해 사용자의 요청에 간결하게 ' +
        '한국어로 답하세요. 메모가 비어 있으면 요청 자체에만 답하세요.',
    },
    en: {
      summarize:
        'You are a note summarizer. Summarize the given note into 1-3 concise sentences. ' +
        'Output only the summary in English, unless the note is written in another language — then use that language.',
      polish:
        'You are a proofreader. Fix spelling and grammar and improve fluency while preserving meaning and language. ' +
        'Output only the corrected text.',
      koToEn:
        'You are a translator. Translate the given text into natural English. Output only the translation.',
      enToKo:
        'You are a translator. Translate the given text into natural Korean. Output only the translation.',
      ask:
        'You are the AI assistant of a sticky-note app. Answer the user request concisely in English, ' +
        'using the note content below as context. If the note is empty, just answer the request.',
    },
  };

  function build(task, text, uiLang, question) {
    const s = (sys[uiLang] || sys.en)[task];
    if (task === 'ask') {
      return [
        { role: 'system', content: s },
        { role: 'user', content: `--- NOTE ---\n${text}\n--- REQUEST ---\n${question}` },
      ];
    }
    return [
      { role: 'system', content: s },
      { role: 'user', content: text },
    ];
  }

  return { build };
})();
