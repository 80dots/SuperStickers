// 네이티브 ↔ 웹 JSON-RPC 브리지
// 요청: {id, method, params} → 응답: {id, ok, result|error}
// 네이티브 발신 이벤트: {event, data}
const bridge = (() => {
  let seq = 0;
  const pending = new Map();
  const listeners = new Map();
  const hasHost = !!(window.chrome && window.chrome.webview);

  if (hasHost) {
    window.chrome.webview.addEventListener('message', (e) => {
      const m = e.data;
      if (m && m.id !== undefined && pending.has(m.id)) {
        const { resolve, reject } = pending.get(m.id);
        pending.delete(m.id);
        if (m.ok) resolve(m.result);
        else reject(new Error(m.error || 'bridge error'));
      } else if (m && m.event) {
        (listeners.get(m.event) || []).forEach((cb) => {
          try { cb(m.data); } catch (err) { console.error(err); }
        });
      }
    });
  }

  return {
    call(method, params = {}) {
      if (!hasHost) return Promise.reject(new Error('no native host'));
      return new Promise((resolve, reject) => {
        const id = ++seq;
        pending.set(id, { resolve, reject });
        window.chrome.webview.postMessage({ id, method, params });
      });
    },
    // File 객체 목록과 함께 호출 — 네이티브가 전체 경로를 params.paths로 합쳐 받는다
    callWithFiles(method, params, files) {
      if (!hasHost) return Promise.reject(new Error('no native host'));
      return new Promise((resolve, reject) => {
        const id = ++seq;
        pending.set(id, { resolve, reject });
        window.chrome.webview.postMessageWithAdditionalObjects(
          { id, method, params: params || {} }, files);
      });
    },
    on(event, cb) {
      if (!listeners.has(event)) listeners.set(event, []);
      listeners.get(event).push(cb);
    },
  };
})();
