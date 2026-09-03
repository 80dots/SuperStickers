// 리치 메모의 캘린더. 넣는 방법은 표와 같다(툴바 버튼 / 본문 우클릭).
//
// **표와 다른 점**: 표는 칸 자체가 본문이라 그대로 저장되지만, 캘린더의 격자는 날짜에서
// 계산해 그리는 것이다. 그래서 저장되는 것은 **데이터뿐**이고 화면은 열 때마다 다시 그린다:
//
//   <div class="mcal" contenteditable="false"
//        data-view="month|week|day" data-date="2026-09-03"
//        data-events='[{"id","d","end","allDay","t","et","title","loc","people","alarm"}]'></div>
//
// 일정 하나의 필드: d(시작일) · end(종료일, 없으면 하루) · allDay(종일) · t/et(시작·종료 시각)
//   title · loc(장소) · people(참가자) · alarm(시작 몇 분 전에 알릴지, 분 단위 문자열)
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
  // 그 날짜에 걸치는 일정 (기간 일정은 시작~종료 사이 모든 날에 나온다)
  const endOf = (e) => (e.end && e.end >= e.d ? e.end : e.d);
  const spans = (e, day) => day >= e.d && day <= endOf(e);
  const eventsOn = (el, day) => events(el).filter((e) => spans(e, day));
  const isMulti = (e) => endOf(e) !== e.d;

  // ---------- 국경일 ----------
  // 앱이 네이티브에서 받은 사용자 지역(__init.country)으로 고른다.
  // **날짜가 해마다 달라지는 명절(설날·추석 등)은 넣지 않았다** — 음력 계산 표가 필요하고,
  // 확실하지 않은 날짜를 넣느니 비워 두는 편이 낫다.
  const HOLIDAYS = {
    KR: { '01-01': '신정', '03-01': '삼일절', '05-05': '어린이날', '06-06': '현충일',
          '08-15': '광복절', '10-03': '개천절', '10-09': '한글날', '12-25': '성탄절' },
    US: { '01-01': "New Year's Day", '06-19': 'Juneteenth', '07-04': 'Independence Day',
          '11-11': 'Veterans Day', '12-25': 'Christmas Day' },
    JP: { '01-01': '元日', '02-11': '建国記念の日', '04-29': '昭和の日', '05-03': '憲法記念日',
          '05-04': 'みどりの日', '05-05': 'こどもの日', '08-11': '山の日', '11-03': '文化の日',
          '11-23': '勤労感謝の日' },
  };
  const country = () => {
    const c = (typeof window !== 'undefined' && window.__init && window.__init.country) || '';
    return String(c).toUpperCase();
  };
  function holidayOn(day) {
    const table = HOLIDAYS[country()];
    return table ? table[String(day).slice(5)] : undefined;
  }

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

  // 화면·툴팁에 쓰는 한 줄 설명
  function summarize(ev) {
    const parts = [];
    if (isMulti(ev)) parts.push(ev.d + ' ~ ' + endOf(ev));
    if (!ev.allDay && ev.t) parts.push(ev.t + (ev.et ? '–' + ev.et : ''));
    if (ev.allDay) parts.push(T('cal.allDay', '종일'));
    if (ev.title) parts.push(ev.title);
    if (ev.loc) parts.push('@' + ev.loc);
    if (ev.people) parts.push('(' + ev.people + ')');
    return parts.join(' ');
  }

  function chip(ev) {
    const c = tag('div', 'mcal-chip' + (isMulti(ev) ? ' span' : ''));
    c.dataset.id = ev.id;
    if (!ev.allDay && ev.t) c.appendChild(tag('span', 'mcal-chip-t', ev.t));
    c.appendChild(tag('span', 'mcal-chip-x', ev.title || ''));
    if (ev.alarm) c.appendChild(tag('span', 'mcal-chip-a', '🔔'));
    c.title = summarize(ev);
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
    const hol = holidayOn(iso(day));
    if (hol) {
      cell.classList.add('holiday');
      const h = tag('div', 'mcal-hol', hol);
      h.title = hol;
      cell.appendChild(h);
    }
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
    // 주간은 칸을 넓게 쓰고 가로로 스크롤한다 (좁은 메모창에서 글자가 뭉개지지 않게)
    const scroller = tag('div', 'mcal-hscroll');
    const grid = tag('div', 'mcal-grid mcal-week');
    for (let i = 0; i < 7; i++) {
      const wd = addDays(start, i);
      grid.appendChild(tag('div', 'mcal-wd' + (i === 0 ? ' sun' : i === 6 ? ' sat' : ''),
                           wd.toLocaleDateString(locale(), { weekday: 'short', day: 'numeric' })));
    }
    for (let i = 0; i < 7; i++) grid.appendChild(dayCell(el, addDays(start, i), null, ' tall'));
    scroller.appendChild(grid);
    body.appendChild(scroller);
  }

  function renderDay(el, body) {
    const day = dateOf(el);
    const list = eventsOn(el, iso(day));
    const box = tag('div', 'mcal-daylist');
    box.dataset.date = iso(day);
    const hol = holidayOn(iso(day));
    if (hol) box.appendChild(tag('div', 'mcal-hol-row', hol));
    if (!list.length) {
      box.appendChild(tag('div', 'mcal-empty', T('cal.noEvents', '일정이 없습니다. 눌러서 추가하세요.')));
    }
    list.forEach((ev) => {
      const row = tag('div', 'mcal-row');
      row.dataset.id = ev.id;
      const when = ev.allDay || !ev.t ? T('cal.allDay', '종일')
                                      : ev.t + (ev.et ? '–' + ev.et : '');
      row.appendChild(tag('span', 'mcal-row-t', when));
      const main = tag('div', 'mcal-row-main');
      main.appendChild(tag('div', 'mcal-row-x', ev.title || ''));
      const meta = [];
      if (isMulti(ev)) meta.push(ev.d + ' ~ ' + endOf(ev));
      if (ev.loc) meta.push('📍 ' + ev.loc);
      if (ev.people) meta.push('👤 ' + ev.people);
      if (ev.alarm) meta.push('🔔 ' + alarmLabel(ev.alarm));
      if (meta.length) main.appendChild(tag('div', 'mcal-row-meta', meta.join('   ')));
      row.appendChild(main);
      box.appendChild(row);
    });
    body.appendChild(box);
  }

  const VIEWS = [['month', 'cal.month', '월'], ['week', 'cal.week', '주'], ['day', 'cal.day', '일']];

  // 알람: 시작 몇 분 전에 알릴지 (분 단위 문자열)
  const ALARMS = [['', 'cal.alarmNone', '알림 없음'], ['0', 'cal.alarmAt', '시작할 때'],
                  ['10', 'cal.alarm10', '10분 전'], ['30', 'cal.alarm30', '30분 전'],
                  ['60', 'cal.alarm60', '1시간 전'], ['1440', 'cal.alarm1d', '하루 전']];
  function alarmLabel(v) {
    const hit = ALARMS.find((a) => a[0] === String(v));
    return hit ? T(hit[1], hit[2]) : '';
  }

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

    const input = (type, value, ph) => {
      const i = document.createElement('input');
      i.type = type;
      if (value) i.value = value;
      if (ph) i.placeholder = ph;
      return i;
    };
    const row = (cls) => tag('div', 'mcal-erow' + (cls ? ' ' + cls : ''));
    const label = (key, fb) => tag('span', 'mcal-elab', T(key, fb));

    // 1줄: 기간 (시작일 ~ 종료일) + 종일
    const date = input('date', ev.d || iso(dateOf(el)));
    const endDate = input('date', ev.end || '');
    const allDay = input('checkbox');
    allDay.checked = !!ev.allDay || (!ev.t && !!ev.id);
    const allDayLab = tag('label', 'mcal-check');
    allDayLab.appendChild(allDay);
    allDayLab.appendChild(document.createTextNode(T('cal.allDay', '종일')));
    const r1 = row();
    r1.appendChild(date);
    r1.appendChild(tag('span', 'mcal-tilde', '~'));
    r1.appendChild(endDate);
    r1.appendChild(allDayLab);
    form.appendChild(r1);

    // 2줄: 시간 (종일이면 잠근다)
    const time = input('time', ev.t || '');
    const endTime = input('time', ev.et || '');
    const r2 = row();
    r2.appendChild(time);
    r2.appendChild(tag('span', 'mcal-tilde', '~'));
    r2.appendChild(endTime);
    const alarm = document.createElement('select');
    ALARMS.forEach(([v, key, fb]) => {
      const o = document.createElement('option');
      o.value = v;
      o.textContent = T(key, fb);
      if (String(ev.alarm || '') === v) o.selected = true;
      alarm.appendChild(o);
    });
    r2.appendChild(alarm);
    form.appendChild(r2);
    const syncAllDay = () => { time.disabled = endTime.disabled = allDay.checked; };
    allDay.addEventListener('change', syncAllDay);
    syncAllDay();

    // 3줄: 내용
    const title = input('text', ev.title || '', T('cal.titlePlaceholder', '일정 내용'));
    const r3 = row();
    r3.appendChild(title);
    form.appendChild(r3);

    // 4줄: 장소 · 참가자
    const loc = input('text', ev.loc || '', T('cal.locPlaceholder', '장소'));
    const people = input('text', ev.people || '', T('cal.peoplePlaceholder', '참가자'));
    const r4 = row();
    r4.appendChild(loc);
    r4.appendChild(people);
    form.appendChild(r4);

    const save = btn('mcal-save', T('cal.save', '저장'));
    const del = btn('mcal-del', T('cal.delete', '삭제'));
    if (!ev.id) del.style.display = 'none';
    const r5 = row('mcal-erow-btns');
    r5.appendChild(save);
    r5.appendChild(del);
    form.appendChild(r5);

    const commit = () => {
      const list = events(el).filter((x) => x.id !== ev.id);
      const text = title.value.trim();
      if (text) {
        const start = date.value || iso(dateOf(el));
        const e = { id: ev.id || ('e' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6)),
                    d: start, title: text };
        if (endDate.value && endDate.value > start) e.end = endDate.value;
        if (allDay.checked) e.allDay = true;
        else {
          if (time.value) e.t = time.value;
          if (endTime.value) e.et = endTime.value;
        }
        if (loc.value.trim()) e.loc = loc.value.trim();
        if (people.value.trim()) e.people = people.value.trim();
        if (alarm.value) e.alarm = alarm.value;
        list.push(e);
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

  // ---------- 알람 ----------
  // 메모 안의 모든 캘린더에서 "언제 무엇을 알릴지"만 뽑는다. 네이티브 타이머가 이 목록을 보고
  // 트레이 알림을 띄운다 — 창이 없는 그룹 소속 메모도 울려야 하므로 본문 HTML이 아니라
  // 메모 메타(calAlarms)에 넣어 둔다.
  function collectAlarms() {
    const out = [];
    if (!editor) return out;
    [...editor.querySelectorAll('.mcal')].forEach((el) => {
      events(el).forEach((ev) => {
        if (!ev.alarm) return;
        // 종일 일정은 그날 09:00을 기준으로 삼는다 (시작 시각이 없다)
        const base = parse(ev.d);
        const hm = (!ev.allDay && ev.t) ? ev.t.split(':') : ['9', '0'];
        base.setHours(Number(hm[0]) || 0, Number(hm[1]) || 0, 0, 0);
        const at = new Date(base.getTime() - Number(ev.alarm) * 60000);
        out.push({
          id: ev.id,
          at: iso(at) + 'T' + pad(at.getHours()) + ':' + pad(at.getMinutes()),
          title: ev.title || '',
          when: summarize(ev),
        });
      });
    });
    return out;
  }

  // ---------- 마크다운 (AI 리뷰용) ----------
  // 격자는 그림이라 옮길 수 없다 — 기간과 일정 목록만 낸다.
  function toMarkdown(el) {
    const out = ['**' + T('cal.title', '캘린더') + ' — ' + periodLabel(el) + '**'];
    const list = events(el);
    if (!list.length) return out;
    out.push('');
    list.forEach((ev) => out.push('- ' + summarize(ev)));
    return out;
  }

  return { init, insert, create, render, renderAll, toMarkdown, setView, goToday, step,
           viewOf, events, openEditor, collectAlarms, summarize, holidayOn, country };
})();
