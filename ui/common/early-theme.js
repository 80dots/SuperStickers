// 문서 생성 직후 테마 속성을 건다. 세 페이지 모두 CSP(script-src 'self')로 인라인 스크립트를
// 막았으므로 파일로 둔다. 메모창·그룹창은 항상 라이트(<html data-theme-fixed="light"> —
// 스티커 색이 유일한 배경이라 다크 전환 시 내부 요소가 어두워지면 안 된다), 설정 창은
// 네이티브가 주입한 __init.theme을 따른다.
(function () {
  var fixed = document.documentElement.dataset.themeFixed;
  var init = window.__init || {};
  document.documentElement.dataset.theme = fixed || init.theme || 'light';
})();
