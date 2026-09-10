// 메모 사이 링크. 본문에 `memo:<id>` 링크를 넣고, 누르면 그 메모창을 띄운다.
//
// - 리치 메모: <a class="memo-link" href="memo:ID">제목</a> — 본문(HTML)에 그대로 저장된다.
// - 마크다운 메모: [제목](memo:ID) — 미리보기에서 <a href="memo:ID">로 그려진다.
// 어느 쪽이든 클릭은 이 모듈의 문서 단위 위임 처리기가 잡아 네이티브 stickers.show를 부른다.
// 그래서 편집기·미리보기·번역 보기·AI 출력 어디에 있어도 같은 방식으로 열린다.
const memoLinkTools = (() => {
  const SCHEME = 'memo:';
  let ctx = null;   // { type, editor, mdSource, selfId, onChange }
  let picker = null;
  let saved = null; // 고르는 동안 잃어버릴 커서 자리 (리치: Range, 마크다운: [start,end])

  // 요소 만들기
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  // 목록에 보일 이름: 제목 → 본문 첫 줄 → (제목 없음)
  function labelOf(m) {
    const t = (m.title || '').trim();
    if (t) return t;
    let text = '';
    if (m.markdown) text = m.markdown;
    else if (m.html) {
      const tmp = document.createElement('div');
      tmp.innerHTML = m.html;
      text = tmp.textContent || '';
    }
    text = text.replace(/\s+/g, ' ').trim();
    return text ? text.slice(0, 40) : i18n.t('link.untitled');
  }

  // 짧은 안내 토스트
  function toast(msg) {
    let t = document.getElementById('memoLinkToast');
    if (!t) {
      t = el('div', 'ml-toast');
      t.id = 'memoLinkToast';
      document.body.appendChild(t);
    }
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(t._timer);
    t._timer = setTimeout(() => t.classList.remove('show'), 2600);
  }

  // 링크된 메모를 띄운다. 지워진 메모면 안내만 한다.
  async function follow(id) {
    try {
      const r = await bridge.call('stickers.show', { id });
      if (r && r.shown === false) toast(i18n.t('link.notFound'));
    } catch (e) {
      console.error(e);
    }
  }

  // 링크의 memo: id
  function idOf(a) {
    const href = a.getAttribute('href') || '';
    return href.startsWith(SCHEME) ? href.slice(SCHEME.length) : '';
  }

  // ---------- 고르기 ----------

  function closePicker() {
    if (picker) picker.classList.add('hidden');
    saved = null;
  }

  // 고르기 창 뼈대 (한 번)
  function buildPicker() {
    picker = el('div', 'ml-back hidden');
    const box = el('div', 'ml-box');
    const head = el('div', 'ml-head');
    head.appendChild(el('div', 'ml-title', i18n.t('link.pick')));
    const close = el('button', 'ml-close');
    close.innerHTML = '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M3.05 2 8 6.95 12.95 2 14 3.05 9.05 8 14 12.95 12.95 14 8 9.05 3.05 14 2 12.95 6.95 8 2 3.05z"/></svg>';
    close.addEventListener('click', closePicker);
    head.appendChild(close);
    const filter = el('input', 'ml-filter');
    filter.type = 'text';
    filter.placeholder = i18n.t('link.filter');
    const list = el('div', 'ml-list');
    box.appendChild(head);
    box.appendChild(filter);
    box.appendChild(list);
    picker.appendChild(box);
    document.body.appendChild(picker);
    picker._filter = filter;
    picker._list = list;
    picker.addEventListener('mousedown', (e) => { if (e.target === picker) closePicker(); });
    document.addEventListener('keydown', (e) => {
      if (e.key !== 'Escape' || !picker || picker.classList.contains('hidden')) return;
      e.preventDefault();
      e.stopPropagation();
      closePicker();
    }, true);
    filter.addEventListener('input', () => renderList(picker._items, filter.value));
  }

  // 검색어로 거른 목록 그리기
  function renderList(items, q) {
    const list = picker._list;
    list.innerHTML = '';
    const needle = (q || '').trim().toLowerCase();
    const hit = items.filter((it) => !needle || it.label.toLowerCase().includes(needle));
    if (!hit.length) {
      list.appendChild(el('div', 'ml-empty', i18n.t('link.none')));
      return;
    }
    hit.forEach((it) => {
      const row = el('button', 'ml-item');
      row.appendChild(el('span', 'ml-badge', it.badge));
      row.appendChild(el('span', 'ml-label', it.label));
      row.title = it.label;
      row.addEventListener('mousedown', (e) => e.preventDefault());  // 커서 자리를 지킨다
      row.addEventListener('click', () => { insert(it); closePicker(); });
      list.appendChild(row);
    });
  }

  // 우클릭 메뉴에서 부른다. 지금 커서 자리를 기억해 두고 목록을 띄운다.
  async function pick() {
    if (!ctx) return;
    if (ctx.type === 'markdown') {
      saved = [ctx.mdSource.selectionStart, ctx.mdSource.selectionEnd];
    } else {
      const sel = window.getSelection();
      saved = sel.rangeCount && ctx.editor.contains(sel.getRangeAt(0).startContainer)
        ? sel.getRangeAt(0).cloneRange() : null;
    }
    let all = [];
    try {
      all = await bridge.call('stickers.list', {});
    } catch (e) {
      console.error(e);
      return;
    }
    const BADGE = { rich: '', markdown: 'MD', file: 'FILE', web: 'WEB', pdf: 'PDF' };
    const items = all
      .filter((m) => m.id !== ctx.selfId)
      .map((m) => ({ id: m.id, label: labelOf(m), badge: BADGE[m.type || 'rich'] || '',
                     at: m.updatedAt || '' }))
      .sort((a, b) => (a.at < b.at ? 1 : a.at > b.at ? -1 : 0));  // 최근 것부터
    if (!picker) buildPicker();
    picker._items = items;
    picker._filter.value = '';
    renderList(items, '');
    picker.classList.remove('hidden');
    picker._filter.focus();
  }

  // 고른 메모를 커서 자리에 링크로 넣는다
  function insert(it) {
    if (!ctx) return;
    if (ctx.type === 'markdown') {
      const md = ctx.mdSource;
      const [s, e] = saved || [md.value.length, md.value.length];
      const text = `[${it.label.replace(/[\[\]]/g, ' ')}](${SCHEME}${it.id})`;
      md.setRangeText(text, s, e, 'end');
      md.focus();
      md.dispatchEvent(new Event('input', { bubbles: true }));  // 자동 저장·미리보기 갱신
      return;
    }
    const a = el('a', 'memo-link', it.label);
    a.href = SCHEME + it.id;
    a.title = it.label;
    const sel = window.getSelection();
    if (saved) {
      sel.removeAllRanges();
      sel.addRange(saved);
    }
    editorCore.insertNodeAtCaret(a);
    // 링크 바로 뒤에 공백을 두어 이어서 쓰는 글이 링크에 붙지 않게 한다
    const space = document.createTextNode(' ');
    a.after(space);
    const r = document.createRange();
    r.setStartAfter(space);
    r.collapse(true);
    sel.removeAllRanges();
    sel.addRange(r);
    if (ctx.onChange) ctx.onChange();
  }

  // ---------- 초기화 ----------

  function init(options) {
    ctx = options;
    // 문서 어디의 memo: 링크든 누르면 그 메모를 띄운다 (편집기 안에서는 기본 탐색이 없지만
    // 미리보기·AI 출력은 target=_blank로 열려 버리므로 먼저 가로챈다)
    document.addEventListener('click', (e) => {
      // 편집기 안의 웹 링크: contenteditable에서는 눌러도 가지 않으므로 여기서 브라우저를 연다
      // (미리보기·AI 출력의 target=_blank 링크는 네이티브 NewWindowRequested가 이미 연다)
      const web = e.target.closest && e.target.closest('#editor a[href^="http://"], #editor a[href^="https://"]');
      if (web) {
        e.preventDefault();
        e.stopPropagation();
        bridge.call('app.openExternal', { url: web.getAttribute('href') }).catch(console.error);
        return;
      }
      const a = e.target.closest && e.target.closest('a[href^="memo:"]');
      if (!a) return;
      e.preventDefault();
      e.stopPropagation();
      const id = idOf(a);
      if (id) follow(id);
    }, true);
  }

  return { init, pick, follow, labelOf };
})();
