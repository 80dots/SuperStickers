// AI 설정 마법사 — 메모창에서 AI Review나 AI 버튼을 눌렀는데 Ollama가 없거나, 모델이
// 없거나, 쓸 모델이 정해지지 않았을 때 뜬다. 세 단계를 마치면 원래 누른 기능이 이어서 돈다.
//
// 설정 창(관리자)이 아니라 메모창 안에서 돌기 때문에, 설치·내려받기 진행률 이벤트는
// 네이티브가 ownerId(= 이 메모의 id)를 보고 이 창으로 보내 준다 (App::SendEventToOwner).
//
// bridge.on은 해제 수단이 없다. 그래서 이벤트 구독은 딱 한 번만 하고, 지금 단계가
// 무엇을 듣고 싶은지는 handlers에 갈아 끼운다 — 단계를 오갈 때마다 구독이 쌓이지 않는다.
const aiWizard = (() => {
  const STEPS = 3;
  let root = null;      // 모달 DOM (처음 필요할 때 만든다)
  let ownerId = '';
  let step = 1;
  let models = [];      // 설치된 모델 이름
  let chosen = '';      // 3단계에서 고른 모델
  let busy = null;      // 'install' | 'pull' | null
  let pullId = null;
  let resolveRun = null;             // ensureReady의 Promise resolve
  const handlers = {};               // 지금 단계가 붙여 둔 이벤트 처리기

  const t = (k) => i18n.t(k);
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };
  const btn = (label, cls, fn) => {
    const b = el('button', 'wiz-btn' + (cls ? ' ' + cls : ''), label);
    b.addEventListener('click', fn);
    return b;
  };

  ['ollama.installProgress', 'ollama.installDone',
   'ollama.pullProgress', 'ollama.pullDone'].forEach((ev) => {
    bridge.on(ev, (d) => { if (handlers[ev]) handlers[ev](d); });
  });

  // ---------- 네이티브 조회 ----------

  async function isInstalled() {
    try {
      return !!(await bridge.call('ollama.checkInstalled', {})).installed;
    } catch {
      return false;
    }
  }

  // ai.listModels는 답을 이벤트로 준다 — 요청 id로 짝을 맞추고, 오래 걸리면 포기한다.
  function listModels() {
    return new Promise((resolve) => {
      const id = 'wiz-' + Date.now() + '-' + Math.random().toString(36).slice(2, 8);
      let done = false;
      const finish = (v) => { if (!done) { done = true; resolve(v); } };
      handlers['ai.models'] = (d) => {
        if (d.requestId !== id) return;
        finish({ ok: !!d.ok, models: d.models || [], error: d.error || '' });
      };
      setTimeout(() => finish({ ok: false, models: [], error: 'timeout' }), 15000);
      bridge.call('ai.listModels', { requestId: id, ownerId, provider: 'ollama' })
        .catch((e) => finish({ ok: false, models: [], error: e.message }));
    });
  }
  bridge.on('ai.models', (d) => { if (handlers['ai.models']) handlers['ai.models'](d); });

  // AI를 지금 쓸 수 있는지. 못 쓰면 어느 단계부터 시작할지 알려 준다.
  async function inspect() {
    const st = await bridge.call('app.getState', {});
    const s = st.settings || {};
    // 마법사는 Ollama 전용이다. 다른 백엔드를 쓰는 중이면 끼어들지 않는다.
    if ((s.aiProvider || 'ollama') !== 'ollama') return { ready: true };
    if (!(await isInstalled())) return { ready: false, from: 1, models: [] };
    const r = await listModels();
    if (!r.ok) return { ready: false, from: 1, models: [] };  // 서버가 응답하지 않음
    if (r.models.length === 0) return { ready: false, from: 2, models: [] };
    const cur = (s.ollama && s.ollama.model) || '';
    // 고른 모델이 지워졌을 수도 있다 — 그때도 다시 고르게 한다
    if (!cur || !r.models.includes(cur)) return { ready: false, from: 3, models: r.models };
    return { ready: true };
  }

  // ---------- 모달 뼈대 ----------

  function build() {
    root = el('div', 'wiz-back hidden');
    const box = el('div', 'wiz-box');
    const head = el('div', 'wiz-head');
    head.appendChild(el('div', 'wiz-title', t('wizard.title')));
    const badge = el('div', 'wiz-step');
    const close = btn('✕', 'wiz-close', () => requestClose());
    close.title = t('wizard.close');
    head.appendChild(badge);
    head.appendChild(close);

    const body = el('div', 'wiz-body');
    const foot = el('div', 'wiz-foot');
    const back = btn(t('wizard.prev'), '', () => { if (step > 1) { step--; render(); } });
    const next = btn(t('wizard.next'), 'primary', () => onNext());
    foot.appendChild(back);
    foot.appendChild(next);

    // 닫기 확인은 이 모달 안에서 묻는다 (앱은 페이지 confirm()을 쓰지 않는다)
    const ask = el('div', 'wiz-ask hidden');
    const askText = el('p', 'wiz-ask-text');
    const askRow = el('div', 'wiz-ask-row');
    ask.appendChild(askText);
    ask.appendChild(askRow);

    box.appendChild(head);
    box.appendChild(body);
    box.appendChild(foot);
    box.appendChild(ask);
    root.appendChild(box);
    document.body.appendChild(root);

    // 배경 클릭도 '닫기'로 본다 (바로 닫지 않고 물어본다)
    root.addEventListener('mousedown', (e) => { if (e.target === root) requestClose(); });
    document.addEventListener('keydown', (e) => {
      if (e.key !== 'Escape' || !root || root.classList.contains('hidden')) return;
      e.preventDefault();
      e.stopPropagation();
      requestClose();
    }, true);

    Object.assign(root, { _badge: badge, _body: body, _back: back, _next: next,
                          _foot: foot, _ask: ask, _askText: askText, _askRow: askRow });
  }

  // ---------- 닫기 확인 ----------

  function requestClose() {
    if (!root._ask.classList.contains('hidden')) return;  // 이미 묻는 중
    root._askText.textContent = busy ? t('wizard.confirmCloseBusy') : t('wizard.confirmClose');
    root._askRow.innerHTML = '';
    root._askRow.appendChild(btn(t('wizard.keepGoing'), '', () => {
      root._ask.classList.add('hidden');
      root._foot.classList.remove('hidden');
    }));
    root._askRow.appendChild(btn(t('wizard.stop'), 'danger', () => {
      if (busy === 'pull' && pullId) bridge.call('ai.abort', { requestId: pullId }).catch(() => {});
      if (busy === 'install') bridge.call('ollama.cancelInstall', {}).catch(() => {});
      finish(false);
    }));
    root._ask.classList.remove('hidden');
    root._foot.classList.add('hidden');
  }

  function finish(ok) {
    busy = null;
    pullId = null;
    for (const k of Object.keys(handlers)) delete handlers[k];
    if (root) {
      root.classList.add('hidden');
      root._ask.classList.add('hidden');
      root._foot.classList.remove('hidden');
    }
    const r = resolveRun;
    resolveRun = null;
    if (r) r(ok);
  }

  // ---------- 단계 공통 ----------

  function setNext(label, enabled) {
    root._next.textContent = label;
    root._next.disabled = !enabled;
  }

  function progressBox(parent) {
    const wrap = el('div', 'wiz-prog hidden');
    const bar = el('progress');
    bar.max = 100;
    bar.value = 0;
    const pct = el('span', 'wiz-pct', '');
    wrap.appendChild(bar);
    wrap.appendChild(pct);
    parent.appendChild(wrap);
    return { wrap, bar, pct };
  }

  function render() {
    root._badge.textContent = t('wizard.stepOf').replace('{n}', step).replace('{total}', STEPS);
    root._back.disabled = step === 1 || !!busy;
    root._body.innerHTML = '';
    delete handlers['ollama.installProgress'];
    delete handlers['ollama.installDone'];
    delete handlers['ollama.pullProgress'];
    delete handlers['ollama.pullDone'];
    if (step === 1) renderStep1();
    else if (step === 2) renderStep2();
    else renderStep3();
  }

  function onNext() {
    if (step < STEPS) { step++; render(); return; }
    // 3단계의 '완료' — 고른 모델을 저장하고 마친다
    bridge.call('settings.set', { ollama: { model: chosen } })
      .then(() => finish(true))
      .catch(() => finish(false));
  }

  // ---------- 1단계: Ollama ----------

  function renderStep1() {
    const b = root._body;
    b.appendChild(el('h3', 'wiz-h', t('wizard.s1Title')));
    b.appendChild(el('p', 'wiz-desc', t('wizard.s1Desc')));
    const status = el('p', 'wiz-status', t('wizard.checking'));
    const actions = el('div', 'wiz-actions');
    b.appendChild(status);
    b.appendChild(actions);
    const prog = progressBox(b);
    setNext(t('wizard.next'), false);

    const startInstall = () => {
      busy = 'install';
      actions.innerHTML = '';
      status.textContent = t('wizard.s1Downloading');
      prog.wrap.classList.remove('hidden');
      root._back.disabled = true;
      bridge.call('ollama.installOllama', { ownerId }).catch(() => {});
    };

    handlers['ollama.installProgress'] = (d) => {
      if (d.stage === 'download' && d.total) {
        const p = Math.round((d.received / d.total) * 100);
        prog.bar.value = p;
        prog.pct.textContent = p + '%';
        status.textContent = t('wizard.s1Downloading');
      } else if (d.stage === 'install') {
        status.textContent = t('wizard.s1Installing');
        prog.pct.textContent = '';
      } else if (d.stage === 'starting') {
        status.textContent = t('wizard.s1Starting');
      }
    };
    handlers['ollama.installDone'] = async (d) => {
      busy = null;
      prog.wrap.classList.add('hidden');
      if (!d.ok) {
        status.textContent = `${t('wizard.s1Failed')}: ${d.error || ''}`;
        actions.innerHTML = '';
        actions.appendChild(btn(t('wizard.retry'), 'primary', startInstall));
        return;
      }
      await check();
    };

    async function check() {
      status.textContent = t('wizard.checking');
      actions.innerHTML = '';
      setNext(t('wizard.next'), false);
      if (!(await isInstalled())) {
        status.textContent = t('wizard.s1Missing');
        actions.appendChild(btn(t('wizard.s1Install'), 'primary', startInstall));
        return;
      }
      const r = await listModels();
      if (!r.ok) {
        // 설치는 되어 있는데 서버가 응답하지 않는다 — Ollama 앱을 띄워 준다
        status.textContent = t('wizard.s1NotRunning');
        actions.appendChild(btn(t('wizard.s1Start'), 'primary', () => {
          status.textContent = t('wizard.s1Starting');
          actions.innerHTML = '';
          busy = 'install';
          bridge.call('ollama.installOllama', { ownerId }).catch(() => {});
        }));
        return;
      }
      models = r.models;
      status.textContent = t('wizard.s1Ok');
      setNext(t('wizard.next'), true);
    }
    check();
  }

  // ---------- 2단계: 모델 설치 ----------

  function renderStep2() {
    const b = root._body;
    b.appendChild(el('h3', 'wiz-h', t('wizard.s2Title')));
    b.appendChild(el('p', 'wiz-desc', t('wizard.s2Desc')));
    const status = el('p', 'wiz-status', '');
    b.appendChild(status);
    const showHave = () => {
      status.textContent = models.length ? `${t('wizard.s2Have')}: ${models.join(', ')}`
                                         : t('wizard.s2None');
    };
    showHave();

    let pick = (ollamaModels.list().find((m) => m.recommended) || ollamaModels.list()[0]).name;
    const list = el('div', 'wiz-list');
    ollamaModels.list().forEach((m) => {
      const row = el('label', 'wiz-item');
      const radio = el('input');
      radio.type = 'radio';
      radio.name = 'wizModel';
      radio.value = m.name;
      radio.checked = m.name === pick;
      radio.addEventListener('change', () => { pick = m.name; });
      row.appendChild(radio);
      const info = el('div', 'wiz-item-main');
      const line = el('div', 'wiz-item-name', m.name);
      if (m.recommended) line.appendChild(el('span', 'wiz-tag', t('wizard.recommended')));
      if (models.includes(m.name)) line.appendChild(el('span', 'wiz-tag ok', t('wizard.installed')));
      info.appendChild(line);
      info.appendChild(el('div', 'wiz-item-sub', `${m.size} · ${t('wizard.note.' + m.note)}`));
      row.appendChild(info);
      list.appendChild(row);
    });
    b.appendChild(list);

    const actions = el('div', 'wiz-actions');
    b.appendChild(actions);
    const prog = progressBox(b);
    const dl = btn(t('wizard.s2Download'), 'primary', () => {
      if (busy === 'pull' && pullId) {          // 진행 중 → 중단
        bridge.call('ai.abort', { requestId: pullId }).catch(() => {});
        return;
      }
      busy = 'pull';
      pullId = 'wizpull-' + Date.now();
      dl.textContent = t('wizard.s2Cancel');
      prog.wrap.classList.remove('hidden');
      prog.bar.value = 0;
      status.textContent = `${t('wizard.s2Downloading')} — ${pick}`;
      root._back.disabled = true;
      setNext(t('wizard.next'), false);
      bridge.call('ollama.pull', { requestId: pullId, ownerId, name: pick }).catch(() => {});
    });
    actions.appendChild(dl);
    setNext(t('wizard.next'), models.length > 0);

    handlers['ollama.pullProgress'] = (d) => {
      if (d.requestId !== pullId) return;
      if (d.total) {
        const p = Math.round((d.completed / d.total) * 100);
        prog.bar.value = p;
        prog.pct.textContent = p + '%';
      }
      status.textContent = `${t('wizard.s2Downloading')} — ${pick} (${d.status || ''})`;
    };
    handlers['ollama.pullDone'] = async (d) => {
      if (d.requestId !== pullId) return;
      const aborted = d.error === 'aborted';
      busy = null;
      pullId = null;
      dl.textContent = t('wizard.s2Download');
      prog.wrap.classList.add('hidden');
      root._back.disabled = false;
      const r = await listModels();             // 실제로 들어왔는지 다시 확인한다
      if (r.ok) models = r.models;
      if (d.ok || aborted) showHave();
      else status.textContent = `${t('wizard.s2Failed')}: ${d.error || ''}`;
      setNext(t('wizard.next'), models.length > 0);
    };
  }

  // ---------- 3단계: 쓸 모델 선택 ----------

  function renderStep3() {
    const b = root._body;
    b.appendChild(el('h3', 'wiz-h', t('wizard.s3Title')));
    b.appendChild(el('p', 'wiz-desc', t('wizard.s3Desc')));
    if (!chosen || !models.includes(chosen)) chosen = models[0] || '';
    const list = el('div', 'wiz-list');
    models.forEach((name) => {
      const row = el('label', 'wiz-item');
      const radio = el('input');
      radio.type = 'radio';
      radio.name = 'wizUse';
      radio.value = name;
      radio.checked = name === chosen;
      radio.addEventListener('change', () => {
        chosen = name;
        setNext(t('wizard.done'), true);
      });
      row.appendChild(radio);
      const info = el('div', 'wiz-item-main');
      info.appendChild(el('div', 'wiz-item-name', name));
      const known = ollamaModels.find(name);
      if (known) info.appendChild(el('div', 'wiz-item-sub', known.size));
      row.appendChild(info);
      list.appendChild(row);
    });
    b.appendChild(list);
    setNext(t('wizard.done'), !!chosen);
  }

  // ---------- 공개 API ----------

  // AI를 쓸 수 있으면 곧바로 true. 아니면 마법사를 띄우고, 다 마치면 true, 닫으면 false.
  async function ensureReady(stickerId) {
    if (resolveRun) return false;   // 이미 떠 있다 — 겹쳐 띄우지 않는다
    ownerId = stickerId || '';
    let info;
    try {
      info = await inspect();
    } catch {
      return true;  // 상태를 못 읽으면 막지 않는다 — 기존 오류 안내에 맡긴다
    }
    if (info.ready) return true;
    if (!root) build();
    models = info.models || [];
    chosen = '';
    step = info.from;
    busy = null;
    root.classList.remove('hidden');
    render();
    return new Promise((resolve) => { resolveRun = resolve; });
  }

  return { ensureReady };
})();
