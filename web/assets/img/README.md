# 이미지 넣는 곳

사이트의 앱 화면은 **스크린샷이 아니라 CSS로 그린 목업**(`assets/style.css`의 `.mock`,
`.selmenu`, `.hlpop`, `.aipanel`)이다. 앱을 새로 찍어 올릴 필요가 없고, 라이트/다크와
확대·축소에 모두 따라온다. 목업 문구를 바꿀 때는 `index.html`의 `.stage` 안을 고친다.

그래서 이 폴더에 필요한 파일은 하나뿐이다.

| 파일명 | 쓰이는 곳 | 크기 |
|---|---|---|
| `og.png` | SNS 공유 미리보기(`og:image`) | **1200×630 고정** |

## og.png 다시 만들기

```bash
python scripts/make_og.py
```

문구·색은 스크립트 안에 있다. 사이트 팔레트를 바꾸면 스크립트의 색 상수도 함께 고친다.
Windows 기본 맑은 고딕으로 렌더링하므로 별도 폰트 설치가 필요 없다.

## 스크린샷을 넣고 싶다면

목업 대신 실제 화면을 쓰고 싶으면 `<figure class="shot">` 같은 자리를 새로 만들고
`index.html`에서 참조한다. PNG가 1MB를 넘으면 WebP로 변환하는 편이 좋다.

```bash
npx @squoosh/cli --webp auto web/assets/img/hero.png
```
