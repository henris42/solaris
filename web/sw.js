// Passes every request of the page on and adds to each answer the two headers a browser asks
// for before it lets a page run threads. For servers that cannot be told to send them.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', e => {
  const r = e.request;
  if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;
  e.respondWith(fetch(r).then(res => {
    if (res.status === 0) return res;  // an answer the page may not look into: left as it is
    const headers = new Headers(res.headers);
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    const empty = [101, 204, 205, 304].includes(res.status);
    return new Response(empty ? null : res.body, { status: res.status, statusText: res.statusText, headers });
  }));
});
