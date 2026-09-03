// 리치 메모의 캘린더. 넣는 방법은 표와 같다(툴바 버튼 / 본문 우클릭).
//
// **표와 다른 점**: 표는 칸 자체가 본문이라 그대로 저장되지만, 캘린더의 격자는 날짜에서
// 계산해 그리는 것이다. 그래서 저장되는 것은 **데이터뿐**이고 화면은 열 때마다 다시 그린다:
//
//   <div class="mcal" contenteditable="false"
//        data-view="month|week|day" data-date="2026-09-03"
//        data-events='[{"d":"2026-09-03","t":"14:00","title":"회의"}]'></div>
//
// 안에 그려 넣는 것은 전부 data-chrome이라 editorCore.getHtml()이 걷어낸다.
const calendarTools = (() => {
  let editor = null;
  let onChange = null;
  const T = (k, fallback) => {
    const v = typeof i18n !== 'undefined' ? i18n.t(k) : k;
    return v === k ? fallback : v;
  };
  const locale = () => ((typeof i18n !== 'undefined' && i18n.lang) === 'en' ? 'en-US' : 'ko-KR');
  const notify = () => { if (onChange) onChange(); };

  // ---------- 날짜 (전부 현지 시각 기준 YYYY-MM-DD 문자열) ----------
  const pad = (n) => String(n).padStart(2, '0');
  const iso = (d) => d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate());
  function parse(s) {
    const p = String(s || '').split('-').map(Number);
    const d = new Date(p[0] || 1970, (p[1] || 1) - 1, p[2] || 1);
    return isNaN(d) ? new Date() : d;
  }
  const addDays = (d, n) => { const x = new Date(d); x.setDate(x.getDate() + n); return x; };
  const addMonths = (d, n) => {
    const x = new Date(d.getFullYear(), d.getMonth() + n, 1);
    x.setDate(Math.min(d.getDate(), new Date(x.getFullYear(), x.getMonth() + 1, 0).getDate()));
    return x;
  };
  const weekStart = (d) => addDays(d, -d.getDay());  // 일요일 시작
  const sameDay = (a, b) => iso(a) === iso(b);

  // ---------- 데이터 ----------
  function events(el) {
    try {
      const v = JSON.parse(el.dataset.events || '[]');
      return Array.isArray(v) ? v.filter((e) => e && e.d) : [];
    } catch (err) {
      return [];
    }
  }
  function setEvents(el, list) {
    list.sort((a, b) => ((a.d + (a.t || '')) < (b.d + (b.t || '')) ? -1 : 1));
    el.dataset.events = JSON.stringify(list);
  }
  const eventsOn = (el, day) => events(el).filter((e) => e.d === day);

  const viewOf = (el) => (['month', 'week', 'day'].includes(el.dataset.view) ? el.dataset.view : 'month');
  const dateOf = (el) => parse(el.dataset.date || iso(new Date()));

  // ---------- 만들기 ----------
  function create() {
    const el = document.createElement('div');
    el.className = 'mcal';
    el.contentEditable = 'false';
    el.dataset.view = 'month';
    el.dataset.date = iso(new Date());
    el.dataset.events = '[]';
    return el;
  }

  function insert() {
    const el = create();
    const after = document.createElement('div');
    after.innerHTML = '<br>';
    editorCore.insertNodeAtCaret(after);
    editorCore.insertNodeAtCaret(el);
    render(el);
    notify();
    return el;
  }

  // ---------- 그리기 (전부 data-chrome — 저장되지 않는다) ----------
  function tag(name, cls, text) {
    const e = document.createElement(name);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function btn(cls, text, title) {
    const b = tag('button', cls, text);
    b.type = 'button';
    if (title) b.title = title;
    return b;
  }

  function periodLabel(el) {
    const d = dateOf(el);
    const view = viewOf(el);
    if (view === 'month') {
      return d.toLocaleDateString(locale(), { year: 'numeric', month: 'long' });
    }
    if (view === 'week') {
      const s = weekStart(d), e = addDays(s, 6);
      const fmt = (x) => x.toLocaleDateString(locale(), { month: 'short', day: 'numeric' });
      return fmt(s) + ' – ' + fmt(e);
    }
    return d.toLocaleDateString(locale(), { year: 'numeric', month: 'long', day: 'numeric',
                                            weekday: 'long' });
  }

  function chip(ev) {
    const c = tag('div', 'mcal-chip');
    c.dataset.id = ev.id;
    if (ev.t) c.appendChild(tag('span', 'mcal-chip-t', ev.t));
    c.appendChild(tag('span', 'mcal-chip-x', ev.title || ''));
    c.title = (ev.t ? ev.t + ' ' : '') + (ev.title || '');
    return c;
  }

  function dayCell(el, day, cur, cls) {
    const cell = tag('div', 'mcal-day' + (cls || ''));
    cell.dataset.date = iso(day);
    if (cur && day.getMonth() !== cur.getMonth()) cell.classList.add('out');
    if (sameDay(day, new Date())) cell.classList.add('today');
    if (day.getDay() === 0) cell.classList.add('sun');
    if (day.getDay() === 6) cell.classList.add('sat');
    cell.appendChild(tag('div', 'mcal-num', String(day.getDate())));
    const list = eventsOn(el, iso(day));
    list.slice(0, 3).forEach((ev) => cell.appendChild(chip(ev)));
    if (list.length > 3) {
      cell.appendChild(tag('div', 'mcal-more', '+' + (list.length - 3)));
    }
    return cell;
  }

  function renderMonth(el, body) {
    const cur = dateOf(el);
    const first = new Date(cur.getFullYear(), cur.getMonth(), 1);
    const start = weekStart(first);
    const lastDay = new Date(cur.getFullYear(), cur.getMonth() + 1, 0);
    const weeks = Math.ceil((first.getDay() + lastDay.getDate()) / 7);
    const grid = tag('div', 'mcal-grid');
    for (let i = 0; i < 7; i++) {
      const wd = addDays(start, i);
      grid.appendChild(tag('div', 'mcal-wd' + (i === 0 ? ' sun' : i === 6 ? ' sat' : ''),
                           wd.toLocaleDateString(locale(), { weekday: 'short' })));
    }
    for (let i = 0; i < weeks * 7; i++) grid.appendChild(dayCell(el, addDays(start, i), cur));
    body.appendChild(grid);
  }

  function renderWeek(el, body) {
    const start = weekStart(dateOf(el));
    const grid = tag('div', 'mcal-grid mcal-week');
    for (let i = 0; i < 7; i++) {
      const wd = addDays(start, i);
      grid.appendChild(tag('div', 'mcal-wd' + (i === 0 ? ' sun' : i === 6 ? ' sat' : ''),
                           wd.toLocaleDateString(locale(), { weekday: 'short', day: 'numeric' })));
    }
    for (let i = 0; i < 7; i++) grid.appendChild(dayCell(el, addDays(start, i), null, ' tall'));
    body.appendChild(grid);
  }

  function renderDay(el, body) {
    const day = dateOf(el);
    const list = eventsOn(el, iso(day));
    const box = tag('div', 'mcal-daylist');
    box.dataset.date = iso(day);
    if (!list.length) {
      box.appendChild(tag('div', 'mcal-empty', T('cal.noEvents', '일정이 없습니다. 눌러서 추가하세요.')));
    }
    list.forEach((ev) => {
      const row = tag('div', 'mcal-row');
      row.dataset.id = ev.id;
      row.appendChild(tag('span', 'mcal-row-t', ev.t || T('cal.allDay', '종일')));
      row.appendChild(tag('span', 'mcal-row-x', ev.title || ''));
      box.appendChild(row);
    });
    body.appendChild(box);
  }

  const VIEWS = [['month', 'cal.month', '월'], ['week', 'cal.week', '주'], ['day', 'cal.day', '일']];

  function render(el) {
    el.querySelectorAll('[data-chrome]').forEach((n) => n.remove());
    const ui = tag('div', 'mcal-ui');
    ui.dataset.chrome = '1';

    const head = tag('div', 'mcal-head');
    head.appendChild(btn('mcal-nav', '‹', T('cal.prev', '이전')));
    head.appendChild(btn('mcal-nav', '›', T('cal.next', '다음')));
    head.appendChild(btn('mcal-today', T('cal.today', '오늘')));
    head.appendChild(tag('div', 'mcal-title', periodLabel(el)));
    const seg = tag('div', 'mcal-views');
    VIEWS.forEach(([v, key, fb]) => {
      const b = btn('mcal-view' + (viewOf(el) === v ? ' on' : ''), T(key, fb));
      b.dataset.view = v;
      seg.appendChild(b);
    });
    head.appendChild(seg);
    head.appendChild(btn('mcal-add', '+', T('cal.add', '일정 추가')));
    ui.appendChild(head);

    const body = tag('div', 'mcal-body');
    const view = viewOf(el);
    if (view === 'month') renderMonth(el, body);
    else if (view === 'week') renderWeek(el, body);
    else renderDay(el, body);
    ui.appendChild(body);

    el.appendChild(ui);
  }

  const calendars = () => (editor ? [...editor.querySelectorAll('.mcal')] : []);
  function renderAll() { calendars().forEach(render); }

  // ---------- 일정 편집기 ----------
  function closeEditor(el) {
    const f = el.querySelector('.mcal-editor');
    if (f) f.remove();
  }

  function openEditor(el, ev) {
    closeEditor(el);
    const form = tag('div', 'mcal-editor');
    form.dataset.chrome = '1';
    const date = document.createElement('input');
    date.type = 'date';
    date.value = ev.d || iso(dateOf(el));
    const time = document.createElement('input');
    time.type = 'time';
    time.value = ev.t || '';
    const title = document.createElement('input');
    title.type = 'text';
    title.placeholder = T('cal.titlePlaceholder', '일정 내용');
    title.value = ev.title || '';
    form.appendChild(date);
    form.appendChild(time);
    form.appendChild(title);

    const save = btn('mcal-save', T('cal.save', '저장'));
    const del = btn('mcal-del', T('cal.delete', '삭제'));
    if (!ev.id) del.style.display = 'none';
    form.appendChild(save);
    form.appendChild(del);

    const commit = () => {
      const list = events(el).filter((x) => x.id !== ev.id);
      const text = title.value.trim();
      if (text) {
        list.push({ id: ev.id || ('e' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6)),
                    d: date.value || iso(dateOf(el)), t: time.value || '', title: text });
      }
      setEvents(el, list);
      render(el);
      notify();
    };
    save.addEventListener('click', commit);
    del.addEventListener('click', () => {
      setEvents(el, events(el).filter((x) => x.id !== ev.id));
      render(el);
      notify();
    });
    title.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); commit(); }
      if (e.key === 'Escape') { e.preventDefault(); closeEditor(el); }
    });

    el.appendChild(form);
    title.focus();
  }

  // ---------- 조작 ----------
  function setView(el, view) {
    el.dataset.view = view;
    render(el);
    notify();
  }
  function goToday(el) {
    el.dataset.date = iso(new Date());
    render(el);
    notify();
  }
  function step(el, dir) {
    const view = viewOf(el);
    const d = dateOf(el);
    el.dataset.date = iso(view === 'month' ? addMonths(d, dir)
                          : addDays(d, view === 'week' ? 7 * dir : dir));
    render(el);
    notify();
  }

  function onClick(e) {
    const el = e.target.closest && e.target.closest('.mcal');
    if (!el || !editor.contains(el)) return;
    if (e.target.closest('.mcal-editor')) return;  // 편집기 안은 그대로 둔다

    const nav = e.target.closest('.mcal-nav');
    if (nav) { step(el, nav.textContent === '›' ? 1 : -1); return; }
    if (e.target.closest('.mcal-today')) { goToday(el); return; }
    const view = e.target.closest('.mcal-view');
    if (view) { setView(el, view.dataset.view); return; }
    if (e.target.closest('.mcal-add')) { openEditor(el, { d: iso(dateOf(el)) }); return; }

    // 일정 하나를 누르면 그 일정을 고친다
    const item = e.target.closest('.mcal-chip, .mcal-row');
    if (item) {
      const ev = events(el).find((x) => x.id === item.dataset.id);
      if (ev) { openEditor(el, ev); return; }
    }
    // 빈 날짜 칸을 누르면 그 날짜로 새 일정
    const cell = e.target.closest('.mcal-day, .mcal-daylist');
    if (cell) { openEditor(el, { d: cell.dataset.date }); return; }
    closeEditor(el);
  }

  function init(el, changeCb) {
    editor = el;
    onChange = changeCb;
    editor.addEventListener('click', onClick);
    renderAll();
  }

  // ---------- 마크다운 (AI 리뷰용) ----------
  // 격자는 그림이라 옮길 수 없다 — 기간과 일정 목록만 낸다.
  function toMarkdown(el) {
    const out = ['**' + T('cal.title', '캘린더') + ' — ' + periodLabel(el) + '**'];
    const list = events(el);
    if (!list.length) return out;
    out.push('');
    list.forEach((ev) => out.push(`- ${ev.d}${ev.t ? ' ' + ev.t : ''} ${ev.title || ''}`.trim()));
    return out;
  }

  return { init, insert, create, render, renderAll, toMarkdown, setView, goToday, step,
           viewOf, events, openEditor };
})();
