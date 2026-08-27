# Super Sticker 랜딩 사이트

Cloudflare Pages로 배포되는 정적 사이트입니다. 빌드 도구나 의존성이 없습니다.

```
web/
├─ index.html          랜딩 페이지
├─ privacy.html        개인정보처리방침
├─ _headers            보안 헤더 · 캐시 정책
├─ assets/
│  ├─ style.css        라이트/다크 테마 토큰 포함
│  ├─ app.js           언어·테마 전환, 최신 릴리스 정보 표시
│  ├─ logo.svg         로고 겸 파비콘
│  └─ img/             스크린샷 (README.md 참고)
└─ functions/          Cloudflare Pages Functions
   ├─ api/latest.js    GET /api/latest → 최신 릴리스 JSON
   └─ download/
      ├─ index.js      GET /download → 최신 설치 프로그램으로 302
      └─ portable.js   GET /download/portable → 최신 포터블 ZIP으로 302
```

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
