// GET /api/latest — 최신 릴리스 정보를 JSON으로 반환한다.
// GitHub API는 미인증 시 IP당 시간당 60회 제한이 있고 Worker는 IP를 공유하므로
// Cloudflare 엣지 캐시(cacheEverything)로 실제 호출 횟수를 크게 줄인다.

const REPO = '80dots/SuperStickers';
// v1.5.3부터 자산 이름이 SuperStickers-로 바뀌었다. 예전 릴리스도 열리도록 둘 다 받는다.
const SETUP_RE = /^SuperStickers?-Setup-.*\.exe$/i;
const PORTABLE_RE = /^SuperStickers?-Portable-.*\.zip$/i;

export async function fetchLatest() {
  const res = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, {
    headers: {
      'User-Agent': 'SuperStickers-Site',
      Accept: 'application/vnd.github+json',
    },
    cf: { cacheTtl: 600, cacheEverything: true },
  });
  if (!res.ok) throw new Error(`github ${res.status}`);

  const rel = await res.json();
  const pick = (re) => (rel.assets || []).find((a) => re.test(a.name)) || null;
  const setup = pick(SETUP_RE);
  const portable = pick(PORTABLE_RE);

  return {
    version: (rel.tag_name || '').replace(/^v/, ''),
    tag: rel.tag_name || '',
    publishedAt: rel.published_at || '',
    releaseUrl: rel.html_url || `https://github.com/${REPO}/releases/latest`,
    setup: setup && { name: setup.name, url: setup.browser_download_url, size: setup.size },
    portable: portable && { name: portable.name, url: portable.browser_download_url, size: portable.size },
  };
}

export async function onRequestGet() {
  try {
    const data = await fetchLatest();
    return Response.json(data, {
      headers: { 'Cache-Control': 'public, max-age=600' },
    });
  } catch (err) {
    return Response.json(
      { error: String(err), releaseUrl: `https://github.com/${REPO}/releases/latest` },
      { status: 502, headers: { 'Cache-Control': 'no-store' } }
    );
  }
}
