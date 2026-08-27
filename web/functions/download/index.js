// GET /download — 항상 최신 설치 프로그램으로 302 리다이렉트한다.
// 릴리스 에셋 파일명에 버전이 포함돼 있어도 사이트의 다운로드 주소는 고정된다.
import { fetchLatest } from '../api/latest.js';

const FALLBACK = 'https://github.com/80dots/SuperStickers/releases/latest';

export async function onRequestGet() {
  try {
    const { setup } = await fetchLatest();
    if (!setup) throw new Error('setup asset not found');
    return Response.redirect(setup.url, 302);
  } catch {
    // API 실패 시에도 사용자가 막히지 않도록 릴리스 페이지로 보낸다.
    return Response.redirect(FALLBACK, 302);
  }
}
