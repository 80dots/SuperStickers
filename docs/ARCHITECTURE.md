# Super Sticker 아키텍처

## 개요

C++/Win32 네이티브 셸이 창·트레이·수명주기·데이터·네트워크를 담당하고,
각 창의 UI는 WebView2(Chromium)로 렌더링하는 하이브리드 구조입니다.
YouTube 임베드 재생 등 웹 콘텐츠 요구사항 때문에 순수 Win32 대신 이 구조를 선택했습니다.

```
┌────────────────────────── SuperSticker.exe (C++/Win32) ──────────────────────────┐
│  main.cpp        단일 인스턴스(뮤텍스), COM 초기화, 메시지 루프                    │
│  App             전역 상태·창 관리·트레이 메뉴·설정 반영·이벤트 브로드캐스트        │
│  StickerWindow   프레임리스 스티커 창 (WM_NCCALCSIZE/WM_NCHITTEST)               │
│  ManagerWindow   설정+목록 탭 창 (닫으면 파괴해 메모리 회수)                       │
│  WebViewHost     WebView2 컨트롤러 래퍼 (공유 환경, 가상 호스트, 브리지)           │
│  Bridge          JSON-RPC 디스패처 (method → handler)                            │
│  Store           settings.json / stickers\*.json 원자적 입출력                    │
│  OllamaClient    WinHTTP NDJSON 스트리밍 (워커 스레드 → UI 스레드 마샬링)          │
│  Theme/I18n/Autostart/TrayIcon/Utils                                             │
└──────────────────────────────────┬───────────────────────────────────────────────┘
                                   │ PostWebMessageAsJson ↔ chrome.webview.postMessage
┌──────────────────────────────────┴───────────────────────────────────────────────┐
│  ui\ (WebView2, 순수 HTML/CSS/JS — 빌드 스텝·외부 라이브러리 없음)                 │
│  sticker.html    에디터(contenteditable)·툴바·AI 패널                             │
│  manager.html    스티커 목록 + 설정 탭                                            │
│  common\         bridge.js, i18n.js, prompts.js, theme.css, base.css             │
│  locales\        ko.json / en.json — 네이티브(트레이)와 웹이 공유하는 단일 소스     │
└──────────────────────────────────────────────────────────────────────────────────┘
```

## 핵심 설계 결정

### 프레임리스 스티커 창

- `WS_POPUP | WS_THICKFRAME` + `WM_NCCALCSIZE`(wParam=TRUE 시 0 반환)로 프레임 제거,
  `DWMWA_WINDOW_CORNER_PREFERENCE`로 Windows 11 라운드 코너 적용.
- **리사이즈**: WebView2가 마우스를 삼키므로 컨트롤러를 클라이언트 사방 6px(DPI 스케일)
  안쪽에 배치하고, 바깥 밴드는 `WM_NCHITTEST`에서 `HTLEFT` 등을 반환해 네이티브 리사이즈 유지.
  밴드는 `WM_ERASEBKGND`에서 스티커 색으로 칠해 이음새가 보이지 않음 (src/Theme.cpp 팔레트는
  ui/common/theme.css와 동기화 유지 필요).
- **드래그**: 웹 타이틀바 `mousedown` → 브리지 `window.startDrag` → 네이티브가
  `ReleaseCapture(); SendMessage(WM_NCLBUTTONDOWN, HTCAPTION)`.
- 닫기(X)는 `WM_CLOSE`를 가로채 숨김 + `hidden=true` 저장. 트레이 "종료"만 실제 종료.

### WebView2 호스팅

- 환경(env) 1개를 전 창이 공유 (user data folder: `%LOCALAPPDATA%\SuperSticker\WebView2`).
- 가상 호스트 매핑:
  - `https://app.sticker/` → 설치 폴더 `ui\` (UI 자산)
  - `https://data.sticker/` → `%APPDATA%\SuperSticker\` (첨부 파일)
- `NavigationStarting`에서 `https://app.sticker/*` 외 최상위 내비게이션 차단,
  외부 링크·`NewWindowRequested`는 기본 브라우저로 열기.
- **모든 WebView2 API는 UI 스레드 전용.** 워커 스레드는 `App::RunOnUi`
  (`WM_APP_RUNNABLE` PostMessage)로 마샬링한다.

### 네이티브 ↔ 웹 브리지

- 요청 `{id, method, params}` → 응답 `{id, ok, result|error}`, 네이티브 발신 이벤트 `{event, data}`.
- 창 공통 메서드(`App::SetupCommonBridge`): `app.getState`, `settings.set`, `stickers.*`, `ollama.*`
- 스티커 전용: `sticker.load/saveContent/setColor/setTopmost/hide/delete`,
  `attachment.save/pickVideo`, `window.startDrag`
- 목록 전용: `sticker.export`(.ssticker zip), `stickers.import`/`stickers.importPaths`
- 창을 파괴하는 작업(삭제 등)은 WebMessageReceived 콜백 안에서 실행하지 않고
  `RunOnUi`로 지연시켜 재진입 문제를 피한다.
- 이벤트(`theme.changed`, `locale.changed`, `ollama.chunk` 등)는 모든 창에 브로드캐스트하고
  웹 측에서 requestId 등으로 필터링한다.

### 데이터 저장

- **메모 1개 = 폴더 1개**: `stickers\<id>\memo.json` + 첨부 하위 폴더
  (`Image`, `Video`, `PDF`, `3D`). 폴더명이 정본 id다(로드 시 폴더명으로 id를 덮어씀).
  쓰기는 임시 파일 → `MoveFileEx(REPLACE_EXISTING)` 원자 교체, 교체 전 직전 성공본을
  `.bak`으로 보존. 읽기 실패 시 `.bak` 폴백.
- 이미지/동영상/PDF/3D 썸네일은 base64 인라인이 아니라 메모 폴더에 파일로 저장하고
  `https://data.sticker/stickers/<id>/<Sub>/<file>` URL로 참조(JSON 비대화 방지).
  앱 시작 시 메모별로 참조되지 않는 첨부를 GC — 판정은 `attachments` 목록 **및** 본문
  문자열 검색을 함께 본다(목록이 누락돼도 살아 있는 파일이 지워지지 않도록).
