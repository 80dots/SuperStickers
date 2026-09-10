// 비밀글 가리기 — 본문 밖으로 새어 나온 비밀글(요약·번역·AI 출력·제목·목록 미리보기)을
// 원문 비밀글과 같은 글자로 찾아 다시 번지게 만든다. 두 번째 방어선이다: 첫 번째는 애초에
// AI에 내용을 보내지 않는 것(getMarkdown의 토큰)이고, 이것은 예전에 만들어진 요약·번역이나
// 다른 경로로 흘러든 글자를 화면에서 가린다.
const secretMask = (() => {
  const MIN_LEN = 2;   // 한 글자짜리 비밀글은 모든 같은 글자를 가려 버리므로 가리지 않는다

  // HTML 문자열에서 비밀글 글자들 (긴 것부터 — 짧은 것이 긴 것의 일부를 먼저 잘라 먹지 않게)
  function textsFromHtml(html) {
    if (!html || html.indexOf('secret') < 0) return [];
    const doc = new DOMParser().parseFromString(html, 'text/html');
    return textsFromRoot(doc.body);
  }
  // 요소 안의 비밀글 글자들
  function textsFromRoot(root) {
    if (!root) return [];
    const set = new Set();
    root.querySelectorAll('.secret').forEach((s) => {
      const copy = s.cloneNode(true);
      copy.querySelectorAll('[data-chrome]').forEach((c) => c.remove());
      const t = (copy.textContent || '').replace(/\s+/g, ' ').trim();
      if (t.length >= MIN_LEN) set.add(t);
    });
    return [...set].sort((a, b) => b.length - a.length);
  }
  // HTML 문자열 → 비밀글을 뺀 순수 텍스트 (목록·카드 미리보기용)
  function stripSecretsFromHtml(html) {
    const doc = new DOMParser().parseFromString(html || '', 'text/html');
    doc.body.querySelectorAll('.secret').forEach((s) => s.remove());
    return doc.body.textContent || '';
  }

  const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  // 글자 사이의 공백은 몇 개든·줄바꿈이든 같은 것으로 본다
  const patternOf = (t) => escapeRe(t).replace(/ /g, '\\s+');

  // 요소 안의 텍스트 노드에서 비밀글 글자를 찾아 <span class="secret locked masked">로 감싼다.
  // 이미 비밀글 안에 있는 글자는 건드리지 않는다. 가린 개수를 돌려준다.
  function maskNode(container, texts) {
    if (!container || !texts || !texts.length) return 0;
    const re = new RegExp(texts.map(patternOf).join('|'), 'g');
    const walker = document.createTreeWalker(container, NodeFilter.SHOW_TEXT, {
      acceptNode: (n) => (n.parentElement && n.parentElement.closest('.secret')
                          ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT),
    });
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    let count = 0;
    nodes.forEach((node) => {
      const text = node.nodeValue;
      re.lastIndex = 0;
      if (!re.test(text)) return;
      re.lastIndex = 0;
      const frag = document.createDocumentFragment();
      let last = 0, m;
      while ((m = re.exec(text))) {
        if (m.index > last) frag.appendChild(document.createTextNode(text.slice(last, m.index)));
        const span = document.createElement('span');
        span.className = 'secret locked masked';
        span.setAttribute('contenteditable', 'false');
        span.textContent = m[0];
        frag.appendChild(span);
        last = m.index + m[0].length;
        count++;
        if (!m[0]) re.lastIndex++;
      }
      if (last < text.length) frag.appendChild(document.createTextNode(text.slice(last)));
      node.parentNode.replaceChild(frag, node);
    });
    return count;
  }

  return { textsFromHtml, textsFromRoot, stripSecretsFromHtml, maskNode };
})();
