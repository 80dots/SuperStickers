// 마크다운 모드: 소스 편집 헬퍼 + 프리뷰 렌더링 (marked, GFM)
const mdTools = (() => {
  let ta = null;      // textarea (#mdSource)
  let onChange = null;

  function init(textarea, changeCb) {
    ta = textarea;
    onChange = changeCb;
    ta.addEventListener('input', () => onChange());
    ta.addEventListener('keydown', (e) => {
      // Tab = 들여쓰기(2칸), Shift+Tab = 내어쓰기. 선택 영역이 있으면 여러 줄 일괄 적용.
      if (e.key === 'Tab') {
        e.preventDefault();
        const s = ta.selectionStart, en = ta.selectionEnd;
        if (e.shiftKey) outdentLines();
        else if (s !== en && ta.value.slice(s, en).includes('\n')) indentLines();
        else insertText('  ');  // 줄 중간에서의 Tab은 캐럿 자리에 넣는다
      }
    });
  }

  // ---------- 소스 편집 헬퍼 ----------
  function insertText(text) {
    ta.focus();
    ta.setRangeText(text, ta.selectionStart, ta.selectionEnd, 'end');
    onChange();
  }

  function wrapSelection(before, after) {
    ta.focus();
    const { selectionStart: s, selectionEnd: e, value } = ta;
    const sel = value.slice(s, e);
    ta.setRangeText(before + sel + after, s, e, sel ? 'select' : 'end');
    if (!sel) ta.setSelectionRange(s + before.length, s + before.length);
    onChange();
  }

  // 선택(또는 캐럿) 줄들의 범위 [시작, 끝)
  function lineRange() {
    const { selectionStart: s, selectionEnd: e, value } = ta;
    const from = value.lastIndexOf('\n', s - 1) + 1;
    let to = value.indexOf('\n', e);
    if (to === -1) to = value.length;
    return [from, to];
  }

  const LIST_LINE = /^\s*(?:[-*+]|\d+[.)])\s/;

  // 들여쓰기: 줄 앞에 2칸을 더한다. 목록 줄은 그대로 중첩 목록이 된다.
  // 목록이 아닌 줄은 앞 공백이 4칸이 되는 순간 코드 블록이 되므로 2칸까지만 허용한다.
  function indentLines() {
    ta.focus();
    const [from, to] = lineRange();
    const block = ta.value.slice(from, to);
    const replaced = block.split('\n').map((l) => {
      if (!LIST_LINE.test(l) && /^ {2,}/.test(l)) return l;
      return '  ' + l;
    }).join('\n');
    if (replaced === block) return;
    ta.setRangeText(replaced, from, to, 'select');
    onChange();
  }

  // 내어쓰기: 줄 앞 공백을 최대 2칸 제거한다.
  function outdentLines() {
    ta.focus();
    const [from, to] = lineRange();
    const block = ta.value.slice(from, to);
    const replaced = block.replace(/^ {1,2}/gm, '');
    if (replaced === block) return;
    ta.setRangeText(replaced, from, to, 'select');
    onChange();
  }

  // 형광펜 지우기: 선택 안의 <mark> 태그를 걷어낸다. 선택이 형광펜 안에 통째로
  // 들어 있으면(태그가 선택 밖에 있으면) 바깥의 짝을 찾아 지운다.
  function clearMarks() {
    ta.focus();
    const { selectionStart: s, selectionEnd: e, value } = ta;
    let from = s, to = e;
    if (!/<\/?mark\b/i.test(value.slice(s, e))) {
      // 선택 앞의 마지막 여는 태그와 선택 뒤의 첫 닫는 태그를 찾는다
      // (인덱스가 어긋나지 않도록 원본 문자열에서 정규식으로 훑는다)
      let open = null;
      for (const m of value.slice(0, s).matchAll(/<mark\b[^>]*>/gi)) open = m;
      const close = /<\/mark>/i.exec(value.slice(e));
      const clean = (a, b) => !/<\/?mark\b/i.test(value.slice(a, b));
      if (open && close) {
        const openEnd = open.index + open[0].length;
        const closeStart = e + close.index;
        if (clean(openEnd, s) && clean(e, closeStart)) {
          from = open.index;
          to = closeStart + close[0].length;
        }
      }
    }
    const sel = value.slice(from, to);
    const stripped = sel.replace(/<mark\b[^>]*>/gi, '').replace(/<\/mark>/gi, '');
    if (stripped === sel) return;
    ta.setRangeText(stripped, from, to, 'select');
    onChange();
  }

  function prefixLines(prefix) {
    ta.focus();
    const { value } = ta;
    let s = ta.selectionStart, e = ta.selectionEnd;
    const lineStart = value.lastIndexOf('\n', s - 1) + 1;
    const block = value.slice(lineStart, e);
    const replaced = block.split('\n').map((l) => prefix + l).join('\n');
    ta.setRangeText(replaced, lineStart, e, 'end');
    onChange();
  }

  // ---------- 프리뷰 렌더링 ----------
  function renderHtml(mdText) {
    return marked.parse(mdText || '', { gfm: true, breaks: true, async: false });
  }

  // container에 렌더링하고 YouTube 링크 → 임베드, 체크박스 → 토글 가능하게 변환.
  // onTaskToggle(index, checked): 소스의 index번째 체크박스 토큰을 갱신하라는 콜백.
  function renderInto(container, mdText, onTaskToggle) {
    container.innerHTML = renderHtml(mdText);

    // YouTube 링크 → 임베드
    container.querySelectorAll('a[href]').forEach((a) => {
      const id = mediaTools.youtubeId(a.getAttribute('href'));
      if (!id) return;
      const wrap = document.createElement('div');
      wrap.className = 'yt-embed';
      const iframe = document.createElement('iframe');
      iframe.src = `https://www.youtube-nocookie.com/embed/${id}`;
      iframe.allow = 'encrypted-media; picture-in-picture; fullscreen';
      iframe.setAttribute('allowfullscreen', '');
      wrap.appendChild(iframe);
      // 링크가 문단 안에 단독으로 있으면 문단 자체를 교체
      const p = a.parentElement;
      if (p && p.tagName === 'P' && p.textContent.trim() === a.textContent.trim()) {
        p.replaceWith(wrap);
      } else {
        a.replaceWith(wrap);
      }
    });

    // 체크박스 토글 활성화 (marked는 disabled로 렌더링함)
    container.querySelectorAll('li input[type="checkbox"]').forEach((cb, idx) => {
      cb.disabled = false;
      cb.addEventListener('change', () => {
        if (onTaskToggle) onTaskToggle(idx, cb.checked);
      });
    });

    // 링크는 기본 브라우저에서 (NewWindowRequested 경유)
    container.querySelectorAll('a[href]').forEach((a) => a.setAttribute('target', '_blank'));
  }

  // 소스에서 idx번째 체크박스 토큰("[ ]"/"[x]")을 갱신한 새 소스를 반환
  function toggleTaskInSource(mdText, idx, checked) {
    const re = /^([ \t]*(?:[-*+]|\d+[.)])[ \t]+)\[( |x|X)\]/gm;
    let i = 0;
    return mdText.replace(re, (m, head) => {
      if (i++ === idx) return head + (checked ? '[x]' : '[ ]');
      return m;
    });
  }

  function getAttachments(mdText) {
    const names = [];
    // https://data.sticker/stickers/<id>/<Sub>/<file> → "<Sub>/<file>"
    const re = /https:\/\/data\.sticker\/stickers\/[^/\s"')]+\/([\w.\-]+\/[\w.\-]+)/g;
    let m;
    while ((m = re.exec(mdText || '')) !== null) names.push(decodeURIComponent(m[1]));
    return names;
  }

  // ---------- '<' 인텔리센스 ----------
  // '<' 입력 시 캐럿 위치에 마크다운 태그 목록을 띄우고 설명과 함께 선택 삽입.
  const SUGGESTIONS = {
    ko: [
      { label: '# 제목 1', desc: '가장 큰 제목', insert: '# ' },
      { label: '## 제목 2', desc: '중간 크기 제목', insert: '## ' },
      { label: '### 제목 3', desc: '작은 제목', insert: '### ' },
      { label: '**굵게**', desc: '굵은 텍스트', insert: '****', caret: 2 },
      { label: '*기울임*', desc: '기울어진 텍스트', insert: '**', caret: 1 },
      { label: '~~취소선~~', desc: '가운데 줄 긋기', insert: '~~~~', caret: 2 },
      { label: '<u>밑줄</u>', desc: '밑줄 텍스트 (HTML)', insert: '<u></u>', caret: 3 },
      { label: '`코드`', desc: '인라인 코드', insert: '``', caret: 1 },
      { label: '``` 코드 블록', desc: '여러 줄 코드', insert: '```\n\n```\n', caret: 4 },
      { label: '[링크](url)', desc: '하이퍼링크', insert: '[](url)', caret: 1 },
      { label: '![이미지](url)', desc: '이미지 삽입', insert: '![](url)', caret: 2 },
      { label: '> 인용', desc: '인용 블록', insert: '> ' },
      { label: '- 글머리 목록', desc: '순서 없는 목록', insert: '- ' },
      { label: '1. 번호 목록', desc: '순서 있는 목록', insert: '1. ' },
      { label: '- [ ] 체크리스트', desc: '체크박스 할 일', insert: '- [ ] ' },
      { label: '| 표 |', desc: '2x2 표 삽입', insert: '| 제목 | 제목 |\n| --- | --- |\n|  |  |\n' },
      { label: '--- 구분선', desc: '가로 구분선', insert: '---\n' },
      { label: '<video> 동영상', desc: '동영상 태그 (HTML)', insert: '<video controls src=""></video>', caret: 22 },
    ],
    en: [
      { label: '# Heading 1', desc: 'Largest heading', insert: '# ' },
      { label: '## Heading 2', desc: 'Medium heading', insert: '## ' },
      { label: '### Heading 3', desc: 'Small heading', insert: '### ' },
      { label: '**Bold**', desc: 'Bold text', insert: '****', caret: 2 },
      { label: '*Italic*', desc: 'Italic text', insert: '**', caret: 1 },
      { label: '~~Strike~~', desc: 'Strikethrough', insert: '~~~~', caret: 2 },
      { label: '<u>Underline</u>', desc: 'Underlined text (HTML)', insert: '<u></u>', caret: 3 },
      { label: '`Code`', desc: 'Inline code', insert: '``', caret: 1 },
      { label: '``` Code block', desc: 'Multi-line code', insert: '```\n\n```\n', caret: 4 },
      { label: '[Link](url)', desc: 'Hyperlink', insert: '[](url)', caret: 1 },
      { label: '![Image](url)', desc: 'Insert image', insert: '![](url)', caret: 2 },
      { label: '> Quote', desc: 'Blockquote', insert: '> ' },
      { label: '- Bullet list', desc: 'Unordered list', insert: '- ' },
      { label: '1. Numbered list', desc: 'Ordered list', insert: '1. ' },
      { label: '- [ ] Checklist', desc: 'Task checkbox', insert: '- [ ] ' },
      { label: '| Table |', desc: 'Insert 2x2 table', insert: '| Head | Head |\n| --- | --- |\n|  |  |\n' },
      { label: '--- Divider', desc: 'Horizontal rule', insert: '---\n' },
      { label: '<video> Video', desc: 'Video tag (HTML)', insert: '<video controls src=""></video>', caret: 22 },
    ],
  };

  // 캐럿 이전 텍스트에서 아직 닫히지 않은 HTML 태그 스택 (위치 포함)
  const VOID_TAGS = new Set(['br', 'hr', 'img', 'input', 'source', 'embed', 'meta', 'link',
                             'area', 'col', 'wbr']);
  function openTagStack(text) {
    const stack = [];
    const re = /<(\/)?([a-zA-Z][a-zA-Z0-9-]*)\b[^<>]*?(\/)?>/g;
    let m;
    while ((m = re.exec(text)) !== null) {
      const closing = !!m[1];
      const name = m[2].toLowerCase();
      const selfClosing = !!m[3];
      if (selfClosing || VOID_TAGS.has(name)) continue;
      if (!closing) {
        stack.push({ name, pos: m.index });
      } else {
        for (let i = stack.length - 1; i >= 0; i--) {
          if (stack[i].name === name) {
            stack.splice(i, 1);
            break;
          }
        }
      }
    }
    return stack;
  }

  // '>' 트리거: 닫히지 않은 모든 구문(HTML 태그·마크다운 마커)의 닫힘 제안 목록.
  // 가까운(캐럿에 가장 근접한) 구문이 맨 위.
  function buildCloseSuggestions(text, caret, lang) {
    const ko = lang === 'ko';
    const items = [];
    const before = text.slice(0, caret);

    // 1) HTML 태그 — 닫는 태그 선택 후 커서는 닫는 태그 뒤에 위치 (기본 동작)
    for (const t of openTagStack(before)) {
      items.push({
        pos: t.pos,
        label: `</${t.name}>`,
        desc: ko ? `<${t.name}> 태그 닫기` : `Close <${t.name}> tag`,
        insert: `</${t.name}>`,
      });
    }

    // 현재 줄 (방금 입력한 '>' 트리거는 제외하고 검사)
    const lineStart = before.lastIndexOf('\n') + 1;
    let line = before.slice(lineStart);
    if (line.endsWith('>')) line = line.slice(0, -1);

    // 2) 인라인 마커: **, *, ~~, `
    const dCount = (line.match(/\*\*/g) || []).length;
    if (dCount % 2 === 1) {
      items.push({ pos: lineStart + line.lastIndexOf('**'), label: '**',
                   desc: ko ? '굵게 닫기' : 'Close bold', insert: '**' });
    }
    const noDouble = line.replace(/\*\*/g, '\x01\x01');
    const sCount = (noDouble.match(/\*/g) || []).length;
    if (sCount % 2 === 1) {
      items.push({ pos: lineStart + noDouble.lastIndexOf('*'), label: '*',
                   desc: ko ? '기울임 닫기' : 'Close italic', insert: '*' });
    }
    const tCount = (line.match(/~~/g) || []).length;
    if (tCount % 2 === 1) {
      items.push({ pos: lineStart + line.lastIndexOf('~~'), label: '~~',
                   desc: ko ? '취소선 닫기' : 'Close strikethrough', insert: '~~' });
    }
    const cCount = (line.match(/`/g) || []).length;
    if (cCount % 2 === 1) {
      items.push({ pos: lineStart + line.lastIndexOf('`'), label: '`',
                   desc: ko ? '인라인 코드 닫기' : 'Close inline code', insert: '`' });
    }

    // 3) 미완성 링크 [텍스트 → ](url)
    const lb = line.lastIndexOf('[');
    if (lb >= 0 && line.indexOf(']', lb) < 0) {
      items.push({ pos: lineStart + lb, label: '](url)',
                   desc: ko ? '링크 닫기 (괄호에 URL 입력)' : 'Close link (type URL in parens)',
                   insert: ']()', caret: 2 });
    }

    // 4) 코드 블록 펜스 (열린 ``` 가 홀수 개면 닫기 제안)
    const fences = [...before.matchAll(/(^|\n)```/g)];
    if (fences.length % 2 === 1) {
      items.push({ pos: fences[fences.length - 1].index, label: '```',
                   desc: ko ? '코드 블록 닫기' : 'Close code block', insert: '\n```\n' });
    }

    // 5) 제목(#{1,6}) — 줄 시작·공백 뒤 토큰 모두 감지, 닫는 해시 삽입 (ATX 스타일)
    const needSpace = line.length > 0 && !/\s$/.test(line);
    const hre = /(^|\s)(#{1,6})(?=\s)/g;
    let hm;
    while ((hm = hre.exec(line)) !== null) {
      const hashes = hm[2];
      items.push({ pos: lineStart + hm.index + hm[1].length, label: hashes,
                   desc: ko ? `제목 닫기 (${hashes})` : `Close heading (${hashes})`,
                   insert: (needSpace ? ' ' : '') + hashes });
    }

    // 6) 줄 시작 블록 명령(목록·인용·체크리스트) → 줄바꿈으로 종료
    if (/^(>\s|-\s\[[ xX]\]\s|[-*+]\s|\d+[.)]\s)/.test(line)) {
      const token = line.match(/^\S+/)[0];
      items.push({ pos: lineStart, label: `${token} ⏎`,
                   desc: ko ? `${token} 줄 끝내기 (줄바꿈)` : `End ${token} line (newline)`,
                   insert: '\n' });
    }

    items.sort((a, b) => b.pos - a.pos);  // 가까운 구문 우선
    return items;
  }

  // textarea 캐럿의 픽셀 좌표 (미러 div 기법)
  function caretPixelPos(textarea, pos) {
    const mirror = document.createElement('div');
    const style = getComputedStyle(textarea);
    for (const prop of ['fontFamily', 'fontSize', 'fontWeight', 'lineHeight', 'letterSpacing',
                        'paddingTop', 'paddingRight', 'paddingBottom', 'paddingLeft',
                        'borderTopWidth', 'borderLeftWidth', 'boxSizing']) {
      mirror.style[prop] = style[prop];
    }
    mirror.style.position = 'absolute';
    mirror.style.visibility = 'hidden';
    mirror.style.whiteSpace = 'pre-wrap';
    mirror.style.wordWrap = 'break-word';
    mirror.style.width = textarea.clientWidth + 'px';
    mirror.textContent = textarea.value.slice(0, pos);
    const marker = document.createElement('span');
    marker.textContent = '​';
    mirror.appendChild(marker);
    document.body.appendChild(mirror);
    const taRect = textarea.getBoundingClientRect();
    const result = {
      x: taRect.left + marker.offsetLeft - textarea.scrollLeft,
      y: taRect.top + marker.offsetTop - textarea.scrollTop + marker.offsetHeight,
    };
    mirror.remove();
    return result;
  }

  function attachIntellisense(textarea, box, getLang) {
    let open = false;
    let mode = 'open';   // 'open': '<' 태그 목록 | 'close': '>' 닫힘 제안
    let startPos = -1;   // 교체 시작 위치 (open: '<' 포함 / close: 트리거 소비 여부에 따름)
    let gtPos = -1;      // close 모드: 트리거 '>' 문자의 위치
    let consumeTrigger = false;  // close 모드: 선택 시 '>'를 함께 교체할지
    let active = 0;
    let filtered = [];
    let closeItems = [];      // '>' 트리거의 닫힘 제안
    let openCloseItems = [];  // '<' 트리거 시 목록 상단에 우선 표시할 닫힘 제안

    function close() {
      open = false;
      box.classList.add('hidden');
    }

    function choose(item) {
      const end = textarea.selectionStart;
      textarea.setRangeText(item.insert, startPos, end, 'end');
      if (item.caret !== undefined) {
        const p = startPos + item.caret;
        textarea.setSelectionRange(p, p);
      }
      close();
      textarea.dispatchEvent(new Event('input'));
      textarea.focus();
    }

    function render() {
      const lang = getLang() === 'ko' ? 'ko' : 'en';
      const all =
          mode === 'open' ? [...openCloseItems, ...SUGGESTIONS[lang]] : closeItems;
      const filterStart =
          mode === 'open' ? startPos + 1 : (consumeTrigger ? startPos + 1 : startPos);
      const filter = textarea.value.slice(filterStart, textarea.selectionStart).toLowerCase();
      filtered = all.filter((s) => !filter || s.label.toLowerCase().includes(filter) ||
                                   s.desc.toLowerCase().includes(filter));
      if (!filtered.length) {
        close();
        return;
      }
      if (active >= filtered.length) active = 0;
      box.innerHTML = '';
      filtered.forEach((s, i) => {
        const div = document.createElement('div');
        div.className = 'sug-item' + (i === active ? ' active' : '');
        const l = document.createElement('span');
        l.className = 'sug-label';
        l.textContent = s.label;
        const d = document.createElement('span');
        d.className = 'sug-desc';
        d.textContent = s.desc;
        div.appendChild(l);
        div.appendChild(d);
        div.addEventListener('mousedown', (e) => {
          e.preventDefault();
          choose(s);
        });
        box.appendChild(div);
      });
      const pos = caretPixelPos(textarea, startPos);
      box.classList.remove('hidden');
      const bw = box.offsetWidth, bh = box.offsetHeight;
      let x = Math.min(pos.x, window.innerWidth - bw - 6);
      let y = pos.y + 2;
      if (y + bh > window.innerHeight) y = pos.y - bh - 22;
      box.style.left = Math.max(2, x) + 'px';
      box.style.top = Math.max(2, y) + 'px';
      // 키보드로 이동한 활성 항목이 목록 스크롤을 따라가도록
      const act = box.children[active];
      if (act) act.scrollIntoView({ block: 'nearest' });
    }

    // '>' 입력 시: 아직 닫히지 않은 모든 구문의 닫힘 제안 (가까운 구문 우선)
    function tryOpenCloseSuggest(caret) {
      const lang = getLang() === 'ko' ? 'ko' : 'en';
      closeItems = buildCloseSuggestions(textarea.value, caret, lang);
      if (!closeItems.length) return;
      mode = 'close';
      open = true;
      gtPos = caret - 1;
      // '>'가 방금 여는 태그(<b> 등)를 완성한 경우가 아니면, 선택 시 '>'를 교체한다
      const tagCompleted = /<\/?[a-zA-Z][^<>]*>$/.test(textarea.value.slice(0, caret));
      consumeTrigger = !tagCompleted;
      startPos = consumeTrigger ? caret - 1 : caret;
      active = 0;
      render();
    }

    textarea.addEventListener('input', (e) => {
      const caret = textarea.selectionStart;
      const typedGt = e.inputType === 'insertText' && e.data === '>';
      if (!open) {
        if (e.inputType === 'insertText' && e.data === '<') {
          mode = 'open';
          open = true;
          startPos = caret - 1;
          // 닫을 수 있는 구문이 있으면 목록 상단에 우선 표시 ('<' 제외한 텍스트 기준)
          const lang = getLang() === 'ko' ? 'ko' : 'en';
          openCloseItems = buildCloseSuggestions(textarea.value, caret - 1, lang);
          active = 0;
          render();
        } else if (typedGt) {
          tryOpenCloseSuggest(caret);
        }
        return;
      }
      // 열려 있는 동안: 트리거 문자가 지워졌거나 공백/줄바꿈 입력 시 닫기
      if (mode === 'open') {
        if (caret <= startPos || textarea.value[startPos] !== '<') return close();
        if (typedGt) {
          // '<u>'처럼 태그가 방금 완성됨 → 닫는 태그 제안으로 전환
          close();
          tryOpenCloseSuggest(caret);
          return;
        }
        const typed = textarea.value.slice(startPos + 1, caret);
        if (/[\s\n]/.test(typed.slice(-1))) return close();
      } else {
        if (textarea.value[gtPos] !== '>') return close();
        const filterStart = consumeTrigger ? startPos + 1 : startPos;
        if (caret < filterStart) return close();
        const typed = textarea.value.slice(filterStart, caret);
        if (/[\s\n<>]/.test(typed.slice(-1))) return close();
      }
      render();
    });

    textarea.addEventListener('keydown', (e) => {
      if (!open) return;
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        active = (active + 1) % filtered.length;
        render();
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        active = (active - 1 + filtered.length) % filtered.length;
        render();
      } else if (e.key === 'Enter' || e.key === 'Tab') {
        e.preventDefault();
        e.stopPropagation();
        if (filtered[active]) choose(filtered[active]);
      } else if (e.key === 'Escape') {
        e.preventDefault();
        close();
      }
    });
    textarea.addEventListener('blur', () => setTimeout(close, 120));
    textarea.addEventListener('scroll', close);
  }

  return {
    init, insertText, wrapSelection, prefixLines,
    indentLines, outdentLines, clearMarks,
    renderHtml, renderInto, toggleTaskInSource, getAttachments,
    attachIntellisense,
    get el() { return ta; },
  };
})();
