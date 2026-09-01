// AI 작업별 프롬프트 (Ollama /api/chat messages 배열 생성)
// uiLang: 결과 안내 언어 ("ko" | "en")
const prompts = (() => {
  const sys = {
    ko: {
      summarize:
        '당신은 메모 요약 도우미입니다. 사용자가 준 메모를 핵심만 남겨 1~3문장으로 요약하세요. ' +
        '다른 설명 없이 요약문만 한국어로 출력하세요. 단, 메모가 다른 언어로 쓰였으면 그 언어로 요약하세요.',
      koToEn:
        '당신은 번역가입니다. 사용자가 준 텍스트를 자연스러운 영어로 번역하세요. ' +
        '다른 설명 없이 번역문만 출력하세요.',
      enToKo:
        '당신은 번역가입니다. 사용자가 준 텍스트를 자연스러운 한국어로 번역하세요. ' +
        '다른 설명 없이 번역문만 출력하세요.',
      spellcheck:
        '당신은 엄격한 맞춤법·표기 교정기입니다. 입력 텍스트의 맞춤법, 띄어쓰기, 문장 부호, 명백한 오타만 바로잡으세요.\n' +
        '규칙:\n' +
        '1. 문체·어투·어순·단어 선택을 바꾸지 마세요. 틀린 것만 고칩니다.\n' +
        '2. 줄바꿈, 들여쓰기, 목록 기호, 마크다운 표기는 원문 그대로 두세요.\n' +
        '3. 고유명사, 인용문, 코드, URL, 숫자는 건드리지 마세요.\n' +
        '4. 입력과 같은 언어로 출력하세요.\n' +
        '5. 고칠 것이 없으면 원문을 그대로 출력하세요.\n' +
        '설명·머리말·따옴표 없이 교정된 텍스트만 출력하세요.',
      refine:
        '당신은 문장을 다듬는 편집자입니다. 입력 텍스트를 더 자연스럽고 읽기 쉽게 고쳐 쓰세요.\n' +
        '규칙:\n' +
        '1. 원문의 뜻과 정보를 하나도 빠뜨리거나 더하지 마세요.\n' +
        '2. 어색한 번역투, 중복 표현, 불필요한 수식어를 걷어내고 자연스러운 어순으로 바꾸세요.\n' +
        '3. 원문의 어투(존댓말/반말)와 격식 수준은 유지하세요.\n' +
        '4. 문단 구성, 줄바꿈, 목록, 마크다운 표기는 원문 구조를 지키세요.\n' +
        '5. 코드, URL, 고유명사, 인용문은 그대로 두세요.\n' +
        '6. 입력과 같은 언어로 출력하세요.\n' +
        '설명·머리말·따옴표 없이 다듬은 텍스트만 출력하세요.',
      ask:
        '당신은 스티커 메모 앱의 AI 도우미입니다. 아래 메모 내용을 참고해 사용자의 요청에 간결하게 ' +
        '한국어로 답하세요. 메모가 비어 있으면 요청 자체에만 답하세요.',
    },
    en: {
      summarize:
        'You are a note summarizer. Summarize the given note into 1-3 concise sentences. ' +
        'Output only the summary in English, unless the note is written in another language — then use that language.',
      koToEn:
        'You are a translator. Translate the given text into natural English. Output only the translation.',
      enToKo:
        'You are a translator. Translate the given text into natural Korean. Output only the translation.',
      spellcheck:
        'You are a strict spelling and punctuation corrector. Fix only spelling, spacing, punctuation, and obvious typos.\n' +
        'Rules:\n' +
        '1. Do not change style, tone, word order, or word choice. Fix only what is wrong.\n' +
        '2. Preserve line breaks, indentation, list markers, and markdown syntax exactly.\n' +
        '3. Leave proper nouns, quotations, code, URLs, and numbers untouched.\n' +
        '4. Reply in the same language as the input.\n' +
        '5. If nothing needs fixing, output the original text unchanged.\n' +
        'Output only the corrected text — no explanation, preamble, or quotes.',
      refine:
        'You are an editor who polishes prose. Rewrite the input to read more naturally and clearly.\n' +
        'Rules:\n' +
        '1. Preserve every piece of meaning and information — add nothing, drop nothing.\n' +
        '2. Remove awkward phrasing, redundancy, and filler; prefer natural word order.\n' +
        '3. Keep the original tone and level of formality.\n' +
        '4. Keep paragraph structure, line breaks, lists, and markdown syntax intact.\n' +
        '5. Leave code, URLs, proper nouns, and quotations unchanged.\n' +
        '6. Reply in the same language as the input.\n' +
        'Output only the polished text — no explanation, preamble, or quotes.',
      ask:
        'You are the AI assistant of a sticky-note app. Answer the user request concisely in English, ' +
        'using the note content below as context. If the note is empty, just answer the request.',
    },
  };

  // AI Review 프롬프트는 언어와 무관하게 한 벌만 쓴다 (응답 형식이 JSON으로 고정되어 있다)
  const reviewDefault =
    '당신은 스티커 메모 앱의 리뷰 도우미입니다. 주어진 메모 내용을 분석해 반드시 아래 JSON 형식으로만 응답하세요. 다른 설명이나 코드 펜스는 출력하지 마세요.\n' +
    '입력은 마크다운 문서입니다.\n' +
    '{"srcLang":"본문의 주 언어. ko 또는 en (그 외 언어면 en)","summary":"한국어 요약 1~3문장 (본문이 다른 언어면 번역해서 요약)","summaryEn":"영어 요약 1~3문장","title":"요약을 바탕으로 한 15자 이내의 한국어 제목","titleEn":"영어 제목 (5단어 이내)","tags":["본문에 그대로 나오는 낱말 3~6개 (각 1~3단어)"],"translation":"본문 전체를 반대 언어로 충실히 번역 (srcLang이 ko면 영어로, en이면 한국어로)"}\n' +
    '\n' +
    '[tags 작성 규칙 - 반드시 지킬 것]\n' +
    '1. 본문에 실제로 등장하는 낱말만 고른다. 본문에 없는 낱말은 절대 만들어 내지 않는다.\n' +
    '2. 본문에서 글자 그대로 복사한다. 번역하거나 어미·표기를 바꾸지 않는다(본문이 영어면 영어 낱말 그대로 쓴다).\n' +
    '3. 코드 블록·인라인 코드·URL·파일 경로 안의 토큰, 마크다운 기호는 태그로 쓰지 않는다.\n' +
    '4. 내용을 대표하는 낱말을 고르되, 조건에 맞는 것이 부족하면 개수를 줄인다.\n' +
    '\n' +
    '[translation 작성 규칙 - 반드시 지킬 것]\n' +
    '1. 마크다운 기호를 원문 그대로 남긴다: 제목 #/##/###, 목록 -/*/1., 체크박스 - [ ] 와 - [x], 굵게 **, 기울임 *, 취소선 ~~, 인용 >, 표 |, 수평선 ---\n' +
    '2. 코드 블록(```)과 인라인 코드(`)의 내용은 번역하지 않고 그대로 복사한다. 언어 표시(```js 등)도 유지한다.\n' +
    '3. 링크와 이미지는 [텍스트](주소), ![대체텍스트](주소) 형태를 유지하고 주소는 절대 바꾸지 않는다.\n' +
    '4. HTML 태그(<u>, <br> 등), 파일 경로, 명령어, 변수와 함수 이름은 그대로 둔다.\n' +
    '5. 줄바꿈과 빈 줄, 들여쓰기를 원문과 똑같이 유지한다. 줄 수가 달라지면 안 된다.\n' +
    '6. 사람이 읽는 문장만 번역한다. 기호나 구조는 절대 지우거나 바꾸지 않는다.';

  // 설정에서 편집한 프롬프트. { task: '...' } 형태이며 값이 비었으면 기본값을 쓴다.
  let overrides = {};
  function setOverrides(map) { overrides = (map && typeof map === 'object') ? map : {}; }
  // 설정 UI가 기본값을 안내 문구(placeholder)로 보여줄 때 쓴다
  function defaultOf(task, uiLang) {
    if (task === 'review') return reviewDefault;
    return (sys[uiLang] || sys.en)[task] || '';
  }
  const TASKS = ['review', 'summarize', 'spellcheck', 'refine', 'koToEn', 'enToKo', 'ask'];

  function build(task, text, uiLang, question) {
    const s = (overrides[task] || '').trim() || defaultOf(task, uiLang);
    if (task === 'review') {
      return [
        { role: 'system', content: s },
        { role: 'user', content: text },
      ];
    }
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

  return { build, setOverrides, defaultOf, TASKS };
})();
