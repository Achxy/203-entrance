export const SECURITY_HEADERS = Object.freeze({
  'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self' https://203-arcade-relay.achyuthanugraha.workers.dev wss://203-arcade-relay.achyuthanugraha.workers.dev; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
  'X-Frame-Options': 'DENY',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'Permissions-Policy': 'camera=(), microphone=(), geolocation=(), payment=(), usb=()',
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cache-Control': 'no-store'
});

// All assets are compiled into this Worker. No URL is fetched, no path is read
// from a filesystem, and no request credentials or body are inspected or stored.
export function createWorker(assets) {
  const encoder = new TextEncoder();
  function response(request, status, body, contentType, extra = {}) {
    const bytes = typeof body === 'string' ? encoder.encode(body) : body;
    return new Response(request.method === 'HEAD' ? null : bytes, {status, headers: {
      ...SECURITY_HEADERS, 'Content-Type': contentType, 'Content-Length': String(bytes.length), ...extra
    }});
  }
  function error(request, status, body, extra) {
    return response(request, status, body, 'text/plain; charset=utf-8', extra);
  }
  return {
    async fetch(request) {
      if (request.method !== 'GET' && request.method !== 'HEAD')
        return error(request, 405, 'Method not allowed\n', {'Allow': 'GET, HEAD'});
      let pathname;
      try {
        const raw = new URL(request.url).pathname;
        // Encoded separators and controls are never asset aliases. Decode once
        // only, then match an exact key; query strings do not select new assets.
        if (/%(?:2f|5c|00)/i.test(raw)) return error(request, 400, 'Invalid path\n');
        pathname = decodeURIComponent(raw);
        if (/[\u0000-\u001f\u007f\\]/u.test(pathname) || pathname.split('/').some(part => part === '.' || part === '..'))
          return error(request, 400, 'Invalid path\n');
      } catch {return error(request, 400, 'Invalid path\n');}
      if (pathname === '/') pathname = '/index.html';
      if (!Object.hasOwn(assets, pathname)) return error(request, 404, 'Not found\n');
      const asset = assets[pathname], headers = {...SECURITY_HEADERS,
        'Content-Type': asset.type, 'Content-Length': String(asset.length)};
      if (request.method === 'HEAD') return new Response(null, {status: 200, headers});
      const binary = atob(asset.body), bytes = Uint8Array.from(binary, character => character.charCodeAt(0));
      return new Response(bytes, {status: 200, headers});
    }
  };
}
