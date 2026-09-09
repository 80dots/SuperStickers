// 이모지 넣기 — 우클릭 메뉴의 '이모지 추가…'로 뜨는 고르기 창. 분류 탭·검색·최근 사용을 갖추고,
// 고르면 커서 자리에 글자로 넣는다(리치는 텍스트 노드, 마크다운은 textarea). 목록은
// ui/common/emoji-data.js가 든다.
const emojiTools = (() => {
  const RECENT_KEY = 'ss.emoji.recent';
  const RECENT_MAX = 24;
  let ctx = null;     // { type, editor, mdSource, onChange }
  let picker = null;
  let saved = null;   // 고르는 동안의 커서 자리
  let group = 'recent';

  // 요소 만들기
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  // 최근 사용 목록
  function recent() {
    try { return JSON.parse(localStorage.getItem(RECENT_KEY) || '[]'); } catch { return []; }
  }
  // 최근 사용에 기록
  function remember(ch) {
    try {
      const list = [ch].concat(recent().filter((x) => x !== ch)).slice(0, RECENT_MAX);
      localStorage.setItem(RECENT_KEY, JSON.stringify(list));
    } catch { /* 저장소가 막힌 환경 — 최근 목록만 없다 */ }
  }

  // 창 닫기
  function close() {
    if (picker) picker.classList.add('hidden');
    saved = null;
  }

  // 창 뼈대 만들기 (한 번)
  function build() {
    picker = el('div', 'em-back hidden');
    const box = el('div', 'em-box');
    const head = el('div', 'em-head');
    const search = el('input', 'em-search');
    search.type = 'text';
    search.placeholder = i18n.t('emoji.search');
    const closeBtn = el('button', 'em-close', '✕');
    closeBtn.addEventListener('click', close);
    head.appendChild(search);
    head.appendChild(closeBtn);
    const tabs = el('div', 'em-tabs');
    const grid = el('div', 'em-grid');
    const foot = el('div', 'em-foot', '');
    box.appendChild(head);
    box.appendChild(tabs);
    box.appendChild(grid);
    box.appendChild(foot);
    picker.appendChild(box);
    document.body.appendChild(picker);
    Object.assign(picker, { _search: search, _tabs: tabs, _grid: grid, _foot: foot });

    picker.addEventListener('mousedown', (e) => { if (e.target === picker) close(); });
    document.addEventListener('keydown', (e) => {
      if (e.key !== 'Escape' || !picker || picker.classList.contains('hidden')) return;
      e.preventDefault();
      e.stopPropagation();
      close();
    }, true);
    search.addEventListener('input', () => renderGrid());

    // 탭: 최근 + 분류들. 탭 아이콘은 그 분류의 대표 이모지.
    const mkTab = (id, icon, title) => {
      const b = el('button', 'em-tab', icon);
      b.dataset.group = id;
      b.title = title;
      b.addEventListener('mousedown', (e) => e.preventDefault());
      b.addEventListener('click', () => { group = id; search.value = ''; renderGrid(); });
      tabs.appendChild(b);
    };
    mkTab('recent', '🕘', i18n.t('emoji.recent'));
    emojiData.groups().forEach((g) => mkTab(g.id, g.icon, i18n.t('emoji.group.' + g.id)));
  }

  // 현재 탭·검색어의 이모지 격자 그리기
  function renderGrid() {
    const q = picker._search.value.trim();
    const grid = picker._grid;
    grid.innerHTML = '';
    let items;
    if (q) {
      items = emojiData.search(q);
    } else if (group === 'recent') {
      items = recent().map((ch) => [ch, '']);
    } else {
      const g = emojiData.groups().find((x) => x.id === group);
      items = g ? g.items : [];
    }
    picker._tabs.querySelectorAll('.em-tab').forEach((b) =>
      b.classList.toggle('on', !q && b.dataset.group === group));
    if (!items.length) {
      grid.appendChild(el('div', 'em-empty', i18n.t(q ? 'emoji.none' : 'emoji.noRecent')));
      return;
    }
    const frag = document.createDocumentFragment();
    items.forEach(([ch, name]) => {
      const b = el('button', 'em-item', ch);
      if (name) b.title = name.split('|')[0];
      b.addEventListener('mousedown', (e) => e.preventDefault());  // 커서 자리를 지킨다
      b.addEventListener('click', () => { insert(ch); });
      b.addEventListener('mouseenter', () => { picker._foot.textContent = name ? name.replace('|', ' · ') : ch; });
      frag.appendChild(b);
    });
    grid.appendChild(frag);
  }

  // 우클릭 메뉴에서 부른다: 커서 자리를 기억하고 창을 띄운다
  function pick() {
    if (!ctx) return;
    if (ctx.type === 'markdown') {
      saved = [ctx.mdSource.selectionStart, ctx.mdSource.selectionEnd];
    } else {
      const sel = window.getSelection();
      saved = sel.rangeCount && ctx.editor.contains(sel.getRangeAt(0).startContainer)
        ? sel.getRangeAt(0).cloneRange() : null;
    }
    if (!picker) build();
    group = recent().length ? 'recent' : 'smileys';
    picker._search.value = '';
    renderGrid();
    picker.classList.remove('hidden');
    picker._search.focus();
  }

  // 고른 이모지를 커서 자리에 넣는다. 창은 열어 두어 여러 개를 이어서 넣을 수 있다.
  function insert(ch) {
    if (!ctx) return;
    remember(ch);
    if (ctx.type === 'markdown') {
      const md = ctx.mdSource;
      const [s, e] = saved || [md.value.length, md.value.length];
      md.setRangeText(ch, s, e, 'end');
      saved = [md.selectionStart, md.selectionEnd];
      md.dispatchEvent(new Event('input', { bubbles: true }));
      return;
    }
    const sel = window.getSelection();
    if (saved) {
      sel.removeAllRanges();
      sel.addRange(saved);
    }
    const node = document.createTextNode(ch);
    editorCore.insertNodeAtCaret(node);
    // 다음 이모지가 이 뒤에 이어지도록 자리를 갱신한다
    const r = document.createRange();
    r.setStartAfter(node);
    r.collapse(true);
    saved = r.cloneRange();
    if (ctx.onChange) ctx.onChange();
  }

  // 편집기 문맥 등록
  function init(options) { ctx = options; }

  return { init, pick };
})();
