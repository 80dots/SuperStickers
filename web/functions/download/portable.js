// GET /download/portable — 최신 포터블 ZIP으로 302 리다이렉트한다.
import { fetchLatest } from '../api/latest.js';

const FALLBACK = 'https://github.com/80dots/SuperStickers/releases/latest';

export async function onRequestGet() {
  try {
    const { portable } = await fetchLatest();
    if (!portable) throw new Error('portable asset not found');
    return Response.redirect(portable.url, 302);
  } catch {
    return Response.redirect(FALLBACK, 302);
  }
}
