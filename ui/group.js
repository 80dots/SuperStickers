// 그룹 창 메인: 멤버 메모를 카드로 렌더링, 재정렬(DnD)·인라인 편집·분리·삭제
(async () => {
  const init = window.__init || { theme: 'light', lang: 'en' };
  const $ = (sel) => document.querySelector(sel);
  const cardsEl = $('#cards');

  // 그룹창은 테마 불변 — 항상 라이트 기준으로 렌더링
  const isDark = () => false;

  await i18n.load(init.lang);
  i18n.apply();

  let group = null;
  let members = [];
  const saveTimers = new Map();  // stickerId → debounce timer
  const mdEditing = new Set();   // 마크다운 편집 중인 카드 id

  // ---------- 데이터 ----------
  async function load() {
    const r = await bridge.call('group.load');
    group = r.group;
    members = r.members;
    renderTitle();
    $('#gpinBtn').classList.toggle('on', !!group.topmost);
    setLayoutUi(group.layout);
    applyAppearance();
    renderCards();
  }

  // ---------- 배경색 / 투명도 ----------
  // 투명도는 #frame 배경(rgba)에만 적용 — 헤더·카드는 항상 불투명
  function hexToRgb(hex) {
    return [
      parseInt(hex.slice(1, 3), 16),
      parseInt(hex.slice(3, 5), 16),
      parseInt(hex.slice(5, 7), 16),
    ];
  }
  function solidHeaderTone(hex, fgIsDark) {
    // 프레임 색보다 살짝 진한/밝은 불투명 헤더 색
    const [r, g, b] = hexToRgb(hex);
    const mix = (v) => Math.round(fgIsDark ? v * 0.93 : v + (255 - v) * 0.1);
    return `rgb(${mix(r)}, ${mix(g)}, ${mix(b)})`;
  }
  function applyAppearance() {
    const root = document.documentElement.style;
    const opacity = Math.min(1, Math.max(0, group.opacity ?? 1));
    let baseHex;
    if (group.color) {
      baseHex = colorUtil.effectiveBg(group.color, isDark());
      const fg = colorUtil.textColorFor(baseHex);
      root.setProperty('--group-fg', fg);
      root.setProperty('--group-header-bg', solidHeaderTone(baseHex, fg === '#1F2328'));
      document.body.classList.add('tinted');
      $('#gcolorDot').style.background = baseHex;
    } else {
      document.body.classList.remove('tinted');
      baseHex = '#F7F7F8';  // 라이트 --bg 기본값 (테마 불변)
      $('#gcolorDot').style.background = '';
    }
    // 반투명 배경 자체는 네이티브 backdrop 창이 그린다.
    // body는 backdrop과 같은 단색으로 — region 경계 안티앨리어싱이 자연스럽게 섞이도록.
    document.body.style.background = baseHex;
    $('#gopacityRange').value = Math.round(opacity * 100);
    $('#gopacityVal').textContent = Math.round(opacity * 100) + '%';
  }

  // ---------- 불투명 영역(shape) 전송 ----------
  // 헤더/카드/팝오버 사각형을 네이티브로 보내 content 창 region으로 사용한다.
  let lastShapeJson = '';
  function collectShape() {
    const dpr = window.devicePixelRatio || 1;
    const rects = [];
    const add = (el, rad, clipRect) => {
      if (!el || el.classList.contains('hidden')) return;
      let b = el.getBoundingClientRect();
      if (clipRect) {
        const x1 = Math.max(b.left, clipRect.left), y1 = Math.max(b.top, clipRect.top);
        const x2 = Math.min(b.right, clipRect.right), y2 = Math.min(b.bottom, clipRect.bottom);
        if (x2 <= x1 || y2 <= y1) return;
        b = { left: x1, top: y1, width: x2 - x1, height: y2 - y1 };
      }
      if (b.width <= 0 || b.height <= 0) return;
      rects.push({
        x: Math.floor(b.left * dpr),
        y: Math.floor(b.top * dpr),
        w: Math.ceil(b.width * dpr) + 1,
        h: Math.ceil(b.height * dpr) + 1,
        r: Math.round(rad * dpr),
      });
    };
    add($('#ghead'), 8);  // 헤더도 카드와 같은 라운드 모서리
    const cardsClip = $('#cards').getBoundingClientRect();
    document.querySelectorAll('.gcard').forEach((c) => add(c, 8, cardsClip));
    // 스크롤이 필요하면 오른쪽 스크롤바 영역도 region에 포함 (없으면 잘려서 안 보임)
    if (cardsEl.scrollHeight > cardsEl.clientHeight + 1) {
      rects.push({
        x: Math.floor((cardsClip.right - 10) * dpr),
        y: Math.floor(cardsClip.top * dpr),
        w: Math.ceil(10 * dpr) + 1,
        h: Math.ceil(cardsClip.height * dpr) + 1,
        r: 0,
      });
    }
    if (!$('#emptyHint').classList.contains('hidden')) add($('#emptyHint').querySelector('span'), 8);
    add($('#gnewMenu'), 8);
    add($('#gcolorPopover'), 8);
    return rects;
  }
  function sendShape() {
    const rects = collectShape();
    const j = JSON.stringify(rects);
    if (j === lastShapeJson) return;
    lastShapeJson = j;
    bridge.call('group.setShape', { rects }).catch(() => {});
  }
  let shapeTimer = null;
  function scheduleShape() {
    if (shapeTimer) return;
    shapeTimer = requestAnimationFrame(() => {
      shapeTimer = null;
      sendShape();
    });
  }
  // 안전망: 관찰이 놓친 레이아웃 변화를 주기적으로 반영.
  // 숨긴 창에서는 DOM 측정을 건너뛴다 (트레이 상주 중 불필요한 상시 부하 제거).
  setInterval(() => { if (document.visibilityState === 'visible') sendShape(); }, 500);
  // 다시 표시되면 즉시 한 번 갱신 (숨긴 동안의 변화 반영)
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') scheduleShape();
  });
  window.addEventListener('resize', scheduleShape);
  cardsEl.addEventListener('scroll', scheduleShape);
  const shapeObserver = new MutationObserver(scheduleShape);
  shapeObserver.observe($('#gnewMenu'), { attributes: true, attributeFilter: ['class'] });
  shapeObserver.observe($('#gcolorPopover'), { attributes: true, attributeFilter: ['class'] });

  function scheduleMemberSave(m) {
    clearTimeout(saveTimers.get(m.id));
    saveTimers.set(m.id, setTimeout(() => flushMemberSave(m), 800));
  }
  function flushMemberSave(m) {
    clearTimeout(saveTimers.get(m.id));
    saveTimers.delete(m.id);
    const attachments = [
      ...new Set([
        ...(m.html.match(
          /https:\/\/data\.sticker\/stickers\/[^/\s"')]+\/[\w.\-]+\/[\w.\-]+/g) || [])
          .map((u) => decodeURIComponent(u.split('/').slice(-2).join('/'))),
        ...mdTools.getAttachments(m.markdown),
      ]),
    ];
    bridge.call('member.save', {
      id: m.id, html: m.html, markdown: m.markdown, mode: m.mode, attachments,
    }).catch(console.error);
  }
  function flushAll() {
    members.forEach((m) => { if (saveTimers.has(m.id)) flushMemberSave(m); });
  }
  bridge.on('app.flush', flushAll);
  window.addEventListener('blur', flushAll);

  // ---------- 카드 렌더링 ----------
  function renderCards() {
    cardsEl.innerHTML = '';
    $('#emptyHint').classList.toggle('hidden', members.length > 0);
    // masonry: 다단(columns) 컨테이너를 높이 제약이 없는 내부 래퍼로 분리.
    // (#cards는 flex로 높이가 고정된 스크롤 박스라, 직접 다단을 걸면 창을 좁혀도
    //  열이 가로로 증식해 화면 밖 카드가 잘려 보이지 않게 됨)
    let target = cardsEl;
    if (group.layout === 'masonry') {
      target = document.createElement('div');
      target.className = 'mwrap';
      cardsEl.appendChild(target);
    }
    members.forEach((m) => target.appendChild(buildCard(m)));
    scheduleShape();
  }

  function buildCard(m) {
    const card = document.createElement('div');
    card.className = 'gcard';
    card.dataset.id = m.id;
    const bg = colorUtil.effectiveBg(m.color, isDark());
    card.style.background = bg;
    card.style.color = colorUtil.textColorFor(bg);

    // --- 상단 바 (재정렬 드래그 핸들 + 버튼) ---
    const bar = document.createElement('div');
    bar.className = 'gcard-bar';
    bar.draggable = true;
    const grip = document.createElement('span');
    grip.className = 'grip';
    grip.textContent = '⋮⋮⋮';
    bar.appendChild(grip);

    const isList = group.layout === 'list';

    // 메모 타이틀 (AI Review 또는 수동 설정).
    // 목록 뷰는 타이틀만 표시 — 타이틀이 없으면 본문 미리보기 한 줄로 대신한다.
    const mTitle = (m.title || '').trim();
    const label = mTitle || (isList ? cardPreviewText(m) : '');
    if (label) {
      bar.classList.add('has-title');
      const ttl = document.createElement('span');
      ttl.className = 'gcard-title' + (mTitle ? '' : ' preview');
      ttl.textContent = label;
      ttl.title = label;
      bar.appendChild(ttl);
    }

    if (!isList && (m.type || m.mode) === 'markdown') {
      const editBtn = mkBtn('✎', i18n.t('tt.editCard'), () => toggleMdEdit(m, card));
      bar.appendChild(editBtn);
    }
    bar.appendChild(mkBtn('⇱', i18n.t('tt.popOut'), () => {
      flushAll();
      bridge.call('group.removeMember', { id: m.id });
    }));
    const delBtn = mkBtn('', i18n.t('manager.delete'), () => {
      // 삭제 확인은 네이티브 대화상자에서 수행 (region 클리핑으로 페이지 confirm이 잘려 보임)
      bridge.call('member.delete', { id: m.id });
    });
    delBtn.innerHTML =
      '<svg viewBox="0 0 16 16" width="12" height="12"><path fill="currentColor" ' +
      'd="M6 2h4l.5 1H14v1.5H2V3h3.5zM3 6h10l-.8 8.2c-.1.5-.5.8-1 .8H4.8c-.5 0-.9-.3-1-.8z"/></svg>';
    bar.appendChild(delBtn);

    card.appendChild(bar);

    // --- 본문 (목록 뷰는 타이틀만 — 본문 없음) ---
    if (!isList) {
      const body = document.createElement('div');
      body.className = 'gcard-body';
      card.appendChild(body);
      renderCardBody(m, card, body);
    }

    // 개별 높이·크기 조절 핸들은 혼합(masonry) 뷰에서만.
    // 균일 그리드는 모든 카드가 같은 크기, 목록은 타이틀 한 줄 고정.
    if (group.layout === 'masonry') {
      const mh = (group.memberHeights || {})[m.id];
      if (mh) {
        card.style.height = mh + 'px';
        card.classList.add('fixed-h');  // 본문 max-height 해제 → 핸들이 카드 하단 고정
      }

      // 하단 높이 조절 핸들 (드래그로 높이만 조정, 더블클릭 = 기본으로 복원)
      const rez = document.createElement('div');
      rez.className = 'gcard-resize';
      rez.addEventListener('pointerdown', (e) => {
        if (e.button !== 0) return;
        e.preventDefault();
        e.stopPropagation();
        try { rez.setPointerCapture(e.pointerId); } catch {}
        card.classList.add('fixed-h');
        const startY = e.clientY;
        const startH = card.getBoundingClientRect().height;
        const onMove = (ev) => {
          const h = Math.max(60, Math.round(startH + ev.clientY - startY));
          card.style.height = h + 'px';
          scheduleShape();
        };
        const onUp = () => {
          rez.removeEventListener('pointermove', onMove);
          rez.removeEventListener('pointerup', onUp);
          const h = Math.round(card.getBoundingClientRect().height);
          group.memberHeights = group.memberHeights || {};
          group.memberHeights[m.id] = h;
          bridge.call('group.setMemberHeight', { id: m.id, height: h });
          scheduleShape();
        };
        rez.addEventListener('pointermove', onMove);
        rez.addEventListener('pointerup', onUp);
      });
      rez.addEventListener('dblclick', () => {
        if (group.memberHeights) delete group.memberHeights[m.id];
        card.style.height = '';
        card.classList.remove('fixed-h');
        bridge.call('group.setMemberHeight', { id: m.id, height: 0 });
        scheduleShape();
      });
      card.appendChild(rez);
    }

    // --- 재정렬 DnD ---
    bar.addEventListener('dragstart', (e) => {
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', m.id);
      card.classList.add('dragging');
      e.dataTransfer.setDragImage(card, 20, 10);
    });
    bar.addEventListener('dragend', (e) => {
      card.classList.remove('dragging');
      // 창 밖에 놓으면 그룹에서 꺼내 그 지점에 플로팅 스티커로 배치
      // (드롭 지점이 다른 그룹 위라면 네이티브가 그 그룹으로 이동시킴)
      const inWin =
        e.screenX >= window.screenX && e.screenX <= window.screenX + window.outerWidth &&
        e.screenY >= window.screenY && e.screenY <= window.screenY + window.outerHeight;
      if (!inWin && (e.screenX || e.screenY)) {
        const dpr = window.devicePixelRatio || 1;
        flushAll();
        bridge.call('group.removeMember', {
          id: m.id,
          x: Math.round(e.screenX * dpr),
          y: Math.round(e.screenY * dpr),
        });
        return;
      }
      commitOrder();
    });
    card.addEventListener('dragover', (e) => {
      e.preventDefault();
      const dragging = card.parentNode.querySelector('.gcard.dragging');
      if (!dragging || dragging === card) return;
      const rect = card.getBoundingClientRect();
      const before = (e.clientY - rect.top) < rect.height / 2;
      card.parentNode.insertBefore(dragging, before ? card : card.nextSibling);
      scheduleShape();
    });
    card.addEventListener('drop', (e) => e.preventDefault());

    return card;
  }

  // 목록 뷰에서 타이틀이 없는 메모의 대체 표시 텍스트 (본문 한 줄 미리보기)
  function cardPreviewText(m) {
    const t = m.type || (m.mode === 'markdown' ? 'markdown' : 'rich');
    let text = '';
    if (t === 'markdown') text = m.markdown || '';
    else if (t === 'file')
      text = '📁 ' + (m.files || []).map((p) => p.split('\\').pop()).join(', ');
    else if (t === 'web') text = '🌐 ' + (m.lastUrl || m.url || '');
    else if (t === 'pdf') text = '📄 ' + (m.pdfTitle || '');
    else {
      const div = document.createElement('div');
      div.innerHTML = m.html || '';
      text = div.innerText;
    }
    text = text.trim().replace(/\s+/g, ' ');
    return text.slice(0, 80) || i18n.t('manager.noText');
  }

  function mkBtn(glyph, title, onClick) {
    const b = document.createElement('button');
    b.textContent = glyph;
    b.title = title;
    b.addEventListener('click', (e) => { e.stopPropagation(); onClick(); });
    return b;
  }

  // 3D 임베드를 썸네일(또는 안내 박스)로 표시. 뷰어는 스티커 창에서만 동작한다.
  function mountThumb3d(el) {
    el.setAttribute('contenteditable', 'false');
    const shadow = el.shadowRoot || el.attachShadow({ mode: 'open' });
    const thumb = el.dataset.thumb || '';
    shadow.innerHTML = `
      <style>
        :host { display: block; }
        .box { width: 100%; height: 100%; border-radius: 6px; overflow: hidden;
               background: #2a2d33; display: flex; align-items: center;
               justify-content: center; }
        img { width: 100%; height: 100%; object-fit: contain; display: block; }
        .ph { color: #aab; font-size: 11px; display: flex; align-items: center; gap: 5px; }
      </style>
      <div class="box">${thumb
        ? `<img src="${thumb}" alt="3D">`
        : `<span class="ph">
             <svg viewBox="0 0 16 16" width="14" height="14"><g fill="none"
               stroke="currentColor" stroke-width="1.1"><path d="M8 1.8 14 5v6L8 14.2 2 11V5z"/>
               <path d="M2 5l6 3.2L14 5M8 8.2v6"/></g></svg>3D</span>`}</div>`;
  }

  function renderCardBody(m, card, body) {
    body.innerHTML = '';
    const t = m.type || (m.mode === 'markdown' ? 'markdown' : 'rich');
    // 파일 메모: 파일 목록 그대로 표시, 더블클릭으로 실행
    if (t === 'file') {
      body.contentEditable = 'false';
      const files = m.files || [];
      if (!files.length) {
        body.dataset.placeholder = i18n.t('manager.noText');
        return;
      }
      files.forEach((p) => {
        const row = document.createElement('div');
        row.className = 'gcard-file';
        const name = p.replace(/\//g, '\\').split('\\').filter(Boolean).pop() || p;
        const isDir = !/\.[^\\\/]+$/.test(name);
        row.textContent = `${isDir ? '📁' : '📄'} ${name}`;
        row.title = p;
        row.addEventListener('dblclick', () => bridge.call('member.openPath', { path: p }));
        body.appendChild(row);
      });
      return;
    }
    // 웹 메모: 등록한 URL을 링크로 표시 (클릭 시 기본 브라우저)
    if (t === 'web') {
      body.contentEditable = 'false';
      const url = m.lastUrl || m.url || '';
      if (!url) {
        body.dataset.placeholder = i18n.t('manager.noText');
        return;
      }
      const line = document.createElement('div');
      line.className = 'gcard-web';
      const a = document.createElement('a');
      a.href = url;
      a.textContent = url;
      a.title = url;
      line.appendChild(document.createTextNode('🌐 '));
      line.appendChild(a);
      body.appendChild(line);
      if (m.url && m.url !== url) {
        const home = document.createElement('div');
        home.className = 'gcard-web-home';
        home.textContent = '⌂ ' + m.url;
        home.title = m.url;
        body.appendChild(home);
      }
      return;
    }
    // PDF 메모: 내장 뷰어로 내용 그대로 표시
    if (t === 'pdf') {
      body.contentEditable = 'false';
      body.classList.add('gcard-pdf-body');
      if (!m.pdfName) {
        body.dataset.placeholder = i18n.t('manager.noText');
        return;
      }
      const title = document.createElement('div');
      title.className = 'gcard-pdf-title';
      title.textContent = '📄 ' + (m.pdfTitle || m.pdfName);
      body.appendChild(title);
      const frame = document.createElement('iframe');
      frame.className = 'gcard-pdf';
      frame.src = 'https://data.sticker/stickers/' + m.id + '/' + m.pdfName + '#toolbar=0';
      body.appendChild(frame);
      return;
    }
    if (t === 'markdown') {
      if (mdEditing.has(m.id)) {
        const ta = document.createElement('textarea');
        ta.className = 'gcard-md-edit';
        ta.value = m.markdown || '';
        ta.addEventListener('input', () => {
          m.markdown = ta.value;
          scheduleMemberSave(m);
          autosize();
          scheduleShape();
        });
        const autosize = () => {
          ta.style.height = 'auto';
          ta.style.height = ta.scrollHeight + 'px';
        };
        body.appendChild(ta);
        requestAnimationFrame(autosize);
        ta.focus();
      } else {
        const view = document.createElement('div');
        body.appendChild(view);
        mdTools.renderInto(view, m.markdown || '', (idx, checked) => {
          m.markdown = mdTools.toggleTaskInSource(m.markdown || '', idx, checked);
          scheduleMemberSave(m);
        });
        if (!(m.markdown || '').trim()) body.dataset.placeholder = i18n.t('manager.noText');
      }
      return;
    }
    // 리치 카드: 인라인 편집 가능
    body.contentEditable = 'true';
    body.spellcheck = false;
    body.innerHTML = m.html || '';
    // 3D 임베드는 그룹 안에서 렌더링하지 않고 캡처해 둔 썸네일 이미지로 표시.
    // (Shadow DOM에 넣으므로 innerHTML 직렬화에 포함되지 않아 원본 임베드가 보존됨 —
    //  그룹 밖으로 꺼내면 스티커 창에서 다시 실시간 렌더링된다)
    body.querySelectorAll('.embed3d').forEach(mountThumb3d);
    body.dataset.placeholder = i18n.t('editor.placeholder');
    body.addEventListener('input', () => {
      m.html = body.innerHTML;
      scheduleMemberSave(m);
      scheduleShape();  // masonry에서 카드 높이 변화 반영
    });
    // 체크박스(리치 .check-item) 상태 반영
    body.addEventListener('change', (e) => {
      if (e.target.matches('.check-item input[type="checkbox"]')) {
        e.target.toggleAttribute('checked', e.target.checked);
        m.html = body.innerHTML;
        scheduleMemberSave(m);
      }
    });
  }

  function toggleMdEdit(m, card) {
    flushAll();
    if (mdEditing.has(m.id)) mdEditing.delete(m.id);
    else mdEditing.add(m.id);
    renderCardBody(m, card, card.querySelector('.gcard-body'));
  }

  function commitOrder() {
    const order = [...cardsEl.querySelectorAll('.gcard')].map((c) => c.dataset.id);
    members.sort((a, b) => order.indexOf(a.id) - order.indexOf(b.id));
    bridge.call('group.reorder', { memberIds: order }).catch(console.error);
  }

  // ---------- 헤더 ----------
  $('#gdrag').addEventListener('mousedown', (e) => {
    if (e.button === 0) bridge.call('window.startDrag').catch(() => {});
  });
  $('#ghead').addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    if (e.target.closest('button') || e.target.closest('#titleInput')) return;
    if (e.target.closest('#gcolorPopover') || e.target.closest('#gnewMenu')) return;
    if (e.target.id === 'gdrag') return;  // 위에서 처리
    bridge.call('window.startDrag').catch(() => {});
  });

  // ---------- 타이틀: 표시 텍스트 + 수정 버튼 → 클릭 시에만 편집 모드 ----------
  function renderTitle() {
    const t = (group.title || '').trim();
    const span = $('#gtitleText');
    span.textContent = t || i18n.t('group.titlePlaceholder');
    span.classList.toggle('placeholder', !t);
  }

  function setTitleEditing(on) {
    $('#gtitleText').classList.toggle('hidden', on);
    $('#titleEditBtn').classList.toggle('hidden', on);
    $('#titleInput').classList.toggle('hidden', !on);
    if (on) {
      const input = $('#titleInput');
      input.value = group.title || '';
      input.focus();
      input.select();
    }
  }

  $('#titleEditBtn').addEventListener('click', () => setTitleEditing(true));

  function commitTitle() {
    const v = $('#titleInput').value.trim();
    if (v !== (group.title || '')) {
      group.title = v;
      bridge.call('group.setTitle', { title: v });
    }
    renderTitle();
    setTitleEditing(false);
  }

  $('#titleInput').addEventListener('blur', () => {
    // 편집 모드일 때만 (Enter/Esc가 먼저 처리한 뒤 발생하는 blur는 무시)
    if (!$('#titleInput').classList.contains('hidden')) commitTitle();
  });
  $('#titleInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      commitTitle();
    } else if (e.key === 'Escape') {
      e.target.value = group.title || '';  // 취소: 원래 값으로
      renderTitle();
      setTitleEditing(false);
    }
  });

  function setLayoutUi(layout) {
    cardsEl.classList.toggle('grid', layout === 'grid');
    cardsEl.classList.toggle('masonry', layout === 'masonry');
    cardsEl.classList.toggle('list', layout === 'list');
    cardsEl.classList.toggle('size-s', group.gridSize === 's');
    cardsEl.classList.toggle('size-m', group.gridSize !== 's' && group.gridSize !== 'l');
    cardsEl.classList.toggle('size-l', group.gridSize === 'l');
    $('#layoutGridBtn').classList.toggle('on', layout === 'grid');
    $('#layoutMasonryBtn').classList.toggle('on', layout === 'masonry');
    $('#layoutListBtn').classList.toggle('on', layout === 'list');
    $('#gsizeSeg').classList.toggle('hidden', layout !== 'grid');
    $('#gsizeS').classList.toggle('on', group.gridSize === 's');
    $('#gsizeM').classList.toggle('on', group.gridSize !== 's' && group.gridSize !== 'l');
    $('#gsizeL').classList.toggle('on', group.gridSize === 'l');
    scheduleShape();
  }
  function chooseLayout(layout) {
    group.layout = layout;
    setLayoutUi(layout);
    renderCards();  // 레이아웃별 높이 적용 방식이 달라 다시 그림
    bridge.call('group.setLayout', { layout });
  }
  $('#layoutGridBtn').addEventListener('click', () => chooseLayout('grid'));
  $('#layoutMasonryBtn').addEventListener('click', () => chooseLayout('masonry'));
  $('#layoutListBtn').addEventListener('click', () => chooseLayout('list'));

  function chooseGridSize(size) {
    group.gridSize = size;
    setLayoutUi(group.layout);
    bridge.call('group.setGridSize', { size });
  }
  $('#gsizeS').addEventListener('click', () => chooseGridSize('s'));
  $('#gsizeM').addEventListener('click', () => chooseGridSize('m'));
  $('#gsizeL').addEventListener('click', () => chooseGridSize('l'));

  // ---------- 헤더 + 메뉴 ----------
  const gnewMenu = $('#gnewMenu');
  $('#gnewBtn').addEventListener('click', () => gnewMenu.classList.toggle('hidden'));
  gnewMenu.querySelectorAll('button').forEach((btn) => {
    btn.addEventListener('click', () => {
      gnewMenu.classList.add('hidden');
      const t = btn.dataset.newtype;
      if (t === 'group') bridge.call('groups.new');
      else bridge.call('group.newMemberMemo', { type: t });
    });
  });

  // ---------- 색상/투명도 팝오버 ----------
  const gcolorPopover = $('#gcolorPopover');
  function buildGroupColorGrid() {
    const grid = $('#gcolorGrid');
    grid.innerHTML = '';
    const current = group.color ? colorUtil.normalize(group.color) : '';
    colorUtil.PRESETS.forEach((hex) => {
      const btn = document.createElement('button');
      btn.className = 'color-opt';
      btn.style.background = colorUtil.effectiveBg(hex, isDark());
      btn.title = hex;
      btn.classList.toggle('sel', hex === current);
      btn.addEventListener('click', () => {
        group.color = hex;
        applyAppearance();
        bridge.call('group.setAppearance', { color: hex });
        gcolorPopover.classList.add('hidden');
      });
      grid.appendChild(btn);
    });
  }
  $('#gcolorBtn').addEventListener('click', () => {
    if (gcolorPopover.classList.contains('hidden')) {
      buildGroupColorGrid();
      $('#gcolorCustom').value = group.color ? colorUtil.normalize(group.color) : '#F7F7F8';
      gcolorPopover.classList.remove('hidden');
    } else {
      gcolorPopover.classList.add('hidden');
    }
  });
  document.addEventListener('mousedown', (e) => {
    if (!e.target.closest('#gcolorPopover') && !e.target.closest('#gcolorBtn'))
      gcolorPopover.classList.add('hidden');
    if (!e.target.closest('#gnewMenu') && !e.target.closest('#gnewBtn'))
      gnewMenu.classList.add('hidden');
  });
  $('#gcolorCustom').addEventListener('change', (e) => {
    group.color = e.target.value.toUpperCase();
    applyAppearance();
    bridge.call('group.setAppearance', { color: group.color });
    gcolorPopover.classList.add('hidden');
  });
  $('#gopacityRange').addEventListener('input', (e) => {
    group.opacity = e.target.value / 100;
    applyAppearance();
    bridge.call('group.setAppearance', { opacity: group.opacity });
  });
  $('#gcolorResetBtn').addEventListener('click', () => {
    group.color = '';
    group.opacity = 1;
    applyAppearance();
    bridge.call('group.setAppearance', { color: '', opacity: 1 });
    gcolorPopover.classList.add('hidden');
  });

  $('#gpinBtn').addEventListener('click', () => {
    const on = !$('#gpinBtn').classList.contains('on');
    $('#gpinBtn').classList.toggle('on', on);
    group.topmost = on;
    bridge.call('group.setTopmost', { topmost: on }).catch(console.error);
  });

  $('#gmanagerBtn').addEventListener('click', () => {
    bridge.call('app.openManager', { tab: 'list' }).catch(console.error);
  });

  $('#ghideBtn').addEventListener('click', () => {
    flushAll();
    bridge.call('group.hide');
  });
  $('#gdeleteBtn').addEventListener('click', async () => {
    flushAll();
    await bridge.call('group.delete');
  });

  // 메모창 밖(그룹창)을 Shift 없이 클릭하면 메모창 다중 선택을 해제한다
  document.addEventListener('mousedown', (e) => {
    if (!e.shiftKey) bridge.call('selection.clear').catch(() => {});
  }, true);

  // ---------- 네이티브 이벤트 ----------
  bridge.on('group.membersChanged', () => {
    flushAll();
    load().catch(console.error);
  });
  bridge.on('group.dragHover', (d) => {
    if (group && d.groupId === group.id) {
      document.body.classList.toggle('drophover', !!d.active);
    }
  });
  // (테마 변경은 그룹창에 영향 없음 — theme.changed 무시)
  bridge.on('locale.changed', async (d) => {
    await i18n.load(d.lang);
    i18n.apply();
    renderCards();
  });

  await load();
})();
