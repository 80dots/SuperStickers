// Manager(스티커 목록 + 설정) 페이지 메인
(async () => {
  const init = window.__init || { theme: 'light', lang: 'en' };
  const $ = (sel) => document.querySelector(sel);

  await i18n.load(init.lang);
  i18n.apply();

  let state = null;  // app.getState 결과
  try {
    state = await bridge.call('app.getState');
  } catch (e) {
    console.error(e);
    state = { settings: { theme: 'system', language: init.lang, autostart: false,
                          ollama: { endpoint: 'http://localhost:11434', model: '' },
                          trash: { enabled: true, retentionDays: 30 } } };
  }
  if (!state.settings.trash) state.settings.trash = { enabled: true, retentionDays: 30 };

  // ---------- 탭 ----------
  function showTab(name) {
    document.querySelectorAll('.tab').forEach((t) =>
      t.classList.toggle('on', t.dataset.tab === name));
    $('#listTab').classList.toggle('hidden', name !== 'list');
    $('#groupsTab').classList.toggle('hidden', name !== 'groups');
    $('#trashTab').classList.toggle('hidden', name !== 'trash');
    $('#settingsTab').classList.toggle('hidden', name !== 'settings');
    $('#dataTab').classList.toggle('hidden', name !== 'data');
    if (name === 'list') refreshList();
    if (name === 'groups') refreshGroups();
    if (name === 'trash') refreshTrash();
    if (name === 'data') refreshDataPath();
    if (name === 'settings') {
      refreshTrashCount();
      runConnectTest();  // Ollama 상태 자동 확인 (미설치면 설치 버튼 노출)
    }
  }
  document.querySelectorAll('.tab').forEach((t) =>
    t.addEventListener('click', () => showTab(t.dataset.tab)));
  bridge.on('manager.showTab', (d) => showTab(d.tab));

  // 메모창 밖(설정·목록 창)을 Shift 없이 클릭하면 메모창 다중 선택을 해제한다
  document.addEventListener('mousedown', (e) => {
    if (!e.shiftKey) bridge.call('selection.clear').catch(() => {});
  }, true);

  // ---------- 스티커 목록 ----------
  function stripHtml(html) {
    const div = document.createElement('div');
    div.innerHTML = html;
    return div.innerText.trim();
  }

  function fmtDate(iso) {
    if (!iso) return '';
    const d = new Date(iso);
    if (isNaN(d)) return '';
    return d.toLocaleString(i18n.lang === 'ko' ? 'ko-KR' : 'en-US', {
      month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
    });
  }

  // 카드 액션용 아이콘 버튼 (목록·그룹·휴지통 탭 공용)
  const ICONS = {
    open: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="none" stroke="currentColor" stroke-width="1.4" d="M13 9v4H3V3h4"/><path fill="currentColor" d="M9 2h5v5l-1.9-1.9-4 4L6.9 8l4-4z"/></svg>',
    del: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M6 2h4l.5 1H14v1.5H2V3h3.5zM3 6h10l-.8 8.2c-.1.5-.5.8-1 .8H4.8c-.5 0-.9-.3-1-.8z"/></svg>',
    export: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="none" stroke="currentColor" stroke-width="1.4" d="M2.6 10.2v2.6c0 .6.5 1.1 1.1 1.1h8.6c.6 0 1.1-.5 1.1-1.1v-2.6"/><path fill="currentColor" d="M8 1.6 11.4 5H9.1v5.1H6.9V5H4.6z"/></svg>',
    restore: '<svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" d="M8 3a5.5 5.5 0 1 1-5.2 7.3l1.5-.5A4 4 0 1 0 8 4.5v2L4.5 3.8 8 1z"/></svg>',
  };
  // 표시/숨김 상태 아이콘 (뜬 눈 / 감은 눈)
  function visibilityIcon(hidden) {
    const el = document.createElement('span');
    el.className = 'eye' + (hidden ? ' off' : '');
    el.title = i18n.t(hidden ? 'manager.hiddenBadge' : 'manager.visibleBadge');
    el.innerHTML = hidden
      ? '<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"><path d="M2 6.4c1.6 2.1 3.6 3.2 6 3.2s4.4-1.1 6-3.2"/><path d="M3.2 8.6 2 10.4M6.2 9.9 5.6 12M9.8 9.9l.6 2.1M12.8 8.6 14 10.4"/></g></svg>'
      : '<svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.3"><path d="M1.6 8S4 4.2 8 4.2 14.4 8 14.4 8 12 11.8 8 11.8 1.6 8 1.6 8z"/><circle cx="8" cy="8" r="1.9"/></g></svg>';
    return el;
  }

  function iconBtn(icon, titleKey, onClick, danger) {
    const b = document.createElement('button');
    b.className = 'icon-btn' + (danger ? ' del' : '');
    b.innerHTML = ICONS[icon];
    b.title = i18n.t(titleKey);
    b.onclick = onClick;
    return b;
  }

  // 스티커 데이터 → 카드 미리보기 요소 (목록·휴지통 탭 공용)
  function buildCardBase(s) {
    const card = document.createElement('div');
    card.className = 'card';
    const dark = document.documentElement.dataset.theme === 'dark';
    const bg = colorUtil.effectiveBg(s.color, dark);
    card.style.background = bg;
    card.style.color = colorUtil.textColorFor(bg);

    const preview = document.createElement('div');
    preview.className = 'preview';
    const t = s.type || (s.mode === 'markdown' ? 'markdown' : 'rich');
    let text = '';
    if (t === 'markdown') text = s.markdown || '';
    else if (t === 'file')
      text = '📁 ' + (s.files || []).map((p) => p.split('\\').pop()).join(', ');
    else if (t === 'web') text = '🌐 ' + (s.lastUrl || s.url || '');
    else if (t === 'pdf') text = '📄 ' + (s.pdfTitle || '');
    else text = stripHtml(s.html);
    preview.textContent = text.trim().replace(/^[📁🌐📄] ?$/u, '').slice(0, 300) ||
                          i18n.t('manager.noText');
    card.appendChild(preview);
    return card;
  }

  async function refreshList() {
    let stickers = [];
    try {
      stickers = await bridge.call('stickers.list');
    } catch (e) {
      console.error(e);
    }
    const cards = $('#cards');
    cards.innerHTML = '';
    $('#listEmpty').classList.toggle('hidden', stickers.length > 0);

    stickers
      .sort((a, b) => (b.updatedAt || '').localeCompare(a.updatedAt || ''))
      .forEach((s) => {
        const card = buildCardBase(s);

        const meta = document.createElement('div');
        meta.className = 'meta';
        const date = document.createElement('span');
        date.textContent = fmtDate(s.updatedAt);
        meta.appendChild(date);
        meta.appendChild(visibilityIcon(!!s.hidden));
        const spacer = document.createElement('span');
        spacer.className = 'spacer';
        meta.appendChild(spacer);

        const actions = document.createElement('span');
        actions.className = 'actions';
        const openBtn = iconBtn('open', 'manager.open',
          () => bridge.call('stickers.show', { id: s.id }));
        const exportBtn = iconBtn('export', 'manager.export',
          () => bridge.call('sticker.export', { id: s.id }).catch(console.error));
        const delBtn = iconBtn('del', 'manager.delete', async () => {
          const r = await bridge.call('stickers.delete', { id: s.id });
          if (r && r.deleted) setTimeout(refreshList, 200);
        }, true);
        actions.appendChild(openBtn);
        actions.appendChild(exportBtn);
        actions.appendChild(delBtn);
        meta.appendChild(actions);
        card.appendChild(meta);
        cards.appendChild(card);
      });
  }

  $('#newStickerBtn').addEventListener('click', () => {
    bridge.call('stickers.new');
    setTimeout(refreshList, 300);
  });

  // ---------- .ssticker 내보내기 / 가져오기 ----------
  let hintTimer = 0;
  function showListStatus(msg, ok) {
    const el = $('#importHint');
    el.textContent = msg;
    el.className = 'hint status ' + (ok ? 'ok' : 'err');
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => {
      el.className = 'hint';
      el.textContent = i18n.t('manager.importHint');
    }, 5000);
  }

  bridge.on('sticker.exportDone', (d) => {
    showListStatus(d.ok ? i18n.t('manager.exportDone') : i18n.t('manager.exportFailed'), d.ok);
  });

  $('#importStickerBtn').addEventListener('click', async () => {
    try {
      const r = await bridge.call('stickers.import');
      if (r && r.count) {
        setTimeout(refreshList, 300);
        showListStatus(i18n.t('manager.importDone').replace('{n}', r.count), true);
      }
    } catch (e) {
      console.error(e);
    }
  });

  bridge.on('stickers.changed', () => {
    if (!$('#listTab').classList.contains('hidden')) refreshList();
  });

  // .ssticker 드래그앤드롭 — 파일 경로는 네이티브가 File 객체에서 뽑아
  // stickers.importPaths의 params.paths로 합쳐 받는다 (bridge.callWithFiles)
  const listTab = $('#listTab');
  let dragDepth = 0;  // 자식 요소 진입/이탈로 dragleave가 여러 번 오는 것 보정
  const isSticker = (f) => /\.ssticker$/i.test(f.name || '');
  listTab.addEventListener('dragenter', (e) => {
    if (![...(e.dataTransfer?.items || [])].some((it) => it.kind === 'file')) return;
    e.preventDefault();
    dragDepth += 1;
    listTab.classList.add('dropping');
  });
  listTab.addEventListener('dragover', (e) => {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  });
  listTab.addEventListener('dragleave', () => {
    dragDepth = Math.max(0, dragDepth - 1);
    if (!dragDepth) listTab.classList.remove('dropping');
  });
  listTab.addEventListener('drop', (e) => {
    e.preventDefault();
    dragDepth = 0;
    listTab.classList.remove('dropping');
    const files = [...(e.dataTransfer?.files || [])].filter(isSticker);
    if (!files.length) return;
    bridge.callWithFiles('stickers.importPaths', {}, files)
      .then((r) => {
        if (r && r.count) {
          setTimeout(refreshList, 300);
          showListStatus(i18n.t('manager.importDone').replace('{n}', r.count), true);
        } else {
          showListStatus(i18n.t('manager.importFailed'), false);
        }
      })
      .catch(console.error);
  });
  window.addEventListener('focus', () => {
    if (!$('#listTab').classList.contains('hidden')) refreshList();
    if (!$('#groupsTab').classList.contains('hidden')) refreshGroups();
    if (!$('#trashTab').classList.contains('hidden')) refreshTrash();
  });

  // ---------- 그룹 목록 탭 ----------
  async function refreshGroups() {
    let groups = [];
    try {
      groups = await bridge.call('groups.list');
    } catch (e) {
      console.error(e);
    }
    const cards = $('#groupCards');
    cards.innerHTML = '';
    $('#groupsEmpty').classList.toggle('hidden', groups.length > 0);

    groups
      .sort((a, b) => (b.updatedAt || '').localeCompare(a.updatedAt || ''))
      .forEach((g) => {
        const card = document.createElement('div');
        card.className = 'card';
        const dark = document.documentElement.dataset.theme === 'dark';
        if (g.color) {
          const bg = colorUtil.effectiveBg(g.color, dark);
          card.style.background = bg;
          card.style.color = colorUtil.textColorFor(bg);
        }

        const preview = document.createElement('div');
        preview.className = 'preview';
        const title = (g.title || '').trim() || i18n.t('group.titlePlaceholder');
        preview.textContent = title;
        card.appendChild(preview);

        const meta = document.createElement('div');
        meta.className = 'meta';
        const count = document.createElement('span');
        count.textContent =
          i18n.t('manager.memberCount').replace('{n}', (g.memberIds || []).length);
        meta.appendChild(count);
        meta.appendChild(visibilityIcon(!!g.hidden));
        const spacer = document.createElement('span');
        spacer.className = 'spacer';
        meta.appendChild(spacer);

        const actions = document.createElement('span');
        actions.className = 'actions';
        const openBtn = iconBtn('open', 'manager.open',
          () => bridge.call('groups.show', { id: g.id }));
        const delBtn = iconBtn('del', 'manager.delete', async () => {
          // 확인 팝업은 네이티브에서 수행 (그룹은 해체되고 메모는 개별 스티커로 분리)
          const r = await bridge.call('groups.delete', { id: g.id });
          if (r && r.deleted) setTimeout(refreshGroups, 300);
        }, true);
        actions.appendChild(openBtn);
        actions.appendChild(delBtn);
        meta.appendChild(actions);
        card.appendChild(meta);
        cards.appendChild(card);
      });
  }

  // ---------- 휴지통 탭 ----------
  async function refreshTrash() {
    let items = [];
    try {
      items = (await bridge.call('trash.list')).items;
    } catch (e) {
      console.error(e);
    }
    const cards = $('#trashCards');
    cards.innerHTML = '';
    $('#trashEmptyMsg').classList.toggle('hidden', items.length > 0);

    items
      .sort((a, b) => (b.deletedAt || '').localeCompare(a.deletedAt || ''))
      .forEach((s) => {
        const card = buildCardBase(s);

        const meta = document.createElement('div');
        meta.className = 'meta';
        const date = document.createElement('span');
        date.textContent = i18n.t('manager.deletedPrefix') + ' ' + fmtDate(s.deletedAt);
        meta.appendChild(date);
        const spacer = document.createElement('span');
        spacer.className = 'spacer';
        meta.appendChild(spacer);

        const actions = document.createElement('span');
        actions.className = 'actions';
        const restoreBtn = iconBtn('restore', 'manager.restore', async () => {
          await bridge.call('trash.restore', { id: s.id });
          setTimeout(refreshTrash, 200);
        });
        const purgeBtn = iconBtn('del', 'manager.purge', async () => {
          // 최종 확인은 네이티브 팝업에서 수행
          const r = await bridge.call('trash.purge', { id: s.id });
          if (r && r.purged) refreshTrash();
        }, true);
        actions.appendChild(restoreBtn);
        actions.appendChild(purgeBtn);
        meta.appendChild(actions);
        card.appendChild(meta);
        cards.appendChild(card);
      });
  }

  // ---------- 설정 ----------
  // 보관 기간 버튼 라벨 (일수는 로케일 문구에 대입)
  function buildTrashDaysLabels() {
    document.querySelectorAll('#trashDaysSeg button').forEach((b) => {
      const d = Number(b.dataset.days);
      if (d > 0) b.textContent = i18n.t('settings.trashDays').replace('{n}', d);
    });
  }

  function applySettingsUi() {
    const s = state.settings;
    document.querySelectorAll('#themeSeg button').forEach((b) =>
      b.classList.toggle('on', b.dataset.value === s.theme));
    $('#langSelect').value = s.language;
    // 저장값과 가장 가까운 배율 옵션을 선택
    const scale = s.uiScale || 1;
    const opts = [...$('#uiScaleSelect').options].map((o) => Number(o.value));
    $('#uiScaleSelect').value = String(
      opts.reduce((a, b) => (Math.abs(b - scale) < Math.abs(a - scale) ? b : a)));
    $('#autostartCheck').checked = !!s.autostart;
    $('#autoHideCheck').checked = s.autoHideUi !== false;  // 기본값 On
    // 클릭할 때만 보이기: 자동 숨김이 켜져 있을 때만 조작할 수 있다
    $('#revealClickCheck').checked = s.uiRevealOnClick !== false;  // 기본값 On
    $('#revealClickCheck').disabled = !$('#autoHideCheck').checked;
    $('#revealClickRow').classList.toggle('disabled', !$('#autoHideCheck').checked);
    const mg = s.magnet || {};
    $('#magnetCheck').checked = mg.enabled !== false;      // 기본값 On
    $('#magnetGapSelect').value = String(mg.gap == null ? 10 : mg.gap);
    // 자석이 꺼져 있으면 민감도·간격 설정은 조작할 수 없다
    const magnetOn = $('#magnetCheck').checked;
    const sens = mg.sensitivity || 'medium';
    document.querySelectorAll('#magnetSensSeg button').forEach((b) => {
      b.classList.toggle('on', b.dataset.value === sens);
      b.disabled = !magnetOn;
    });
    $('#magnetSensSeg').classList.toggle('disabled', !magnetOn);
    $('#magnetSensRow').classList.toggle('disabled', !magnetOn);
    $('#magnetGapSelect').disabled = !magnetOn;
    $('#magnetGapRow').classList.toggle('disabled', !magnetOn);
    $('#endpointInput').value = s.ollama.endpoint;
    setModelOptions(s.ollama.model ? [s.ollama.model] : [], s.ollama.model);

    const t = s.trash;
    $('#trashEnabledCheck').checked = !!t.enabled;
    buildTrashDaysLabels();
    // 보관 기간: 테마 세그먼트와 동일한 UI. 저장값과 가장 가까운 옵션을 선택 표시
    const btns = [...document.querySelectorAll('#trashDaysSeg button')];
    const days = Number(t.retentionDays) || 0;
    const dayOpts = btns.map((b) => Number(b.dataset.days));
    const sel = dayOpts.includes(days)
      ? days
      : dayOpts.filter((v) => v > 0)
               .reduce((a, b) => (Math.abs(b - days) < Math.abs(a - days) ? b : a));
    btns.forEach((b) => {
      b.classList.toggle('on', Number(b.dataset.days) === sel);
      b.disabled = !t.enabled;
    });
    $('#trashDaysSeg').classList.toggle('disabled', !t.enabled);
  }

  async function refreshTrashCount() {
    try {
      const r = await bridge.call('trash.count');
      $('#trashCount').textContent = i18n.t('settings.trashCount').replace('{n}', r.count);
    } catch (e) { console.error(e); }
  }

  function setModelOptions(models, selected) {
    const sel = $('#modelSelect');
    sel.innerHTML = '';
    const ph = document.createElement('option');
    ph.value = '';
    ph.textContent = i18n.t('settings.selectModel');
    sel.appendChild(ph);
    models.forEach((m) => {
      const o = document.createElement('option');
      o.value = m;
      o.textContent = m;
      sel.appendChild(o);
    });
    sel.value = selected || '';
  }

  document.querySelectorAll('#themeSeg button').forEach((b) =>
    b.addEventListener('click', async () => {
      state.settings.theme = b.dataset.value;
      applySettingsUi();
      const r = await bridge.call('settings.set', { theme: b.dataset.value });
      document.documentElement.dataset.theme = r.effectiveTheme;
    }));

  $('#langSelect').addEventListener('change', (e) => {
    state.settings.language = e.target.value;
    bridge.call('settings.set', { language: e.target.value });
  });

  $('#uiScaleSelect').addEventListener('change', (e) => {
    state.settings.uiScale = Number(e.target.value);
    bridge.call('settings.set', { uiScale: Number(e.target.value) });
  });

  $('#autostartCheck').addEventListener('change', (e) => {
    state.settings.autostart = e.target.checked;
    bridge.call('settings.set', { autostart: e.target.checked });
  });

  // 자동 숨김: 저장 + 모든 메모창에 즉시 반영 (네이티브가 ui.autoHideChanged 방송)
  $('#autoHideCheck').addEventListener('change', (e) => {
    state.settings.autoHideUi = e.target.checked;
    applySettingsUi();  // 하위 옵션 활성/비활성 갱신
    bridge.call('settings.set', { autoHideUi: e.target.checked }).catch(console.error);
  });

  $('#revealClickCheck').addEventListener('change', (e) => {
    state.settings.uiRevealOnClick = e.target.checked;
    bridge.call('settings.set', { uiRevealOnClick: e.target.checked }).catch(console.error);
  });

  // 자석 정렬: 켜져 있을 때만 간격 설정을 쓸 수 있다
  $('#magnetCheck').addEventListener('change', (e) => {
    if (!state.settings.magnet) state.settings.magnet = {};
    state.settings.magnet.enabled = e.target.checked;
    applySettingsUi();
    bridge.call('settings.set', { magnet: { enabled: e.target.checked } }).catch(console.error);
  });

  document.querySelectorAll('#magnetSensSeg button').forEach((b) =>
    b.addEventListener('click', () => {
      if (!state.settings.magnet) state.settings.magnet = {};
      state.settings.magnet.sensitivity = b.dataset.value;
      applySettingsUi();
      bridge.call('settings.set', { magnet: { sensitivity: b.dataset.value } })
        .catch(console.error);
    }));

  $('#magnetGapSelect').addEventListener('change', (e) => {
    const gap = Number(e.target.value);
    if (!state.settings.magnet) state.settings.magnet = {};
    state.settings.magnet.gap = gap;
    bridge.call('settings.set', { magnet: { gap } }).catch(console.error);
  });

  // ---------- 휴지통 설정 ----------
  $('#trashEnabledCheck').addEventListener('change', (e) => {
    state.settings.trash.enabled = e.target.checked;
    applySettingsUi();
    bridge.call('settings.set', { trash: { enabled: e.target.checked } });
  });

  document.querySelectorAll('#trashDaysSeg button').forEach((b) =>
    b.addEventListener('click', () => {
      const days = Number(b.dataset.days);
      state.settings.trash.retentionDays = days;
      applySettingsUi();
      bridge.call('settings.set', { trash: { retentionDays: days } });
      refreshTrashCount();  // 기간 단축으로 즉시 정리됐을 수 있음
    }));

  $('#emptyTrashBtn').addEventListener('click', async () => {
    // 최종 확인은 네이티브 팝업에서 수행
    try {
      const r = await bridge.call('trash.empty');
      $('#trashCount').textContent = i18n.t('settings.trashCount').replace('{n}', r.count);
    } catch (e) { console.error(e); }
  });

  bridge.on('trash.changed', (d) => {
    $('#trashCount').textContent = i18n.t('settings.trashCount').replace('{n}', d.count);
    if (!$('#trashTab').classList.contains('hidden')) refreshTrash();
  });

  $('#endpointInput').addEventListener('change', (e) => {
    state.settings.ollama.endpoint = e.target.value.trim();
    bridge.call('settings.set', { ollama: { endpoint: e.target.value.trim() } });
  });

  $('#modelSelect').addEventListener('change', (e) => {
    state.settings.ollama.model = e.target.value;
    bridge.call('settings.set', { ollama: { model: e.target.value } });
  });

  // 연결 테스트 → 모델 목록 로드
  let testRequestId = null;
  function runConnectTest() {
    const status = $('#ollamaStatus');
    status.className = 'status busy';  // 결과 도착 시 ok/err로 교체되며 스피너가 사라진다
    status.textContent = i18n.t('settings.testing');
    testRequestId = 'test-' + Date.now();
    bridge.call('ollama.listModels', {
      requestId: testRequestId,
      endpoint: $('#endpointInput').value.trim(),
    });
  }
  $('#testBtn').addEventListener('click', runConnectTest);

  bridge.on('ollama.models', (d) => {
    if (d.requestId !== testRequestId) return;
    const status = $('#ollamaStatus');
    if (d.ok) {
      // 결과 표시를 먼저 — 뒤따르는 DOM 작업이 실패해도 "확인 중…"에 멈추지 않는다
      if (d.models.length === 0) {
        status.className = 'status err';
        status.textContent = i18n.t('settings.noModels');
      } else {
        status.className = 'status ok';
        status.textContent = `${i18n.t('settings.connected')} (${d.models.length})`;
      }
      // 연결 성공 시 다운로드 섹션은 건드리지 않는다 (기본 접힘 상태이고,
      // 사용자가 모델 내려받는 중일 수 있어 임의로 닫으면 안 됨)
      const cur = state.settings.ollama.model;
      setModelOptions(d.models, d.models.includes(cur) ? cur : '');
    } else {
      status.className = 'status err';
      status.textContent = `${i18n.t('settings.connectFailed')}: ${d.error}`;
      openDlPanel();  // 연결 실패 → Ollama 미설치 가능성: 다운로드 섹션 자동 펼침
    }
  });

  // ---------- 접이식 다운로드 섹션 ----------
  // 다운로드 가능한 모델: 엄선된 고정 목록만 제공
  const PULL_MODELS = [
    'llama3:8b',
    'gemma4:12b',
    'gemma3:4b',
    'gemma3:12b',
    'qwen3.5:4b',
    'qwen3.5:9b',
    'gpt-oss:20b',
  ];
  function buildPullCombo() {
    const sel = $('#pullModelSelect');
    sel.innerHTML = '';
    const ph = document.createElement('option');
    ph.value = '';
    ph.textContent = i18n.t('settings.pullSelectModel');
    sel.appendChild(ph);
    PULL_MODELS.forEach((m) => {
      const o = document.createElement('option');
      o.value = m;
      o.textContent = m;
      sel.appendChild(o);
    });
  }
  buildPullCombo();

  function openDlPanel() {
    if (!$('#dlPanel').classList.contains('hidden')) return;
    $('#dlPanel').classList.remove('hidden');
    $('#dlArrow').classList.add('open');
  }
  $('#dlToggle').addEventListener('click', () => {
    if ($('#dlPanel').classList.contains('hidden')) openDlPanel();
    else {
      $('#dlPanel').classList.add('hidden');
      $('#dlArrow').classList.remove('open');
    }
  });

  function fmtBytes(n) {
    if (n >= 1e9) return (n / 1e9).toFixed(1) + ' GB';
    if (n >= 1e6) return (n / 1e6).toFixed(0) + ' MB';
    return Math.round(n / 1e3) + ' KB';
  }

  // ---------- Ollama 다운로드/설치 (진행률 표시) ----------
  $('#installOllamaBtn').addEventListener('click', () => {
    $('#installOllamaBtn').disabled = true;
    $('#downloadPageBtn').classList.add('hidden');
    $('#installStatus').className = 'status';
    $('#installStatus').textContent = i18n.t('settings.installChecking');
    bridge.call('ollama.installOllama').catch((e) => {
      $('#installOllamaBtn').disabled = false;
      $('#installStatus').className = 'status err';
      $('#installStatus').textContent = `${i18n.t('ai.error')}: ${e.message}`;
    });
  });

  bridge.on('ollama.installProgress', (d) => {
    const row = $('#installProgressRow');
    const bar = $('#installProgress');
    row.classList.remove('hidden');
    if (d.stage === 'download' && d.total > 0) {
      const pct = Math.floor((d.received / d.total) * 100);
      bar.classList.remove('indeterminate');
      bar.value = pct;
      $('#installPct').textContent =
        `${pct}% (${fmtBytes(d.received)} / ${fmtBytes(d.total)})`;
      $('#installStatus').textContent = i18n.t('settings.installDownloading');
    } else {
      bar.removeAttribute('value');  // indeterminate
      bar.classList.add('indeterminate');
      $('#installPct').textContent = '';
      $('#installStatus').textContent =
        d.stage === 'install' ? i18n.t('settings.installRunning')
                              : i18n.t('settings.installStarting');
    }
  });

  bridge.on('ollama.installDone', (d) => {
    $('#installOllamaBtn').disabled = false;
    $('#installProgressRow').classList.add('hidden');
    if (d.ok) {
      $('#installStatus').className = 'status ok';
      if (d.already) {
        $('#installStatus').textContent = i18n.t('settings.installAlready');
      } else if (d.exposeSet) {
        $('#installStatus').textContent = i18n.t('settings.installOkExposed');
      } else {
        $('#installStatus').textContent = i18n.t('settings.installOkExposeManual');
      }
      setTimeout(runConnectTest, 1500);
    } else {
      $('#installStatus').className = 'status err';
      $('#installStatus').textContent = i18n.t('settings.installFail');
      $('#downloadPageBtn').classList.remove('hidden');
    }
  });

  $('#downloadPageBtn').addEventListener('click', () => {
    bridge.call('app.openExternal', { url: 'https://ollama.com/download' });
  });

  // ---------- 모델 다운로드 (ollama pull) ----------
  let pullRequestId = null;
  function pullModelName() {
    return $('#pullModelSelect').value;
  }
  $('#pullBtn').addEventListener('click', () => {
    if (pullRequestId) {  // 진행 중 → 취소
      bridge.call('ollama.abort', { requestId: pullRequestId });
      return;
    }
    const name = pullModelName();
    if (!name) return;
    pullRequestId = 'pull-' + Date.now();
    $('#pullBtn').textContent = i18n.t('settings.pullCancel');
    $('#pullStatus').className = 'status';
    $('#pullStatus').textContent = i18n.t('settings.pulling') + '…';
    $('#pullProgressRow').classList.remove('hidden');
    $('#pullProgress').removeAttribute('value');
    $('#pullProgress').classList.add('indeterminate');
    bridge.call('ollama.pull', { requestId: pullRequestId, name }).catch((e) => {
      pullRequestId = null;
      $('#pullBtn').textContent = i18n.t('settings.pull');
      $('#pullProgressRow').classList.add('hidden');
      $('#pullStatus').className = 'status err';
      $('#pullStatus').textContent = `${i18n.t('ai.error')}: ${e.message}`;
    });
  });

  bridge.on('ollama.pullProgress', (d) => {
    if (d.requestId !== pullRequestId) return;
    let text = d.status || i18n.t('settings.pulling');
    const bar = $('#pullProgress');
    if (d.total > 0) {
      const pct = Math.floor((d.completed / d.total) * 100);
      bar.classList.remove('indeterminate');
      bar.value = pct;
      text = `${i18n.t('settings.pulling')} ${pct}% (${fmtBytes(d.completed)} / ${fmtBytes(d.total)})`;
    }
    $('#pullStatus').className = 'status';
    $('#pullStatus').textContent = text;
  });

  bridge.on('ollama.pullDone', (d) => {
    if (d.requestId !== pullRequestId) return;
    const name = pullModelName();
    pullRequestId = null;
    $('#pullBtn').textContent = i18n.t('settings.pull');
    $('#pullProgressRow').classList.add('hidden');
    if (d.ok) {
      $('#pullStatus').className = 'status ok';
      $('#pullStatus').textContent = i18n.t('settings.pullDone');
      // 사용 모델이 아직 없으면 방금 받은 모델을 자동 선택
      if (!state.settings.ollama.model && name) {
        state.settings.ollama.model = name;
        bridge.call('settings.set', { ollama: { model: name } });
      }
      runConnectTest();  // 목록 갱신
    } else {
      $('#pullStatus').className = 'status err';
      $('#pullStatus').textContent =
        d.error === 'aborted' ? i18n.t('ai.aborted')
                              : `${i18n.t('settings.pullFailed')}: ${d.error}`;
    }
  });

  // ---------- 데이터 탭 ----------
  async function refreshDataPath() {
    try {
      const r = await bridge.call('data.getPath');
      $('#dataPathText').textContent = r.path;
    } catch (e) { console.error(e); }
  }

  $('#openDataFolderBtn').addEventListener('click', () => {
    bridge.call('data.openFolder').catch(console.error);
  });

  $('#changeDataDirBtn').addEventListener('click', () => {
    // 폴더 선택·확인·복사·재시작은 모두 네이티브에서 진행
    bridge.call('data.changeLocation').catch(console.error);
  });

  $('#backupBtn').addEventListener('click', async () => {
    try {
      const r = await bridge.call('data.backup');
      if (r && r.started) {
        $('#backupBtn').disabled = true;
        $('#backupStatus').className = 'status';
        $('#backupStatus').textContent = i18n.t('data.backingUp');
      }
    } catch (e) {
      $('#backupStatus').className = 'status err';
      $('#backupStatus').textContent = `${i18n.t('ai.error')}: ${e.message}`;
    }
  });

  $('#deleteAllBtn').addEventListener('click', async () => {
    // 확인 팝업(2단계)은 네이티브에서 수행 — 되돌릴 수 없는 작업
    try {
      const r = await bridge.call('data.deleteAll');
      if (r && r.deleted) {
        refreshList();
        refreshTrashCount();
      }
    } catch (e) { console.error(e); }
  });

  // 전체 삭제 후 열려 있는 목록 갱신
  bridge.on('data.cleared', () => {
    refreshList();
    refreshGroups();
    refreshTrash();
    refreshTrashCount();
  });

  bridge.on('data.backupDone', (d) => {
    $('#backupBtn').disabled = false;
    if (d.ok) {
      $('#backupStatus').className = 'status ok';
      $('#backupStatus').textContent = `${i18n.t('data.backupDone')}: ${d.path}`;
    } else {
      $('#backupStatus').className = 'status err';
      $('#backupStatus').textContent = i18n.t('data.backupFailed');
    }
  });

  // ---------- 네이티브 이벤트 ----------
  bridge.on('theme.changed', (d) => {
    document.documentElement.dataset.theme = d.effective;
  });
  bridge.on('locale.changed', async (d) => {
    await i18n.load(d.lang);
    i18n.apply();
    applySettingsUi();
    buildPullCombo();  // 콤보 안내 문구 언어 갱신
    if (!$('#listTab').classList.contains('hidden')) refreshList();
  });

  // ---------- 초기 표시 ----------
  applySettingsUi();
  const tab = new URLSearchParams(location.search).get('tab') || 'list';
  showTab(tab);
})();
