// 3D 모델 임베드 뷰어 (rich 메모)
// 본문에는 <div class="embed3d" data-src data-name contenteditable="false"> 껍데기만
// 직렬화되고, 뷰어 UI(canvas·버튼)는 Shadow DOM에 렌더되어 저장 HTML을 오염시키지 않는다.
const viewer3d = (() => {
  let libsPromise = null;

  // three.js 라이브러리 지연 로드 (3D 임베드가 실제로 있을 때만)
  function ensureLibs() {
    if (libsPromise) return libsPromise;
    const load = (src) => new Promise((res, rej) => {
      const s = document.createElement('script');
      s.src = src;
      s.onload = res;
      s.onerror = () => rej(new Error('load failed: ' + src));
      document.head.appendChild(s);
    });
    libsPromise = load('/vendor/three/three.min.js')
      .then(() => Promise.all([
        load('/vendor/three/OrbitControls.js'),
        load('/vendor/three/GLTFLoader.js'),
        load('/vendor/three/OBJLoader.js'),
        load('/vendor/three/STLLoader.js'),
        load('/vendor/three/RGBELoader.js'),
      ]));
    return libsPromise;
  }

  // PBR 환경맵 (CC0 HDRI, 로컬 번들)
  const HDRIS = [
    { id: 'royal_esplanade_1k.hdr', label: 'Esplanade' },
    { id: 'venice_sunset_1k.hdr', label: 'Venice Sunset' },
    { id: 'quarry_01_1k.hdr', label: 'Quarry' },
  ];

  function loaderFor(url) {
    const ext = (url.split('.').pop() || '').toLowerCase();
    if (ext === 'glb' || ext === 'gltf') return 'gltf';
    if (ext === 'obj') return 'obj';
    if (ext === 'stl') return 'stl';
    return null;
  }

  function base64ToBuffer(b64) {
    const bin = atob(b64);
    const buf = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
    return buf.buffer;
  }

  // .gltf(JSON)는 .bin·텍스처를 별도 파일로 참조한다.
  // 브리지로 같은 폴더의 파일을 읽어 blob URL로 바꿔치기하는 매니저를 만든다.
  function makeRelativeManager(basePath) {
    const manager = new THREE.LoadingManager();
    const cache = new Map();
    manager.setURLModifier((url) => {
      if (/^(blob:|data:|https?:)/.test(url)) return url;
      if (cache.has(url)) return cache.get(url);
      return url;  // 미리 로드되지 않은 경우 원본 유지 (아래에서 사전 로드)
    });
    manager.__preload = async (relPaths) => {
      for (const rel of relPaths) {
        if (cache.has(rel)) continue;
        try {
          const r = await bridge.call('model.readFile', { path: basePath, relative: rel });
          const blob = new Blob([base64ToBuffer(r.dataBase64)]);
          cache.set(rel, URL.createObjectURL(blob));
        } catch (e) {
          console.warn('gltf resource missing:', rel, e);
        }
      }
    };
    manager.__revoke = () => {
      cache.forEach((u) => URL.revokeObjectURL(u));
      cache.clear();
    };
    return manager;
  }

  // gltf JSON에서 외부 리소스(uri) 목록 추출 (data: URI 제외)
  function gltfExternalUris(json) {
    const uris = [];
    const collect = (arr) => (arr || []).forEach((x) => {
      if (x && typeof x.uri === 'string' && !/^data:/.test(x.uri)) uris.push(x.uri);
    });
    collect(json.buffers);
    collect(json.images);
    return [...new Set(uris)];
  }

  // ArrayBuffer를 확장자에 맞는 로더로 파싱해 THREE.Object3D 반환
  function parseModel(kind, buffer, manager) {
    return new Promise((resolve, reject) => {
      if (kind === 'gltf') {
        new THREE.GLTFLoader(manager).parse(buffer, '', (g) => resolve(g.scene), reject);
      } else if (kind === 'obj') {
        try {
          resolve(new THREE.OBJLoader().parse(new TextDecoder().decode(buffer)));
        } catch (e) { reject(e); }
      } else if (kind === 'stl') {
        try {
          const geo = new THREE.STLLoader().parse(buffer);
          resolve(new THREE.Mesh(geo, new THREE.MeshStandardMaterial({ color: 0x8899aa })));
        } catch (e) { reject(e); }
      } else {
        reject(new Error('unsupported format'));
      }
    });
  }

  // 모델 로드: data-path(원본 파일 — 복사 없음)는 브리지로 내용을 읽고,
  // data-src(구버전 첨부 URL)는 fetch로 하위 호환 로드
  async function loadModel(el) {
    let kind, buffer, basePath = el.dataset.path || '';
    if (basePath) {
      kind = loaderFor(basePath);
      const r = await bridge.call('model.readFile', { path: basePath });
      buffer = base64ToBuffer(r.dataBase64);
    } else {
      kind = loaderFor(el.dataset.src || '');
      buffer = await (await fetch(el.dataset.src)).arrayBuffer();
    }

    // .gltf(JSON 텍스트)면 참조 파일들을 먼저 blob으로 준비
    let manager;
    if (kind === 'gltf' && basePath && /\.gltf$/i.test(basePath)) {
      const text = new TextDecoder().decode(buffer);
      let json = null;
      try { json = JSON.parse(text); } catch {}
      if (json) {
        manager = makeRelativeManager(basePath);
        await manager.__preload(gltfExternalUris(json));
        // setURLModifier가 캐시를 보게 재설정
        const cacheGet = manager.__preload;  // (참조 유지)
        void cacheGet;
      }
    }
    const obj = await parseModel(kind, buffer, manager);
    if (manager) obj.userData.__revoke = manager.__revoke;
    return obj;
  }

  // 임베드 요소에 뷰어 마운트 (onRemove: 휴지통 클릭 시 호출)
  async function mount(el, onRemove) {
    if (el.__mounted) return;
    el.__mounted = true;
    const shadow = el.shadowRoot || el.attachShadow({ mode: 'open' });
    shadow.innerHTML = `
      <style>
        :host { display: block; }
        .wrap { position: relative; width: 100%; height: 100%;
                border-radius: 8px; overflow: hidden; background: #2a2d33; }
        canvas { display: block; width: 100%; height: 100%; }
        /* 렌더 영역 안쪽 하단 오버레이 컨트롤 */
        .bar { position: absolute; left: 8px; bottom: 8px; display: flex;
               align-items: center; gap: 6px; }
        .seg { display: flex; border-radius: 6px; overflow: hidden;
               background: rgba(0,0,0,0.5); }
        /* 높이는 Skybox 셀렉트와 동일하게 유지 (아래 select의 padding 기준) */
        .seg button { display: flex; align-items: center; justify-content: center;
                      width: 26px; height: 23px; padding: 0; border: 0; cursor: pointer;
                      background: transparent; color: #fff; opacity: 0.75; }
        .seg button:hover { background: rgba(255,255,255,0.15); opacity: 1; }
        .seg button.on { background: rgba(255,255,255,0.28); opacity: 1; }
        select { font-size: 11px; height: 23px; padding: 0 5px; border-radius: 6px;
                 border: 0; background: rgba(0,0,0,0.5); color: #fff;
                 font-family: inherit; max-width: 120px; }
        select.hidden { display: none; }
        .btn { position: absolute; display: flex; align-items: center; justify-content: center;
               background: rgba(0,0,0,0.5); color: #fff; border: none; border-radius: 6px;
               cursor: pointer; font-size: 12px; padding: 5px 10px; opacity: 0.9; }
        .btn:hover { background: rgba(0,0,0,0.75); opacity: 1; }
        .open { right: 8px; bottom: 8px; }
        .del  { right: 8px; top: 8px; width: 28px; height: 28px; padding: 0; }
        .msg { position: absolute; inset: 0; display: flex; align-items: center;
               justify-content: center; color: #ccc; font-size: 12px; pointer-events: none; }
      </style>
      <div class="wrap">
        <div class="msg">Loading 3D…</div>
        <button class="btn open" title="파일 위치 열기">Open</button>
        <button class="btn del" title="제거">
          <svg viewBox="0 0 16 16" width="13" height="13"><path fill="currentColor"
            d="M6 2h4l.5 1H14v1.5H2V3h3.5zM3 6h10l-.8 8.2c-.1.5-.5.8-1 .8H4.8c-.5 0-.9-.3-1-.8z"/></svg>
        </button>
        <div class="bar">
          <div class="seg mode">
            <button data-mode="wireframe" title="Wireframe">
              <svg viewBox="0 0 16 16" width="14" height="14"><g fill="none" stroke="currentColor" stroke-width="1.1">
                <path d="M8 1.8 14 5v6L8 14.2 2 11V5z"/><path d="M2 5l6 3.2L14 5M8 8.2v6M2 11l6-2.8 6 2.8"/></g></svg>
            </button>
            <button data-mode="solid" title="Solid">
              <svg viewBox="0 0 16 16" width="14" height="14"><path fill="currentColor" opacity="0.95"
                d="M8 1.8 14 5v6L8 14.2 2 11V5z"/></svg>
            </button>
            <button data-mode="pbr" title="PBR">
              <svg viewBox="0 0 16 16" width="14" height="14">
                <circle cx="8" cy="8" r="5.6" fill="currentColor" opacity="0.9"/>
                <circle cx="6" cy="6" r="1.8" fill="#000" opacity="0.35"/>
                <circle cx="10.2" cy="10" r="2.4" fill="#fff" opacity="0.45"/></svg>
            </button>
          </div>
          <select class="ibl hidden" title="Skybox IBL">
            ${HDRIS.map((h) => `<option value="${h.id}">${h.label}</option>`).join('')}
          </select>
        </div>
      </div>`;
    const wrap = shadow.querySelector('.wrap');
    const msg = shadow.querySelector('.msg');

    shadow.querySelector('.open').addEventListener('click', (e) => {
      e.stopPropagation();
      // 원본 경로(신규) 또는 첨부 사본(구버전) 위치 열기
      if (el.dataset.path) {
        bridge.call('model.reveal', { path: el.dataset.path }).catch(console.error);
      } else if (el.dataset.name) {
        bridge.call('files.open', { path: '' }).catch(() => {});
      }
    });
    shadow.querySelector('.del').addEventListener('click', (e) => {
      e.stopPropagation();
      if (el.__cleanup) el.__cleanup();
      el.remove();
      if (onRemove) onRemove();
    });

    try {
      await ensureLibs();
      // preserveDrawingBuffer: 그룹 카드용 썸네일을 캔버스에서 캡처하기 위해 필요
      const renderer = new THREE.WebGLRenderer({
        antialias: true, alpha: false, preserveDrawingBuffer: true,
      });
      renderer.setPixelRatio(window.devicePixelRatio || 1);
      wrap.insertBefore(renderer.domElement, msg);

      const scene = new THREE.Scene();
      scene.background = new THREE.Color(0x2a2d33);
      scene.add(new THREE.HemisphereLight(0xffffff, 0x445566, 1.2));
      const dir = new THREE.DirectionalLight(0xffffff, 1.0);
      dir.position.set(3, 5, 4);
      scene.add(dir);

      const camera = new THREE.PerspectiveCamera(50, 1, 0.01, 1000);
      const controls = new THREE.OrbitControls(camera, renderer.domElement);
      // 좌드래그 = Orbit, 가운데 버튼 = Pan, 우드래그 = 줌.
      // 휠은 OrbitControls가 잡지 않게 두어(enableZoom=false) 메모 본문이 스크롤되게 한다.
      controls.mouseButtons = {
        LEFT: THREE.MOUSE.ROTATE,
        MIDDLE: THREE.MOUSE.PAN,
        RIGHT: null,  // 우클릭은 아래에서 직접 줌으로 처리
      };
      controls.enableZoom = false;
      controls.enableDamping = true;

      const model = await loadModel(el);
      scene.add(model);
      msg.remove();

      // ---------- View Mode (Wireframe / Solid / PBR) ----------
      const original = new Map();  // mesh → 원본 material (PBR 복원용)
      model.traverse((o) => { if (o.isMesh) original.set(o, o.material); });
      const pmrem = new THREE.PMREMGenerator(renderer);
      let envRT = null;
      const envCache = new Map();

      async function loadEnv(id) {
        if (envCache.has(id)) return envCache.get(id);
        const tex = await new Promise((res, rej) =>
          new THREE.RGBELoader().load('/vendor/hdri/' + id, res, undefined, rej));
        tex.mapping = THREE.EquirectangularReflectionMapping;
        const rt = pmrem.fromEquirectangular(tex);
        tex.dispose();
        envCache.set(id, rt);
        return rt;
      }

      async function applyMode(mode, iblId) {
        el.dataset.mode = mode;
        shadow.querySelectorAll('.seg.mode button').forEach((b) =>
          b.classList.toggle('on', b.dataset.mode === mode));
        shadow.querySelector('.ibl').classList.toggle('hidden', mode !== 'pbr');

        if (mode === 'pbr') {
          try {
            envRT = await loadEnv(iblId);
            scene.environment = envRT.texture;
            scene.background = envRT.texture;  // Skybox 표시
          } catch (e) {
            console.warn('IBL load failed', e);
          }
        } else {
          scene.environment = null;
          scene.background = new THREE.Color(0x2a2d33);
        }
        model.traverse((o) => {
          if (!o.isMesh) return;
          if (mode === 'wireframe') {
            o.material = new THREE.MeshBasicMaterial({ color: 0xcfd6e4, wireframe: true });
          } else if (mode === 'solid') {
            o.material = new THREE.MeshPhongMaterial({ color: 0xb9c2d0, flatShading: false });
          } else {
            o.material = original.get(o) || o.material;
          }
        });
      }

      const savedMode = el.dataset.mode || 'pbr';
      const iblSel = shadow.querySelector('.ibl');
      if (el.dataset.ibl) iblSel.value = el.dataset.ibl;
      shadow.querySelectorAll('.seg.mode button').forEach((b) =>
        b.addEventListener('click', (e) => {
          e.stopPropagation();
          applyMode(b.dataset.mode, iblSel.value);
          if (onRemove) onRemove();  // 상태 변경 저장 (scheduleSave)
        }));
      iblSel.addEventListener('change', (e) => {
        e.stopPropagation();
        el.dataset.ibl = iblSel.value;
        applyMode('pbr', iblSel.value);
        if (onRemove) onRemove();
      });
      iblSel.addEventListener('click', (e) => e.stopPropagation());
      applyMode(savedMode, iblSel.value);

      // 모델 크기에 맞춰 카메라 프레이밍
      const box = new THREE.Box3().setFromObject(model);
      const size = box.getSize(new THREE.Vector3()).length() || 1;
      const center = box.getCenter(new THREE.Vector3());
      controls.target.copy(center);
      camera.position.copy(center).add(new THREE.Vector3(size * 0.6, size * 0.45, size * 0.8));
      camera.near = size / 500;
      camera.far = size * 20;
      camera.updateProjectionMatrix();

      // ---------- 우클릭 드래그 = 줌 ----------
      // 위로 끌면 확대, 아래로 끌면 축소. 카메라를 target 방향으로 당기고 미는 방식이라
      // OrbitControls의 회전·팬과 그대로 어울린다.
      (() => {
        const cv = renderer.domElement;
        let zooming = false, lastY = 0;
        const minD = size * 0.05, maxD = size * 8;  // 모델을 뚫고 들어가거나 잃어버리지 않게
        cv.addEventListener('contextmenu', (e) => e.preventDefault());
        cv.addEventListener('pointerdown', (e) => {
          if (e.button !== 2) return;
          zooming = true;
          lastY = e.clientY;
          cv.setPointerCapture(e.pointerId);
          e.preventDefault();
          e.stopPropagation();
        });
        cv.addEventListener('pointermove', (e) => {
          if (!zooming) return;
          const dy = e.clientY - lastY;
          lastY = e.clientY;
          const offset = camera.position.clone().sub(controls.target);
          const d = offset.length();
          const next = Math.min(maxD, Math.max(minD, d * Math.pow(1.01, dy)));
          camera.position.copy(controls.target).add(offset.setLength(next));
          e.preventDefault();
        });
        const end = (e) => {
          if (!zooming) return;
          zooming = false;
          if (cv.hasPointerCapture(e.pointerId)) cv.releasePointerCapture(e.pointerId);
          scheduleThumb(1500);  // 조작이 끝났으니 썸네일 갱신 (controls 'end'와 동일)
        };
        cv.addEventListener('pointerup', end);
        cv.addEventListener('pointercancel', end);
      })();

      function resize() {
        const w = wrap.clientWidth, h = wrap.clientHeight;
        if (w === 0 || h === 0) return;
        renderer.setSize(w, h, false);
        camera.aspect = w / h;
        camera.updateProjectionMatrix();
      }
      const ro = new ResizeObserver(resize);
      ro.observe(wrap);
      resize();

      let raf = 0;
      function loop() {
        raf = requestAnimationFrame(loop);
        controls.update();
        renderer.render(scene, camera);
      }
      loop();

      // ---------- 그룹 카드용 썸네일 ----------
      // 그룹에 넣으면 창이 즉시 파괴되므로, 렌더링 중에 미리 캡처해 메모에 저장해 둔다.
      let thumbTimer = null;
      let capturing = false;
      async function captureThumb() {
        if (capturing) return;
        capturing = true;
        try {
          const src = renderer.domElement;
          if (!src.width || !src.height) return;
          const maxW = 640;
          const scale = Math.min(1, maxW / src.width);
          const c = document.createElement('canvas');
          c.width = Math.max(1, Math.round(src.width * scale));
          c.height = Math.max(1, Math.round(src.height * scale));
          c.getContext('2d').drawImage(src, 0, 0, c.width, c.height);
          const dataUrl = c.toDataURL('image/png');
          // kind '3d' → 메모 폴더의 3D\ 하위에 저장 (그룹 카드 썸네일용)
          const r = await bridge.call('attachment.save', {
            dataBase64: dataUrl.split(',')[1], ext: 'png', kind: '3d',
          });
          el.dataset.thumb = r.url;
          if (onRemove) onRemove();  // 변경 저장 (scheduleSave)
        } catch (e) {
          console.warn('3D thumbnail failed', e);
        } finally {
          capturing = false;
        }
      }
      function scheduleThumb(delay) {
        clearTimeout(thumbTimer);
        thumbTimer = setTimeout(captureThumb, delay);
      }
      scheduleThumb(700);                                  // 최초 렌더 직후
      controls.addEventListener('end', () => scheduleThumb(1500));  // 조작 후
      shadow.querySelectorAll('.seg.mode button').forEach((b) =>
        b.addEventListener('click', () => scheduleThumb(1200)));    // 모드 변경 후
      shadow.querySelector('.ibl').addEventListener('change',
        () => scheduleThumb(1500));

      el.__cleanup = () => {
        clearTimeout(thumbTimer);
        cancelAnimationFrame(raf);
        ro.disconnect();
        envCache.forEach((rt) => rt.dispose());
        pmrem.dispose();
        if (model.userData.__revoke) model.userData.__revoke();
        renderer.dispose();
      };
    } catch (err) {
      console.error(err);
      msg.textContent = '3D load failed';
    }
  }

  // 새 임베드 요소 생성 (원본 파일 경로 참조 — 복사하지 않음)
  function createElement(path) {
    const el = document.createElement('div');
    el.className = 'embed3d';
    el.dataset.path = path;
    el.setAttribute('contenteditable', 'false');
    return el;
  }

  return { mount, createElement };
})();
