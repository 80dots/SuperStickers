// 로케일 로딩 + DOM 적용. ui/locales/<lang>.json은 네이티브(트레이)와 공유되는 단일 소스.
const i18n = (() => {
  let dict = {};
  let lang = 'en';

  async function load(newLang) {
    lang = newLang;
    try {
      const res = await fetch(`/locales/${newLang}.json`);
      dict = await res.json();
    } catch {
      dict = {};
    }
    document.documentElement.lang = lang;
  }

  function t(key) {
    return dict[key] !== undefined ? dict[key] : key;
  }

  // data-i18n(텍스트), data-i18n-title(툴팁), data-i18n-ph(placeholder) 치환
  function apply(root = document) {
    root.querySelectorAll('[data-i18n]').forEach((el) => {
      el.textContent = t(el.dataset.i18n);
    });
    root.querySelectorAll('[data-i18n-title]').forEach((el) => {
      el.title = t(el.dataset.i18nTitle);
    });
    root.querySelectorAll('[data-i18n-ph]').forEach((el) => {
      el.placeholder = t(el.dataset.i18nPh);
    });
  }

  return { load, t, apply, get lang() { return lang; } };
})();
