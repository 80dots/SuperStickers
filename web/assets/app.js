/* Super Stickers 랜딩 사이트 — 언어/테마 전환, 스크롤 연출, 최신 릴리스 정보 표시 */
(function () {
  'use strict';

  var root = document.documentElement;
  var store = {
    get: function (k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set: function (k, v) { try { localStorage.setItem(k, v); } catch (e) { /* 사생활 보호 모드 등 */ } }
  };

  /* ---------- 언어 ---------- */
  // CSS가 html[lang]을 보고 [data-lang-ko] / [data-lang-en] 중 한쪽만 표시한다.
  function applyLang(lang) {
    root.lang = lang;
    var btn = document.getElementById('lang-toggle');
    if (btn) {
      btn.textContent = lang === 'ko' ? 'EN' : '한국어';
      btn.setAttribute('aria-label', lang === 'ko' ? 'Switch to English' : '한국어로 전환');
    }
  }

  var savedLang = store.get('ss-lang');
  var isKoBrowser = (navigator.language || '').toLowerCase().indexOf('ko') === 0;
  applyLang(savedLang || (isKoBrowser ? 'ko' : 'en'));

  var langBtn = document.getElementById('lang-toggle');
  if (langBtn) {
    langBtn.addEventListener('click', function () {
      var next = root.lang === 'ko' ? 'en' : 'ko';
      applyLang(next);
      store.set('ss-lang', next);
    });
  }

  /* ---------- 테마 ---------- */
  // 저장된 값이 없으면 data-theme을 비워 두고 OS 설정(prefers-color-scheme)을 따른다.
  function applyTheme(theme) {
    if (theme) root.setAttribute('data-theme', theme);
    else root.removeAttribute('data-theme');
    var btn = document.getElementById('theme-toggle');
    if (btn) {
      var dark = theme === 'dark' ||
        (!theme && window.matchMedia('(prefers-color-scheme: dark)').matches);
      btn.textContent = dark ? '☀' : '☾';
    }
  }
  applyTheme(store.get('ss-theme'));

  var themeBtn = document.getElementById('theme-toggle');
  if (themeBtn) {
    themeBtn.addEventListener('click', function () {
      var dark = root.getAttribute('data-theme') === 'dark' ||
        (!root.getAttribute('data-theme') && window.matchMedia('(prefers-color-scheme: dark)').matches);
      var next = dark ? 'light' : 'dark';
      applyTheme(next);
      store.set('ss-theme', next);
    });
  }

  /* ---------- 스크롤 연출 ---------- */
  // 모션을 줄이도록 설정한 사용자에게는 아무것도 하지 않는다(CSS도 .reveal을 즉시 보이게 한다).
  var reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var targets = document.querySelectorAll('.reveal');

  if (reduced || !('IntersectionObserver' in window)) {
    Array.prototype.forEach.call(targets, function (el) { el.classList.add('in'); });
  } else {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        e.target.classList.add('in');
        io.unobserve(e.target);
      });
    }, { rootMargin: '0px 0px -12% 0px', threshold: .08 });
    Array.prototype.forEach.call(targets, function (el) { io.observe(el); });
  }

  // 헤더는 스크롤이 시작되면 그림자를 키운다.
  var header = document.getElementById('site-header');
  if (header) {
    var onScroll = function () { header.classList.toggle('scrolled', window.scrollY > 8); };
    onScroll();
    window.addEventListener('scroll', onScroll, { passive: true });
  }

  /* ---------- 최신 릴리스 정보 ---------- */
  function formatSize(bytes) {
    if (!bytes) return '';
    return (bytes / 1048576).toFixed(1) + ' MB';
  }

  fetch('/api/latest')
    .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
    .then(function (d) {
      if (!d || !d.version) return;

      Array.prototype.forEach.call(document.querySelectorAll('[data-rel-version]'), function (el) {
        el.textContent = 'v' + d.version;
      });
      if (d.setup) {
        Array.prototype.forEach.call(document.querySelectorAll('[data-rel-setup-size]'), function (el) {
          el.textContent = formatSize(d.setup.size);
        });
      }
      if (d.portable) {
        Array.prototype.forEach.call(document.querySelectorAll('[data-rel-portable-size]'), function (el) {
          el.textContent = formatSize(d.portable.size);
        });
      }
      if (d.publishedAt) {
        Array.prototype.forEach.call(document.querySelectorAll('[data-rel-date]'), function (el) {
          el.textContent = d.publishedAt.slice(0, 10);
        });
      }
    })
    .catch(function () {
      // 실패해도 HTML에 적힌 정적 폴백 값이 그대로 남는다. 다운로드 링크는 /download가 처리.
    });
})();
