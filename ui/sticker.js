// 스티커 페이지 메인 — 타입(rich/markdown/file/web/pdf)별 UI 구성
(async () => {
  const init = window.__init || { page: 'sticker', theme: 'light', lang: 'en' };
  // 설정에서 편집한 AI 프롬프트를 반영한다 (비어 있으면 prompts.js의 기본값 사용)
  prompts.setOverrides(init.prompts || {});
  bridge.on('prompts.changed', (d) => prompts.setOverrides(d.prompts || {}));
  const $ = (sel) => document.querySelector(sel);
  const editor = $('#editor');
  const mdSource = $('#mdSource');
  const mdPreview = $('#mdPreview');

  // 메모창은 테마 불변 — 항상 라이트 기준으로 렌더링
  const isDark = () => false;

  await i18n.load(init.lang);
  i18n.apply();

  // ---------- 데이터 로드 ----------
  let data = { type: 'rich', color: 'yellow', topmost: false, html: '', markdown: '' };
  try {
    data = await bridge.call('sticker.load');
  } catch (e) {
    console.error(e);
  }
  const type = data.type || 'rich';
  const isText = type === 'rich' || type === 'markdown';
  colorUtil.apply(data.color, isDark());
  $('#pinBtn').classList.toggle('on', !!data.topmost);
  $('#typeBadge').textContent =
    { markdown: 'MD', file: 'FILE', web: 'WEB', pdf: 'PDF' }[type] || '';

  // ---------- 자동 저장 (rich/markdown) ----------
  let saveTimer = null;
  let dirty = false;
  function scheduleSave() {
    dirty = true;
    data.needsReview = true;  // 입력 즉시 AI Review 버튼 활성화
    setReviewState();
    clearTimeout(saveTimer);
    saveTimer = setTimeout(saveNow, 800);
  }
  function saveNow() {
    if (!dirty) return;
    dirty = false;
    clearTimeout(saveTimer);
    // 타입별로 해당 필드만 갱신하고 나머지는 기존 값 유지
    // (markdown 타입에서는 editorCore가 초기화되지 않으므로 접근하면 안 됨)
    const html = type === 'rich' ? editorCore.getHtml() : (data.html || '');
    const markdown = type === 'markdown' ? mdSource.value : (data.markdown || '');
    const attachments = [
      ...new Set([
        ...(type === 'rich' ? editorCore.getAttachments() : []),
        ...(type === 'markdown' ? mdTools.getAttachments(markdown) : []),
      ]),
    ];
    data.html = html;
    data.markdown = markdown;
    data.needsReview = true;  // 새 내용 → AI Review 활성화 (네이티브도 동일하게 기록)
    setReviewState();
    bridge
      .call('sticker.saveContent', {
        html,
        markdown,
        mode: type === 'markdown' ? 'markdown' : 'rich',
        attachments,
      })
      .catch(console.error);
  }
  if (isText) {
    window.addEventListener('blur', saveNow);
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') saveNow();
    });
    bridge.on('app.flush', saveNow);
  }

  // ---------- 타이틀바 공통 ----------
  $('#titlebar').addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (e.target.closest('button') || e.target.closest('#colorPopover') ||
        e.target.closest('#newMenu') || e.target.closest('#stTitleInput'))
      return;
    bridge.call('window.startDrag').catch(() => {});
  });

  // 웹 메모: 팝오버가 사이트 뷰에 가려지지 않도록 열려 있는 동안 사이트 뷰 숨김
  function syncSiteSuspend() {
    if (type !== 'web') return;
    const open = !$('#colorPopover').classList.contains('hidden') ||
                 !$('#newMenu').classList.contains('hidden');
    bridge.call('web.suspendSite', { on: open }).catch(() => {});
  }

  // 색상 팝오버
  const popover = $('#colorPopover');
  const colorCustom = $('#colorCustom');
  function setColor(color) {
    data.color = color;
    colorUtil.apply(color, isDark());
    updateColorDot();
    bridge.call('sticker.setColor', { color }).catch(console.error);
  }
  function updateColorDot() {
    $('#colorDot').style.background = colorUtil.effectiveBg(data.color, isDark());
  }
  function buildColorGrid() {
    const grid = $('#colorGrid');
    grid.innerHTML = '';
    const current = colorUtil.normalize(data.color);
    colorUtil.PRESETS.forEach((hex) => {
      const btn = document.createElement('button');
      btn.className = 'color-opt';
      btn.style.background = colorUtil.effectiveBg(hex, isDark());
      btn.title = hex;
      btn.classList.toggle('sel', hex === current);
      btn.addEventListener('click', () => {
        setColor(hex);
        popover.classList.add('hidden');
        syncSiteSuspend();
      });
      grid.appendChild(btn);
    });
  }
  $('#colorBtn').addEventListener('click', () => {
    if (popover.classList.contains('hidden')) {
      buildColorGrid();
      colorCustom.value = colorUtil.normalize(data.color);
      popover.classList.remove('hidden');
    } else {
      popover.classList.add('hidden');
    }
    syncSiteSuspend();
  });
  document.addEventListener('mousedown', (e) => {
    let changed = false;
    if (!e.target.closest('#colorPopover') && !e.target.closest('#colorBtn')) {
      if (!popover.classList.contains('hidden')) changed = true;
      popover.classList.add('hidden');
    }
    if (!e.target.closest('#newMenu') && !e.target.closest('#newBtn')) {
      if (!newMenu.classList.contains('hidden')) changed = true;
      newMenu.classList.add('hidden');
    }
    if (changed) syncSiteSuspend();
  });
  colorCustom.addEventListener('input', () => colorUtil.apply(colorCustom.value, isDark()));
  colorCustom.addEventListener('change', () => {
    setColor(colorCustom.value.toUpperCase());
    popover.classList.add('hidden');
    syncSiteSuspend();
  });
  updateColorDot();

  // + 메뉴 (6종)
  const newMenu = $('#newMenu');
  $('#newBtn').addEventListener('click', () => {
    newMenu.classList.toggle('hidden');
    syncSiteSuspend();
  });
  newMenu.querySelectorAll('button').forEach((btn) => {
    btn.addEventListener('click', () => {
      newMenu.classList.add('hidden');
      syncSiteSuspend();
      const t = btn.dataset.newtype;
      if (t === 'group') bridge.call('groups.new');
      else bridge.call('stickers.new', { type: t });
    });
  });

  $('#managerBtn').addEventListener('click', () => {
    bridge.call('app.openManager', { tab: 'list' }).catch(console.error);
  });

  $('#pinBtn').addEventListener('click', () => {
    const on = !$('#pinBtn').classList.contains('on');
    $('#pinBtn').classList.toggle('on', on);
    bridge.call('sticker.setTopmost', { topmost: on }).catch(console.error);
  });
  $('#hideBtn').addEventListener('click', () => {
    saveNow();
    bridge.call('sticker.hide');
  });
  $('#deleteBtn').addEventListener('click', () => {
    // 삭제 확인은 네이티브 대화상자에서 수행 (그룹창 region 클리핑 회피)
    bridge.call('sticker.delete');
  });
  // AI 패널 진입 버튼은 타이틀바에서 제거됨 (패널 자체는 다른 진입 방식을 위해 유지)

  // ==================================================================
  // 태그 / AI 제목 / AI 요약 / AI Review (rich·markdown)
  // ==================================================================
  // AI Review 입력: 두 타입 모두 마크다운으로 넘겨 번역 후에도 문서 형식이 유지되게 한다
  const noteText = () =>
    type === 'markdown' ? mdSource.value.trim()
    : type === 'rich' ? editorCore.getMarkdown().trim() : '';

  function saveMeta(patch) {
    bridge.call('sticker.setMeta', patch).catch(console.error);
  }

  // ---------- 태그 ----------
  function normalizeTag(t) {
    return t.trim().replace(/^#+/, '').trim();
  }
  // ---------- 태그로 본문 찾기 ----------
  // 태그를 누를 때마다 본문에서 그 낱말의 다음 위치로 이동한다(대소문자 무시, 순환).
  // 어디까지 찾았는지는 태그별로 기억해 둔다.
  const findState = { key: '', from: 0 };
  function findNextInBody(term) {
    if (!term) return;
    const needle = term.toLowerCase();
    // 리치/마크다운(편집)은 편집 대상에서, 마크다운 보기 모드는 렌더된 미리보기에서 찾는다
    const inPreview = type === 'markdown' && mdView !== 'edit';
    const host = type === 'markdown' ? (inPreview ? mdPreview : mdSource) : editor;
    if (!host) return;
    // 같은 태그를 연속으로 누르면 이어서, 다른 태그면 처음부터 찾는다.
    // 편집/보기 모드는 본문이 서로 달라(원본 vs 렌더 결과) 위치가 호환되지 않으므로
    // 모드까지 키에 넣어 모드가 바뀌면 처음부터 다시 찾게 한다.
    const stateKey = needle + '|' + (inPreview ? 'view' : 'edit');
    if (findState.key !== stateKey) { findState.key = stateKey; findState.from = 0; }

    if (type === 'markdown' && !inPreview) {
      const hay = host.value.toLowerCase();
      let at = hay.indexOf(needle, findState.from);
      if (at < 0) at = hay.indexOf(needle);        // 끝까지 갔으면 처음으로 순환
      if (at < 0) { findState.from = 0; return; }
      findState.from = at + needle.length;
      host.focus();
      host.setSelectionRange(at, at + needle.length);
      // 캐럿이 보이도록 대략 가운데로 스크롤
      const before = host.value.slice(0, at).split(String.fromCharCode(10)).length - 1;
      const lineH = parseFloat(getComputedStyle(host).lineHeight) || 18;
      host.scrollTop = Math.max(0, before * lineH - host.clientHeight / 2);
      return;
    }

    // contenteditable / 미리보기: 텍스트 노드를 이어 붙여 건초더미를 만든다.
    // innerText를 쓰면 블록 사이에 줄바꿈이 끼어들어 노드 오프셋과 어긋난다.
    const parts = [];
    let hay = '';
    const walker = document.createTreeWalker(host, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
      parts.push({ node: walker.currentNode, start: hay.length });
      hay += walker.currentNode.nodeValue;
    }
    if (!hay) return;
    const low = hay.toLowerCase();
    let at = low.indexOf(needle, findState.from);
    if (at < 0) at = low.indexOf(needle);
    if (at < 0) { findState.from = 0; return; }
    findState.from = at + needle.length;

    // at이 속한 텍스트 노드를 찾는다
    let hit = null;
    for (const p of parts) {
      if (at < p.start + p.node.nodeValue.length) { hit = p; break; }
    }
    if (!hit) return;
    const offset = at - hit.start;
    const range = document.createRange();
    range.setStart(hit.node, offset);
    // 인라인 서식으로 노드가 쪼개졌으면 그 노드 끝까지만 선택한다
    range.setEnd(hit.node, Math.min(hit.node.nodeValue.length, offset + needle.length));
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
    if (!inPreview) host.focus();
    const el = hit.node.parentElement;
    if (el) el.scrollIntoView({ block: 'center', behavior: 'smooth' });
  }

  function renderTags() {
    const chips = $('#tagChips');
    chips.innerHTML = '';
    const mkChip = (t, isAi) => {
      const chip = document.createElement('span');
      chip.className = 'tag-chip' + (isAi ? ' ai' : '');
      const label = document.createElement('span');
      label.className = 'tag-find';
      label.textContent = '#' + t;
      label.title = i18n.t('tags.find');
      // 태그를 누르면 본문에서 같은 글자를 찾아 그 위치로 스크롤·커서 이동한다.
      // 누를 때마다 다음 것으로, 끝에 닿으면 처음으로 돌아온다.
      label.addEventListener('mousedown', (e) => e.preventDefault());
      label.addEventListener('click', () => findNextInBody(t));
      const x = document.createElement('button');
      x.className = 'tag-x';
      x.textContent = '✕';
      x.title = i18n.t('tags.remove');
      x.addEventListener('click', () => {
        if (isAi) {
          data.aiTags = (data.aiTags || []).filter((v) => v !== t);
          saveMeta({ aiTags: data.aiTags });
        } else {
          data.tags = (data.tags || []).filter((v) => v !== t);
          saveMeta({ tags: data.tags });
        }
        renderTags();
      });
      chip.appendChild(label);
      chip.appendChild(x);
      chips.appendChild(chip);
    };
    (data.tags || []).forEach((t) => mkChip(t, false));   // 사용자 태그
    (data.aiTags || []).forEach((t) => mkChip(t, true));  // AI 생성 태그 (다른 색)
  }
  function addTags(list) {
    const cur = new Set(data.tags || []);
    let changed = false;
    list.map(normalizeTag).filter(Boolean).forEach((t) => {
      if (!cur.has(t)) { cur.add(t); changed = true; }
    });
    if (changed) {
      data.tags = [...cur];
      const patch = { tags: data.tags };
      // 같은 이름의 AI 태그가 있으면 사용자 태그로 승격 (AI 목록에서 제거)
      const lower = new Set(data.tags.map((t) => t.toLowerCase()));
      const newAi = (data.aiTags || []).filter((t) => !lower.has(t.toLowerCase()));
      if (newAi.length !== (data.aiTags || []).length) {
        data.aiTags = newAi;
        patch.aiTags = newAi;
      }
      renderTags();
      saveMeta(patch);
    }
  }
  if (isText) {
    $('#tagBar').classList.remove('hidden');
    const tagInput = $('#tagInput');
    tagInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ',') {
        e.preventDefault();
        if (tagInput.value.trim()) {
          addTags(tagInput.value.split(','));
          tagInput.value = '';
        }
      } else if (e.key === 'Backspace' && !tagInput.value && (data.tags || []).length) {
        data.tags = data.tags.slice(0, -1);
        renderTags();
        saveMeta({ tags: data.tags });
      }
    });
    tagInput.addEventListener('blur', () => {
      if (tagInput.value.trim()) {
        addTags(tagInput.value.split(','));
        tagInput.value = '';
      }
    });
    renderTags();
  }

  // ---------- 표시 언어 (AI Review 후 한국어/영어 전환) ----------
  const viewLang = () => data.viewLang || data.srcLang || 'ko';
  const hasTranslation = () =>
    !!data.srcLang && !!((data.transKo || '').trim() || (data.transEn || '').trim());
  const dispTitle = () =>
    viewLang() === 'en' ? (data.titleEn || data.title || '') : (data.title || data.titleEn || '');
  const dispSummary = () =>
    viewLang() === 'en' ? (data.summaryEn || data.summary || '')
                        : (data.summary || data.summaryEn || '');

  function renderLangSeg() {
    const seg = $('#langSeg');
    const show = isText && hasTranslation();
    seg.classList.toggle('hidden', !show);
    if (!show) return;
    seg.querySelectorAll('button').forEach((b) =>
      b.classList.toggle('on', b.dataset.lang === viewLang()));
  }

  // 선택 언어가 원문과 다르면 본문 대신 번역 뷰 표시
  function applyLangView() {
    const translated = isText && hasTranslation() && viewLang() !== data.srcLang;
    const trans = viewLang() === 'ko' ? data.transKo : data.transEn;
    $('#transView').classList.toggle('hidden', !translated);
    if (translated) {
      $('#transView').innerHTML = marked.parse(trans || '');
      $('#toolbar').classList.add('hidden');
      if (type === 'rich') editor.classList.add('hidden');
      if (type === 'markdown') {
        mdSource.classList.add('hidden');
        mdPreview.classList.add('hidden');
      }
    } else if (type === 'rich') {
      editor.classList.remove('hidden');
      $('#toolbar').classList.remove('hidden');
    } else if (type === 'markdown') {
      $('#toolbar').classList.remove('hidden');
      applyMdView();
    }
    renderLangSeg();
    renderStTitle();
    renderSummary();
  }

  document.querySelectorAll('#langSeg button').forEach((b) =>
    b.addEventListener('click', () => {
      if (data.viewLang === b.dataset.lang) return;
      data.viewLang = b.dataset.lang;
      saveMeta({ viewLang: data.viewLang });
      applyLangView();
    }));

  // ---------- AI 제목 (타이틀바 표시 + 연필로 수정, 표시 언어 반영) ----------
  function renderStTitle() {
    const t = dispTitle().trim();
    $('#stTitle').classList.toggle('hidden', !t);
    $('#stTitle').textContent = t;
    $('#stTitle').title = t;
    $('#stTitleEditBtn').classList.toggle('hidden', !t);
  }
  function setTitleEditing(on) {
    if (on) {
      $('#stTitle').classList.add('hidden');
      $('#stTitleEditBtn').classList.add('hidden');
      const input = $('#stTitleInput');
      input.classList.remove('hidden');
      input.value = dispTitle();
      input.focus();
      input.select();
    } else {
      $('#stTitleInput').classList.add('hidden');
      renderStTitle();
    }
  }
  function commitTitle() {
    const v = $('#stTitleInput').value.trim();
    // 현재 표시 언어의 제목 필드에 저장
    if (viewLang() === 'en' && (data.titleEn || data.srcLang)) {
      if (v !== (data.titleEn || '')) {
        data.titleEn = v;
        saveMeta({ titleEn: v });
      }
    } else if (v !== (data.title || '')) {
      data.title = v;
      saveMeta({ title: v });
    }
    setTitleEditing(false);
  }
  $('#stTitleEditBtn').addEventListener('click', () => setTitleEditing(true));
  $('#stTitleInput').addEventListener('blur', () => {
    if (!$('#stTitleInput').classList.contains('hidden')) commitTitle();
  });
  $('#stTitleInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      commitTitle();
    } else if (e.key === 'Escape') {
      e.target.value = dispTitle();
      setTitleEditing(false);
    }
  });
  renderStTitle();

  // ---------- AI 요약 (메모 상단) ----------
  // AI Review 오류는 이 시간이 지나면 스스로 사라지고 원래 요약으로 돌아간다
  const SUMMARY_ERROR_MS = 5000;
  let summaryErrorTimer = 0;
  function clearSummaryErrorTimer() {
    clearTimeout(summaryErrorTimer);
    summaryErrorTimer = 0;
  }

  function renderSummary(errorMsg) {
    const box = $('#summaryBox');
    clearSummaryErrorTimer();
    if (errorMsg) {
      box.classList.remove('hidden');
      box.classList.add('error');
      $('#summaryText').textContent = errorMsg;
      // CSS 애니메이션과 JS 타이머가 같은 값을 쓰도록 지속 시간을 직접 넣는다
      box.style.setProperty('--summary-timeout', SUMMARY_ERROR_MS + 'ms');
      // 연속 오류에도 바가 처음부터 다시 흐르도록 애니메이션 재시작
      const bar = $('#summaryTimerBar');
      bar.style.animation = 'none';
      void bar.offsetWidth;  // 리플로우 강제 (없으면 재시작되지 않음)
      bar.style.animation = '';
      summaryErrorTimer = setTimeout(() => {
        summaryErrorTimer = 0;
        renderSummary();  // 저장된 요약으로 복귀 (없으면 상자 숨김)
      }, SUMMARY_ERROR_MS);
      return;
    }
    box.classList.remove('error');
    const s = dispSummary().trim();
    box.classList.toggle('hidden', !s || !isText);
    $('#summaryText').textContent = s;
  }
  renderSummary();
  renderLangSeg();

  // ---------- AI Review ----------
  let reviewRequestId = null;
  let reviewBuf = '';
  let reviewSrc = '';  // 이번 리뷰에 넘긴 원문 (코드 블록 복원용)

  // 코드 블록은 번역 대상이 아니다. 모델이 내용을 바꾸거나 글자를 흘리는 경우가 있어
  // 원문의 블록을 그대로 되돌려 넣는다. 블록 수가 다르면 짝을 확신할 수 없어 손대지 않는다.
  function restoreCodeBlocks(src, translated) {
    const re = /^```[^\n]*\n[\s\S]*?^```/gm;
    const from = src.match(re);
    if (!from) return translated;
    const to = translated.match(re);
    if (!to || to.length !== from.length) return translated;
    let i = 0;
    return translated.replace(re, () => from[i++]);
  }
  function setReviewState() {
    if (!isText) return;
    const btn = $('#aiReviewBtn');
    btn.classList.remove('hidden');
    const busy = !!reviewRequestId;
    btn.disabled = busy;  // 항상 실행 가능 — 분석 진행 중에만 잠금(중복 방지)
    const need = !busy && !!data.needsReview && !!noteText();
    btn.classList.toggle('need', need);  // 새 내용 입력 시에는 강조 표시 유지
    btn.classList.toggle('busy', busy);
  }
  function parseReviewJson(text) {
    const s = text.indexOf('{');
    const e = text.lastIndexOf('}');
    if (s < 0 || e <= s) return null;
    try {
      return JSON.parse(text.slice(s, e + 1));
    } catch {
      return null;
    }
  }
  function runAiReview() {
    const text = noteText();
    if (!text || reviewRequestId) return;
    reviewBuf = '';
    reviewSrc = text;
    reviewRequestId = 'review-' + Date.now();
    setReviewState();
    clearSummaryErrorTimer();  // 새 리뷰 시작 — 이전 오류의 자동 소멸 예약 취소
    $('#summaryBox').classList.remove('hidden', 'error');
    $('#summaryText').textContent = i18n.t('review.working');
    // 리뷰 프롬프트는 prompts.js가 단일 출처 — 설정에서 편집한 값이 있으면 그것을 쓴다
    const messages = prompts.build('review', text, i18n.lang);
    bridge.call('ai.chat',
                { requestId: reviewRequestId, ownerId: init.stickerId, messages,
                  jsonFormat: true })  // 응답을 JSON으로 강제 — 형식 파싱 실패 방지
      .catch((e) => {
      reviewRequestId = null;
      renderSummary(/no model/.test(e.message) ? i18n.t('ai.noModel')
                                              : `${i18n.t('ai.error')}: ${e.message}`);
      setReviewState();
    });
  }
  if (isText) {
    $('#aiReviewBtn').addEventListener('click', runAiReview);
    bridge.on('ai.chunk', (d) => {
      if (d.requestId === reviewRequestId) reviewBuf += d.delta;
    });
    bridge.on('ai.status', (d) => {
      // 내장 모델을 처음 쓸 때는 모델 로딩(수십 초)이 먼저다
      if (d.requestId === reviewRequestId && d.state === 'loading') {
        $('#summaryText').textContent = i18n.t('ai.loadingModel');
      }
    });
    bridge.on('ai.done', (d) => {
      if (d.requestId !== reviewRequestId) return;
      reviewRequestId = null;
      if (!d.ok) {
        renderSummary(d.error === 'aborted' ? i18n.t('ai.aborted')
                                            : `${i18n.t('ai.error')}: ${d.error}`);
        setReviewState();
        return;
      }
      const r = parseReviewJson(reviewBuf);
      if (!r || typeof r.summary !== 'string') {
        renderSummary(i18n.t('review.parseFailed'));
        setReviewState();
        return;
      }
      data.summary = r.summary.trim();
      if (typeof r.summaryEn === 'string') data.summaryEn = r.summaryEn.trim();
      if (typeof r.title === 'string' && r.title.trim()) data.title = r.title.trim();
      if (typeof r.titleEn === 'string' && r.titleEn.trim()) data.titleEn = r.titleEn.trim();
      data.srcLang = r.srcLang === 'ko' ? 'ko' : 'en';
      if (typeof r.translation === 'string' && r.translation.trim()) {
        // 원문의 반대 언어 번역본 저장 (코드 블록은 원문 그대로 되돌림)
        const trans = restoreCodeBlocks(reviewSrc, r.translation.trim());
        if (data.srcLang === 'ko') data.transEn = trans;
        else data.transKo = trans;
      }
      if (!data.viewLang) data.viewLang = data.srcLang;  // 기본 표시는 원문 언어
      if (Array.isArray(r.tags)) {
        // AI 태그는 리뷰마다 새로 대체 — 사용자 태그는 보존, 중복되는 AI 태그는 제외.
        // 본문에 없는 낱말은 버린다: 프롬프트로 요구해도 모델이 지어내는 일이 있고,
        // 태그를 누르면 본문에서 찾아 이동하므로 없는 낱말은 아무 데도 닿지 못한다.
        const userSet = new Set((data.tags || []).map((t) => t.toLowerCase()));
        const body = (reviewSrc || '').toLowerCase();
        data.aiTags = [...new Set(
          r.tags.map((t) => normalizeTag(String(t))).filter(Boolean))]
          .filter((t) => !userSet.has(t.toLowerCase()))
          .filter((t) => body.includes(t.toLowerCase()));
      }
      data.needsReview = false;
      renderTags();
      setReviewState();
      applyLangView();  // 세그먼트·제목·요약·본문 뷰 갱신
      saveMeta({
        summary: data.summary,
        summaryEn: data.summaryEn || '',
        title: data.title || '',
        titleEn: data.titleEn || '',
        transKo: data.transKo || '',
        transEn: data.transEn || '',
        srcLang: data.srcLang,
        viewLang: data.viewLang || '',
        tags: data.tags || [],
        aiTags: data.aiTags || [],
        needsReview: false,
      });
    });
    setReviewState();
  }

  // ==================================================================
  // 리치 / 마크다운
  // ==================================================================
  let mdView = 'edit';

  function renderPreview() {
    mdTools.renderInto(mdPreview, mdSource.value, (idx, checked) => {
      mdSource.value = mdTools.toggleTaskInSource(mdSource.value, idx, checked);
      scheduleSave();
    });
  }

  function applyMdView() {
    const showSource = mdView === 'edit';
    mdSource.classList.toggle('hidden', !showSource);
    mdPreview.classList.toggle('hidden', showSource);
    $('#previewBtn').textContent = i18n.t(mdView === 'edit' ? 'md.preview' : 'md.edit');
    document.querySelectorAll(
      '#toolbar .fmt, #checkBtn, #imageBtn, #videoBtn, #hlBtn, #indentBtn, #outdentBtn'
    ).forEach((b) => {
      b.disabled = !showSource && type === 'markdown';
    });
    if (!showSource) renderPreview();
  }

  if (type === 'rich') {
    editor.classList.remove('hidden');
    $('#toolbar').classList.remove('hidden');
    $('#hlBtn').classList.remove('hidden');
    $('#indentBtn').classList.remove('hidden');
    $('#outdentBtn').classList.remove('hidden');
    editor.dataset.placeholder = i18n.t('editor.placeholder');
    editor.innerHTML = data.html || '';
    editorCore.init(editor, scheduleSave);
    // 저장돼 있던 3D 임베드에 뷰어 마운트 (UI는 Shadow DOM — 저장 HTML 미오염)
    editor.querySelectorAll('.embed3d').forEach((el) => viewer3d.mount(el, scheduleSave));
    // 3D 파일 드롭은 미디어 드롭보다 먼저 가로챈다
    editor.addEventListener('drop', (e) => {
      const files = [...(e.dataTransfer?.files || [])];
      const model = files.find((f) => /\.(glb|gltf|obj|stl)$/i.test(f.name));
      if (!model) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      bridge.callWithFiles('model.importPath', {}, [model])
        .then((r) => window.__insertModel3d(r.path))
        .catch(console.error);
    });
    editor.addEventListener('paste', mediaTools.handlePaste);
    editor.addEventListener('drop', mediaTools.handleDrop);
    editor.addEventListener('dragover', (e) => e.preventDefault());
  } else if (type === 'markdown') {
    $('#toolbar').classList.remove('hidden');
    $('#previewBtn').classList.remove('hidden');
    // 형광펜(<mark>)·들여쓰기(앞 공백)는 마크다운 소스로 표현할 수 있어 함께 제공한다.
    // 3D 임베드만 rich 전용이다 (본문이 HTML이 아니라 원본 텍스트라 담을 자리가 없다).
    $('#hlBtn').classList.remove('hidden');
    $('#indentBtn').classList.remove('hidden');
    $('#outdentBtn').classList.remove('hidden');
    mdSource.placeholder = i18n.t('editor.mdPlaceholder');
    mdSource.value = data.markdown || '';
    mdTools.init(mdSource, scheduleSave);
    mdTools.attachIntellisense(mdSource, $('#mdSuggest'), () => i18n.lang);
    mdSource.addEventListener('paste', mediaTools.handlePaste);
    mdSource.addEventListener('drop', mediaTools.handleDrop);
    mdSource.addEventListener('dragover', (e) => e.preventDefault());
    $('#previewBtn').addEventListener('click', () => {
      saveNow();
      mdView = mdView === 'edit' ? 'view' : 'edit';
      applyMdView();
      if (mdView === 'edit') mdSource.focus();
    });
    mdView = (data.markdown || '').trim() ? 'view' : 'edit';
    applyMdView();
  }

  if (isText) {
    const MD_FMT = {
      bold: () => mdTools.wrapSelection('**', '**'),
      italic: () => mdTools.wrapSelection('*', '*'),
      underline: () => mdTools.wrapSelection('<u>', '</u>'),
      strikeThrough: () => mdTools.wrapSelection('~~', '~~'),
      insertUnorderedList: () => mdTools.prefixLines('- '),
      insertOrderedList: () => mdTools.prefixLines('1. '),
    };
    document.querySelectorAll('#toolbar .fmt').forEach((btn) => {
      btn.addEventListener('mousedown', (e) => e.preventDefault());
      btn.addEventListener('click', () => {
        if (type === 'markdown') MD_FMT[btn.dataset.cmd]();
        else editorCore.exec(btn.dataset.cmd);
      });
    });
    // ---------- 형광펜 (rich 전용) ----------
    // 프리셋 8색 + 사용자 추가 색. 사용자 색은 설정에 저장되어 모든 메모창이 공유하고,
    // 변경은 highlight.colorsChanged 방송으로 즉시 퍼진다.
    (() => {
      const HL_PRESETS = ['#FFF176', '#FFD54F', '#FFAB91', '#F48FB1',
                          '#CE93D8', '#90CAF9', '#80DEEA', '#A5D6A7'];
      const hlBtn = $('#hlBtn');
      const pop = $('#hlPopover');
      const grid = $('#hlGrid');
      let userColors = Array.isArray(init.highlightColors) ? [...init.highlightColors] : [];

      function renderGrid() {
        grid.innerHTML = '';
        const make = (color, deletable) => {
          const sw = document.createElement('div');
          sw.className = 'hl-swatch';
          sw.style.background = color;
          sw.title = color;
          // mousedown을 막아야 클릭하는 순간 에디터 선택이 풀리지 않는다
          sw.addEventListener('mousedown', (e) => e.preventDefault());
          sw.addEventListener('click', () => { apply(color); close(); });
          if (deletable) {
            const del = document.createElement('span');
            del.className = 'hl-del';
            del.textContent = '×';
            del.title = i18n.t('hl.deleteColor');
            del.addEventListener('mousedown', (e) => e.preventDefault());
            del.addEventListener('click', (e) => {
              e.stopPropagation();
              userColors = userColors.filter((c) => c !== color);
              bridge.call('settings.set', { highlightColors: userColors }).catch(() => {});
              renderGrid();
            });
            sw.appendChild(del);
          }
          grid.appendChild(sw);
        };
        HL_PRESETS.forEach((c) => make(c, false));
        userColors.forEach((c) => make(c, true));
      }

      // 배경이 어두운지 (YIQ 가중 밝기 < 128). 'rgb(r, g, b)'와 '#RRGGBB' 모두 다룬다.
      const isDarkBg = (c) => {
        let r, g, b;
        const m = /rgba?\((\d+),\s*(\d+),\s*(\d+)/.exec(c);
        if (m) { r = +m[1]; g = +m[2]; b = +m[3]; }
        else if (/^#[0-9a-fA-F]{6}$/.test(c)) {
          const n = parseInt(c.slice(1), 16);
          r = n >> 16; g = (n >> 8) & 255; b = n & 255;
        } else return false;  // transparent 등 — 밝음으로 취급
        return (r * 299 + g * 587 + b * 114) / 1000 < 128;
      };

      const apply = (color) => {
        // 마크다운은 원본에 <mark>를 감싼다 (프리뷰가 원시 HTML을 그대로 렌더한다).
        // 글자색은 형광펜 색의 밝기로 정해 함께 박아 둔다 — 소스만 보고도 대비가
        // 결정되므로 나중에 메모 색을 바꿔도 글자가 묻히지 않는다.
        if (type === 'markdown') {
          const fg = isDarkBg(color) ? '#FFFFFF' : '#1B2620';
          mdTools.wrapSelection(
            '<mark style="background:' + color + ';color:' + fg + '">', '</mark>');
          return;
        }
        editorCore.exec('hiliteColor', color);
        // 어두운 형광펜 위에서는 글자가 묻히므로 밝게 바꾼다. 글자색 스팬은 이 형광펜
        // 로직만 만들므로, 밝은 배경(또는 지운 자리)에서는 걷어내면 원래 색으로 돌아온다.
        editor.querySelectorAll('span[style*="background-color"]').forEach((sp) => {
          if (isDarkBg(sp.style.backgroundColor)) sp.style.color = '#FFFFFF';
          else sp.style.removeProperty('color');
        });
      };
      const close = () => {
        pop.classList.add('hidden');
        $('#hlPicker').classList.add('hidden');
        $('#hlAddBtn').classList.remove('on');
      };

      hlBtn.addEventListener('mousedown', (e) => e.preventDefault());
      hlBtn.addEventListener('click', () => {
        if (pop.classList.contains('hidden')) {
          renderGrid();
          pop.classList.remove('hidden');
        } else close();
      });

      // 형광펜 지우기: rich는 배경을 투명으로 덮어쓰고, 마크다운은 <mark> 태그를 걷어낸다
      $('#hlClearBtn').addEventListener('mousedown', (e) => e.preventDefault());
      $('#hlClearBtn').addEventListener('click', () => {
        if (type === 'markdown') mdTools.clearMarks();
        else apply('transparent');
        close();
      });

      // ---------- 색 추가 피커 (자체 UI — 네이티브 피커의 스포이드 문제 회피) ----------
      // '+'로 펼치고, 색을 고른 뒤 '추가' 버튼을 눌러야 목록에 들어간다.
      const picker = $('#hlPicker');
      const sv = $('#hlSv');
      const svDot = $('#hlSvDot');
      const hueSlider = $('#hlHue');
      const hexInput = $('#hlHex');
      const preview = $('#hlPreview');
      // HSV 상태 (h 0-360, s/v 0-1). 기본값은 형광펜다운 파스텔 노랑.
      let hsv = { h: 50, s: 0.55, v: 1 };

      const hsvToHex = ({ h, s, v }) => {
        const f = (n) => {
          const k = (n + h / 60) % 6;
          const c = v - v * s * Math.max(0, Math.min(k, 4 - k, 1));
          return Math.round(c * 255).toString(16).padStart(2, '0');
        };
        return ('#' + f(5) + f(3) + f(1)).toUpperCase();
      };
      const hexToHsv = (hex) => {
        const n = parseInt(hex.slice(1), 16);
        const r = (n >> 16) / 255, g = ((n >> 8) & 255) / 255, b = (n & 255) / 255;
        const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
        let h = 0;
        if (d) {
          if (max === r) h = 60 * (((g - b) / d) % 6);
          else if (max === g) h = 60 * ((b - r) / d + 2);
          else h = 60 * ((r - g) / d + 4);
        }
        if (h < 0) h += 360;
        return { h, s: max ? d / max : 0, v: max };
      };

      // 피커의 세 입력(SV 영역·색상 슬라이더·HEX)을 상태와 동기화
      function syncPicker(fromHex) {
        const hex = hsvToHex(hsv);
        sv.style.background =
          'linear-gradient(to top, #000, transparent), ' +
          'linear-gradient(to right, #fff, hsl(' + Math.round(hsv.h) + ', 100%, 50%))';
        svDot.style.left = (hsv.s * 100) + '%';
        svDot.style.top = ((1 - hsv.v) * 100) + '%';
        hueSlider.value = Math.round(hsv.h);
        preview.style.background = hex;
        if (!fromHex) hexInput.value = hex;
      }

      // SV 영역: 클릭·드래그로 채도/명도 선택
      const pickSv = (e) => {
        const r = sv.getBoundingClientRect();
        if (!r.width || !r.height) return;  // 보이지 않는 상태 — 0으로 나누면 NaN이 전염된다
        hsv.s = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
        hsv.v = Math.max(0, Math.min(1, 1 - (e.clientY - r.top) / r.height));
        syncPicker();
      };
      sv.addEventListener('mousedown', (e) => {
        e.preventDefault();
        pickSv(e);
        const move = (ev) => pickSv(ev);
        const up = () => {
          document.removeEventListener('mousemove', move);
          document.removeEventListener('mouseup', up);
        };
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', up);
      });
      hueSlider.addEventListener('input', () => {
        hsv.h = +hueSlider.value;
        syncPicker();
      });
      hexInput.addEventListener('input', () => {
        const v = hexInput.value.trim();
        if (/^#[0-9a-fA-F]{6}$/.test(v)) {
          hsv = hexToHsv(v.toUpperCase());
          syncPicker(true);
        }
      });

      const togglePicker = (show) => {
        picker.classList.toggle('hidden', !show);
        $('#hlAddBtn').classList.toggle('on', show);
        if (show) syncPicker();
      };
      $('#hlAddBtn').addEventListener('mousedown', (e) => e.preventDefault());
      $('#hlAddBtn').addEventListener('click', () =>
        togglePicker(picker.classList.contains('hidden')));

      // 추가 버튼을 눌렀을 때만 목록에 들어간다
      $('#hlConfirmBtn').addEventListener('mousedown', (e) => e.preventDefault());
      $('#hlConfirmBtn').addEventListener('click', () => {
        const c = hsvToHex(hsv);
        if (!HL_PRESETS.includes(c) && !userColors.includes(c) && userColors.length < 24) {
          userColors.push(c);
          bridge.call('settings.set', { highlightColors: userColors }).catch(() => {});
          renderGrid();
        }
        togglePicker(false);
      });

      // 팝오버 밖 클릭으로 닫기
      document.addEventListener('mousedown', (e) => {
        if (!e.target.closest('#hlPopover') && !e.target.closest('#hlBtn')) close();
      });

      // 다른 메모창에서 색을 추가/삭제하면 즉시 반영
      bridge.on('highlight.colorsChanged', (d) => {
        userColors = Array.isArray(d.colors) ? [...d.colors] : [];
        if (!pop.classList.contains('hidden')) renderGrid();
      });
    })();

    // 들여쓰기/내어쓰기. rich는 execCommand(목록 안에서는 중첩 목록, 일반 문단은 블록
    // 들여쓰기), 마크다운은 줄 앞 공백 2칸이다 (Tab/Shift+Tab과 같은 동작).
    ['indent', 'outdent'].forEach((cmd) => {
      const b = $('#' + cmd + 'Btn');
      b.addEventListener('mousedown', (e) => e.preventDefault());
      b.addEventListener('click', () => {
        if (type === 'markdown') {
          if (cmd === 'indent') mdTools.indentLines();
          else mdTools.outdentLines();
        } else editorCore.exec(cmd);
      });
    });

    $('#checkBtn').addEventListener('mousedown', (e) => e.preventDefault());
    $('#checkBtn').addEventListener('click', () => {
      if (type === 'markdown') mdTools.prefixLines('- [ ] ');
      else editorCore.insertChecklist();
    });
    $('#imageBtn').addEventListener('click', () => mediaTools.pickImageFile());
    $('#videoBtn').addEventListener('click', () => mediaTools.pickVideo());

    // 3D 모델 삽입 (rich 전용 — 원본 파일 경로 참조, 복사하지 않음)
    function insertModel3d(path) {
      const el = viewer3d.createElement(path);
      editorCore.insertNodeAtCaret(el);
      viewer3d.mount(el, scheduleSave);
      scheduleSave();
    }
    window.__insertModel3d = insertModel3d;
    $('#model3dBtn').addEventListener('click', () => {
      if (type !== 'rich') return;
      bridge.call('model.pick')
        .then((r) => { if (!r.cancelled) insertModel3d(r.path); })
        .catch(console.error);
    });
    if (type !== 'rich') $('#model3dBtn').classList.add('hidden');

    window.__insertMedia = {
      isMarkdown: () => type === 'markdown',
      image: (url) => mdTools.insertText(`![](${url})\n`),
      video: (url) => mdTools.insertText(`<video controls src="${url}"></video>\n`),
    };

    setReviewState();  // 에디터 초기화 후 본문 기준으로 버튼 상태 재계산
    applyLangView();   // 저장된 표시 언어(번역 뷰) 복원
  }

  // ==================================================================
  // 파일 메모
  // ==================================================================
  if (type === 'file') {
    const fileArea = $('#fileArea');
    const fileListEl = $('#fileList');
    fileArea.classList.remove('hidden');
    $('#fileToolbar').classList.remove('hidden');

    let items = [];
    let view = data.fileView || 'list';
    const selection = new Set();
    const thumbCache = new Map();  // `${size}|${path}` → dataUrl
    let thumbSeq = 0;

    const thumbSize = () => (view === 'list' ? 32 : view === 'thumbS' ? 96 : 192);

    function applyViewButtons() {
      $('#viewListBtn').classList.toggle('on', view === 'list');
      $('#viewThumbSBtn').classList.toggle('on', view === 'thumbS');
      $('#viewThumbLBtn').classList.toggle('on', view === 'thumbL');
    }

    function renderFiles() {
      fileListEl.className = view;
      fileListEl.innerHTML = '';
      $('#fileEmpty').classList.toggle('hidden', items.length > 0);
      const size = thumbSize();
      const need = [];
      items.forEach((f) => {
        const item = document.createElement('div');
        item.className = 'fitem' + (f.exists ? '' : ' missing') +
                         (selection.has(f.path) ? ' sel' : '');
        item.dataset.path = f.path;
        const icon = document.createElement('div');
        icon.className = 'ficon';
        const cached = thumbCache.get(size + '|' + f.path);
        if (cached) {
          icon.style.backgroundImage = `url(${cached})`;
          icon.style.backgroundSize = 'contain';
          icon.style.backgroundRepeat = 'no-repeat';
          icon.style.backgroundPosition = 'center';
        } else {
          icon.textContent = f.isDir ? '📁' : '📄';
          if (f.exists) need.push(f.path);
        }
        const name = document.createElement('span');
        name.className = 'fname';
        name.textContent = f.name || f.path;
        name.title = f.path;
        item.appendChild(icon);
        item.appendChild(name);

        item.addEventListener('click', (e) => {
          if (!e.ctrlKey) selection.clear();
          if (selection.has(f.path)) selection.delete(f.path);
          else selection.add(f.path);
          renderFiles();
        });
        item.addEventListener('dblclick', () => bridge.call('files.open', { path: f.path }));
        fileListEl.appendChild(item);
      });
      if (need.length) {
        const requestId = 'th-' + (++thumbSeq);
        bridge.call('files.requestThumbs', { requestId, paths: need, size }).catch(() => {});
      }
    }

    bridge.on('files.thumb', (d) => {
      if (!d.dataUrl) return;
      const size = thumbSize();
      thumbCache.set(size + '|' + d.path, d.dataUrl);
      const item = fileListEl.querySelector(`.fitem[data-path="${CSS.escape(d.path)}"]`);
      if (item) {
        const icon = item.querySelector('.ficon');
        icon.textContent = '';
        icon.style.backgroundImage = `url(${d.dataUrl})`;
        icon.style.backgroundSize = 'contain';
        icon.style.backgroundRepeat = 'no-repeat';
        icon.style.backgroundPosition = 'center';
      }
    });

    function setItems(list) {
      items = list;
      renderFiles();
    }

    $('#fileAddBtn').addEventListener('click', () =>
      bridge.call('files.addDialog', { folders: false }).then(setItems));
    $('#folderAddBtn').addEventListener('click', () =>
      bridge.call('files.addDialog', { folders: true }).then(setItems));
    $('#fileCopyBtn').addEventListener('click', () => {
      if (selection.size) bridge.call('files.copyClipboard', { paths: [...selection] });
    });
    $('#fileRemoveBtn').addEventListener('click', async () => {
      let list = items;
      for (const p of selection) list = await bridge.call('files.remove', { path: p });
      selection.clear();
      setItems(list);
    });
    const setView = (v) => {
      view = v;
      applyViewButtons();
      renderFiles();
      bridge.call('files.setView', { view: v });
    };
    $('#viewListBtn').addEventListener('click', () => setView('list'));
    $('#viewThumbSBtn').addEventListener('click', () => setView('thumbS'));
    $('#viewThumbLBtn').addEventListener('click', () => setView('thumbL'));

    // 탐색기에서 드래그앤드롭 (WebView2가 File 경로를 네이티브로 전달)
    fileArea.addEventListener('dragover', (e) => {
      e.preventDefault();
      fileArea.classList.add('dragover');
    });
    fileArea.addEventListener('dragleave', () => fileArea.classList.remove('dragover'));
    fileArea.addEventListener('drop', (e) => {
      e.preventDefault();
      fileArea.classList.remove('dragover');
      const files = [...(e.dataTransfer?.files || [])];
      if (files.length)
        bridge.callWithFiles('files.addPaths', {}, files).then(setItems).catch(console.error);
    });

    applyViewButtons();
    bridge.call('files.list').then(setItems).catch(console.error);
  }

  // ==================================================================
  // 웹 메모
  // ==================================================================
  if (type === 'web') {
    $('#webBar').classList.remove('hidden');
    const urlInput = $('#webUrlInput');
    let state = { url: '', lastUrl: '' };
    try {
      state = await bridge.call('web.getState');
    } catch {}
    urlInput.value = state.lastUrl || state.url || '';

    function go() {
      const u = urlInput.value.trim();
      if (!u) return;
      if (!state.url) {
        bridge.call('web.setHome', { url: u }).then((r) => { state.url = r.url; });
      } else {
        bridge.call('web.navigate', { url: u });
      }
    }
    $('#webGoBtn').addEventListener('click', go);
    urlInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') go();
    });
    $('#webHomeBtn').addEventListener('click', () => {
      if (state.url) bridge.call('web.goHome');
      else go();
    });
    $('#webLastBtn').addEventListener('click', () => {
      if (state.lastUrl) bridge.call('web.navigate', { url: state.lastUrl });
    });
    $('#webSetHomeBtn').addEventListener('click', () => {
      const u = state.lastUrl || urlInput.value.trim();
      if (u) bridge.call('web.setHome', { url: u }).then((r) => { state.url = r.url; });
    });
    bridge.on('web.urlChanged', (d) => {
      state.lastUrl = d.url;
      if (document.activeElement !== urlInput) urlInput.value = d.url;
    });
  }

  // ==================================================================
  // PDF 메모
  // ==================================================================
  if (type === 'pdf') {
    const pdfArea = $('#pdfArea');
    pdfArea.classList.remove('hidden');

    function showPdf(r) {
      if (!r || !r.url) return;
      $('#pdfFrame').src = r.url;
      $('#pdfFrame').classList.remove('hidden');
      $('#pdfEmpty').classList.add('hidden');
    }
    $('#pdfPickBtn').addEventListener('click', () =>
      bridge.call('pdf.pick').then((r) => { if (!r.cancelled) showPdf(r); }).catch(console.error));
    pdfArea.addEventListener('dragover', (e) => {
      e.preventDefault();
      pdfArea.classList.add('dragover');
    });
    pdfArea.addEventListener('dragleave', () => pdfArea.classList.remove('dragover'));
    pdfArea.addEventListener('drop', (e) => {
      e.preventDefault();
      pdfArea.classList.remove('dragover');
      const files = [...(e.dataTransfer?.files || [])].filter((f) =>
        /\.pdf$/i.test(f.name));
      if (files.length)
        bridge.callWithFiles('pdf.setPath', {}, [files[0]]).then(showPdf).catch(console.error);
    });
    bridge.call('pdf.get').then(showPdf).catch(console.error);
  }

  // ==================================================================
  // AI 패널 (rich/markdown 전용)
  // ==================================================================
  if (isText) {
    const aiPanel = $('#aiPanel');
    const aiOutput = $('#aiOutput');
    const aiActions = $('#aiActions');
    const aiAskRow = $('#aiAskRow');
    let currentRequestId = null;
    let savedRange = null;
    let savedMdSel = null;
    let resultText = '';
    let streaming = false;
    // 자동 숨김 쪽에서 "지금 응답을 받는 중인가"를 물어본다.
    // #aiStopBtn.disabled는 setActionsState()가 한 번이라도 돌기 전에는 false라 못 쓴다.
    window.__aiStreaming = () => streaming;

    function hasSelection() {
      if (type === 'markdown') return savedMdSel && savedMdSel[0] !== savedMdSel[1];
      return savedRange && !savedRange.collapsed;
    }
    function setActionsState() {
      $('#aiStopBtn').disabled = !streaming;
      const hasResult = resultText.length > 0 && !streaming;
      $('#aiInsertBtn').disabled = !hasResult;
      $('#aiCopyBtn').disabled = !hasResult;
      $('#aiReplaceBtn').disabled = !hasResult || !hasSelection();
    }
    // 패널 진입점은 서식 툴바 맨 오른쪽의 'AI' 버튼이다 (타이틀바 버튼은 없앴다).
    // 다른 경로(단축키 등)에서 열고 싶으면 이 함수를 부른다.
    window.__toggleAiPanel = () => aiPanel.classList.toggle('hidden');
    $('#aiPanelBtn').addEventListener('mousedown', (e) => e.preventDefault());
    $('#aiPanelBtn').addEventListener('click', () => {
      // 선택을 붙잡아 두면 패널의 '바꾸기'로 그 자리를 바로 교체할 수 있다
      captureSelection();
      aiPanel.classList.toggle('hidden');
    });
    $('#aiCloseBtn').addEventListener('click', () => {
      if (currentRequestId) bridge.call('ai.abort', { requestId: currentRequestId });
      aiPanel.classList.add('hidden');
    });
    function captureSelection() {
      savedRange = null;
      savedMdSel = null;
      if (type === 'markdown') {
        if (mdView === 'edit') {
          savedMdSel = [mdSource.selectionStart, mdSource.selectionEnd];
        } else {
          // 보기 모드: 렌더된 미리보기(mdPreview)에서의 선택을 Range로 붙잡는다
          const sel = window.getSelection();
          if (sel.rangeCount && mdPreview.contains(sel.getRangeAt(0).startContainer)) {
            savedRange = sel.getRangeAt(0).cloneRange();
          }
        }
        return;
      }
      const sel = window.getSelection();
      if (sel.rangeCount && editor.contains(sel.getRangeAt(0).startContainer)) {
        savedRange = sel.getRangeAt(0).cloneRange();
      }
    }
    function selectedOrAllText() {
      if (type === 'markdown') {
        if (savedMdSel && savedMdSel[0] !== savedMdSel[1])
          return mdSource.value.slice(savedMdSel[0], savedMdSel[1]).trim();
        if (savedRange && !savedRange.collapsed) return savedRange.toString().trim();
        return mdSource.value.trim();
      }
      if (savedRange && !savedRange.collapsed) return savedRange.toString().trim();
      return editorCore.getPlainText();
    }
    function startRequest(messages) {
      currentRequestId = 'req-' + Date.now() + '-' + Math.random().toString(36).slice(2, 8);
      resultText = '';
      streaming = true;
      aiOutput.classList.remove('hidden', 'error');
      aiOutput.textContent = i18n.t('ai.working');
      aiActions.classList.remove('hidden');
      setActionsState();
      bridge.call('ai.chat',
                  { requestId: currentRequestId, ownerId: init.stickerId, messages })
        .catch((e) => {
        streaming = false;
        aiOutput.classList.add('error');
        aiOutput.textContent =
          /no model/.test(e.message) ? i18n.t('ai.noModel') : `${i18n.t('ai.error')}: ${e.message}`;
        setActionsState();
      });
    }
    function runTask(task) {
      captureSelection();
      const text = selectedOrAllText();
      if (task !== 'ask' && !text) {
        aiOutput.classList.remove('hidden');
        aiOutput.classList.add('error');
        aiOutput.textContent = i18n.t('ai.empty');
        return;
      }
      const question = $('#aiQuestion').value.trim();
      if (task === 'ask' && !question) return;
      startRequest(prompts.build(task, text, i18n.lang, question));
    }
    document.querySelectorAll('.ai-task').forEach((btn) => {
      btn.addEventListener('mousedown', (e) => e.preventDefault());
      btn.addEventListener('click', () => {
        document.querySelectorAll('.ai-task').forEach((b) => b.classList.remove('on'));
        btn.classList.add('on');
        const task = btn.dataset.task;
        if (task === 'ask') {
          aiAskRow.classList.remove('hidden');
          $('#aiQuestion').focus();
        } else {
          aiAskRow.classList.add('hidden');
          runTask(task);
        }
      });
    });
    $('#aiRunBtn').addEventListener('click', () => runTask('ask'));
    $('#aiQuestion').addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        runTask('ask');
      }
    });
    bridge.on('ai.status', (d) => {
      if (d.requestId !== currentRequestId || d.state !== 'loading') return;
      aiOutput.textContent = i18n.t('ai.loadingModel');
    });
    bridge.on('ai.chunk', (d) => {
      if (d.requestId !== currentRequestId) return;
      if (resultText === '') aiOutput.textContent = '';
      resultText += d.delta;
      aiOutput.textContent = resultText;
      aiOutput.scrollTop = aiOutput.scrollHeight;
    });
    bridge.on('ai.done', (d) => {
      if (d.requestId !== currentRequestId) return;
      streaming = false;
      if (!d.ok) {
        aiOutput.classList.add('error');
        const msg =
          d.error === 'aborted' ? i18n.t('ai.aborted') : `${i18n.t('ai.error')}: ${d.error}`;
        aiOutput.textContent = resultText ? `${resultText}\n\n[${msg}]` : msg;
      }
      setActionsState();
    });
    $('#aiStopBtn').addEventListener('click', () => {
      if (currentRequestId) bridge.call('ai.abort', { requestId: currentRequestId });
    });
    $('#aiInsertBtn').addEventListener('click', () => {
      if (!resultText) return;
      if (type === 'markdown') {
        if (mdView === 'view') {
          mdSource.value += (mdSource.value.endsWith('\n') || !mdSource.value ? '' : '\n') +
            '\n' + resultText + '\n';
          renderPreview();
        } else {
          mdTools.insertText('\n' + resultText + '\n');
        }
        scheduleSave();
        return;
      }
      editor.focus();
      const sel = window.getSelection();
      const range = document.createRange();
      range.selectNodeContents(editor);
      range.collapse(false);
      sel.removeAllRanges();
      sel.addRange(range);
      document.execCommand('insertText', false, '\n' + resultText);
      scheduleSave();
    });
    $('#aiReplaceBtn').addEventListener('click', () => {
      if (!resultText || !hasSelection()) return;
      if (type === 'markdown') {
        mdSource.setRangeText(resultText, savedMdSel[0], savedMdSel[1], 'end');
        savedMdSel = null;
        setActionsState();
        scheduleSave();
        return;
      }
      editor.focus();
      const sel = window.getSelection();
      sel.removeAllRanges();
      sel.addRange(savedRange);
      document.execCommand('insertText', false, resultText);
      savedRange = null;
      setActionsState();
      scheduleSave();
    });
    // ---------- 텍스트 선택 메뉴 ----------
    // 선택한 글에 대해 할 수 있는 동작을 선택 영역 위에 띄운다.
    // 위쪽은 서식 버튼 한 줄(SEL_FORMATS), 구분선 아래는 AI 동작(SEL_ACTIONS)이다.
    // 항목을 늘리려면 해당 배열에 하나만 추가하면 된다.
    // 서식 툴바와 같은 항목을 그대로 제공한다 (아이콘도 툴바와 동일한 것을 쓴다)
    const ICON = {
      ul: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M2 3.5a1 1 0 1 1 0 2 1 1 0 0 1 0-2zm0 4a1 1 0 1 1 0 2 1 1 0 0 1 0-2zm0 4a1 1 0 1 1 0 2 1 1 0 0 1 0-2zM5.5 4h9v1.5h-9zm0 4h9v1.5h-9zm0 4h9v1.5h-9z"/></svg>',
      ol: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M2.3 2h1v3h-1V3.2l-.6.3-.4-.8zM1.2 8.7c0-.9.7-1.4 1.4-1.4s1.3.5 1.3 1.2c0 .5-.3.9-.8 1.3l-.5.4h1.4V11H1.2v-.8l1.3-1.1c.3-.3.4-.4.4-.6 0-.2-.2-.4-.4-.4-.3 0-.4.2-.5.5zM5.5 4h9v1.5h-9zm0 4h9v1.5h-9zm0 4h9v1.5h-9zM1.2 13.4c.1-.6.6-1 1.3-1 .8 0 1.3.4 1.3 1 0 .4-.2.6-.5.8.4.1.6.4.6.8 0 .7-.6 1.1-1.4 1.1-.7 0-1.2-.4-1.3-1h.9c0 .2.2.3.4.3.3 0 .4-.1.4-.4 0-.2-.1-.3-.4-.3h-.3v-.7h.3c.2 0 .4-.1.4-.3s-.1-.3-.4-.3c-.2 0-.3.1-.4.3z"/></svg>',
      check: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="none" stroke="currentColor" stroke-width="1.4" d="M2.5 2.5h11v11h-11z"/><path fill="none" stroke="currentColor" stroke-width="1.6" d="m4.5 8 2.5 2.5L11.5 5"/></svg>',
      outdent: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M2 2.5h12V4H2zm6 3.2h6v1.5H8zm0 3.1h6v1.5H8zM2 12h12v1.5H2zM5.5 6v4L2.6 8z"/></svg>',
      indent: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M2 2.5h12V4H2zm6 3.2h6v1.5H8zm0 3.1h6v1.5H8zM2 12h12v1.5H2zM2.6 6v4L5.5 8z"/></svg>',
      hl: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M11.3 1.7a1 1 0 0 1 1.4 0l1.6 1.6a1 1 0 0 1 0 1.4l-6.8 6.8-3.4.4a.5.5 0 0 1-.6-.6l.4-3.4zM10 4.4 5 9.4l-.2 1.8 1.8-.2 5-5zM2 13.5h12V15H2z"/></svg>',
    };
    // 서식 툴바와 같은 10종. 마크다운에도 대응 문법이 있어 richOnly 항목은 없다
    // (형광펜은 <mark>, 들여쓰기는 줄 앞 공백 2칸).
    const SEL_FORMATS = [
      { cmd: 'bold', title: 'tt.bold', glyph: '<b>B</b>' },
      { cmd: 'italic', title: 'tt.italic', glyph: '<i>I</i>' },
      { cmd: 'underline', title: 'tt.underline', glyph: '<u>U</u>' },
      { cmd: 'strikeThrough', title: 'tt.strike', glyph: '<s>S</s>' },
      { cmd: 'highlight', title: 'sel.highlight', glyph: ICON.hl },
      { cmd: 'insertUnorderedList', title: 'tt.ul', glyph: ICON.ul },
      { cmd: 'insertOrderedList', title: 'tt.ol', glyph: ICON.ol },
      { cmd: 'checklist', title: 'tt.check', glyph: ICON.check },
      { cmd: 'outdent', title: 'tt.outdent', glyph: ICON.outdent },
      { cmd: 'indent', title: 'tt.indent', glyph: ICON.indent },
    ];
    // 마크다운은 원본을 감싸고, 리치는 execCommand를 쓴다 (서식 툴바와 같은 규칙)
    const MD_WRAP = {
      bold: ['**', '**'], italic: ['*', '*'],
      underline: ['<u>', '</u>'], strikeThrough: ['~~', '~~'],
    };
    const MD_PREFIX = {
      insertUnorderedList: '- ', insertOrderedList: '1. ', checklist: '- [ ] ',
    };
    // 마크다운 보기 모드는 렌더된 결과라 편집할 수 없다 — 서식 줄을 감춘다
    const canFormat = () => type !== 'markdown' || mdView === 'edit';
    function applyFormat(cmd) {
      // 형광펜은 색을 골라야 하므로 서식 툴바의 팝오버를 그대로 연다 (rich·markdown 공통)
      if (cmd === 'highlight') { $('#hlBtn').click(); return; }
      if (type === 'markdown') {
        if (MD_WRAP[cmd]) mdTools.wrapSelection(...MD_WRAP[cmd]);
        else if (MD_PREFIX[cmd]) mdTools.prefixLines(MD_PREFIX[cmd]);
        else if (cmd === 'indent') mdTools.indentLines();
        else if (cmd === 'outdent') mdTools.outdentLines();
        return;
      }
      if (cmd === 'checklist') { editorCore.insertChecklist(); return; }
      editorCore.exec(cmd);
    }

    // AI 항목: 선택한 글을 문맥으로 넣고 해당 작업을 바로 실행한다.
    // 결과는 AI 패널에 스트리밍되고, 패널의 '바꾸기'로 원문을 교체할 수 있다.
    const aiAction = (id, task, labelKey, icon) => ({
      id, icon,
      label: () => i18n.t(labelKey),
      run() {
        captureSelection();          // 선택 범위를 잡아 둔다 (결과 교체 시 사용)
        aiPanel.classList.remove('hidden');
        aiAskRow.classList.add('hidden');
        document.querySelectorAll('.ai-task').forEach((b) => b.classList.remove('on'));
        const btn = document.querySelector('.ai-task[data-task="' + task + '"]');
        if (btn) btn.classList.add('on');
        runTask(task);
      },
    });

    const SEL_ACTIONS = [
      aiAction('summarize', 'summarize', 'sel.summarize',
        '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" '
        + 'd="M2 2.5h12V4H2zm0 3.4h12v1.5H2zm0 3.4h8v1.5H2zm0 3.4h5V14H2z"/></svg>'),
      aiAction('spellcheck', 'spellcheck', 'sel.spellcheck',
        '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="none" stroke="currentColor" '
        + 'stroke-width="1.5" d="m1.8 9.4 2.6 2.6 5-6.4"/><path fill="currentColor" '
        + 'd="M9.6 12.2h5.1v1.4H9.6zM11.4 2.3h1.6l2.4 6.3h-1.5l-.5-1.5h-2.5l-.5 1.5H8.9zm.8 1.9-.8 2.3h1.6z"/></svg>'),
      aiAction('refine', 'refine', 'sel.refine',
        '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" '
        + 'd="M11.3 1.7a1 1 0 0 1 1.4 0l1.6 1.6a1 1 0 0 1 0 1.4l-7.6 7.6-3.4.4a.5.5 0 0 1-.6-.6'
        + 'l.4-3.4zM10 4.4l-5 5-.2 1.8 1.8-.2 5-5z"/><path fill="currentColor" '
        + 'd="M2.6 2 3 3.2l1.2.4-1.2.4-.4 1.2-.4-1.2L1 3.6l1.2-.4z"/></svg>'),
      {
        id: 'ask',
        label: () => i18n.t('sel.askAi'),
        icon: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" '
            + 'd="M8 1l1.6 4.1L14 6.7l-4.4 1.6L8 12.4 6.4 8.3 2 6.7l4.4-1.6zM12.8 10.6l.8 2 2 .8'
            + '-2 .8-.8 2-.8-2-2-.8 2-.8z"/></svg>',
        run() {
          // 선택을 AI 패널이 쓰는 형태로 붙잡는다 — 실행 시 선택 전문이 문맥(NOTE)으로 들어간다
          captureSelection();
          const selText = selectedOrAllText();
          aiPanel.classList.remove('hidden');
          const askBtn = document.querySelector('.ai-task[data-task="ask"]');
          document.querySelectorAll('.ai-task').forEach((b) => b.classList.remove('on'));
          if (askBtn) askBtn.classList.add('on');
          aiAskRow.classList.remove('hidden');
          // 질문을 자동으로 채워 바로 실행한다. 인용이 길면 질문 문구에서는 줄이되,
          // 선택 전문은 위에서 문맥으로 함께 전달되므로 정보가 잘리지 않는다.
          // ("조사해서"는 웹 검색을 암시해 로컬 AI에 맞지 않아 "설명" 문구를 쓴다)
          const quote = selText.length > 40 ? selText.slice(0, 40) + '…' : selText;
          $('#aiQuestion').value = i18n.t('sel.askPrompt').replace('{text}', quote);
          runTask('ask');
        },
      },
    ];

    const selMenu = $('#selMenu');
    selMenu.innerHTML = '';

    const fmtRow = document.createElement('div');
    fmtRow.className = 'sel-formats';
    const fmtGrid = document.createElement('div');
    fmtGrid.className = 'sel-fmt-grid';
    fmtRow.appendChild(fmtGrid);
    SEL_FORMATS.forEach((f) => {
      const b = document.createElement('button');
      b.className = 'sel-fmt';
      b.dataset.cmd = f.cmd;
      b.innerHTML = f.glyph;
      b.title = i18n.t(f.title);
      // mousedown 기본 동작을 막아야 클릭하는 순간 선택이 풀리지 않는다
      b.addEventListener('mousedown', (e) => e.preventDefault());
      b.addEventListener('click', () => { hideSelMenu(); applyFormat(f.cmd); });
      fmtGrid.appendChild(b);
    });
    selMenu.appendChild(fmtRow);

    const selSep = document.createElement('div');
    selSep.className = 'sel-sep';
    selMenu.appendChild(selSep);

    SEL_ACTIONS.forEach((a) => {
      const b = document.createElement('button');
      b.className = 'sel-item';
      b.dataset.action = a.id;
      b.innerHTML = a.icon + '<span></span>';
      b.querySelector('span').textContent = a.label();
      // mousedown 기본 동작을 막아야 클릭하는 순간 선택이 풀리지 않는다
      b.addEventListener('mousedown', (e) => e.preventDefault());
      // AI 동작은 한 번 고르면 끝이므로 메뉴를 닫아 둔다. 이 mouseup이 document까지
      // 올라가면 updateSelMenu가 (선택이 남아 있으니) 메뉴를 곧바로 되살린다.
      // 서식 버튼은 일부러 막지 않는다 — 메뉴가 남아 여러 서식을 이어서 적용할 수 있다.
      b.addEventListener('mouseup', (e) => e.stopPropagation());
      b.addEventListener('click', () => { hideSelMenu(); a.run(); });
      selMenu.appendChild(b);
    });

    function hideSelMenu() { selMenu.classList.add('hidden'); }

    // 선택 영역의 화면 좌표. textarea(마크다운)는 Range API가 없어 미러 div로 잰다.
    function selectionRect() {
      if (type === 'markdown') {
        if (mdView !== 'edit') {
          // 보기 모드: 렌더된 DOM이므로 리치 메모와 같은 방식으로 잰다
          const sel = window.getSelection();
          if (!sel.rangeCount || sel.isCollapsed) return null;
          const r = sel.getRangeAt(0);
          if (!mdPreview.contains(r.commonAncestorContainer)) return null;
          const box = r.getBoundingClientRect();
          if (!box.width && !box.height) return null;
          return { left: box.left, top: box.top, width: box.width };
        }
        const a = mdSource.selectionStart, b = mdSource.selectionEnd;
        if (a === b) return null;
        const cs = getComputedStyle(mdSource);
        const mirror = document.createElement('div');
        ['fontFamily', 'fontSize', 'fontWeight', 'lineHeight', 'letterSpacing', 'padding',
         'border', 'boxSizing', 'whiteSpace', 'wordBreak'].forEach((k) => {
          mirror.style[k] = cs[k];
        });
        mirror.style.position = 'absolute';
        mirror.style.visibility = 'hidden';
        mirror.style.whiteSpace = 'pre-wrap';
        mirror.style.width = mdSource.clientWidth + 'px';
        mirror.textContent = mdSource.value.slice(0, a);
        const mark = document.createElement('span');
        mark.textContent = mdSource.value.slice(a, b) || ' ';
        mirror.appendChild(mark);
        document.body.appendChild(mirror);
        const mr = mark.getBoundingClientRect(), rootRect = mirror.getBoundingClientRect();
        const taRect = mdSource.getBoundingClientRect();
        document.body.removeChild(mirror);
        return {
          left: taRect.left + (mr.left - rootRect.left),
          top: taRect.top + (mr.top - rootRect.top) - mdSource.scrollTop,
          width: mr.width,
        };
      }
      const sel = window.getSelection();
      if (!sel.rangeCount || sel.isCollapsed) return null;
      const r = sel.getRangeAt(0);
      if (!editor.contains(r.commonAncestorContainer)) return null;
      const box = r.getBoundingClientRect();
      if (!box.width && !box.height) return null;
      return { left: box.left, top: box.top, width: box.width };
    }

    function updateSelMenu() {
      const rect = selectionRect();
      const text = (type === 'markdown' && mdView === 'edit')
        ? mdSource.value.slice(mdSource.selectionStart, mdSource.selectionEnd).trim()
        : String(window.getSelection());
      if (!rect || !text.trim()) { hideSelMenu(); return; }
      const fmt = canFormat();
      fmtGrid.classList.toggle('hidden', !fmt);  // 편집할 수 없으면 격자째 감춘다
      selMenu.classList.remove('hidden');
      const mw = selMenu.offsetWidth, mh = selMenu.offsetHeight;
      let left = rect.left + rect.width / 2 - mw / 2;
      left = Math.max(6, Math.min(left, window.innerWidth - mw - 6));
      let top = rect.top - mh - 8;
      if (top < 4) top = rect.top + 24;  // 위가 좁으면 선택 아래로
      selMenu.style.left = Math.round(left) + 'px';
      selMenu.style.top = Math.round(top) + 'px';
    }

    // 선택이 끝나는 시점에만 갱신 (드래그 중에는 방해되지 않게)
    document.addEventListener('mouseup', () => setTimeout(updateSelMenu, 0));
    document.addEventListener('keyup', (e) => {
      if (e.shiftKey || e.key === 'Escape' || e.ctrlKey) setTimeout(updateSelMenu, 0);
    });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') hideSelMenu(); });
    document.addEventListener('mousedown', (e) => {
      if (!selMenu.contains(e.target)) hideSelMenu();
    }, true);
    document.addEventListener('scroll', hideSelMenu, true);
    window.addEventListener('blur', hideSelMenu);

    $('#aiCopyBtn').addEventListener('click', async () => {
      try {
        await navigator.clipboard.writeText(resultText);
      } catch {
        const ta = document.createElement('textarea');
        ta.value = resultText;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        ta.remove();
      }
    });
  }

  // ---------- 툴바 오버플로 ('더보기') ----------
  // 폭이 모자라면 지정한 버튼들을 뒤에서부터 '더보기' 메뉴로 접어 넣는다.
  // 좁아질수록 더 많이 접힌다. 헤더(타이틀바)와 서식 툴바가 같은 코드를 쓴다.
  //
  // 버튼을 DOM에서 옮기지 않고 감추기만 하고, 메뉴에는 원래 버튼을 대신 눌러 주는
  // 대리 항목을 만든다 — 리스너와 팝오버(형광펜·새 메모)의 위치 기준이 그대로 유지된다.
  function setupOverflow(bar, moreBtn, moreMenu, items, reserveExtra) {
    if (!bar || !moreBtn || !moreMenu) return () => {};
    const closeMore = () => moreMenu.classList.add('hidden');

    // 접힌 버튼들로 메뉴를 다시 만든다 (아이콘은 원본을 복제, 이름은 title에서)
    function buildMenu(folded) {
      moreMenu.innerHTML = '';
      folded.forEach((btn) => {
        if (!btn.matches('button')) return;  // 구분선은 메뉴에 넣지 않는다
        const item = document.createElement('button');
        item.className = 'tb-more-item';
        if (btn.disabled) item.disabled = true;
        const icon = document.createElement('span');
        icon.className = 'tb-more-icon';
        icon.innerHTML = btn.innerHTML;
        const label = document.createElement('span');
        label.textContent = btn.title || '';
        item.appendChild(icon);
        item.appendChild(label);
        item.addEventListener('mousedown', (e) => e.preventDefault());
        item.addEventListener('click', () => { closeMore(); btn.click(); });
        moreMenu.appendChild(item);
      });
    }

    function layout() {
      // 숨겨졌거나 아직 배치 전이면 잴 수 없다 (UI 자동 숨김 중에는 폭이 0이다)
      if (bar.classList.contains('hidden') || !bar.clientWidth) return;
      // 1) 모두 펼친 상태로 되돌려 실제 폭을 잰다 (감춘 뒤에는 offsetWidth가 0이다)
      items.forEach((el) => el.classList.remove('tb-overflow'));
      moreBtn.classList.add('hidden');
      closeMore();
      const style = getComputedStyle(bar);
      const gap = parseFloat(style.gap) || 0;
      const pad = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
      // 접을 수 없는 것들이 차지하는 폭
      let fixed = 0;
      const walk = (el) => {
        for (const c of el.children) {
          if (c === moreMenu || c === moreBtn) continue;
          if (c.classList.contains('hidden')) continue;
          if (items.includes(c)) continue;
          const cs = getComputedStyle(c);
          if (cs.position === 'absolute') continue;
          // 늘어나는 여백(드래그 영역·스페이서)은 지금 폭이 "남은 공간"이라 세면 안 된다.
          // 세는 순간 avail이 0에 가까워져 폭과 상관없이 다 접힌다(실측).
          // 대신 reserveExtra로 최소 여백만 잡아 둔다.
          if (parseFloat(cs.flexGrow) > 0) continue;
          if (c.classList.contains('tb-group')) { walk(c); continue; }
          fixed += c.offsetWidth + gap;
        }
      };
      walk(bar);
      const live = items.filter((el) => !el.classList.contains('hidden'));
      const widths = live.map((el) => el.offsetWidth + gap);
      let need = widths.reduce((a, b) => a + b, 0);
      let avail = bar.clientWidth - pad - fixed - (reserveExtra || 0);
      if (need <= avail) return;  // 다 들어간다

      // 2) 더보기 버튼 자리를 확보하고, 뒤에서부터 접는다
      moreBtn.classList.remove('hidden');
      avail -= moreBtn.offsetWidth + gap;
      const folded = [];
      for (let i = live.length - 1; i >= 0 && need > avail; i--) {
        need -= widths[i];
        live[i].classList.add('tb-overflow');
        folded.unshift(live[i]);
      }
      buildMenu(folded);
    }

    moreBtn.addEventListener('mousedown', (e) => e.preventDefault());
    moreBtn.addEventListener('click', () => moreMenu.classList.toggle('hidden'));
    document.addEventListener('mousedown', (e) => {
      if (!moreMenu.contains(e.target) && !moreBtn.contains(e.target)) closeMore();
    });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMore(); });
    if (window.ResizeObserver) new ResizeObserver(layout).observe(bar);
    window.addEventListener('resize', layout);
    return layout;
  }

  (() => {
    // --- 서식 툴바: 스페이서 앞의 항목이 접힘 대상 (미리보기·AI는 항상 남는다) ---
    const toolbar = $('#toolbar');
    const tbItems = [];
    if (toolbar) {
      const spacer = toolbar.querySelector('.tb-spacer');
      for (const el of toolbar.children) {
        if (el === spacer) break;
        if (el.id === 'tbMoreBtn' || el.id === 'tbMoreMenu' || el.id === 'hlPopover') continue;
        tbItems.push(el);
      }
    }
    const layoutToolbar =
      setupOverflow(toolbar, $('#tbMoreBtn'), $('#tbMoreMenu'), tbItems, 0);

    // --- 헤더: 오른쪽 묶음에서 숨기기(창 닫기 성격)만 남기고 접는다.
    //     제목·드래그 영역이 늘어나는 자리라 최소 여백(28px)을 남겨 둔다. ---
    const hdrItems = ['#managerBtn', '#newBtn', '#aiReviewBtn', '#deleteBtn']
      .map((sel) => $(sel)).filter(Boolean);
    const layoutHeader =
      setupOverflow($('#titlebar'), $('#hdrMoreBtn'), $('#hdrMoreMenu'), hdrItems, 28);

    const relayout = () => { layoutToolbar(); layoutHeader(); };
    window.__relayoutToolbar = relayout;
    requestAnimationFrame(() => requestAnimationFrame(relayout));
  })();

  // ---------- UI 자동 숨김 (헤더 + 서식 툴바) ----------
  // 마우스가 창을 벗어나고 3초 뒤: 창 내용 전체를 페이드 아웃 → 헤더/서식 툴바를
  // 레이아웃에서 제거해 본문이 그 여백을 채움 → 새 레이아웃으로 페이드 인.
  // 보일 때는 역순. 텍스트가 움직이는 순간은 항상 화면이 비어 있을 때다.
  // 창 크기는 바뀌지 않는다.
  (() => {
    const root = document.documentElement;
    const FADE_MS = 180, HIDE_DELAY_MS = 3000, INITIAL_DELAY_MS = 3000;
    let enabled = init.autoHideUi !== false;
    // true면 창을 클릭해야 UI가 나타난다 (마우스만 올리는 것으로는 나타나지 않음)
    let clickOnly = init.uiRevealOnClick !== false;
    let hidden = false;  // 레이아웃 기준 목표 상태
    let seq = 0;         // 진행 중인 전환의 취소 토큰
    let timer = 0;
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));

    // 팝오버가 열려 있으면 숨기지 않는다
    const popoverOpen = () =>
      !$('#newMenu').classList.contains('hidden') ||
      !$('#colorPopover').classList.contains('hidden') ||
      !$('#hlPopover').classList.contains('hidden') ||
      !$('#tbMoreMenu').classList.contains('hidden') ||
      !$('#hdrMoreMenu').classList.contains('hidden');
    // 텍스트 입력 중(에디터·마크다운·태그·타이틀·URL 입력에 캐럿이 있음)에는 숨기지 않는다
    const isEditing = () => {
      if (!document.hasFocus()) return false;
      const a = document.activeElement;
      return !!a && (a.isContentEditable || a.tagName === 'TEXTAREA' || a.tagName === 'INPUT');
    };
    // AI 응답을 받는 중이면 숨기지 않는다 — 숨김이 패널을 닫으면서 요청을 끊기 때문
    const aiBusy = () =>
      typeof window.__aiStreaming === 'function' && window.__aiStreaming();
    // 클릭해야 보이는 모드에서는 마우스를 올려둔 것만으로 UI를 붙잡아 두지 않는다
    const canHide = () =>
      (clickOnly || !root.matches(':hover')) && !isEditing() && !popoverOpen() && !aiBusy();

    // UI를 감출 때 AI 패널도 닫는다. 닫기 버튼에 맡겨 진행 중 요청 정리까지 함께 한다.
    // (다시 보일 때 패널을 되살리지는 않는다 — 사용자가 직접 열어야 한다)
    function closeAiPanel() {
      const panel = $('#aiPanel');
      if (panel && !panel.classList.contains('hidden')) $('#aiCloseBtn').click();
    }

    // web 메모는 사이트 뷰가 별도의 네이티브 자식 창이라 CSS 리플로우가 닿지 않는다.
    // 타이틀바가 빠진 만큼 상단 스트립을 줄여 달라고 네이티브에 알린다.
    const syncNative = (on) => {
      if (type === 'web') bridge.call('window.setUiHidden', { hidden: on }).catch(() => {});
    };

    async function hide() {
      if (!enabled || hidden || !canHide()) return;
      const my = ++seq;
      document.body.classList.add('ui-fading');
      await wait(FADE_MS + 40);
      if (my !== seq) return;  // 도중에 show()가 취소함
      if (!enabled || !canHide()) {
        document.body.classList.remove('ui-fading');
        return;
      }
      hidden = true;
      closeAiPanel();
      root.classList.add('ui-collapsed');  // 화면이 비어 있는 동안에만 자리가 바뀐다
      syncNative(true);
      requestAnimationFrame(() => {
        if (my === seq) document.body.classList.remove('ui-fading');  // 페이드 인
      });
    }

    async function show() {
      clearTimeout(timer);
      if (!hidden) {
        // 페이드 아웃이 진행 중이던 hide를 취소하고 즉시 되살린다
        if (document.body.classList.contains('ui-fading')) {
          seq++;
          document.body.classList.remove('ui-fading');
        }
        return;
      }
      const my = ++seq;
      document.body.classList.add('ui-fading');
      await wait(FADE_MS + 40);
      if (my !== seq) return;
      hidden = false;
      root.classList.remove('ui-collapsed');
      syncNative(false);
      requestAnimationFrame(() => {
        if (my === seq) document.body.classList.remove('ui-fading');
      });
    }

    const schedule = () => {
      if (!enabled) return;
      clearTimeout(timer);
      timer = setTimeout(hide, HIDE_DELAY_MS);
    };
    // 호버로 보이기 (클릭 전용 모드가 아닐 때만)
    root.addEventListener('mouseenter', () => { if (!clickOnly) show(); });
    root.addEventListener('mouseleave', schedule);
    // 클릭으로 보이기 — 창 안 어디를 눌러도 UI가 올라온다
    document.addEventListener('mousedown', () => show(), true);
    // 입력을 마치고 포커스가 떠나거나 창이 비활성화되면 (마우스도 밖이면) 숨김 예약
    document.addEventListener('focusout', schedule);
    window.addEventListener('blur', schedule);
    // 키보드 포커스로 입력이 재개되면 (Alt+Tab 복귀 등) 숨겨진 UI를 되살린다
    window.addEventListener('focus', () => { if (isEditing()) show(); });

    bridge.on('ui.autoHideChanged', (d) => {
      enabled = !!d.on;
      if (!enabled) show();
      else if (canHide()) hide();
    });

    // 표시 조건(호버 vs 클릭) 변경 — 클릭 전용으로 바뀌면 호버로 떠 있던 UI를 정리한다
    bridge.on('ui.revealModeChanged', (d) => {
      clickOnly = !!d.clickOnly;
      if (enabled && clickOnly && canHide()) hide();
    });

    // 최초 로드: 잠시 뒤 마우스가 창 위에 없고 입력 중도 아니면 숨긴다
    setTimeout(() => { if (enabled && canHide()) hide(); }, INITIAL_DELAY_MS);
  })();

  // ---------- 다중 선택 (Shift+클릭으로 고르고, 함께 옮기거나 Delete로 숨김) ----------
  (() => {
    let selected = false;

    // 클릭을 네이티브에 알린다. Shift면 토글, 아니면 (선택 밖 창일 때) 전체 해제.
    // capture 단계에서 잡아 에디터·버튼보다 먼저 처리한다.
    document.addEventListener('mousedown', (e) => {
      if (e.shiftKey) {
        // Shift+클릭은 창 선택 전용 — 본문 텍스트 선택 확장은 막는다
        e.preventDefault();
        e.stopPropagation();
      }
      bridge.call('selection.click', { id: init.stickerId, shift: !!e.shiftKey })
        .catch(() => {});
    }, true);

    bridge.on('selection.changed', (d) => {
      const on = Array.isArray(d.ids) && d.ids.includes(init.stickerId);
      if (on === selected) return;
      selected = on;
      // 선택 테두리는 네이티브가 그린다(그룹 드롭 하이라이트와 동일한 모양).
      // 페이지는 Delete 처리를 위해 선택 여부만 알고 있으면 된다.
      // 선택되면 캐럿을 빼서 Delete가 글자 지우기와 헷갈리지 않게 한다
      if (on && document.activeElement && document.activeElement !== document.body) {
        document.activeElement.blur();
      }
    });

    // Delete: 선택된 창들을 숨긴다. 글을 쓰는 중이면 원래의 글자 삭제로 둔다.
    document.addEventListener('keydown', (e) => {
      if (e.key !== 'Delete' || !selected) return;
      const a = document.activeElement;
      const editing = a && (a.isContentEditable || a.tagName === 'TEXTAREA' ||
                            a.tagName === 'INPUT');
      if (editing) return;
      e.preventDefault();
      bridge.call('selection.hide').catch(() => {});
    });
  })();

  // ---------- 네이티브 이벤트 ----------
  // (테마 변경은 메모창에 영향 없음 — theme.changed 무시)
  bridge.on('locale.changed', async (d) => {
    await i18n.load(d.lang);
    i18n.apply();
    if (type === 'rich') editor.dataset.placeholder = i18n.t('editor.placeholder');
    if (type === 'markdown') {
      mdSource.placeholder = i18n.t('editor.mdPlaceholder');
      applyMdView();
    }
  });
})();
