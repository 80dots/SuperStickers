// 메모창 테두리 — 창 가장자리 여백(리사이즈 밴드)과 다중 선택 테두리를 페이지가 그린다.
//
// 예전에는 네이티브 창이 가장자리 6px를 GDI로 칠하고 WebView를 그 안쪽에 두었다. 두 표면은
// 서로 다른 프로세스가 서로 다른 순간에 화면에 올리므로, 창이 뜨거나 색이 바뀌는 순간
// 한쪽만 바뀌어 두꺼운 테두리처럼 보였다. 이제 WebView가 창 전체를 덮고 밴드는 body의
// 여백이다. WebView가 마우스를 모두 받으므로 크기 조절은 여기서 네이티브에 넘긴다.
//
// <head>에서 불린다 — 첫 그림부터 메모 색과 여백이 맞아야 하기 때문 (color.js가 먼저 로드된다).
(function () {
  const init = window.__init || {};
  const root = document.documentElement;

  // 메모 색: sticker.load 응답을 기다리면 그 사이 theme.css의 폴백 색이 한 번 그려진다
  if (init.color) colorUtil.apply(init.color, false);

  // 치수는 네이티브가 물리 px로 준다 (StickerWindow::WindowMetricsJson). CSS px로 옮길 때
  // devicePixelRatio(=DPI 배율 × UI 배율)로 나눠야 네이티브의 반올림과 정확히 맞는다.
  let metrics = init.winMetrics || { band: 6, ring: 3, radius: 8 };
  function applyMetrics() {
    const dpr = window.devicePixelRatio || 1;
    root.style.setProperty('--frame', metrics.band / dpr + 'px');
    root.style.setProperty('--sel-ring', metrics.ring / dpr + 'px');
    // 테두리 바깥 곡선이 DWM 라운드 모서리를 따르도록 (그룹창 드롭 하이라이트와 같은 모양)
    root.style.setProperty('--win-radius', (metrics.radius + metrics.ring / 2) / dpr + 'px');
  }
  applyMetrics();
  // DPI·UI 배율이 바뀌면 devicePixelRatio가 바뀌고 창 크기도 함께 바뀐다
  window.addEventListener('resize', applyMetrics);

  const EDGES = ['n', 's', 'w', 'e', 'nw', 'ne', 'sw', 'se'];

  // 가장자리 영역에서 일어난 입력인가
  const edgeOf = (e) => {
    const t = e.target;
    return t && t.classList && t.classList.contains('wf-zone') ? t.dataset.edge : '';
  };

  function build() {
    // body 밖(<html> 바로 아래)에 둔다 — body의 규칙(최소화 시 제목줄 외 숨김, UI 페이드)이
    // 조작 영역과 선택 테두리까지 가리지 않게
    const frame = document.createElement('div');
    frame.id = 'winFrame';
    for (const edge of EDGES) {
      const zone = document.createElement('div');
      zone.className = 'wf-zone wf-' + edge;
      zone.dataset.edge = edge;
      frame.appendChild(zone);
    }
    root.appendChild(frame);

    // 예전 밴드는 네이티브 영역이라 페이지가 클릭을 전혀 받지 않았다. 같은 동작을 지키려고
    // window 캡처 단계에서 먼저 받아 문서의 다른 처리(선택 해제·UI 표시·팝오버 닫기)로 흘리지 않는다.
    window.addEventListener('mousedown', (e) => {
      const edge = edgeOf(e);
      if (!edge) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      if (e.button === 0) bridge.call('window.startResize', { edge }).catch(() => {});
    }, true);
    // pointerdown은 preventDefault하면 뒤따르는 mousedown이 사라지므로 전파만 막는다
    for (const type of ['pointerdown', 'mouseup', 'click', 'dblclick', 'contextmenu']) {
      window.addEventListener(type, (e) => {
        if (!edgeOf(e)) return;
        if (type !== 'pointerdown') e.preventDefault();
        e.stopImmediatePropagation();
      }, true);
    }

    bridge.on('window.metrics', (d) => { metrics = d; applyMetrics(); });
    bridge.call('window.getMetrics')
      .then((d) => { metrics = d; applyMetrics(); })
      .catch(() => {});

    // 다중 선택 테두리 — 네이티브가 방송하는 선택 목록에 이 메모가 있으면 켠다
    bridge.on('selection.changed', (d) => {
      root.classList.toggle('win-selected',
        Array.isArray(d.ids) && d.ids.includes(init.stickerId));
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', build);
  else build();
})();