- **휴지통**은 메모 폴더를 `trash\<id>\`로 그대로 이동(복원은 반대 방향), 완전 삭제는
  폴더 삭제. 3D 모델 원본만은 복사하지 않고 원본 경로를 참조한다.
- **내보내기/가져오기**: `.ssticker`는 메모 폴더의 zip(PowerShell
  `Compress-Archive`/`Expand-Archive`, `.bak` 제외). 가져오면 새 id를 부여하고 본문 안의
  `stickers/<옛 id>/` URL을 새 id로 치환한다.
- 폴더 열거 중 삭제하면 `FindNextFile`이 항목을 건너뛰므로, 삭제 루프는 반드시
  `util::ListDirEntries`로 **먼저 모아서** 처리한다(구버전 정리에서 실제로 파일이 남았음).

### 디자인 토큰 (2026-08-27, Claude Design 시안 반영)

- 시안: "Windows 바탕화면 스티커 메모" (Windows 11 Fluent x Notion). 라이트/다크 아트보드.
- **모든 색·반경·그림자는 `ui/common/theme.css`의 토큰이 단일 출처**다. 화면별 CSS는
  토큰만 참조하고 하드코딩된 색을 두지 않는다.
  - 중성색은 따뜻한 회색(Notion 계열): `--fg #37352F`, `--muted #787774`,
    `--bg #F7F7F5`, `--border #E9E9E7`
  - 강조색 `--accent #6355E0`(보라). **네이티브의 선택 테두리·그룹 드롭 하이라이트
    (`0x63,0x55,0xE0`)와 반드시 같은 값으로 유지할 것** — StickerWindow.cpp,
    GroupWindow.cpp 두 곳.
  - 반경 `--r-sm 6 / --r-md 10 / --r-lg 12`, 그림자 `--shadow-sm/md/lg`
    (얇은 윤곽선 + 부드러운 확산의 2겹 구조)
- **본문 서체는 Pretendard 가변 폰트를 번들**한다 (`ui/vendor/fonts/PretendardVariable.woff2`,
  2.0MB, OFL 1.1 — 같은 폴더의 OFL.txt). 오프라인 동작이 필요해 CDN을 쓸 수 없고,
  가변 폰트 하나로 100~900 굵기를 모두 담아 파일이 이것뿐이다. `@font-face`는
  `format("woff2-variations")` + `font-weight: 45 920`로 선언한다.
  이전에는 라틴은 Segoe UI Variable, 한글은 맑은 고딕으로 폴백되어 두 서체가 섞였다.
- 스티커 배경 팔레트(20색)는 시안과 별개로 유지한다. 기존 메모가 hex를 저장하고 있어
  팔레트를 바꿔도 과거 메모에는 반영되지 않아 색이 섞이기 때문.

### 성능 원칙 (2026-08-27 정리)

- **3D 뷰어는 필요할 때만 그린다**: rAF 루프는 유지하되 `OrbitControls.update()`가
  true(카메라 이동·댐핑 감쇠 중)를 반환하거나 변화 지점(리사이즈, 뷰 모드/IBL 변경,
  우클릭 줌)이 `invalidate()`를 부른 프레임에만 `renderer.render`를 호출한다.
  유휴 상태 렌더 횟수 0/3초 (이전 ~180). 검증용 카운터 `el.__renderCount`.
- **WebView 컨트롤러 가시성은 창 상태와 동기화한다**: `ShowWin`이 `SetVisible(show)`를
  호출하고, 컨트롤러 생성 시점에도 `IsWindowVisible(호스트)`로 초기화한다. 숨긴
  창(트레이 상주)의 렌더링·rAF가 실제로 멈추고, 페이지 `visibilityState`가 정확해진다.
  **주의**: 이 동기화 없이는 표시 중인 창이 'hidden'으로 남는 등 visibilityState를
  신뢰할 수 없다 (실측 — shape 폴링 가드가 이 때문에 한 번 회귀했다).
- **그룹 shape 안전망 폴링(500ms)은 visible일 때만** DOM을 측정한다. 다시 표시되면
  visibilitychange에서 즉시 한 번 갱신한다.
- **Ollama 스트리밍 이벤트는 구독 창에만 보낸다**: chunk/done은 ownerId의 스티커 창,
  models/pull·install 진행은 설정(매니저) 창 (`SendEventToSticker/ToManager`,
  창이 없으면 브로드캐스트 폴백). 이전에는 청크마다 모든 창에 IPC가 발생했다.
- **브로드캐스트는 1회 직렬화**: `BroadcastEvent`가 payload를 한 번 dump()하고
  `PostEventRaw`로 각 창에 전달한다.

### 들여쓰기/내어쓰기 (rich 메모 전용)

- 서식 툴바의 두 버튼이 `execCommand('indent'/'outdent')`를 부른다. 목록 안에서는 중첩
  목록이 되고, 일반 문단은 크로미움이 `blockquote(margin, border:none)`로 감싼다.
- 마크다운 직렬화(getMarkdown)에서 이 들여쓰기 blockquote는 **인용(`>`)이 아니라
  접두사 없이 풀어낸다** — `border:none` 스타일이 들여쓰기 산물의 표식이다.
  (마크다운에는 문단 들여쓰기 표현이 마땅치 않다)

### 태그로 본문 찾기

- 태그 칩의 이름을 누르면 본문에서 그 낱말의 **다음 위치로 이동**한다(대소문자 무시).
  누를 때마다 다음 것으로 가고 끝에 닿으면 처음으로 순환한다. 진행 위치는
  `findState {key, from}`에 담는다.
- **함정 — 두 건초더미의 좌표계가 다르다**: contenteditable에서 `innerText`로 찾으면
  블록 사이에 줄바꿈이 끼어들어, TreeWalker가 이어 붙인 텍스트 노드 오프셋과 어긋난다
  (실측: 두 번째 일치부터 2칸씩 밀림). 그래서 **텍스트 노드를 직접 이어 붙여**
  건초더미를 만들고 각 노드의 시작 위치를 함께 기록해, 찾은 인덱스를 노드+오프셋으로
  정확히 되돌린다.
