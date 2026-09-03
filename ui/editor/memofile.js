// 일반 메모 안의 파일·폴더. 파일 메모의 기능을 리치 본문으로 옮겨 온 것이다.
//
// 저장되는 것은 데이터뿐이고 겉모습은 열 때 다시 그린다(캘린더와 같은 방식):
//   링크   <div class="mfile" contenteditable="false" data-kind="link"
//               data-path="C:\Users\me\a.txt" data-name="a.txt" data-dir="0"></div>
//   복사본 <div class="mfile" contenteditable="false" data-kind="copy"
//               data-rel="File/<guid>.txt" data-name="a.txt"></div>
//
// 안에 그려 넣는 것(아이콘·이름·뱃지·끊김 표시)은 전부 data-chrome이라
// editorCore.getHtml()이 저장 직전에 걷어낸다.
const memoFileTools = (() => {
  let editor = null;
  let onChange = null;
  const T = (k, fallback) => {
    const v = typeof i18n !== 'undefined' ? i18n.t(k) : k;
    return v === k ? fallback : v;
  };
  const notify = () => { if (onChange) onChange(); };

  // 없어진 링크 경로 (memofile.exists로 확인한 결과)
  const missing = new Set();

  const items = () => (editor ? [...editor.querySelectorAll('.mfile')] : []);
  const isLink = (el) => el.dataset.kind !== 'copy';
  const nameOf = (el) => el.dataset.name || el.dataset.path || '';
  const isDir = (el) => el.dataset.dir === '1';
  const broken = (el) => isLink(el) && missing.has(el.dataset.path || '');

  function tag(name, cls, text) {
    const e = document.createElement(name);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  // ---------- 만들기 ----------
  function makeLink(path, name, dir) {
    const el = document.createElement('div');
    el.className = 'mfile';
    el.contentEditable = 'false';
    el.dataset.kind = 'link';
    el.dataset.path = path;
    el.dataset.name = name || path.split(/[\\/]/).filter(Boolean).pop() || path;
    el.dataset.dir = dir ? '1' : '0';
    return el;
  }
  function makeCopy(rel, name) {
    const el = document.createElement('div');
    el.className = 'mfile';
    el.contentEditable = 'false';
    el.dataset.kind = 'copy';
    el.dataset.rel = rel;
    el.dataset.name = name || rel.split('/').pop();
    el.dataset.dir = '0';
    return el;
  }

  // 경로 목록을 본문에 넣는다. 파일은 링크·복사본을 물어보고, 폴더는 링크로만 넣는다.
  async function addPaths(paths) {
    const list = (paths || []).filter(Boolean);
    if (!list.length) return;
    // 폴더인지는 확장자 유무로 어림잡고, 존재 확인은 뒤에서 한 번에 한다
    const looksDir = (p) => !/\.[^\\/.]+$/.test(p);
    const files = list.filter((p) => !looksDir(p));

    let kind = 'link';
    if (files.length) {
      const r = await bridge.call('memofile.askKind', { isDir: false, count: files.length })
        .catch(() => ({ kind: 'cancel' }));
      kind = r.kind;
      if (kind === 'cancel') return;
    }
    for (const p of list) {                    // 사용자가 넣은 순서를 지킨다
      if (looksDir(p)) { insertNode(makeLink(p, null, true)); continue; }
      if (kind === 'copy') {
        try {
          const r = await bridge.call('memofile.copyIn', { path: p });
          insertNode(makeCopy(r.rel, r.name));
          continue;
        } catch (e) {
          console.error(e);  // 복사에 실패하면 링크로라도 넣는다
        }
      }
      insertNode(makeLink(p, null, false));
    }
    await refreshAll();
    notify();
  }

  function insertNode(el) {
    editorCore.insertNodeAtCaret(el);
    const after = document.createElement('div');
    after.innerHTML = '<br>';
    editorCore.insertNodeAtCaret(after);
  }

  // ---------- 그리기 ----------
  function render(el) {
    el.querySelectorAll('[data-chrome]').forEach((n) => n.remove());
    const ui = tag('div', 'mfile-ui');
    ui.dataset.chrome = '1';
    const bad = broken(el);
    el.classList.toggle('broken', bad);
    el.classList.toggle('is-dir', isDir(el));

    ui.appendChild(tag('span', 'mfile-icon', bad ? '⚠' : isDir(el) ? '📁' : '📄'));
    ui.appendChild(tag('span', 'mfile-name', nameOf(el)));
    const badge = tag('span', 'mfile-badge ' + (isLink(el) ? 'link' : 'copy'),
                      isLink(el) ? T('mf.badgeLink', '링크') : T('mf.badgeCopy', '복사본'));
    ui.appendChild(badge);
    if (bad) ui.appendChild(tag('span', 'mfile-broken', T('mf.broken', '링크가 끊어졌습니다')));
    el.title = (isLink(el) ? el.dataset.path : nameOf(el)) +
               (bad ? '  —  ' + T('mf.broken', '링크가 끊어졌습니다') : '');
    el.appendChild(ui);
  }

  // 링크가 살아 있는지 네이티브에 물어본 뒤 다시 그린다
  async function refreshAll() {
    if (!editor) return;
    const paths = items().filter(isLink).map((el) => el.dataset.path).filter(Boolean);
    if (paths.length) {
      try {
        const r = await bridge.call('memofile.exists', { paths });
        missing.clear();
        Object.keys(r || {}).forEach((p) => { if (!r[p]) missing.add(p); });
      } catch (e) {
        missing.clear();  // 확인하지 못했으면 끊겼다고 단정하지 않는다
      }
    } else {
      missing.clear();
    }
    items().forEach(render);
  }

  // ---------- 선택 (Shift+클릭으로 범위) ----------
  let anchor = null;
  const selected = () => items().filter((el) => el.classList.contains('tsel'));
  function clearSelection() {
    items().forEach((el) => el.classList.remove('tsel'));
  }
  function select(el, extend) {
    const all = items();
    if (!extend || !anchor || !all.includes(anchor)) {
      clearSelection();
      el.classList.add('tsel');
      anchor = el;
      return;
    }
    const a = all.indexOf(anchor), b = all.indexOf(el);
    clearSelection();
    for (let i = Math.min(a, b); i <= Math.max(a, b); i++) all[i].classList.add('tsel');
  }

  // ---------- 열기·지우기 ----------
  function open(el) {
    if (broken(el)) {
      bridge.call('memofile.notFound', { path: el.dataset.path }).catch(console.error);
      return;
    }
    if (isLink(el)) bridge.call('files.open', { path: el.dataset.path }).catch(console.error);
    else bridge.call('memofile.openCopy', { rel: el.dataset.rel }).catch(console.error);
  }
  function reveal(el) {
    if (!isLink(el)) return;
    if (broken(el)) {
      bridge.call('memofile.notFound', { path: el.dataset.path }).catch(console.error);
      return;
    }
    bridge.call('memofile.reveal', { path: el.dataset.path }).catch(console.error);
  }
  function copyToClipboard(list) {
    const paths = list.filter(isLink).map((el) => el.dataset.path);
    if (paths.length) bridge.call('files.copyClipboard', { paths }).catch(console.error);
  }

  function remove(list) {
    const hadLink = list.some(isLink);
    list.forEach((el) => el.remove());
    notify();
    // 링크는 메모에서만 빠진다 — 원본은 그대로다. 그 사실을 한 번 알려 준다.
    if (hadLink) bridge.call('memofile.linkDeleteNotice').catch(() => {});
  }

  // ---------- 입력 ----------
  function onPointerDown(e) {
    const el = e.target.closest && e.target.closest('.mfile');
    if (!el || !editor.contains(el)) { clearSelection(); return; }
    if (e.button === 2) {                       // 오른쪽 버튼은 선택을 건드리지 않는다
      if (!el.classList.contains('tsel')) select(el, false);
      return;
    }
    select(el, e.shiftKey);
  }
  function onDblClick(e) {
    const el = e.target.closest && e.target.closest('.mfile');
    if (el && editor.contains(el)) { e.preventDefault(); open(el); }
  }

  function init(el, changeCb) {
    editor = el;
    onChange = changeCb;
    editor.addEventListener('pointerdown', onPointerDown, true);
    editor.addEventListener('dblclick', onDblClick);
    refreshAll();
  }

  // ---------- 마크다운 (AI 리뷰용) ----------
  function toMarkdown(el) {
    const kind = isLink(el) ? T('mf.badgeLink', '링크') : T('mf.badgeCopy', '복사본');
    return `- ${isDir(el) ? '📁' : '📄'} ${nameOf(el)} (${kind})`;
  }

  return { init, addPaths, refreshAll, render, items, selected, clearSelection,
           open, reveal, copyToClipboard, remove, isLink, isDir, broken, toMarkdown,
           makeLink, makeCopy };
})();
