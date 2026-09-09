// 메모창 스타일 — 배경 이미지·서체·글자 크기. 설정(settings.style)이 단일 출처이고,
// 페이지는 init JSON으로 첫 그림부터 적용하고 style.changed 이벤트로 갈아입는다.
//
// 값은 CSS 변수로만 흘린다: --memo-font, --memo-font-size, --memo-bg-image.
// base.css가 이 변수를 읽고(없으면 기본값) sticker.css가 배경을 그린다.
const appStyle = (() => {
  // 함께 배포하는 서체 (전부 SIL OFL 1.1, ui/fonts/licenses). id는 설정에 저장되는 값이다.
  const FONTS = [
    { id: 'gowun-dodum', family: 'Gowun Dodum', file: 'GowunDodum-Regular.woff2', kind: 'sans' },
    { id: 'sunflower', family: 'Sunflower', file: 'Sunflower-Medium.woff2', kind: 'sans' },
    { id: 'gowun-batang', family: 'Gowun Batang', file: 'GowunBatang-Regular.woff2', kind: 'serif' },
    { id: 'nanum-myeongjo', family: 'Nanum Myeongjo', file: 'NanumMyeongjo-Regular.woff2', kind: 'serif' },
    { id: 'do-hyeon', family: 'Do Hyeon', file: 'DoHyeon-Regular.woff2', kind: 'display' },
    { id: 'black-han-sans', family: 'Black Han Sans', file: 'BlackHanSans-Regular.woff2', kind: 'display' },
    { id: 'jua', family: 'Jua', file: 'Jua-Regular.woff2', kind: 'cute' },
    { id: 'gaegu', family: 'Gaegu', file: 'Gaegu-Regular.woff2', kind: 'hand' },
    { id: 'hi-melody', family: 'Hi Melody', file: 'HiMelody-Regular.woff2', kind: 'cute' },
    { id: 'nanum-pen', family: 'Nanum Pen Script', file: 'NanumPenScript-Regular.woff2', kind: 'hand' },
  ];
  // 배경 프리셋 (ui/bg/*.svg — 이 앱을 위해 그린 원본). 이름은 로케일 style.bg.<id>.
  const PRESETS = ['cream-blobs', 'cat-doodle', 'night-stars', 'coffee-time', 'clouds',
                   'memphis', 'grid-paper', 'sakura', 'bear-friends', 'waves'];
  const FALLBACK = '"Pretendard Variable", "Segoe UI Variable", "Segoe UI", "Malgun Gothic", system-ui, sans-serif';
  const DEFAULT_SIZE = 14;
  const loaded = new Set();

  // 서체를 처음 쓸 때만 @font-face를 붙인다 (미리 다 올리면 4MB를 매번 읽는다)
  function ensureFace(f) {
    if (loaded.has(f.id)) return;
    loaded.add(f.id);
    const st = document.createElement('style');
    st.textContent = `@font-face{font-family:"${f.family}";src:url("/fonts/${f.file}") format("woff2");font-display:block;}`;
    document.head.appendChild(st);
  }

  const fontOf = (id) => FONTS.find((f) => f.id === id) || null;
  // 서체 id → CSS font-family 값 (설정 미리보기도 같은 문자열을 쓴다)
  function familyOf(id) {
    const f = fontOf(id);
    return f ? `"${f.family}", ${FALLBACK}` : FALLBACK;
  }
  // 프리셋 배경 URL
  function presetUrl(id) { return `/bg/${id}.svg`; }
  // 설정값 → 배경 이미지 URL (프리셋/사용자 파일)
  function backgroundUrl(bg) {
    if (!bg) return '';
    if (bg.startsWith('preset:')) {
      const id = bg.slice(7);
      return PRESETS.includes(id) ? presetUrl(id) : '';
    }
    if (bg.startsWith('file:')) return 'https://data.sticker/style/' + encodeURIComponent(bg.slice(5));
    return '';
  }

  // 설정을 이 문서에 반영한다. 빈 값은 기본으로 되돌린다.
  function apply(style) {
    const s = style || {};
    const root = document.documentElement;
    const f = fontOf(s.font);
    if (f) {
      ensureFace(f);
      root.style.setProperty('--memo-font', familyOf(f.id));
    } else {
      root.style.removeProperty('--memo-font');
    }
    const size = Number(s.fontSize) || 0;
    if (size >= 11 && size <= 24) root.style.setProperty('--memo-font-size', size + 'px');
    else root.style.removeProperty('--memo-font-size');
    const url = backgroundUrl(s.background);
    if (url) root.style.setProperty('--memo-bg-image', `url("${url}")`);
    else root.style.removeProperty('--memo-bg-image');
    root.classList.toggle('has-bg', !!url);
  }

  return { FONTS, PRESETS, DEFAULT_SIZE, apply, familyOf, fontOf, presetUrl, backgroundUrl, ensureFace };
})();