- 마크다운 편집 모드는 textarea라 `setSelectionRange` + 줄 수 기반 스크롤을 쓴다.
  **편집/보기 모드는 본문이 서로 달라(원본 vs 렌더 결과) 위치가 호환되지 않으므로**
  `findState.key`에 모드를 함께 넣어 모드가 바뀌면 처음부터 다시 찾는다.

### 툴바 오버플로 ('더보기')

- 창이 좁아지면 **뒤쪽 버튼부터** ⋯ 메뉴로 접힌다. 좁아질수록 더 많이 접힌다.
  `setupOverflow(bar, moreBtn, moreMenu, items, reserveExtra)` 하나를 **헤더와 서식 툴바가
  함께 쓴다**.
  - 서식 툴바(`#tbMoreBtn`): 스페이서 앞의 항목이 대상, 미리보기·AI는 항상 남는다.
    실측 408px 2개 → 328px 4개 → 268px 7개 → 228px 8개.
  - 헤더(`#hdrMoreBtn`): 관리자·새 메모·AI Review·삭제가 대상이고 **숨기기(닫기)는 항상
    남는다**. 메뉴는 타이틀바 아래로 열린다. 제목이 없으면 버튼 7개가 최소 폭에도 다
    들어가 접히지 않고, 제목이 붙으면 접힌다(실측 328px부터 2개).
- **버튼을 DOM에서 옮기지 않는다.** 접힌 버튼에는 `.tb-overflow`(display:none)만 붙이고,
  메뉴에는 아이콘을 복제하고 `title`을 이름으로 쓰는 **대리 항목**을 만들어 누르면 원래
  버튼의 `click()`을 부른다. 리스너와 형광펜 팝오버의 위치 기준이 그대로 유지된다.
- **함정 — flex가 버튼을 눌러 버린다**: 툴바 버튼에 `flex: none`이 없으면 폭이 모자랄 때
  버튼이 찌그러지기만 하고 `offsetWidth` 합이 늘 컨테이너에 "들어간다"고 나와 아무것도
  접히지 않는다(실측: 238px에서도 접힘 0). 그래서 `#toolbar > .tb-btn/.tb-sep`에
  `flex: none`을 준다.
- **또 하나의 함정 — 늘어나는 여백을 세면 안 된다**: `#dragArea`/`.tb-spacer`는
  `flex: 1`이라 지금 폭이 곧 "남은 공간"이다. 이걸 고정 폭에 더하면 가용 폭이 0에 가까워져
  창 크기와 무관하게 다 접힌다(실측: 508px에서도 2개 접힘). `flexGrow > 0`인 요소는
  빼고, 대신 `reserveExtra`로 최소 여백(헤더 28px)만 잡는다.
- 폭 측정은 **접기 전에 한 번에** 한다(감춘 뒤에는 `offsetWidth`가 0이다).
  재계산은 `ResizeObserver`(+window resize)가 맡고, UI 자동 숨김으로 툴바가
  `display:none`일 때는 `clientWidth`가 0이라 건너뛴 뒤 다시 보일 때 옵저버가 다시 부른다.

### 형광펜 (rich 메모 전용)

- 서식 툴바의 형광펜 버튼(`#hlBtn`)이 팝오버(`#hlPopover`)를 연다. **프리셋 8색은
  sticker.js의 `HL_PRESETS`에**, 사용자 추가 색은 `settings.highlightColors`
  (`#RRGGBB` 배열, 최대 24개)에 있다. 프리셋은 삭제할 수 없고, 사용자 색에는 호버 시
  삭제 배지(×)가 뜬다.
- 적용은 `editorCore.exec('hiliteColor', color)` — exec가 값 인자를 받도록 확장됐다.
  지우기는 `hiliteColor`에 `transparent`를 덮어쓴다(크로미움이 기존 span을 정리한다).
  **어두운 형광펜에서는 글자를 밝게 바꾼다**: 적용 직후 에디터의 배경색 스팬을 훑어
  YIQ 밝기 128 미만이면 `color:#FFFFFF`를 넣고, 밝으면 걷어낸다. 글자색 스팬은 이
  형광펜 로직만 만들므로 걷어내면 원래 색으로 돌아온다(별도의 글자색 서식 기능이
  생기면 이 가정은 재검토).
  팝오버의 모든 버튼은 `mousedown` 기본 동작을 막아 에디터 선택이 풀리지 않게 한다
  (텍스트 선택 메뉴와 같은 이유).
- 선택 메뉴의 형광펜 항목은 이 팝오버를 그대로 연다(색을 골라야 하므로 바로 적용하지
  않는다).
- **모든 메모창이 색 목록을 공유한다**: 추가/삭제 시 `settings.set {highlightColors}`로
  저장하고, 네이티브 `ApplySettingsPatch`가 검증(형식·개수) 후
  `highlight.colorsChanged {colors}`를 방송해 다른 창이 즉시 반영한다. 페이지 첫 로드는
  `MakeInitJson`의 `highlightColors`로 받는다.
- **색 추가 피커는 자체 UI다** (`#hlPicker`: SV 사각형 + Hue 슬라이더 + HEX 입력 +
  '추가' 버튼). `<input type="color">`의 네이티브 피커는 WebView2에서 스포이드가
  동작하지 않아 쓰지 않는다. 색은 '추가' 버튼을 눌러야 목록에 들어간다(고르기만 해서는
  추가되지 않음). HSV↔HEX 변환은 페이지 안에서 하고, SV 영역이 보이지 않을 때의
  클릭은 무시한다(0으로 나누면 NaN이 상태에 전염되어 복구가 안 된다).

### 텍스트 선택 메뉴

- 메모에서 글을 선택하면 선택 영역 **위에** 메뉴가 뜬다(`#selMenu`). 위쪽 공간이 모자라면
  선택 아래로 뒤집는다. 창이 작아 화면 밖으로 나갈 수 없으므로 좌우도 뷰포트에 클램프한다.
- 메뉴는 **위쪽 서식 격자 → 구분선 → AI 동작** 순서다. 서식은 `SEL_FORMATS`
  (서식 툴바와 같은 10종: 굵게/기울임/밑줄/취소선/형광펜/글머리·번호 목록/체크리스트/
  들여쓰기·내어쓰기), AI 동작은 `SEL_ACTIONS`가 단일 출처이며 둘 다 마크업이 아니라
  배열에서 DOM을 만든다 — 항목을 늘리려면 해당 배열에 하나만 추가하면 된다.
