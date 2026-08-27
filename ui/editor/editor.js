// contenteditable 에디터 코어: 서식 명령, 체크리스트, 직렬화
const editorCore = (() => {
  let editor = null;
  let onChange = null;

  function init(el, changeCb) {
    editor = el;
    onChange = changeCb;

    // 체크박스 토글 → DOM 속성에 반영해 innerHTML 직렬화에 포함되게 함
    editor.addEventListener('change', (e) => {
      if (e.target.matches('.check-item input[type="checkbox"]')) {
        e.target.toggleAttribute('checked', e.target.checked);
        notify();
      }
    });

    editor.addEventListener('keydown', onKeydown);
    editor.addEventListener('input', notify);
  }

  function notify() {
    if (onChange) onChange();
  }

  function exec(cmd) {
    editor.focus();
    document.execCommand(cmd, false, null);
    notify();
  }

  // ---------- 체크리스트 ----------
  function makeCheckItem(text) {
    const item = document.createElement('div');
    item.className = 'check-item';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.contentEditable = 'false';
    const span = document.createElement('span');
    span.className = 'check-text';
    if (text) span.textContent = text;
    else span.innerHTML = '<br>';
    item.appendChild(cb);
    item.appendChild(span);
    return item;
  }

  function currentBlock() {
    const sel = window.getSelection();
    if (!sel.rangeCount) return null;
    let node = sel.getRangeAt(0).startContainer;
    while (node && node.parentNode !== editor) node = node.parentNode;
    return node && node.nodeType === 1 ? node : null;
  }

  function placeCaret(el, atStart = true) {
    const range = document.createRange();
    range.selectNodeContents(el);
    range.collapse(atStart);
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
  }

  function insertChecklist() {
    editor.focus();
    const block = currentBlock();
    const item = makeCheckItem('');
    if (block) {
      // 현재 블록이 텍스트만 있으면 체크 항목으로 변환, 아니면 뒤에 삽입
      if (block.classList && block.classList.contains('check-item')) return;
      const text = block.textContent.trim();
      if (block.tagName === 'DIV' && !block.querySelector('img,video,iframe,ul,ol')) {
        item.querySelector('.check-text').textContent = text;
        if (!text) item.querySelector('.check-text').innerHTML = '<br>';
        block.replaceWith(item);
      } else {
        block.after(item);
      }
    } else {
      editor.appendChild(item);
    }
    placeCaret(item.querySelector('.check-text'), false);
    notify();
  }

  function onKeydown(e) {
    if (e.key !== 'Enter' || e.shiftKey) return;
    const sel = window.getSelection();
    if (!sel.rangeCount) return;
    const item = sel.anchorNode
      ? (sel.anchorNode.nodeType === 1 ? sel.anchorNode : sel.anchorNode.parentNode).closest?.(
          '.check-item')
      : null;
    if (!item || !editor.contains(item)) return;

    e.preventDefault();
    const textEl = item.querySelector('.check-text');
    const isEmpty = !textEl || textEl.textContent.trim() === '';
    if (isEmpty) {
      // 빈 체크 항목에서 Enter → 일반 문단으로 전환
      const div = document.createElement('div');
      div.innerHTML = '<br>';
      item.replaceWith(div);
      placeCaret(div);
    } else {
      const next = makeCheckItem('');
      item.after(next);
      placeCaret(next.querySelector('.check-text'));
    }
    notify();
  }

  // ---------- 직렬화 ----------
  function getHtml() {
    return editor ? editor.innerHTML : '';
  }

  // 메모 폴더 기준 상대 경로(예: "Image/xxx.png")만 추출한다
  const ATTACH_RE = /^https:\/\/data\.sticker\/stickers\/[^/]+\/(.+)$/;

  function getAttachments() {
    const names = [];
    if (!editor) return names;
    const add = (src) => {
      const m = (src || '').match(ATTACH_RE);
      if (m) names.push(decodeURIComponent(m[1]));
    };
    editor.querySelectorAll('img, video, source').forEach((el) => {
      add(el.getAttribute('src'));
    });
    // 3D 임베드 첨부와 그룹 카드용 썸네일도 참조 목록에 포함 (GC 보호)
    editor.querySelectorAll('.embed3d').forEach((el) => {
      if (el.dataset.name) names.push(el.dataset.name);
      add(el.dataset.thumb);
    });
    return names;
  }

  function getPlainText() {
    return editor ? editor.innerText.trim() : '';
  }

  // 리치 본문을 마크다운으로 직렬화한다.
  // AI Review 번역은 이 결과를 원문으로 쓰고 결과도 마크다운으로 렌더하므로,
  // 서식(제목·목록·강조·링크)이 번역 후에도 살아남는다. innerText를 쓰면 여기서 이미 서식이 사라진다.
  function getMarkdown() {
    if (!editor) return '';
    const esc = (t) => t.replace(/([*_`~])/g, '\\$1');

    // 인라인 노드 → 마크다운 조각
    function inline(node) {
      if (node.nodeType === Node.TEXT_NODE) return esc(node.nodeValue);
      if (node.nodeType !== Node.ELEMENT_NODE) return '';
      const tag = node.tagName.toLowerCase();
      if (tag === 'br') return '\n';
      if (tag === 'img') return `![](${node.getAttribute('src') || ''})`;
      const inner = [...node.childNodes].map(inline).join('');
      if (!inner.trim()) {
        // 내용 없는 미디어/임베드는 그대로 두면 정보가 사라지므로 원본 태그를 유지
        if (tag === 'video' || node.classList.contains('embed3d')) return node.outerHTML;
        return inner;
      }
      if (tag === 'b' || tag === 'strong') return `**${inner}**`;
      if (tag === 'i' || tag === 'em') return `*${inner}*`;
      if (tag === 's' || tag === 'strike' || tag === 'del') return `~~${inner}~~`;
      if (tag === 'u') return `<u>${inner}</u>`;  // 마크다운에 밑줄이 없어 HTML 유지
      if (tag === 'code') return '`' + inner.replace(/\\([*_`~])/g, '$1') + '`';
      if (tag === 'a') return `[${inner}](${node.getAttribute('href') || ''})`;
      return inner;
    }

    // 블록 노드 → 마크다운 줄
    function block(node, out, depth) {
      if (node.nodeType === Node.TEXT_NODE) {
        const t = esc(node.nodeValue).trim();
        if (t) out.push(t);
        return;
      }
      if (node.nodeType !== Node.ELEMENT_NODE) return;
      const tag = node.tagName.toLowerCase();
      const pad = '  '.repeat(depth);
      if (tag === 'ul' || tag === 'ol') {
        let n = 1;
        [...node.children].forEach((li) => {
          if (li.tagName.toLowerCase() !== 'li') return;
          const nested = [...li.children].filter((c) => /^(ul|ol)$/i.test(c.tagName));
          const text = [...li.childNodes]
            .filter((c) => !(c.nodeType === Node.ELEMENT_NODE && /^(ul|ol)$/i.test(c.tagName)))
            .map(inline).join('').trim();
          out.push(`${pad}${tag === 'ol' ? n++ + '.' : '-'} ${text}`);
          nested.forEach((c) => block(c, out, depth + 1));
        });
        return;
      }
      if (node.classList && node.classList.contains('check-item')) {
        const cb = node.querySelector('input[type="checkbox"]');
        const text = [...(node.querySelector('.check-text')?.childNodes || [])]
          .map(inline).join('').trim();
        out.push(`${pad}- [${cb && cb.checked ? 'x' : ' '}] ${text}`);
        return;
      }
      if (/^h[1-6]$/.test(tag)) {
        out.push('#'.repeat(Number(tag[1])) + ' ' + [...node.childNodes].map(inline).join('').trim());
        return;
      }
      if (tag === 'blockquote') {
        const sub = [];
        [...node.childNodes].forEach((c) => block(c, sub, depth));
        sub.forEach((l) => out.push('> ' + l));
        return;
      }
      if (tag === 'pre') {
        out.push('```', node.textContent.replace(/\n$/, ''), '```');
        return;
      }
      if (tag === 'hr') { out.push('---'); return; }
      // 일반 블록(div/p) — 자식에 블록이 섞여 있으면 재귀
      const hasBlock = [...node.childNodes].some(
        (c) => c.nodeType === Node.ELEMENT_NODE &&
               (/^(div|p|ul|ol|h[1-6]|pre|blockquote|hr)$/i.test(c.tagName) ||
                (c.classList && c.classList.contains('check-item'))));
      if (hasBlock) {
        [...node.childNodes].forEach((c) => block(c, out, depth));
        return;
      }
      const line = [...node.childNodes].map(inline).join('').trim();
      out.push(line);  // 빈 줄도 그대로 (문단 구분)
    }

    const out = [];
    [...editor.childNodes].forEach((c) => block(c, out, 0));
    return out.join('\n').replace(/\n{3,}/g, '\n\n').trim();
  }

  function insertNodeAtCaret(node) {
    editor.focus();
    const sel = window.getSelection();
    if (sel.rangeCount && editor.contains(sel.getRangeAt(0).startContainer)) {
      const range = sel.getRangeAt(0);
      range.collapse(false);
      range.insertNode(node);
      range.setStartAfter(node);
      range.collapse(true);
      sel.removeAllRanges();
      sel.addRange(range);
    } else {
      editor.appendChild(node);
    }
    notify();
  }

  return {
    init, exec, insertChecklist, getHtml, getAttachments, getPlainText, getMarkdown,
    insertNodeAtCaret,
    notify,
    get el() { return editor; },
  };
})();
