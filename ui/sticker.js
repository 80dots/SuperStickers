// 스티커 페이지 메인 — 타입(rich/markdown/file/web/pdf)별 UI 구성
(async () => {
  const init = window.__init || { page: 'sticker', theme: 'light', lang: 'en' };
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
  if (!isText) $('#aiBtn').classList.add('hidden');

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
  function renderTags() {
    const chips = $('#tagChips');
    chips.innerHTML = '';
    const mkChip = (t, isAi) => {
      const chip = document.createElement('span');
      chip.className = 'tag-chip' + (isAi ? ' ai' : '');
      const label = document.createElement('span');
      label.textContent = '#' + t;
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
    const messages = [
      {
        role: 'system',
        content:
          '당신은 스티커 메모 앱의 리뷰 도우미입니다. 주어진 메모 내용을 분석해 반드시 아래 JSON ' +
          '형식으로만 응답하세요. 다른 설명이나 코드 펜스는 출력하지 마세요.\n' +
          '입력은 마크다운 문서입니다.\n' +
          '{"srcLang":"본문의 주 언어. ko 또는 en (그 외 언어면 en)",' +
          '"summary":"한국어 요약 1~3문장 (본문이 다른 언어면 번역해서 요약)",' +
          '"summaryEn":"영어 요약 1~3문장",' +
          '"title":"요약을 바탕으로 한 15자 이내의 한국어 제목",' +
          '"titleEn":"영어 제목 (5단어 이내)",' +
          '"tags":["중요 키워드 3~6개, 각각 1~3단어의 한국어"],' +
          '"translation":"본문 전체를 반대 언어로 충실히 번역 (srcLang이 ko면 영어로, en이면 한국어로)"}\n' +
          '\n[translation 작성 규칙 - 반드시 지킬 것]\n' +
          '1. 마크다운 기호를 원문 그대로 남긴다: 제목 #/##/###, 목록 -/*/1., ' +
          '체크박스 - [ ] 와 - [x], 굵게 **, 기울임 *, 취소선 ~~, 인용 >, 표 |, 수평선 ---\n' +
          '2. 코드 블록(```)과 인라인 코드(`)의 내용은 번역하지 않고 그대로 복사한다. ' +
          '언어 표시(```js 등)도 유지한다.\n' +
          '3. 링크와 이미지는 [텍스트](주소), ![대체텍스트](주소) 형태를 유지하고 ' +
          '주소는 절대 바꾸지 않는다.\n' +
          '4. HTML 태그(<u>, <br> 등), 파일 경로, 명령어, 변수와 함수 이름은 그대로 둔다.\n' +
          '5. 줄바꿈과 빈 줄, 들여쓰기를 원문과 똑같이 유지한다. 줄 수가 달라지면 안 된다.\n' +
          '6. 사람이 읽는 문장만 번역한다. 기호나 구조는 절대 지우거나 바꾸지 않는다.\n' +
          '예) "## 설치\\n- `npm install` 실행" -> "## Install\\n- Run `npm install`"',
      },
      { role: 'user', content: text },
    ];
    bridge.call('ollama.chat',
                { requestId: reviewRequestId, ownerId: init.stickerId, messages,
                  format: 'json' })  // Ollama JSON 강제 — 형식 파싱 실패 방지
      .catch((e) => {
      reviewRequestId = null;
      renderSummary(/no model/.test(e.message) ? i18n.t('ai.noModel')
                                              : `${i18n.t('ai.error')}: ${e.message}`);
      setReviewState();
    });
  }
  if (isText) {
    $('#aiReviewBtn').addEventListener('click', runAiReview);
    bridge.on('ollama.chunk', (d) => {
      if (d.requestId === reviewRequestId) reviewBuf += d.delta;
    });
    bridge.on('ollama.done', (d) => {
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
        // AI 태그는 리뷰마다 새로 대체 — 사용자 태그는 보존, 중복되는 AI 태그는 제외
        const userSet = new Set((data.tags || []).map((t) => t.toLowerCase()));
        data.aiTags = [...new Set(
          r.tags.map((t) => normalizeTag(String(t))).filter(Boolean))]
          .filter((t) => !userSet.has(t.toLowerCase()));
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
    document.querySelectorAll('#toolbar .fmt, #checkBtn, #imageBtn, #videoBtn').forEach((b) => {
      b.disabled = !showSource && type === 'markdown';
    });
    if (!showSource) renderPreview();
  }

  if (type === 'rich') {
    editor.classList.remove('hidden');
    $('#toolbar').classList.remove('hidden');
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
    $('#aiBtn').addEventListener('click', () => aiPanel.classList.toggle('hidden'));
    $('#aiCloseBtn').addEventListener('click', () => {
      if (currentRequestId) bridge.call('ollama.abort', { requestId: currentRequestId });
      aiPanel.classList.add('hidden');
    });
    function captureSelection() {
      savedRange = null;
      savedMdSel = null;
      if (type === 'markdown') {
        if (mdView === 'edit') savedMdSel = [mdSource.selectionStart, mdSource.selectionEnd];
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
      bridge.call('ollama.chat',
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
    bridge.on('ollama.chunk', (d) => {
      if (d.requestId !== currentRequestId) return;
      if (resultText === '') aiOutput.textContent = '';
      resultText += d.delta;
      aiOutput.textContent = resultText;
      aiOutput.scrollTop = aiOutput.scrollHeight;
    });
    bridge.on('ollama.done', (d) => {
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
      if (currentRequestId) bridge.call('ollama.abort', { requestId: currentRequestId });
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

  // ---------- UI 자동 숨김 (헤더 + 서식 툴바) ----------
  // 마우스가 창을 벗어나고 3초 뒤: 헤더/서식 툴바를 페이드 아웃한 다음 네이티브가
  // 창 가장자리만 200ms 동안 부드럽게 줄인다. 페이지 레이아웃은 절대 바꾸지 않으며,
  // 네이티브가 WebView 자식 창을 화면에 고정(핀)하므로 본문은 1px도 움직이지 않는다
  // — 잘려나가는 부분은 이미 투명해진 헤더/툴바 영역뿐이다. 펼칠 때는 역순.
  (() => {
    const header = $('#titlebar');
    const toolbar = $('#toolbar');
    const root = document.documentElement;
    const FADE_MS = 180, RESIZE_MS = 200, HIDE_DELAY_MS = 3000, INITIAL_DELAY_MS = 3000;
    let enabled = init.autoHideUi !== false;
    // true면 창을 클릭해야 UI가 나타난다 (마우스만 올리는 것으로는 나타나지 않음)
    let clickOnly = init.uiRevealOnClick !== false;
    let state = 'expanded';  // 'expanded' | 'fading'(페이드 아웃 중) | 'collapsed'
    let timer = 0;

    // 접기 대상: 헤더는 항상, 서식 툴바는 표시 중일 때만 (file/web/pdf 메모는 툴바 없음)
    const parts = () => {
      const els = [header];
      if (toolbar && !toolbar.classList.contains('hidden')) els.push(toolbar);
      return els;
    };
    // 팝오버가 열려 있으면 접지 않는다
    const popoverOpen = () =>
      !$('#newMenu').classList.contains('hidden') ||
      !$('#colorPopover').classList.contains('hidden');
    // 텍스트 입력 중(에디터·마크다운·태그·타이틀·URL 입력에 캐럿이 있음)에는 숨기지 않는다
    const isEditing = () => {
      if (!document.hasFocus()) return false;
      const a = document.activeElement;
      return !!a && (a.isContentEditable || a.tagName === 'TEXTAREA' || a.tagName === 'INPUT');
    };
    // 클릭해야 보이는 모드에서는 마우스를 올려둔 것만으로 UI를 붙잡아 두지 않는다
    const canCollapse = () =>
      (clickOnly || !root.matches(':hover')) && !isEditing() && !popoverOpen();

    // 헤더·툴바 높이를 네이티브에 알린다. 자석 정렬이 "UI가 숨겨졌을 때"의 창 모양을
    // 기준으로 삼기 위해 필요하며, 접힘 여부와 무관하게 항상 측정된다(페이드만 하므로).
    function reportUiExtents() {
      const els = parts();
      bridge.call('window.setUiExtents', {
        top: header.offsetHeight,
        bottom: els.includes(toolbar) ? toolbar.offsetHeight : 0,
      }).catch(() => {});
    }
    const unfade = () =>
      document.querySelectorAll('.autofade').forEach((el) => el.classList.remove('autofade'));

    async function collapse() {
      if (!enabled || state !== 'expanded' || !canCollapse()) return;
      const els = parts();
      state = 'fading';
      els.forEach((el) => el.classList.add('autofade'));
      await new Promise((r) => setTimeout(r, FADE_MS + 40));
      if (state !== 'fading') return;  // 도중에 expand()가 취소함
      if (!enabled || !canCollapse()) {
        state = 'expanded';
        unfade();
        return;
      }
      state = 'collapsed';
      // 창 축소량 = 요소가 레이아웃에서 차지하는 높이 (레이아웃은 그대로, 창만 줄어
      // 그 영역이 잘린다)
      const top = header.offsetHeight;
      const bottom = els.includes(toolbar) ? toolbar.offsetHeight : 0;
      reportUiExtents();
      bridge.call('window.setCollapse', { top, bottom }).catch(() => {});
    }

    function expand(withFade) {
      clearTimeout(timer);
      if (state === 'fading') {
        state = 'expanded';
        unfade();
        return;
      }
      if (state !== 'collapsed') return;
      state = 'expanded';
      bridge.call('window.setCollapse', { top: 0, bottom: 0 }).catch(() => {});
      if (withFade === false) unfade();
      else setTimeout(unfade, RESIZE_MS + 30);  // 창이 다 커진 뒤 페이드 인
    }

    const schedule = () => {
      if (!enabled) return;
      clearTimeout(timer);
      timer = setTimeout(collapse, HIDE_DELAY_MS);
    };
    // 호버로 보이기 (클릭 전용 모드가 아닐 때만)
    root.addEventListener('mouseenter', () => { if (!clickOnly) expand(true); });
    root.addEventListener('mouseleave', schedule);
    // 클릭으로 보이기 — 창 안 어디를 눌러도 UI가 올라온다
    document.addEventListener('mousedown', () => expand(true), true);
    // 입력을 마치고 포커스가 떠나거나 창이 비활성화되면 (마우스도 밖이면) 숨김 예약
    document.addEventListener('focusout', schedule);
    window.addEventListener('blur', schedule);
    // 키보드 포커스로 입력이 재개되면 (Alt+Tab 복귀 등) 접힌 UI를 되살린다
    window.addEventListener('focus', () => { if (isEditing()) expand(true); });

    bridge.on('ui.autoHideChanged', (d) => {
      enabled = !!d.on;
      if (!enabled) expand(false);
      else if (canCollapse()) collapse();
    });

    // 표시 조건(호버 vs 클릭) 변경 — 클릭 전용으로 바뀌면 호버로 떠 있던 UI를 정리한다
    bridge.on('ui.revealModeChanged', (d) => {
      clickOnly = !!d.clickOnly;
      if (enabled && clickOnly && canCollapse()) collapse();
    });

    // 최초 로드: 레이아웃이 잡힌 뒤 높이를 보고하고, 잠시 뒤 마우스가 창 위에 없고
    // 입력 중도 아니면 접는다
    requestAnimationFrame(() => requestAnimationFrame(reportUiExtents));
    setTimeout(() => { if (enabled && canCollapse()) collapse(); }, INITIAL_DELAY_MS);
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
