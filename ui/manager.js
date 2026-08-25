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
    if (name === 'list') refreshList();
    if (name === 'groups') refreshGroups();
    if (name === 'trash') refreshTrash();
    if (name === 'settings') {
      refreshTrashCount();
      runConnectTest();  // Ollama 상태 자동 확인 (미설치면 설치 버튼 노출)
    }
  }
  document.querySelectorAll('.tab').forEach((t) =>
    t.addEventListener('click', () => showTab(t.dataset.tab)));
  bridge.on('manager.showTab', (d) => showTab(d.tab));

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
        if (s.hidden) {
          const badge = document.createElement('span');
          badge.className = 'badge';
          badge.textContent = i18n.t('manager.hiddenBadge');
          meta.appendChild(badge);
        }
        const spacer = document.createElement('span');
        spacer.className = 'spacer';
        meta.appendChild(spacer);

        const actions = document.createElement('span');
        actions.className = 'actions';
        const openBtn = document.createElement('button');
        openBtn.textContent = i18n.t('manager.open');
        openBtn.onclick = () => bridge.call('stickers.show', { id: s.id });
        const delBtn = document.createElement('button');
        delBtn.className = 'del';
        delBtn.textContent = i18n.t('manager.delete');
        delBtn.onclick = async () => {
          const r = await bridge.call('stickers.delete', { id: s.id });
          if (r && r.deleted) setTimeout(refreshList, 200);
        };
        actions.appendChild(openBtn);
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
        if (g.hidden) {
          const badge = document.createElement('span');
          badge.className = 'badge';
          badge.textContent = i18n.t('manager.hiddenBadge');
          meta.appendChild(badge);
        }
        const spacer = document.createElement('span');
        spacer.className = 'spacer';
        meta.appendChild(spacer);

        const actions = document.createElement('span');
        actions.className = 'actions';
        const openBtn = document.createElement('button');
        openBtn.textContent = i18n.t('manager.open');
        openBtn.onclick = () => bridge.call('groups.show', { id: g.id });
        const delBtn = document.createElement('button');
        delBtn.className = 'del';
        delBtn.textContent = i18n.t('manager.delete');
        delBtn.onclick = async () => {
          // 확인 팝업은 네이티브에서 수행 (그룹은 해체되고 메모는 개별 스티커로 분리)
          const r = await bridge.call('groups.delete', { id: g.id });
          if (r && r.deleted) setTimeout(refreshGroups, 300);
        };
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
        const restoreBtn = document.createElement('button');
        restoreBtn.textContent = i18n.t('manager.restore');
        restoreBtn.onclick = async () => {
          await bridge.call('trash.restore', { id: s.id });
          setTimeout(refreshTrash, 200);
        };
        const purgeBtn = document.createElement('button');
        purgeBtn.className = 'del';
        purgeBtn.textContent = i18n.t('manager.purge');
        purgeBtn.onclick = async () => {
          // 최종 확인은 네이티브 팝업에서 수행
          const r = await bridge.call('trash.purge', { id: s.id });
          if (r && r.purged) refreshTrash();
        };
        actions.appendChild(restoreBtn);
        actions.appendChild(purgeBtn);
        meta.appendChild(actions);
        card.appendChild(meta);
        cards.appendChild(card);
      });
  }

  // ---------- 설정 ----------
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
    $('#endpointInput').value = s.ollama.endpoint;
    setModelOptions(s.ollama.model ? [s.ollama.model] : [], s.ollama.model);

    const t = s.trash;
    $('#trashEnabledCheck').checked = !!t.enabled;
    const noAuto = t.retentionDays === 0;
    $('#trashNoAutoCheck').checked = noAuto;
    $('#trashDaysInput').value = noAuto ? 30 : t.retentionDays;
    $('#trashDaysInput').disabled = noAuto || !t.enabled;
    $('#trashNoAutoCheck').disabled = !t.enabled;
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

  // ---------- 휴지통 설정 ----------
  $('#trashEnabledCheck').addEventListener('change', (e) => {
    state.settings.trash.enabled = e.target.checked;
    applySettingsUi();
    bridge.call('settings.set', { trash: { enabled: e.target.checked } });
  });

  $('#trashDaysInput').addEventListener('change', (e) => {
    let v = Math.round(Number(e.target.value));
    if (!Number.isFinite(v) || v < 1) v = 1;
    if (v > 36500) v = 36500;
    e.target.value = v;
    state.settings.trash.retentionDays = v;
    bridge.call('settings.set', { trash: { retentionDays: v } });
  });

  $('#trashNoAutoCheck').addEventListener('change', (e) => {
    const days = e.target.checked ? 0 : (Math.round(Number($('#trashDaysInput').value)) || 30);
    state.settings.trash.retentionDays = days;
    applySettingsUi();
    bridge.call('settings.set', { trash: { retentionDays: days } });
    refreshTrashCount();  // 기간 단축으로 즉시 정리됐을 수 있음
  });

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
    status.className = 'status';
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
      $('#installRow').classList.add('hidden');
      if (d.models.length === 0) {
        status.className = 'status err';
        status.textContent = i18n.t('settings.noModels');
      } else {
        status.className = 'status ok';
        status.textContent = `${i18n.t('settings.connected')} (${d.models.length})`;
      }
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
