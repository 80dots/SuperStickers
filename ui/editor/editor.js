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

  function getAttachments() {
    const names = [];
    if (!editor) return names;
    editor.querySelectorAll('img, video, source').forEach((el) => {
      const src = el.getAttribute('src') || '';
      const m = src.match(/^https:\/\/data\.sticker\/attachments\/(.+)$/);
      if (m) names.push(decodeURIComponent(m[1]));
    });
    return names;
  }

  function getPlainText() {
    return editor ? editor.innerText.trim() : '';
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
    init, exec, insertChecklist, getHtml, getAttachments, getPlainText, insertNodeAtCaret,
    notify,
    get el() { return editor; },
  };
})();