- 서식 적용은 서식 툴바와 같은 규칙이다: 리치는 `editorCore.exec`(체크리스트는
  `insertChecklist`, 형광펜은 툴바의 색 팝오버를 그대로 연다), 마크다운은
  `mdTools.wrapSelection`/`prefixLines`로 원본을 다룬다. 마크다운에 대응 문법이 없는
  항목(형광펜·들여쓰기)은 `richOnly`로 표시해 rich에서만 보인다.
  **마크다운 보기 모드에서는 서식 격자와 구분선을 감춘다** — 렌더된 결과라 편집할 수
  없기 때문이며, AI 동작만 남는다.
- **AI 패널 진입점은 서식 툴바 맨 오른쪽의 `#aiPanelBtn`**이다(선택 메뉴 안이 아니라
  툴바에 있다. 타이틀바 버튼은 없앴다). 누르면 선택을 붙잡고 패널을 토글한다.
- **AI 항목을 고르면 메뉴가 닫힌 채로 남고, 서식 버튼은 메뉴를 유지한다.**
  선택 메뉴는 document의 `mouseup`에서 갱신되므로 버튼 클릭의 mouseup이 올라가면
  (선택이 그대로라) 메뉴가 곧바로 되살아난다. 서식은 이걸 그대로 둬서 여러 서식을
  이어 적용할 수 있고, AI 항목만 버튼에서 `mouseup`을 `stopPropagation()`으로 막아
  되살아나지 않게 한다.
- AI 항목(요약·맞춤법 검사·문장 다듬기·AI에게 물어보기)은 `aiAction()` 헬퍼가 만든다:
  선택을 `captureSelection()`으로 잡고 AI 패널을 열어 해당 작업을 바로 실행하므로,
  결과 스트리밍과 '바꾸기'(원문 교체)는 패널의 기존 경로를 그대로 쓴다.
- 메뉴 버튼은 `mousedown`의 기본 동작을 막는다 — 막지 않으면 클릭하는 순간 선택이 풀려
  대상 텍스트를 잃는다.
- 선택 사각형은 리치 메모는 `Range.getBoundingClientRect()`로, 마크다운(textarea)은
  Range API가 없어 **미러 div**(같은 폰트·패딩을 복제하고 선택 구간을 span으로 감싸 측정)로
  구한다.
- **마크다운 보기 모드도 지원**한다. 렌더된 `#mdPreview`는 일반 DOM이라 `window.getSelection()`
  과 Range rect를 그대로 쓴다. 분기 기준은 메모 타입이 아니라 `type === 'markdown' && mdView === 'edit'`
  일 때만 textarea(미러 div) 경로 — 그 외에는 전부 Selection/Range 경로다.
  `captureSelection()`도 보기 모드의 선택을 `savedRange`로 붙잡아 AI 질문의 문맥으로 쓴다.
- 'AI에게 물어보기'는 기존 AI 패널을 재사용한다: `captureSelection()`으로 선택을 붙잡고
  패널을 질문 모드로 연다. 답변 스트리밍·삽입·교체·복사가 이미 패널에 있어 중복 구현이 없다.
  (타이틀바의 AI 버튼을 없앤 뒤 패널의 새 진입점이 되었다.)

### AI 프롬프트 편집 (설정 → AI 탭)

- 설정 창에 **AI 탭**이 있고 Ollama 연결 설정이 여기로 옮겨졌다(예전에는 '설정' 탭).
  같은 탭 아래에 작업별 프롬프트 편집 UI(`#promptList`)가 있다.
- 편집 값은 `settings.prompts`(task → 시스템 프롬프트)에 저장한다.
  **빈 문자열은 "기본값으로 되돌리기"**라서 네이티브가 그 키를 지운다.
  변경은 `prompts.changed` 방송으로 열려 있는 모든 메모창에 즉시 반영된다
  (`prompts.setOverrides`). 첫 로드는 `MakeInitJson`의 `prompts`로 받는다.
- **prompts.js가 모든 프롬프트의 단일 출처**다. AI Review 프롬프트도 sticker.js에
  인라인으로 있던 것을 `review` 작업으로 옮겼다 — 그래야 같은 방식으로 편집·재정의된다.
  편집 UI는 `prompts.TASKS`를 순회하고 `prompts.defaultOf(task, lang)`를 안내 문구로
  보여주므로, 작업을 추가하면 UI에도 자동으로 나타난다.
- AI Review는 JSON 형식 응답에 의존하므로, 편집 시 형식 부분을 지우면 파싱에 실패한다.
  UI 설명문에 이 점을 적어 두었다.

### AI 작업 프롬프트

- `ui/common/prompts.js`의 `sys[lang][task]`가 작업별 시스템 프롬프트다. 한국어·영어
  두 벌을 같은 규칙으로 유지한다.
- 예전의 `polish`(다듬기)는 `refine`(문장 다듬기)과 역할이 겹쳐 제거했다.
- **맞춤법 검사(`spellcheck`)와 문장 다듬기(`refine`)는 역할이 겹치지 않게 갈라 놨다**:
  spellcheck는 "틀린 것만" 고치고 문체·어순·단어 선택을 건드리지 않으며, refine은 뜻을
  보존한 채 자연스럽게 고쳐 쓴다. 둘 다 줄바꿈·목록·마크다운 표기와 코드/URL/고유명사를
  보존하고, 입력과 같은 언어로, 머리말 없이 결과만 출력하도록 못 박았다
  (작은 모델은 "다듬어 보겠습니다" 같은 머리말을 흘리는 경향이 있다).

### AI Review의 태그

- **태그는 본문에 그대로 있는 낱말만 쓴다.** 프롬프트의 tags 규칙이 "본문에 실제로
  등장하는 낱말을 글자 그대로 복사, 번역·변형 금지"를 요구하고, 코드/URL/경로 토큰과
  마크다운 기호는 제외하게 한다. 예전에는 "한국어 키워드"를 요구해서 영문 메모의 태그가
  본문에 없는 한국어로 나왔다.
