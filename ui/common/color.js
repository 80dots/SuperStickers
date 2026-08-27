// 스티커 색상 유틸. 다크 변형 알고리즘은 src/Theme.cpp의 StickerColor()와 동일하게 유지할 것.
const colorUtil = (() => {
  // 20개 프리셋 (라이트 기준 파스텔)
  const PRESETS = [
    '#FFF4B8', '#FFE3A3', '#FFD8B5', '#FFCFC2', '#FFC9C9',
    '#FFD9E3', '#F8C8DC', '#E6D9FF', '#D9CCFF', '#CFE5FF',
    '#C7E8FF', '#C2F0F0', '#BFEEE0', '#C8F0DC', '#D3F2C2',
    '#E4F7B8', '#E8D5C4', '#E0DCD3', '#EAEAEA', '#FFFFFF',
  ];

  const LEGACY = {
    yellow: '#FFF4B8', mint: '#C8F0DC', pink: '#FFD9E3', blue: '#CFE5FF', gray: '#EAEAEA',
  };

  function normalize(color) {
    if (!color) return PRESETS[0];
    if (color[0] === '#') return color.length === 7 ? color.toUpperCase() : PRESETS[0];
    return LEGACY[color] || PRESETS[0];
  }

  function hexToRgb(hex) {
    return [
      parseInt(hex.slice(1, 3), 16),
      parseInt(hex.slice(3, 5), 16),
      parseInt(hex.slice(5, 7), 16),
    ];
  }

  function rgbToHex([r, g, b]) {
    const h = (v) => Math.round(v).toString(16).padStart(2, '0');
    return ('#' + h(r) + h(g) + h(b)).toUpperCase();
  }

  function rgbToHsl([r, g, b]) {
    r /= 255; g /= 255; b /= 255;
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b);
    const l = (mx + mn) / 2;
    let h = 0, s = 0;
    if (mx !== mn) {
      const d = mx - mn;
      s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
      if (mx === r) h = (g - b) / d + (g < b ? 6 : 0);
      else if (mx === g) h = (b - r) / d + 2;
      else h = (r - g) / d + 4;
      h /= 6;
    }
    return [h, s, l];
  }

  function hslToRgb([h, s, l]) {
    if (s === 0) return [l * 255, l * 255, l * 255];
    const hue2rgb = (p, q, t) => {
      if (t < 0) t += 1;
      if (t > 1) t -= 1;
      if (t < 1 / 6) return p + (q - p) * 6 * t;
      if (t < 1 / 2) return q;
      if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
      return p;
    };
    const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const p = 2 * l - q;
    return [hue2rgb(p, q, h + 1 / 3) * 255, hue2rgb(p, q, h) * 255, hue2rgb(p, q, h - 1 / 3) * 255];
  }

  // 유효 배경색: 테마와 무관하게 사용자가 고른 색 그대로 (Theme.cpp StickerColor와 동일)
  function effectiveBg(color, dark) {
    return normalize(color);
  }

  // 배경 대비에 따른 글자색
  function textColorFor(bgHex) {
    const [r, g, b] = hexToRgb(bgHex);
    const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
    return lum < 0.5 ? '#ECECEC' : '#1F2328';
  }

  // 문서에 스티커 색 적용 (CSS 변수 설정)
  function apply(color, dark) {
    const bg = effectiveBg(color, dark);
    const fg = textColorFor(bg);
    const root = document.documentElement.style;
    root.setProperty('--note-bg', bg);
    root.setProperty('--note-fg', fg);
    root.setProperty('--note-titlebar',
      fg === '#1F2328' ? 'rgba(0,0,0,0.06)' : 'rgba(255,255,255,0.08)');
  }

  return { PRESETS, normalize, effectiveBg, textColorFor, apply };
})();
