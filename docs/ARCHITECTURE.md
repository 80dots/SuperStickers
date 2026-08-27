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
- **창 자석 정렬** (`settings.magnet.enabled` 기본 On, `.gap` 기본 10 논리 px): 메모창을
  드래그해 다른 메모창 근처로 가져가면 정해진 간격으로 붙고 가장자리가 맞춰진다.
  `WM_MOVING`이 준 제안 사각형을 `App::SnapStickerRect`가 보정한 뒤 TRUE를 반환하는
  방식(창을 직접 옮기지 않으므로 드래그가 끊기지 않는다). 축별로 후보를 모아 가장 가까운
  것을 고른다 — X: 상대 오른쪽+간격 / 왼쪽-간격 / 좌·우 가장자리 정렬, Y: 아래+간격 /
  위-간격 / 위·아래 가장자리 정렬. 다른 축이 멀면 후보에서 제외해 엉뚱한 창에 끌리지 않게
  한다. 자석이 당기는 거리는 민감도 설정(`settings.magnet.sensitivity`)이 정한다 —
  상 26 / 중 12(기본) / 하 6 논리 px.
  **UI 자동 숨김과의 관계**: 드래그 중인 창은 헤더 위에 마우스가 있어 펼쳐져 있고 나머지는
  접혀 있으므로, 실제 rect로 맞추면 손을 떼는 순간 어긋난다. 그래서 모든 계산을
  `StickerWindow::AlignBasis()`가 만드는 **"UI가 숨겨졌을 때의 사각형"** 기준으로 하고,
  구한 이동량만 실제 사각형에 적용한다. 이를 위해 페이지가 `window.setUiExtents`로 헤더·툴바
  높이(CSS px)를 보고하며, 접혀도 페이드만 하므로 값은 항상 측정 가능하다.
  자동 숨김이 꺼져 있으면 현재 rect가 곧 기준이다.
- **메모창 UI 자동 숨김** (`settings.autoHideUi`, 기본 On, **스티커 창 전용**): 마우스가
  창을 벗어나고 3초 뒤 헤더·서식 툴바가 페이드 아웃된 다음 창이 그만큼 줄어든다.
  그룹창은 대상이 아니다 (반투명 backdrop + shape region 구조라 헤더를 접으면 얻는 것보다
  깨질 위험이 크다).
  **표시 조건**(`settings.uiRevealOnClick`, 기본 On, 자동 숨김이 켜져 있을 때만 의미 있음):
  On이면 창을 **클릭해야** UI가 올라오고(마우스를 올리는 것만으로는 반응하지 않음),
  Off면 예전처럼 호버로 올라온다. 클릭 전용일 때는 `canCollapse()`가 호버를 무시해
  마우스를 올려둔 것만으로 UI가 붙잡히지 않는다. 토글은 `ui.revealModeChanged` 방송.
  **텍스트 입력 중에는 숨기지 않는다** — `document.hasFocus()` +
  activeElement가 편집 요소면 보류, focusout/창 blur 때 다시 예약.
  **핵심 설계 — 페이지 레이아웃은 절대 바꾸지 않는다**: 창 크기를 페이지와 동기
  애니메이션하는 방식은 WebView2의 비동기 래스터라이즈 때문에 본문이 덜컹거린다
  (실측으로 폐기). 대신 네이티브가 부모 창 rect만 200ms 선형(10ms 타이머)으로
  애니메이션하고, 매 프레임 WM_SIZE → `LayoutWebView`가 **WebView 자식 창을 "펼친 기준"
  화면 위치·크기에 고정(핀)**한다 (자식 뷰 bounds를 `band − 접힘오프셋`, 음수 top 허용).
  본문을 담은 창이 물리적으로 움직이지 않으므로 덜컹거림이 원천 차단되고, 부모 밖으로
  벗어난 부분(투명해진 헤더/툴바)만 잘려나간다. 접힘 오프셋은 CSS px로 보관하고
  위치·크기 저장은 `StoreGeometryFromWindow()`가 항상 "펼친 상태" 기준으로 보정
  (WM_EXITSIZEMOVE·클램프·UI Scale 변경 공통). 드래그 시작 시엔 애니메이션을 즉시 목표
  상태로 마무리해 충돌을 막는다. web 메모의 상단 스트립도 애니메이션 중간값을 따라간다.
  토글은 `ui.autoHideChanged` 브로드캐스트로 즉시 반영.
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