- **프롬프트만으로는 보장이 안 되므로 사후에도 거른다**: 응답의 태그 중 본문
  (`reviewSrc`)에 문자열로 없는 것은 버린다. 태그를 누르면 본문에서 찾아 이동하는 기능이
  있어(위 "태그로 본문 찾기"), 본문에 없는 태그는 아무 데도 닿지 못한다.

### AI Review 번역의 서식 유지

- **입력은 항상 마크다운**: markdown 메모는 원본을, rich 메모는 `editorCore.getMarkdown()`
  (editor.js)으로 DOM을 마크다운으로 직렬화해 넘긴다. 예전처럼 `innerText`를 넘기면
  모델에 닿기도 전에 서식이 사라져 번역문에 형식이 남을 수 없었다.
  직렬화 대상: 제목, 목록(중첩 포함), 체크박스(`- [ ]`/`- [x]`), 굵게/기울임/취소선,
  인라인 코드, 링크, 이미지, 인용, 코드 블록, 수평선. 밑줄은 마크다운에 없어 `<u>`로 둔다.
- **프롬프트에 번역 규칙 6개를 명시**: 마크다운 기호 유지, 코드 블록·인라인 코드 원문 복사,
  링크/이미지 주소 불변, HTML 태그·경로·명령어 유지, 줄 수와 빈 줄 유지, 문장만 번역.
  짧은 예시(`## 설치` → `## Install`)를 함께 준다.
- **코드 블록은 결정적으로 복원**: 프롬프트만으로는 작은 모델이 코드 글자를 흘리는 일이
  있어(`npm install` → `pm install` 실측), `restoreCodeBlocks()`가 번역문의 펜스 블록을
  원문 블록으로 되돌린다. 블록 수가 다르면 짝을 확신할 수 없으므로 손대지 않는다.
- 번역 뷰는 `marked.parse()`로 렌더하므로 rich/markdown 어느 쪽이든 서식이 그대로 보인다.

### Ollama 연동

- **네이티브 WinHTTP 사용** (WebView2 fetch 배제): 가상 호스트 origin에서
  `http://localhost:11434`로의 fetch는 CORS/mixed content로 차단되기 때문.
- `POST /api/chat` (stream=true) NDJSON 응답을 워커 스레드에서 라인 단위 파싱 →
  `ollama.chunk` 이벤트로 전달. **주의**: `WinHttpReadData`만 쓰면 내부 버퍼를 채울 때까지
  블록되어 스트리밍이 되지 않음 — 반드시 `WinHttpQueryDataAvailable`로 도착분만 즉시 읽는다.
- **중단**: WinHTTP 동기 모드에서 타 스레드의 핸들 닫기는 진행 중인 호출을 취소하지 못하므로,
  requestId별 `atomic<bool>` 플래그를 두고 워커가 청크 사이마다 확인한다.
- 프롬프트는 `ui/common/prompts.js`에 언어별 정의 (UI 언어에 따라 출력 언어 지시).

### 메모 타입

- `StickerData.type`: "rich" | "markdown" | "file" | "web" | "pdf" (레거시 `mode`에서 자동
  마이그레이션). 타입은 생성 시 고정되며 `sticker.html` 한 페이지가 타입별 UI를 분기한다.
- **파일 메모**: 경로 목록을 저장(원본 참조). 썸네일은 `IShellItemImageFactory::GetImage`
  (STA 워커 스레드, 실패 시 ICONONLY 폴백) → GDI+ PNG → data URL로 `files.thumb` 이벤트
  전달. 클립보드 복사는 CF_HDROP, 실행은 ShellExecute. 탐색기 드래그앤드롭은
  `postMessageWithAdditionalObjects`로 File 객체를 넘기고 네이티브가
  `ICoreWebView2File::get_Path`로 전체 경로를 회수해 params.paths에 합친다.
  **주의**: `SHCreateItemFromParsingName`은 슬래시(`/`) 경로를 거부 — 백슬래시 정규화 필수.
- **웹 메모**: 같은 스티커 창 안에 **두 번째 WebView2**(browserMode: 내비게이션 제한 없음,
  컨텍스트 메뉴/단축키 허용)를 상단 스트립(64 CSS px: 타이틀바+URL바) 아래에 배치 —
  iframe과 달리 X-Frame-Options에 막히지 않는다. `SourceChanged`로 `lastUrl` 저장,
  시작 시 lastUrl 우선 복원. 팝오버가 열리면 `web.suspendSite`로 사이트 뷰를 잠시 숨김
  (팝오버가 하위 컨트롤러에 가려지는 문제 회피).
- **PDF 메모**: 파일을 메모 폴더의 `PDF\`로 복사(`ImportAttachment(id, path, "pdf")`) 후
  `https://data.sticker/stickers/<id>/PDF/…pdf`를 iframe으로 로드 — Chromium 내장 PDF 뷰어 사용.
- **마크다운 인텔리센스**: `<` 입력 시 `mdTools.attachIntellisense`가 캐럿 픽셀 위치(미러
  div 기법)에 태그 목록을 띄움. ↑↓/Enter/Tab/Esc, 타이핑 필터링 지원.

### 마크다운 모드

- 스티커별 `mode`("rich"|"markdown") 필드. 마크다운 원본은 `markdown` 필드에 별도 저장
  (리치 `html`과 공존 — 모드 전환 시 비어 있는 쪽만 상대 모드 내용으로 시드: rich→md는
  innerText, md→rich는 렌더된 HTML).
- 렌더러: `ui/vendor/marked.min.js` (marked v15, MIT, 오프라인 번들). GFM + breaks 옵션.
- `ui/editor/markdown.js`: 소스 편집 헬퍼(선택 래핑·라인 프리픽스·삽입), `renderInto()`가
  프리뷰 후처리 담당 — YouTube 링크 → `youtube-nocookie` iframe 임베드, GFM 체크박스를
  활성화하고 토글 시 `toggleTaskInSource()`로 소스의 n번째 `[ ]`/`[x]` 토큰을 갱신해 저장.
- 툴바 버튼은 모드에 따라 동작 분기: 리치 모드 execCommand, 마크다운 모드 문법 삽입
  (`**`, `*`, `- `, `- [ ] ` 등). 이미지/동영상 삽입도 md 모드에서는 `![](url)` /
  `<video>` 문법으로 삽입 (`window.__insertMedia` 훅).

### 스티커 색상 파이프라인

