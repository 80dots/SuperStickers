// 리치 메모의 표. Notion의 표 조작감을 참고했다.
//
// 저장되는 것은 순수한 표뿐이다:
//   <div class="mtable-wrap"><table class="mtable">
//     <thead><tr><th data-sort="asc">제목</th>…</tr></thead>
//     <tbody><tr><td>…</td>…</tr></tbody>
//     <tfoot><tr><td data-fn="sum">합계 12</td>…</tr></tfoot>   ← 통계 행(선택)
//   </table></div>
//
// 조작용 장식(정렬 버튼·행/열 손잡이)은 전부 data-chrome을 달고 들어간다.
// **저장 직전에 editorCore.getHtml()이 걷어낸다** — 본문 HTML에 UI 부스러기가 남으면
// 그룹 카드·AI 리뷰·내보내기까지 따라다닌다. 셀 선택 표시(.tsel)도 같은 이유로 걷어낸다.
const tableTools = (() => {
  let editor = null;
  let onChange = null;
  const T = (k, fallback) => {
    const v = typeof i18n !== 'undefined' ? i18n.t(k) : k;
    return v === k ? fallback : v;
  };

  // 통계 함수. 셀을 누를 때마다 이 순서로 돈다.
  const FNS = ['none', 'sum', 'avg', 'count', 'min', 'max'];
  const FN_LABEL = {
    none: () => T('table.fnNone', '—'),
    sum: () => T('table.fnSum', '합계'),
    avg: () => T('table.fnAvg', '평균'),
    count: () => T('table.fnCount', '개수'),
    min: () => T('table.fnMin', '최소'),
    max: () => T('table.fnMax', '최대'),
  };

  const notify = () => { if (onChange) onChange(); };
  const tables = () => [...editor.querySelectorAll('table.mtable')];
  const bodyRows = (table) => [...table.tBodies[0].rows];
  const colCount = (table) => table.tHead.rows[0].cells.length;

  function closest(node, sel) {
    let el = node && node.nodeType === 3 ? node.parentElement : node;
    return el && el.closest ? el.closest(sel) : null;
  }
  const tableOf = (node) => closest(node, 'table.mtable');
  const cellOf = (node) => closest(node, 'td, th');

  // ---------- 만들기 ----------
  function makeCell(tag, text) {
    const c = document.createElement(tag);
    if (text) c.textContent = text;
    else c.innerHTML = '<br>';
    return c;
  }

  function create(cols = 2, rows = 2) {
    const wrap = document.createElement('div');
    wrap.className = 'mtable-wrap';
    const table = document.createElement('table');
    table.className = 'mtable';
    const thead = table.createTHead();
    const hr = thead.insertRow();
    for (let c = 0; c < cols; c++) {
      hr.appendChild(makeCell('th', T('table.colName', '제목') + ' ' + (c + 1)));
    }
    const tbody = table.createTBody();
    for (let r = 0; r < rows; r++) {
      const tr = tbody.insertRow();
      for (let c = 0; c < cols; c++) tr.appendChild(makeCell('td'));
    }
    wrap.appendChild(table);
    return wrap;
  }

  // 커서 자리에 새 표를 넣는다. 표 뒤에는 빠져나올 빈 줄을 하나 둔다
  // (표가 본문 맨 끝이면 그 아래에 커서를 둘 자리가 없다).
  function insert(cols = 2, rows = 2) {
    const wrap = create(cols, rows);
    const after = document.createElement('div');
    after.innerHTML = '<br>';
    editorCore.insertNodeAtCaret(after);
    editorCore.insertNodeAtCaret(wrap);
    refreshAll();
    const first = wrap.querySelector('th');
    if (first) {
      const range = document.createRange();
      range.selectNodeContents(first);
      const sel = window.getSelection();
      sel.removeAllRanges();
      sel.addRange(range);
    }
    notify();
    return wrap;
  }

  // ---------- 행·열 ----------
  function insertRow(table, at, below = true) {
    const rows = bodyRows(table);
    const idx = Math.max(0, Math.min(rows.length, at + (below ? 1 : 0)));
    const tr = table.tBodies[0].insertRow(idx);
    const head = hasRowHeader(table);
    const statAt = statColIndex(table);
    for (let c = 0; c < colCount(table); c++) {
      if (c === 0 && head) {
        const th = makeCell('th');
        th.className = 'mth-row';
        tr.appendChild(th);
      } else if (c === statAt) {
        const cell = makeCell('td');
        cell.dataset.statcell = '1';
        cell.contentEditable = 'false';
        tr.appendChild(cell);
      } else {
        tr.appendChild(makeCell('td'));
      }
    }
    return tr;
  }

  function deleteRows(table, indices) {
    const rows = bodyRows(table);
    // 뒤에서부터 지워야 인덱스가 밀리지 않는다. 본문 행은 최소 하나 남긴다.
    [...new Set(indices)].sort((a, b) => b - a).forEach((i) => {
      if (rows.length - 1 < 1) return;
      if (rows[i]) { rows[i].remove(); rows.splice(i, 1); }
    });
  }

  function insertCol(table, at, right = true) {
    // 타이틀 열은 언제나 맨 왼쪽, 통계 열은 언제나 맨 오른쪽에 남아야 한다
    const lo = hasRowHeader(table) ? 1 : 0;
    const statAt = statColIndex(table);
    const hi = statAt >= 0 ? statAt : colCount(table);
    const idx = Math.max(lo, Math.min(hi, at + (right ? 1 : 0)));
    const hr = table.tHead.rows[0];
    hr.insertBefore(makeCell('th', T('table.colName', '제목') + ' ' + (idx + 1)),
                    hr.cells[idx] || null);
    bodyRows(table).forEach((tr) => {
      tr.insertBefore(makeCell('td'), tr.cells[idx] || null);
    });
    if (table.tFoot) {
      const fr = table.tFoot.rows[0];
      const cell = makeCell('td');
      cell.dataset.fn = 'none';
      cell.contentEditable = 'false';
      fr.insertBefore(cell, fr.cells[idx] || null);
    }
  }

  function deleteCols(table, indices) {
    const statAt = statColIndex(table);
    [...new Set(indices)].sort((a, b) => b - a).forEach((i) => {
      if (i === statAt) return;             // 통계 열은 메뉴에서 끄는 것이지 지우는 게 아니다
      if (colCount(table) - 1 < 1) return;  // 열은 최소 하나
      [table.tHead.rows[0], ...bodyRows(table),
       ...(table.tFoot ? [table.tFoot.rows[0]] : [])].forEach((tr) => {
        if (tr && tr.cells[i]) tr.cells[i].remove();
      });
    });
  }

  // 손잡이 드래그로 개수를 맞춘다 (모자라면 추가, 남으면 뒤에서 제거)
  function setRowCount(table, n) {
    n = Math.max(1, n);
    let cur = bodyRows(table).length;
    while (cur < n) { insertRow(table, cur - 1, true); cur++; }
    while (cur > n) { bodyRows(table)[cur - 1].remove(); cur--; }
  }
  function setColCount(table, n) {
    n = Math.max(1, n);
    let cur = colCount(table);
    while (cur < n) { insertCol(table, cur - 1, true); cur++; }
    while (cur > n) { deleteCols(table, [cur - 1]); cur--; }
  }

  // ---------- 통계 행 ----------
  const numOf = (text) => {
    // "1,200원" 처럼 단위가 붙어도 숫자를 읽는다
    const m = String(text).replace(/,/g, '').match(/-?\d+(\.\d+)?/);
    return m ? parseFloat(m[0]) : null;
  };

  function computeStat(table, col, fn) {
    const texts = bodyRows(table)
      .map((tr) => (tr.cells[col] ? cellText(tr.cells[col]) : '').trim());
    const vals = texts.map(numOf).filter((v) => v !== null);
    // 개수는 '칸의 수'가 아니라 '내용이 있는 칸의 수'다
    if (fn === 'count') return String(texts.filter((t) => t !== '').length);
    if (!vals.length) return '';
    const sum = vals.reduce((a, b) => a + b, 0);
    const round = (v) => String(Math.round(v * 1000) / 1000);
    if (fn === 'sum') return round(sum);
    if (fn === 'avg') return round(sum / vals.length);
    if (fn === 'min') return round(Math.min(...vals));
    if (fn === 'max') return round(Math.max(...vals));
    return '';
  }

  function hasStats(table) { return !!table.tFoot; }

  function toggleStats(table) {
    if (table.tFoot) { table.tFoot.remove(); return; }
    const tfoot = table.createTFoot();
    const tr = tfoot.insertRow();
    for (let c = 0; c < colCount(table); c++) {
      const cell = makeCell('td');
      cell.dataset.fn = 'none';  // 무엇을 셀지는 사용자가 고른다 (칸을 누르면 돈다)
      cell.contentEditable = 'false';
      tr.appendChild(cell);
    }
  }

  // ---------- 타이틀 열 (본문 행의 첫 칸을 머리글로) ----------
  function hasRowHeader(table) {
    const r = table.tBodies[0].rows[0];
    return !!(r && r.cells[0] && r.cells[0].tagName === 'TH');
  }

  function toggleRowHeader(table) {
    const on = hasRowHeader(table);
    bodyRows(table).forEach((tr) => {
      const cell = tr.cells[0];
      if (!cell) return;
      const want = on ? 'TD' : 'TH';
      if (cell.tagName === want) return;
      const next = document.createElement(want);
      next.innerHTML = cell.innerHTML;
      if (!on) next.className = 'mth-row';
      cell.replaceWith(next);
    });
  }

  // ---------- 통계 열 (행마다 계산해 맨 오른쪽에 붙는다) ----------
  // 함수는 열 전체가 하나를 쓴다 — 머리글 th의 data-statcol에 남는다.
  function statColIndex(table) {
    return [...table.tHead.rows[0].cells].findIndex((c) => c.dataset.statcol !== undefined);
  }
  const hasStatCol = (table) => statColIndex(table) >= 0;

  function toggleStatCol(table) {
    const at = statColIndex(table);
    if (at >= 0) {  // 끄기 — 일반 열 삭제 규칙(통계 열 보호)을 피해 직접 지운다
      [table.tHead.rows[0], ...bodyRows(table),
       ...(table.tFoot ? [table.tFoot.rows[0]] : [])].forEach((tr) => {
        if (tr && tr.cells[at]) tr.cells[at].remove();
      });
      return;
    }
    const th = makeCell('th');
    th.dataset.statcol = 'sum';
    table.tHead.rows[0].appendChild(th);
    bodyRows(table).forEach((tr) => {
      const cell = makeCell('td');
      cell.dataset.statcell = '1';
      cell.contentEditable = 'false';
      tr.appendChild(cell);
    });
    if (table.tFoot) {
      const cell = makeCell('td');
      cell.dataset.fn = 'none';
      cell.contentEditable = 'false';
      table.tFoot.rows[0].appendChild(cell);
    }
  }

  // 한 행의 숫자들을 모은다 (통계 열 자신과 타이틀 열은 빼고)
  function computeRowStat(table, tr, fn) {
    const statAt = statColIndex(table);
    const cells = [...tr.cells].filter((c, i) => i !== statAt && c.tagName !== 'TH');
    const texts = cells.map((c) => cellText(c).trim());
    const vals = texts.map(numOf).filter((v) => v !== null);
    if (fn === 'count') return String(texts.filter((t) => t !== '').length);
    if (!vals.length) return '';
    const sum = vals.reduce((a, b) => a + b, 0);
    const round = (v) => String(Math.round(v * 1000) / 1000);
    if (fn === 'sum') return round(sum);
    if (fn === 'avg') return round(sum / vals.length);
    if (fn === 'min') return round(Math.min(...vals));
    if (fn === 'max') return round(Math.max(...vals));
    return '';
  }

  function renderStatCol(table) {
    const at = statColIndex(table);
    if (at < 0) return;
    const th = table.tHead.rows[0].cells[at];
    const fn = th.dataset.statcol || 'sum';
    th.childNodes.forEach(() => {});
    // 머리글에는 함수 이름을 적는다 (정렬 버튼은 refresh가 따로 붙인다)
    th.textContent = FN_LABEL[fn] ? FN_LABEL[fn]() : '';
    th.dataset.statcol = fn;
    th.title = T('table.statHint', '눌러서 통계 함수 바꾸기');
    bodyRows(table).forEach((tr) => {
      const cell = tr.cells[at];
      if (!cell) return;
      cell.contentEditable = 'false';
      cell.textContent = fn === 'none' ? '' : computeRowStat(table, tr, fn);
    });
  }

  function cycleStatCol(table) {
    const at = statColIndex(table);
    if (at < 0) return;
    const th = table.tHead.rows[0].cells[at];
    const cur = th.dataset.statcol || 'sum';
    th.dataset.statcol = FNS[(FNS.indexOf(cur) + 1) % FNS.length];
  }

  // ---------- 표 제목 · 표 설명 ----------
  // 제목은 <caption>(표 위), 설명은 wrap 안 표 아래의 div. 둘 다 편집 가능한 본문이고
  // 장식이 아니므로 저장 HTML에 그대로 남는다.
  const hasCaption = (table) => !!table.caption;

  function toggleCaption(table) {
    if (table.caption) { table.caption.remove(); return; }
    const cap = document.createElement('caption');
    cap.className = 'mtable-caption';
    cap.dataset.placeholder = T('table.captionPlaceholder', '표 제목');
    table.insertBefore(cap, table.firstChild);  // caption은 표의 첫 자식이어야 한다
    return cap;
  }

  const descOf = (table) => table.parentElement.querySelector(':scope > .mtable-desc');
  const hasDesc = (table) => !!descOf(table);

  function toggleDesc(table) {
    const cur = descOf(table);
    if (cur) { cur.remove(); return; }
    const d = document.createElement('div');
    d.className = 'mtable-desc';
    d.dataset.placeholder = T('table.descPlaceholder', '표 설명');
    table.parentElement.appendChild(d);
    return d;
  }

  // 통계 행·열을 함께 다시 계산한다
  function recompute(table) {
    renderStatCol(table);
    renderStats(table);
  }

  function cycleStat(cell) {
    const cur = cell.dataset.fn || 'none';
    cell.dataset.fn = FNS[(FNS.indexOf(cur) + 1) % FNS.length];
  }

  function renderStats(table) {
    if (!table.tFoot) return;
    const tr = table.tFoot.rows[0];
    [...tr.cells].forEach((cell, i) => {
      const fn = cell.dataset.fn || 'none';
      cell.contentEditable = 'false';
      const value = fn === 'none' ? '' : computeStat(table, i, fn);
      cell.textContent = fn === 'none' ? FN_LABEL.none() : `${FN_LABEL[fn]()} ${value}`;
      cell.title = T('table.statHint', '눌러서 통계 함수 바꾸기');
    });
  }

  // ---------- 정렬 ----------
  function sortBy(table, col) {
    const th = table.tHead.rows[0].cells[col];
    const dir = th.dataset.sort === 'asc' ? 'desc' : 'asc';
    [...table.tHead.rows[0].cells].forEach((c) => delete c.dataset.sort);
    th.dataset.sort = dir;
    const rows = bodyRows(table);
    const key = (tr) => (tr.cells[col] ? tr.cells[col].textContent.trim() : '');
    const allNum = rows.every((tr) => key(tr) === '' || numOf(key(tr)) !== null);
    rows.sort((a, b) => {
      const ka = key(a), kb = key(b);
      if (ka === '' && kb === '') return 0;
      if (ka === '') return 1;   // 빈 칸은 언제나 아래로
      if (kb === '') return -1;
      const r = allNum ? numOf(ka) - numOf(kb) : ka.localeCompare(kb, undefined, { numeric: true });
      return dir === 'asc' ? r : -r;
    });
    rows.forEach((tr) => table.tBodies[0].appendChild(tr));
  }

  // ---------- 셀 선택 ----------
  function clearSelection(root) {
    (root || editor).querySelectorAll('.tsel').forEach((c) => c.classList.remove('tsel'));
  }

  function selectRange(table, a, b) {
    clearSelection(table);
    const r0 = Math.min(a.row, b.row), r1 = Math.max(a.row, b.row);
    const c0 = Math.min(a.col, b.col), c1 = Math.max(a.col, b.col);
    const rows = [table.tHead.rows[0], ...bodyRows(table)];
    for (let r = r0; r <= r1; r++) {
      for (let c = c0; c <= c1; c++) {
        const cell = rows[r] && rows[r].cells[c];
        if (cell) cell.classList.add('tsel');
      }
    }
  }

  // 선택된 셀의 행·열 번호 (본문 행 기준. 머리글 행은 -1)
  function selectedCells(table) {
    return [...table.querySelectorAll('.tsel')];
  }
  function cellPos(table, cell) {
    const rows = [table.tHead.rows[0], ...bodyRows(table)];
    const r = rows.indexOf(cell.parentElement);
    return { row: r, col: cell.cellIndex };
  }
  function selectedRows(table) {
    const set = new Set();
    selectedCells(table).forEach((c) => {
      const p = cellPos(table, c);
      if (p.row > 0) set.add(p.row - 1);  // 본문 행 인덱스
    });
    return [...set];
  }
  function selectedCols(table) {
    return [...new Set(selectedCells(table).map((c) => cellPos(table, c).col))];
  }

  // ---------- 장식(저장되지 않는 UI) ----------
  function chrome(tag, cls) {
    const el = document.createElement(tag);
    el.className = cls;
    el.dataset.chrome = '1';
    el.contentEditable = 'false';
    return el;
  }

  function refresh(table) {
    // 이전 장식을 걷고 다시 붙인다 (행·열 수가 바뀌면 자리도 바뀐다)
    table.parentElement.querySelectorAll('[data-chrome]').forEach((el) => el.remove());

    // 머리글마다 정렬 버튼
    [...table.tHead.rows[0].cells].forEach((th, i) => {
      const btn = chrome('span', 'mtable-sort');
      const dir = th.dataset.sort;
      btn.textContent = dir === 'asc' ? '▲' : dir === 'desc' ? '▼' : '⇅';
      btn.title = T('table.sort', '이 열로 정렬');
      btn.dataset.col = String(i);
      th.appendChild(btn);
    });

    // 아래 테두리를 위아래로 끌면 행이, 오른쪽 테두리를 좌우로 끌면 열이 늘고 준다
    const wrap = table.parentElement;
    const rowGrip = chrome('div', 'mtable-grip mtable-grip-row');
    rowGrip.title = T('table.gripRow', '위아래로 끌어 행 늘리기·줄이기');
    const colGrip = chrome('div', 'mtable-grip mtable-grip-col');
    colGrip.title = T('table.gripCol', '좌우로 끌어 열 늘리기·줄이기');
    wrap.appendChild(rowGrip);
    wrap.appendChild(colGrip);
    // 제목(caption)·설명이 붙으면 wrap과 표의 경계가 어긋난다 — 표의 실제 자리에 맞춘다
    const place = () => {
      const top = table.offsetTop, left = table.offsetLeft;
      const w = table.offsetWidth, h = table.offsetHeight;
      rowGrip.style.left = left + 'px';
      rowGrip.style.top = (top + h + 2) + 'px';
      rowGrip.style.width = w + 'px';
      colGrip.style.left = (left + w + 2) + 'px';
      colGrip.style.top = top + 'px';
      colGrip.style.height = h + 'px';
    };
    place();
    requestAnimationFrame(place);  // 글꼴·이미지가 자리를 잡은 뒤 한 번 더

    recompute(table);
  }

  function refreshAll() {
    if (!editor) return;
    tables().forEach(refresh);
  }

  // ---------- 입력 처리 ----------
  function onPointerDown(e) {
    // 오른쪽 버튼은 우클릭 메뉴의 몫이다. 여기서 선택을 지우면 메뉴의 행·열 삭제가
    // 방금 고른 영역을 잃는다.
    if (e.button !== 0) return;
    const sortBtn = closest(e.target, '.mtable-sort');
    if (sortBtn) {
      e.preventDefault();
      const table = tableOf(sortBtn);
      sortBy(table, Number(sortBtn.dataset.col));
      refresh(table);
      notify();
      return;
    }
    const grip = closest(e.target, '.mtable-grip');
    if (grip) { startGripDrag(e, grip); return; }

    const footCell = closest(e.target, 'tfoot td');
    if (footCell && tableOf(footCell)) {
      e.preventDefault();
      cycleStat(footCell);
      recompute(tableOf(footCell));
      notify();
      return;
    }
    // 통계 열: 머리글이든 칸이든 누르면 열 전체의 함수가 돈다
    const statCell = closest(e.target, '[data-statcell], th[data-statcol]');
    if (statCell && tableOf(statCell)) {
      e.preventDefault();
      const table = tableOf(statCell);
      cycleStatCol(table);
      refresh(table);
      notify();
      return;
    }

    const cell = cellOf(e.target);
    const table = cell && tableOf(cell);
    if (!table) { clearSelection(); return; }
    startCellDrag(e, table, cell);
  }

  function startGripDrag(e, grip) {
    e.preventDefault();
    const table = tableOf(grip) || grip.parentElement.querySelector('table.mtable');
    if (!table) return;
    const isRow = grip.classList.contains('mtable-grip-row');
    const startX = e.clientX, startY = e.clientY;
    const startN = isRow ? bodyRows(table).length : colCount(table);
    // 한 칸 크기: 지금 표에서 잰다 (글꼴·배율이 달라도 맞는다)
    const rect = table.getBoundingClientRect();
    const unit = isRow
      ? Math.max(18, rect.height / (bodyRows(table).length + 1))
      : Math.max(40, rect.width / colCount(table));
    const move = (ev) => {
      const delta = isRow ? ev.clientY - startY : ev.clientX - startX;
      const want = startN + Math.round(delta / unit);
      const cur = isRow ? bodyRows(table).length : colCount(table);
      if (want === cur) return;
      if (isRow) setRowCount(table, want); else setColCount(table, want);
      refresh(table);
    };
    const up = () => {
      document.removeEventListener('pointermove', move);
      document.removeEventListener('pointerup', up);
      notify();
    };
    document.addEventListener('pointermove', move);
    document.addEventListener('pointerup', up);
  }

  // 셀 안에서 끄는 것은 평범한 글자 선택이고, 다른 셀로 넘어가는 순간부터 셀 선택이 된다.
  function startCellDrag(e, table, anchorCell) {
    clearSelection();
    const anchor = cellPos(table, anchorCell);
    let dragging = false;
    const move = (ev) => {
      const overCell = cellOf(document.elementFromPoint(ev.clientX, ev.clientY));
      if (!overCell || tableOf(overCell) !== table) return;
      if (overCell === anchorCell && !dragging) return;
      dragging = true;
      ev.preventDefault();
      window.getSelection().removeAllRanges();  // 글자 선택과 겹치지 않게
      selectRange(table, anchor, cellPos(table, overCell));
    };
    const up = () => {
      document.removeEventListener('pointermove', move);
      document.removeEventListener('pointerup', up);
    };
    document.addEventListener('pointermove', move);
    document.addEventListener('pointerup', up);
  }

  // 표 안에서 편집하면 통계가 따라 바뀌어야 한다
  function onInput(e) {
    const table = tableOf(e.target) || tableOf(window.getSelection().anchorNode);
    if (table) recompute(table);
  }

  function init(el, changeCb) {
    editor = el;
    onChange = changeCb;
    editor.addEventListener('pointerdown', onPointerDown, true);
    editor.addEventListener('input', onInput);
    refreshAll();
  }

  // ---------- 마크다운 (AI 리뷰·번역용) ----------
  // 셀의 글자만 (정렬 버튼 같은 장식은 뺀다)
  function cellText(cell) {
    if (!cell.querySelector('[data-chrome]')) return cell.textContent;
    const copy = cell.cloneNode(true);
    copy.querySelectorAll('[data-chrome]').forEach((el) => el.remove());
    return copy.textContent;
  }

  function toMarkdown(table) {
    const line = (cells) => '| ' + cells.map((c) =>
      cellText(c).replace(/\|/g, '\\|').trim() || ' ').join(' | ') + ' |';
    const heads = [...table.tHead.rows[0].cells];
    const out = [];
    // 표 제목은 표 앞줄에 굵게 (설명은 wrap 안 div라 editor.js의 일반 블록 경로가 낸다)
    const cap = table.caption && table.caption.textContent.trim();
    if (cap) out.push('**' + cap + '**', '');
    out.push(line(heads), '| ' + heads.map(() => '---').join(' | ') + ' |');
    bodyRows(table).forEach((tr) => out.push(line([...tr.cells])));
    if (table.tFoot) out.push(line([...table.tFoot.rows[0].cells]));
    return out;
  }

  return {
    init, insert, create, refresh, refreshAll, toMarkdown, cellText,
    tableOf, cellOf, cellPos,
    insertRow, deleteRows, insertCol, deleteCols,
    selectedRows, selectedCols, selectedCells, clearSelection,
    toggleStats, hasStats, bodyRows, colCount, recompute,
    hasRowHeader, toggleRowHeader, hasStatCol, toggleStatCol,
    hasCaption, toggleCaption, hasDesc, toggleDesc,
  };
})();
