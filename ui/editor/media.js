// 이미지/동영상/YouTube 삽입 처리
const mediaTools = (() => {
  const YT_RE =
    /^(?:https?:\/\/)?(?:www\.|m\.)?(?:youtube\.com\/(?:watch\?[^ ]*v=|shorts\/|embed\/)|youtu\.be\/)([\w-]{6,20})/;

  function youtubeId(text) {
    const m = (text || '').trim().match(YT_RE);
    return m ? m[1] : null;
  }

  // 블록 요소(임베드/동영상)는 체크 항목·목록 안에 중첩되지 않도록
  // 캐럿이 속한 최상위 블록 뒤에 삽입한다.
  function insertBlockNode(node) {
    const editor = editorCore.el;
    editor.focus();
    const sel = window.getSelection();
    let block = null;
    if (sel.rangeCount && editor.contains(sel.getRangeAt(0).startContainer)) {
      let n = sel.getRangeAt(0).startContainer;
      while (n && n.parentNode !== editor) n = n.parentNode;
      if (n && n.nodeType === 1) block = n;
    }
    if (block) block.after(node);
    else editor.appendChild(node);
    // 임베드 뒤에 이어서 입력할 수 있도록 빈 문단 추가 + 캐럿 이동
    const p = document.createElement('div');
    p.innerHTML = '<br>';
    node.after(p);
    const range = document.createRange();
    range.selectNodeContents(p);
    range.collapse(true);
    sel.removeAllRanges();
    sel.addRange(range);
    editorCore.notify();
  }

  function insertYoutube(id) {
    const wrap = document.createElement('div');
    wrap.className = 'yt-embed';
    wrap.contentEditable = 'false';
    const iframe = document.createElement('iframe');
    iframe.src = `https://www.youtube-nocookie.com/embed/${id}`;
    iframe.allow = 'encrypted-media; picture-in-picture; fullscreen';
    iframe.setAttribute('allowfullscreen', '');
    wrap.appendChild(iframe);
    insertBlockNode(wrap);
  }

  // Blob → 네이티브 저장 → <img> 삽입
  async function insertImageBlob(blob) {
    const ext = (blob.type.split('/')[1] || 'png').replace('jpeg', 'jpg');
    const dataUrl = await new Promise((resolve, reject) => {
      const r = new FileReader();
      r.onload = () => resolve(r.result);
      r.onerror = reject;
      r.readAsDataURL(blob);
    });
    const base64 = String(dataUrl).split(',')[1];
    // kind 'image' → 메모 폴더의 Image\ 하위에 저장
    const res = await bridge.call('attachment.save',
      { dataBase64: base64, ext, kind: 'image' });
    // 마크다운 모드면 마크다운 문법으로, 아니면 <img>로 삽입
    if (window.__insertMedia && window.__insertMedia.isMarkdown()) {
      window.__insertMedia.image(res.url);
      return;
    }
    const img = document.createElement('img');
    img.src = res.url;
    editorCore.insertNodeAtCaret(img);
  }

  // 첨부로 들여온 동영상을 본문에 넣는다 (툴바 picker와 드래그앤드롭이 함께 쓴다)
  function insertVideoUrl(url) {
    if (window.__insertMedia && window.__insertMedia.isMarkdown()) {
      window.__insertMedia.video(url);
      return;
    }
    const video = document.createElement('video');
    video.controls = true;
    video.src = url;
    video.contentEditable = 'false';
    insertBlockNode(video);
  }

  async function pickVideo() {
    const res = await bridge.call('attachment.pickVideo');
    if (res.cancelled) return;
    insertVideoUrl(res.url);
  }

  // paste 이벤트 처리: 이미지 / YouTube URL / 일반 텍스트
  function handlePaste(e) {
    const items = e.clipboardData?.items || [];
    for (const item of items) {
      if (item.type.startsWith('image/')) {
        e.preventDefault();
        insertImageBlob(item.getAsFile()).catch(console.error);
        return;
      }
    }
    // 마크다운 소스(textarea)는 이미지 외엔 기본 붙여넣기 그대로 사용
    if (e.target && e.target.tagName === 'TEXTAREA') return;
    const text = e.clipboardData?.getData('text/plain') || '';
    const yt = youtubeId(text);
    if (yt) {
      e.preventDefault();
      insertYoutube(yt);
      return;
    }
    // 서식 있는 외부 HTML은 평문으로 붙여넣어 스티커 서식을 단순하게 유지
    if (e.clipboardData?.getData('text/html')) {
      e.preventDefault();
      document.execCommand('insertText', false, text);
    }
  }

  function handleDrop(e) {
    const imgs = [...(e.dataTransfer?.files || [])].filter((f) => f.type.startsWith('image/'));
    if (!imgs.length) return;
    e.preventDefault();
    // 여러 장을 떨어뜨리면 전부 넣는다 (예전에는 첫 장에서 빠져나갔다)
    (async () => {
      for (const f of imgs) await insertImageBlob(f).catch(console.error);
    })();
  }

  function pickImageFile() {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = 'image/*';
    input.onchange = () => {
      if (input.files[0]) insertImageBlob(input.files[0]).catch(console.error);
    };
    input.click();
  }

  return { handlePaste, handleDrop, pickImageFile, pickVideo, insertVideoUrl,
           youtubeId, insertYoutube };
})();