- `color` 필드: `#RRGGBB` hex (레거시 5색 이름은 로드 시 hex로 매핑). 프리셋 20색 +
  `<input type="color">` 컬러 휠(Chromium 내장 피커)로 자유 선택.
- 다크 테마 변형은 저장하지 않고 **런타임에 자동 생성**: HSL 변환 후 `L' = 0.18 + 0.12·L`,
  `S' = 0.40·S`. 이 알고리즘은 두 곳에 중복 구현되어 있으며 반드시 동기화 유지:
  - 웹: `ui/common/color.js` `effectiveBg()` — body 배경·글자색(`--note-bg`/`--note-fg`),
    글자색은 배경 상대 휘도 < 0.5이면 밝은 색으로 자동 선택(`textColorFor`).
  - 네이티브: `src/Theme.cpp` `StickerColor()` — 리사이즈 밴드 브러시 (웹 배경과 이음새 없이 일치).

### 메모 그룹

- `GroupWindow`(src/GroupWindow.cpp): StickerWindow와 동일한 프레임리스 구조의 그룹 창.
  `ui/group.html`이 멤버 메모들을 카드로 렌더링한다. 그룹 소속 메모는 **개별 창을 만들지 않고**
  데이터만 `App::groupedStickers_`(map)에 보관 — 카드 편집은 `member.save` 브리지로 저장.
- 데이터: `groups\<uuid>.json` = {title, layout, x/y/w/h, hidden, memberIds(카드 순서)}.
  스티커의 `groupId` 필드가 소속을 나타냄. 시작 시 고아 groupId는 자동 해제.
- **드래그 앤 드롭 추가**: 플로팅 스티커의 `WM_EXITSIZEMOVE`에서 순수 이동(크기 불변)일 때만
  커서 아래 표시 중인 그룹을 z-order 순으로 탐색(`GroupUnderCursor`). 발견 시 해당 스티커
  페이지에 `app.flush`를 보내고 250ms 지연(`RunOnUiDelayed`) 후 창 파괴 → 그룹에 흡수.
  드래그 중에는 `WM_MOVING` → `group.dragHover` 이벤트로 그룹 카드 영역을 하이라이트.
- **정렬**: `#cards.grid`(CSS grid, S/M/L 크기 클래스가 열 폭·기본 높이 `--card-h` 결정) /
  `#cards.masonry`(CSS multi-column) / `#cards.list`(한 열 전체 폭). 그리드 크기는
  `gridSize`("s"|"m"|"l", 전역), 멤버별 높이는 `memberHeights{id→px}`로 저장되며 카드 하단
  `.gcard-resize` 핸들 드래그(`group.setMemberHeight`, 더블클릭=0=기본 복원)로 조절 —
  인라인 height가 CSS 기본값을 덮는다. 멤버 제거 시 높이 엔트리도 함께 정리.
  카드 재정렬은 상단 바 HTML5 DnD → DOM 순서 → `group.reorder`.
- **분리/삭제**: 팝아웃(`PopOutStickerAt`)은 좌표 없으면 그룹 오른쪽 옆, 좌표가 있으면
  그 지점에 플로팅 창으로 복원. 카드 상단 바를 창 밖으로 드래그하면 `dragend`의
  screen 좌표(×devicePixelRatio=물리)를 브리지로 전달 — 드롭 지점이 다른 그룹 위이면
  그 그룹으로 멤버십을 이전한다. 그룹 삭제(`DeleteGroupReleaseMembers`)는 멤버를
  삭제하지 않고 전부 분리한다.
- **외형**: `color`(hex, 빈 값 = 테마 기본)·`opacity`(0.0~1.0). 투명도는 **그룹 배경에만**
  적용되고 헤더·카드는 불투명을 유지한다. **이중 창 구조**로 구현:
  - **backdrop 창**(`GroupWindow::hwnd_`, 주 창): 배경색만 칠하고
    `WS_EX_LAYERED + SetLayeredWindowAttributes(LWA_ALPHA)`로 반투명 — 자식 없는 단순
    창의 균일 알파는 데스크톱 투과가 안정적으로 동작한다. 리사이즈 밴드·이동(HTCAPTION,
    배경 드래그로 그룹 이동)·스티커 드롭 판정·트레이 토글의 기준. 알파는 최소 1로
    클램프(0이면 클릭이 통과해 조작 불가).
  - **content 창**(owned popup): WebView2 호스트. 페이지가 헤더/카드/팝오버의 사각형을
    `group.setShape` 브리지로 보내면 `SetWindowRgn`으로 그 영역만 남기고 잘라낸다 —
    잘린 영역 뒤로 backdrop(반투명 배경)이 보인다. 페이지 body는 backdrop과 같은 단색으로
    칠해 region 경계 AA가 자연스럽다. shape는 렌더/스크롤/리사이즈/팝오버 토글/카드 편집
    시 갱신 + 500ms 안전망 폴링(변경 없으면 스킵).
  - 실패한 접근(재시도 금지): 창 전체 LWA_ALPHA는 카드·헤더까지 반투명해짐. Win11에서
    `DwmExtendFrameIntoClientArea(-1)`는 불투명 흰 재질, `SetWindowCompositionAttribute`
    ACCENT_ENABLE_TRANSPARENTGRADIENT는 데스크톱이 아닌 시스템 단색을 백드롭으로 그린다.
  헤더 `+` 메뉴로 그룹 안에 새 메모를 직접 생성(`NewMemoInGroup`).
- 전체 보이기/감추기·테마·브로드캐스트·종료 시퀀스는 그룹 창도 포함.

### 테마 / 다국어

- 테마: `system`이면 레지스트리 `AppsUseLightTheme` 조회, `WM_SETTINGCHANGE("ImmersiveColorSet")`
  수신 시 재평가 → `theme.changed` 브로드캐스트 → 웹은 `data-theme` 속성 전환(CSS 변수).
  Manager 타이틀바는 `DWMWA_USE_IMMERSIVE_DARK_MODE`.
- 다국어: `ui/locales/*.json` 단일 소스. 웹은 `data-i18n` 속성 치환, 네이티브(트레이 메뉴)는
  같은 파일을 nlohmann/json으로 파싱. 언어 변경은 재시작 없이 반영.

### 수명주기

