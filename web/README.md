# Super Sticker 랜딩 사이트

Cloudflare Pages로 배포되는 정적 사이트입니다. 빌드 도구나 의존성이 없습니다.

```
web/
├─ index.html          랜딩 페이지
├─ privacy.html        개인정보처리방침
├─ _headers            보안 헤더 · 캐시 정책
├─ assets/
│  ├─ style.css        디자인 토큰(라이트/다크) + 앱 UI 목업 스타일
│  ├─ app.js           언어·테마 전환, 스크롤 등장, 최신 릴리스 정보 표시
│  ├─ logo.svg         로고 겸 파비콘
│  └─ img/og.png       SNS 공유 미리보기 (scripts/make_og.py로 생성)
└─ functions/          Cloudflare Pages Functions
   ├─ api/latest.js    GET /api/latest → 최신 릴리스 JSON
   └─ download/
      ├─ index.js      GET /download → 최신 설치 프로그램으로 302
      └─ portable.js   GET /download/portable → 최신 포터블 ZIP으로 302
```

## 디자인 메모

- **앱 화면은 스크린샷이 아니라 CSS 목업이다.** `.mock`(스티커 창), `.selmenu`(텍스트 선택
  메뉴), `.hlpop`(형광펜 팝오버), `.aipanel`(AI 패널)을 조합해 `index.html`의 `.stage`
  안에서 배치한다. 내부 치수가 전부 `em`이라 `.stage`의 `font-size`만 바꾸면 통째로
  확대·축소되고, 배치는 %로 잡아 반응형에서도 유지된다. 릴리스마다 화면을 다시 찍을 필요가 없다.
- **색은 앱과 같은 출처**다. 스티커 프리셋(`ui/common/color.js`)과 형광펜 프리셋에서 가져온
  고정색은 테마와 무관하게 그대로 두고(앱의 메모창도 그렇다), 페이지 배경·글자만 테마 토큰을 쓴다.
- **언어 전환은 CSS가 한다.** `html[lang]`을 보고 `[data-lang-ko]` / `[data-lang-en]` 중
  한쪽만 표시한다. 숨김 규칙의 명시도를 (0,2,1)로 올려 두었으니, `display`를 지정하는
  컴포넌트 규칙(`.stat .cap` 같은)을 새로 만들 때 **lang 속성을 가진 요소 자체에 걸지 말고**
  바깥 요소에 걸 것. 그러지 않으면 두 언어가 동시에 보인다(실제로 겪음).
- 새 기능이 나오면 고칠 곳: 히어로 문구, `#new` 섹션 세 장, 기능 그리드, `og.png`.

## 다운로드 링크가 동작하는 방식

릴리스 에셋 파일명에 버전이 들어 있어(`SuperSticker-Setup-1.1.0.exe`) GitHub의
`releases/latest/download/<고정이름>` 방식을 쓸 수 없습니다. 대신 Pages Function이
GitHub API로 최신 릴리스를 조회해 리다이렉트합니다.

- 사이트의 다운로드 주소는 **항상 `/download`** 로 고정됩니다.
- 새 버전을 릴리스하면 사이트는 **자동으로** 최신 파일을 가리킵니다. HTML 수정 불필요.
- GitHub API 응답은 Cloudflare 엣지에 10분간 캐시되어 호출 횟수 제한(IP당 시간당 60회)에
  걸리지 않습니다.
- API 조회가 실패하면 릴리스 페이지로 리다이렉트되어 사용자가 막히지 않습니다.

## Cloudflare Pages 설정

대시보드 → Workers & Pages → Create → Pages → Connect to Git

| 항목 | 값 |
|---|---|
| Production branch | `main` |
| Framework preset | None |
| Build command | *(비워 둠)* |
| Build output directory | `/` |
| Root directory | `web` |

`functions/` 폴더는 Root directory(`web`) 기준으로 인식되므로 별도 설정이 필요 없습니다.

## 로컬 확인

Functions까지 포함해 확인하려면 **반드시 `web/` 안에서** 실행합니다.
wrangler는 `functions/`를 현재 작업 디렉터리 기준으로 찾기 때문에,
바깥에서 `wrangler pages dev web`으로 실행하면 Functions가 무시되고
`/download`가 index.html을 반환합니다.

```bash
cd web && npx wrangler pages dev .
```

정적 부분만 빠르게 보려면:

```bash
python -m http.server 8791 --directory web
```

## 배포 직후 확인할 것

Pages 첫 배포가 끝나면 Functions가 제대로 인식됐는지 확인합니다.

```bash
curl -sI https://<프로젝트>.pages.dev/download
```

`HTTP/2 302` 와 `location: https://github.com/.../SuperSticker-Setup-*.exe` 가 보이면 정상입니다.
`200`이 오면서 HTML이 반환되면 Functions가 인식되지 않은 것이므로,
Pages 설정의 **Root directory가 `web`인지** 확인하세요.

## 릴리스 후 할 일

없습니다. GitHub에 새 릴리스를 올리면 최대 10분 안에 사이트가 최신 버전을 가리킵니다.
