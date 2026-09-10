// AI 설정 마법사 창의 부트스트랩. 메모창이나 설정 창이 `wizard.open`으로 이 창을 띄우고,
// 여기서는 마법사를 창 전체에 그린다. 다 마치거나 접으면 `wizard.close`로 네이티브에 알리고,
// 네이티브가 창을 닫으면서 결과를 부른 창(ownerId)에 `wizard.result`로 돌려준다.
(async () => {
  const init = window.__init || { theme: 'light', lang: 'en' };
  await i18n.load(init.lang);
  document.title = i18n.t('wizard.title');
  const q = new URLSearchParams(location.search);
  const ok = await aiWizard.runStandalone({ force: q.get('force') === '1' });
  bridge.call('wizard.close', { ok }).catch(() => {});
})();