- 단일 인스턴스: `Local\SuperSticker.Instance` 뮤텍스. 중복 실행 시 기존 인스턴스에
  `WM_COPYDATA`를 보내 전체 표시 후 종료.
- 자동 시작: HKCU `...\Run`에 `"<exe>" --hidden`. 설정 화면은 레지스트리 실제 상태와 동기화.
- **메모창 다중 선택** (세션 한정, 저장 안 함 — `App::selected_`): Shift+클릭으로 창을
  고르고, 선택된 창을 다시 Shift+클릭하면 해제된다. 페이지는 capture 단계의 mousedown을
  `selection.click {id, shift}`로 보고하고 판단은 네이티브가 한다 —
  shift면 토글, 아니면 **선택 밖 창일 때만** 전체 해제(선택된 창의 평범한 클릭은 함께
  드래그하려는 동작이므로 선택을 지킨다). 상태 변경은 `selection.changed {ids}` 방송으로
  각 창의 선택 테두리를 갱신한다(`SyncSelectionLook`). 테두리는 **그룹창에 메모를 드롭할
  때의 하이라이트와 같은 모양** — 네이티브 WM_ERASEBKGND에서 GDI+ 안티앨리어싱 라운드
  패스로 그리고(색 #3B82F6, 두께 3dip, 모서리 지름 16dip), DWM 보더 색도 같은 액센트로
  맞춘다. 그룹창의 드롭 하이라이트 코드와 동일하게 유지할 것 (한쪽만 바꾸면 어긋난다).
  **주의**: 창 클래스에 CS_HREDRAW/CS_VREDRAW가 없어 크기가 바뀌면 새로 드러난 영역만
  다시 그려진다 → 테두리가 점선처럼 끊긴다. 그래서 선택 중에는 WM_SIZE에서 클라이언트
  전체를 무효화해 테두리를 이어 준다.
  - **함께 이동**: `WM_ENTERSIZEMOVE`에서 선택된 다른 창들의 시작 위치를 모아 두고,
    `WM_MOVING`마다 드래그 창의 이동량(자석 보정 후)을 그대로 적용해 상대 위치를 지킨다.
    자석은 함께 끌려오는 창을 후보에서 제외한다(서로 당겨 레이아웃이 무너지므로).
    종료 시 따라온 창들의 좌표도 함께 저장한다.
  - **Delete = 숨김**: 선택된 창에서 Delete를 누르면 `selection.hide`로 모두 숨긴다.
    단 캐럿이 편집 요소에 있으면 원래의 글자 삭제로 둔다. 선택되는 순간 캐럿을 빼
    (`blur()`) 두 동작이 헷갈리지 않게 한다.
  - **해제 경로**: 선택 밖 메모창 평범한 클릭 / 그룹·설정 창의 Shift 없는 클릭
    (`selection.clear`) / 다른 앱·바탕화면으로 포커스 이동(`WM_ACTIVATEAPP` wParam=FALSE).

- **창 자석 정렬** (`settings.magnet.enabled` 기본 On, `.gap` 기본 10 논리 px): 메모창을
  드래그해 다른 메모창 근처로 가져가면 정해진 간격으로 붙고 가장자리가 맞춰진다.
  `WM_MOVING`이 준 제안 사각형을 `App::SnapStickerRect`가 보정한 뒤 TRUE를 반환하는
  방식(창을 직접 옮기지 않으므로 드래그가 끊기지 않는다). 축별로 후보를 모아 가장 가까운
  것을 고른다 — X: 상대 오른쪽+간격 / 왼쪽-간격 / 좌·우 가장자리 정렬, Y: 아래+간격 /
  위-간격 / 위·아래 가장자리 정렬. 다른 축이 멀면 후보에서 제외해 엉뚱한 창에 끌리지 않게
  한다. 자석이 당기는 거리는 민감도 설정(`settings.magnet.sensitivity`)이 정한다 —
  상 26 / 중 12(기본) / 하 6 논리 px.
  **함정 — 자석 피드백 루프**: WM_MOVING의 제안 사각형을 보정하면 이동 루프의 기준
  사각형도 그 위치로 갱신된다. 다음 제안은 "붙은 위치 + 직전 메시지의 작은 이동량"이라
  항상 임계값 안에 머물고, **아무리 멀리 끌어도 창이 자석에서 떨어지지 않는다**
  (실측: 커서 300px 이동에도 창 이동량 0). 그래서 WM_MOVING마다 드래그 시작 시점의
  rect·커서(`dragStartRect_`/`dragStartCursor_`)로부터 **커서만 따라가는 자유 위치를
  다시 계산해** 자석의 입력으로 준다. 자석이 창을 어디로 옮기든 자유 위치는 영향을 받지
  않으므로 커서가 임계값 밖으로 나가면 자연스럽게 떨어진다. 커서가 전혀 움직이지
  않았으면 키보드 이동(Alt+Space → 이동)으로 보고 제안 좌표를 그대로 존중한다.
- **크기 변경 중 자석** (`App::SnapStickerResize`, `WM_SIZING`): 테두리를 끌어 크기를
  바꿀 때도 인접한 창의 변에 맞춰진다. 이동 자석과 결정적으로 다른 점은 **사각형을
  통째로 옮기면 안 된다**는 것 — 잡고 있는 변만 움직여야 하므로 `OffsetRect` 대신
  그 변의 좌표에만 이동량을 더한다. 어느 변을 잡았는지는 `WM_SIZING`의 wParam
  (`WMSZ_*`)이 알려 주며, 모서리는 가로·세로를 한 변씩 동시에 끄는 것으로 다룬다.
  변마다 후보는 셋이다 — 예를 들어 아랫변은 상대의 아랫변(같은 Y로 정렬) / 윗변 /
  윗변-간격(위에 간격 두고 맞대기). 근접 조건은 이동 자석과 같다.
  **자유 좌표를 쓰면 최소 크기 제한을 지나친다**: 잡은 변을 커서 기준으로 다시 계산하는
  순간 `DefWindowProc`이 `WM_GETMINMAXINFO`로 걸어 둔 제한을 우회하게 되므로,
  핸들러 끝에서 같은 계산(`kMinWDip`/`kMinHDip`)으로 다시 지킨다.
  `inSizeMove_`는 `dragStartRect_`/`dragStartCursor_`가 유효한 구간(모달 이동·크기
  루프 안)을 표시한다. 루프 밖에서 `WM_SIZING`이 들어오면 낡은 기준값으로 창이 튀므로,
  이 플래그가 서 있을 때만 자유 좌표 재계산을 적용한다.
- **메모창 UI 자동 숨김** (`settings.autoHideUi`, 기본 On, **스티커 창 전용**): 마우스가
  창을 벗어나고 3초 뒤 **창 내용 전체를 페이드 아웃 → 헤더/서식 툴바를 레이아웃에서
  제거(`html.ui-collapsed`로 display:none, 본문이 그 여백을 채움) → 새 레이아웃으로
  페이드 인**한다. 보일 때는 역순. 텍스트가 자리를 옮기는 순간은 항상 화면이 비어 있을
  때라 갑작스러운 이동이 보이지 않는다. **창 크기는 바뀌지 않는다.**
  페이드는 `body.ui-fading > * {opacity:0}`(0.18s) 하나로 처리하고, 전환 중 취소는
  세대 토큰(`seq`)으로 한다 — 페이드 아웃 도중 클릭이 오면 토큰이 갈려 이전 전환이
  무효화되고 즉시 되살아난다.
  **web 메모만 네이티브가 한 발 낀다**: 사이트 뷰가 별도의 네이티브 자식 창이라 페이지
  CSS 리플로우가 닿지 않는다. 페이지가 `window.setUiHidden`으로 상태를 알리면
  `LayoutWebView`가 상단 스트립을 64→32 CSS px(URL바만)로 줄여 사이트 뷰가 타이틀바
  자리를 채운다. 사이트 뷰는 페이드할 수 없어 이 확장은 즉시 일어난다.
  그룹창은 대상이 아니다 (반투명 backdrop + shape region 구조라 헤더를 접으면 얻는 것보다
  깨질 위험이 크다).
  **표시 조건**(`settings.uiRevealOnClick`, 기본 On, 자동 숨김이 켜져 있을 때만 의미 있음):
  On이면 창을 **클릭해야** UI가 올라오고(마우스를 올리는 것만으로는 반응하지 않음),
  Off면 호버로 올라온다. 클릭 전용일 때는 `canHide()`가 호버를 무시해
  마우스를 올려둔 것만으로 UI가 붙잡히지 않는다. 토글은 `ui.revealModeChanged` 방송.
  **텍스트 입력 중에는 숨기지 않는다** — `document.hasFocus()` +
  activeElement가 편집 요소면 보류, focusout/창 blur 때 다시 예약.
  **숨길 때 AI 패널도 닫는다**(닫기 버튼에 맡겨 진행 중 요청 정리까지 함께 한다).
  다시 보일 때 패널을 되살리지는 않는다. 다만 **응답을 받는 중이면 숨기지 않는다** —
  숨김이 패널을 닫으며 요청을 끊기 때문이며, 진행 여부는 AI 쪽이 노출하는
  `window.__aiStreaming()`으로 묻는다(`#aiStopBtn.disabled`는 `setActionsState()`가
  한 번이라도 돌기 전에는 false라 신뢰할 수 없다 — 실측으로 자동 숨김이 아예 막혔다).
  토글은 `ui.autoHideChanged` 브로드캐스트로 즉시 반영.
  **연혁**: 처음에는 페이드 후 네이티브가 창을 그만큼 줄이는 방식이었다(자식 뷰를 화면에
  고정하는 핀, `SetWindowRgn` 클리핑, 자석의 숨김-기준 좌표 환산 `AlignBasis`까지 딸려
  있었다). 창 크기를 그대로 두라는 요구로 v1.5에서 제거해 제자리 페이드(여백 유지)로
  줄였고, 곧이어 "본문이 여백을 채우되 이동이 보이지 않게"라는 요구로 지금의
  페이드 아웃 → 리플로우 → 페이드 인 3단계가 됐다. 자석은 실제 rect를 그대로 쓴다.
- 종료: `app.flush` 브로드캐스트로 웹 측 저장 디바운스를 플러시할 시간(350ms)을 준 뒤 창 파괴.
- explorer 재시작 시 `TaskbarCreated` 메시지로 트레이 아이콘 복구.
- **해상도/모니터 변경 대응**: `WM_DISPLAYCHANGE`·`WM_SETTINGCHANGE(SPI_SETWORKAREA)` 수신 시
  400ms 디바운스 후 `App::ClampAllWindowsToScreen()` — 모든 스티커·그룹 창을
  `util::ClampRectToWorkArea`(가장 가까운 모니터 작업 영역, 크기 초과 시 축소 포함)로
  보정하고 위치를 저장한다. 창 생성 시에도 같은 보정을 적용해 저장된 좌표가
  현재 화면 밖이면(모니터 해제 등) 자동으로 안으로 들어온다.

## 빌드 시스템

- CMake + Ninja (VS 번들) + `CMakePresets.json`. MSVC `/utf-8 /W4`, Release `/O2`.
- 의존성은 FetchContent로 nupkg(zip)를 직접 다운로드 — nuget.exe/vcpkg 불필요:
  - `Microsoft.Web.WebView2` (정적 로더 `WebView2LoaderStatic.lib` 링크 → DLL 배포 불필요.
    정적 CRT `/MT` 필요 — `CMAKE_MSVC_RUNTIME_LIBRARY`로 설정)
  - `Microsoft.Windows.ImplementationLibrary` (WIL, 헤더 온리)
  - `nlohmann/json` (단일 헤더)
- `ui\` 폴더는 빌드 후 출력 폴더로 복사 (`add_custom_command`).
- 디버그 빌드는 DevTools 활성화(`SS_DEBUG`). E2E 테스트 시
  `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223` 환경변수로
  CDP를 붙일 수 있다.

## 설치 프로그램 (Inno Setup)

- 사용자 단위 설치(`PrivilegesRequired=lowest`) — 권한 상승 불필요.
- 한국어/영어 설치 UI, 자동 시작 태스크, WebView2 런타임 미설치 시 부트스트래퍼 자동 실행.
- 언인스톨 시 Run 키 제거, 사용자 데이터(`%APPDATA%\SuperSticker`)는 보존.
