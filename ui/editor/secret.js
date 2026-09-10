// 비밀글 — 본문 안의 `<div class="secret">` 블록. 평소에는 잠겨(locked) 글자가 번져 보이지 않고
// 누르면 풀린다. 설정에서 비밀번호를 켜 두었으면 풀 때 비밀번호를 묻고, 틀리면 열리지 않는다.
//
// 저장되는 것은 `<div class="secret">내용</div>`뿐이다. 잠김 표시(locked, data-hint,
// contenteditable)는 화면용이라 editorCore.getHtml이 저장 직전에 걷어내고, 열 때는 무조건
// 잠근 채로 시작한다. AI·읽어주기가 쓰는 getPlainText는 비밀글을 아예 빼놓는다.
const secretTools = (() => {
  let editor = null;
  let onChange = null;
  let modal = null;
  let pendingEl = null;   // 비밀번호를 묻는 중인 블록

  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };
  const t = (k) => i18n.t(k);

  const isSecret = (node) => !!(node && node.closest && node.closest('.secret'));
  const blocks = () => [...editor.querySelectorAll('.secret')];

  // 잠그기: 번짐 표시 + 편집 불가 + 안내 문구
  function lock(s) {
    s.classList.add('locked');
    s.setAttribute('data-hint', t('secret.hint'));
    s.setAttribute('contenteditable', 'false');
  }
  // 풀기: 다시 편집할 수 있다 (편집 가능 여부는 편집기에서 물려받는다)
  function unlock(s) {
    s.classList.remove('locked');
    s.removeAttribute('data-hint');
    s.removeAttribute('contenteditable');
  }
  function lockAll() { if (editor) blocks().forEach(lock); }

  // 우클릭 메뉴 '비밀글 입력': 고른 글이 있으면 그것을 감싸고, 없으면 빈 블록을 만들어 커서를 둔다
  function insert() {
    const s = el('div', 'secret');
    const sel = window.getSelection();
    const range = sel.rangeCount && editor.contains(sel.getRangeAt(0).startContainer)
      ? sel.getRangeAt(0) : null;
    if (range && !range.collapsed) {
      s.appendChild(range.extractContents());
      range.insertNode(s);
    } else {
      s.appendChild(document.createElement('br'));
      editorCore.insertNodeAtCaret(s);
    }
    // 새로 만든 비밀글은 바로 쓸 수 있게 풀린 채로 두고 커서를 안에 놓는다
    const r = document.createRange();
    r.selectNodeContents(s);
    r.collapse(range && !range.collapsed ? false : true);
    sel.removeAllRanges();
    sel.addRange(r);
    if (onChange) onChange();
  }

  // 선택 메뉴 '비밀글로 설정': 고른 글만 그 자리에서 인라인 비밀글로 감싼다 (줄을 바꾸지 않는다).
  // 표에서 여러 칸을 골랐으면 칸마다 따로 감싼다 — 칸을 가로지르는 범위를 한 덩어리로 뽑으면 표가 깨진다.
  function wrapSelection() {
    const sel = window.getSelection();
    if (!sel.rangeCount) return;
    const range = sel.getRangeAt(0);
    if (range.collapsed || !editor.contains(range.commonAncestorContainer)) return;
    const wrapCell = (cell) => {
      if (!cell.textContent.trim() || cell.querySelector('.secret')) return;
      const s = el('span', 'secret');
      while (cell.firstChild) s.appendChild(cell.firstChild);
      cell.appendChild(s);
    };
    const cells = typeof tableTools !== 'undefined' && editor.querySelectorAll('.tsel').length
      ? [...editor.querySelectorAll('.tsel')] : [];
    if (cells.length > 1) {
      cells.forEach(wrapCell);
    } else {
      const s = el('span', 'secret');
      s.appendChild(range.extractContents());
      range.insertNode(s);
      // 감싼 뒤에는 그 뒤에 커서를 두고 곧바로 잠근다
      const r = document.createRange();
      r.setStartAfter(s);
      r.collapse(true);
      sel.removeAllRanges();
      sel.addRange(r);
      lock(s);
    }
    if (cells.length > 1) lockAll();
    if (onChange) onChange();
  }

  // 비밀글 해제: 블록을 벗기고 내용은 그대로 둔다
  function unwrap(s) {
    const parent = s.parentNode;
    while (s.firstChild) parent.insertBefore(s.firstChild, s);
    s.remove();
    if (onChange) onChange();
  }

  // ---------- 비밀번호 창 ----------

  function buildModal() {
    modal = el('div', 'sc-back hidden');
    const box = el('div', 'sc-box');
    const title = el('div', 'sc-title', t('secret.pwTitle'));
    const body = el('div', 'sc-body');
    box.appendChild(title);
    box.appendChild(body);
    modal.appendChild(box);
    document.body.appendChild(modal);
    modal._body = body;
    modal._title = title;
    modal.addEventListener('mousedown', (e) => { if (e.target === modal) closeModal(); });
    document.addEventListener('keydown', (e) => {
      if (e.key !== 'Escape' || !modal || modal.classList.contains('hidden')) return;
      e.preventDefault();
      e.stopPropagation();
      closeModal();
    }, true);
  }
  function closeModal() {
    if (modal) modal.classList.add('hidden');
    pendingEl = null;
  }
  function field(type, placeholder) {
    const i = el('input', 'sc-input');
    i.type = type;
    i.placeholder = placeholder;
    i.autocomplete = 'off';
    return i;
  }
  function row(...children) {
    const r = el('div', 'sc-row');
    children.forEach((c) => r.appendChild(c));
    return r;
  }
  function btn(label, cls, fn) {
    const b = el('button', 'sc-btn' + (cls ? ' ' + cls : ''), label);
    b.addEventListener('click', fn);
    return b;
  }

  // 비밀번호 입력 화면
  function showPassword(s) {
    if (!modal) buildModal();
    pendingEl = s;
    modal._title.textContent = t('secret.pwTitle');
    const body = modal._body;
    body.innerHTML = '';
    const pw = field('password', t('secret.pwPlaceholder'));
    const msg = el('div', 'sc-msg');
    const submit = async () => {
      const r = await bridge.call('secret.verify', { password: pw.value }).catch(() => ({ ok: false }));
      if (r.ok) {
        unlock(s);
        closeModal();
      } else {
        msg.textContent = t('secret.pwWrong');
        pw.value = '';
        pw.classList.remove('shake');
        void pw.offsetWidth;
        pw.classList.add('shake');
        pw.focus();
      }
    };
    pw.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); submit(); } });
    body.appendChild(row(pw));
    body.appendChild(msg);
    const actions = el('div', 'sc-actions');
    const forgot = btn(t('secret.forgot'), 'link', () => showRecover(s));
    actions.appendChild(forgot);
    actions.appendChild(el('span', 'sc-spacer'));
    actions.appendChild(btn(t('secret.cancel'), '', closeModal));
    actions.appendChild(btn(t('secret.ok'), 'primary', submit));
    body.appendChild(actions);
    modal.classList.remove('hidden');
    pw.focus();
  }

  // 비밀번호 찾기: 설정 때 적어 둔 질문에 답하면 새 비밀번호를 정한다
  async function showRecover(s) {
    const st = await bridge.call('secret.status', {}).catch(() => ({}));
    modal._title.textContent = t('secret.forgot');
    const body = modal._body;
    body.innerHTML = '';
    body.appendChild(el('div', 'sc-question', st.question || t('secret.noQuestion')));
    const ans = field('text', t('secret.answerPlaceholder'));
    const pw1 = field('password', t('secret.newPw'));
    const pw2 = field('password', t('secret.newPw2'));
    const msg = el('div', 'sc-msg');
    const submit = async () => {
      if (!pw1.value) { msg.textContent = t('secret.pwEmpty'); return; }
      if (pw1.value !== pw2.value) { msg.textContent = t('secret.pwMismatch'); return; }
      const r = await bridge.call('secret.resetByAnswer', { answer: ans.value, password: pw1.value })
        .catch(() => ({ ok: false }));
      if (r.ok) {
        unlock(s);
        closeModal();
      } else {
        msg.textContent = t('secret.answerWrong');
        ans.focus();
      }
    };
    [ans, pw1, pw2].forEach((i) => i.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); submit(); }
    }));
    body.appendChild(row(ans));
    body.appendChild(row(pw1));
    body.appendChild(row(pw2));
    body.appendChild(msg);
    const actions = el('div', 'sc-actions');
    actions.appendChild(el('span', 'sc-spacer'));
    actions.appendChild(btn(t('secret.cancel'), '', closeModal));
    actions.appendChild(btn(t('secret.resetOk'), 'primary', submit));
    body.appendChild(actions);
    ans.focus();
  }

  // 잠긴 블록을 눌렀을 때: 비밀번호를 쓰는 중이면 묻고, 아니면 바로 푼다
  async function tryUnlock(s) {
    let st = { usePassword: false, hasPassword: false };
    try { st = await bridge.call('secret.status', {}); } catch { /* 설정을 못 읽으면 바로 푼다 */ }
    if (st.usePassword && st.hasPassword) showPassword(s);
    else unlock(s);
  }

  // ---------- 초기화 ----------

  function init(editorEl, changeCb) {
    editor = editorEl;
    onChange = changeCb;
    lockAll();
    // 잠긴 블록은 누르면 풀린다 (커서가 들어가지 않게 기본 동작을 막는다)
    editor.addEventListener('mousedown', (e) => {
      const s = e.target.closest && e.target.closest('.secret.locked');
      if (!s || !editor.contains(s)) return;
      e.preventDefault();
      e.stopPropagation();
      tryUnlock(s);
    }, true);
    // 창을 떠나거나 숨기면 모두 다시 잠근다
    window.addEventListener('blur', lockAll);
    document.addEventListener('visibilitychange', () => { if (document.hidden) lockAll(); });
    // 되돌리기 등으로 다시 들어온 블록도 잠근 채로
    new MutationObserver(() => {
      blocks().forEach((s) => {
        if (!s.classList.contains('locked') && !editor.contains(document.activeElement) &&
            !s.contains(window.getSelection().anchorNode)) lock(s);
      });
    }).observe(editor, { childList: true, subtree: true });
  }

  return { init, insert, wrapSelection, unwrap, lock, lockAll, isSecret };
})();
