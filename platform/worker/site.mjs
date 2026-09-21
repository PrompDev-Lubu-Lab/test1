import accountWorker, { createHandler } from './app.mjs';
import { SecurityError, verifyAccess } from './security.mjs';
export { AuthLimiter } from './app.mjs';

const apiPath = path => path === '/api' || path.startsWith('/api/');
const safePath = path => !/[\\%\u0000-\u0020\u007f]/.test(path) && !path.includes('//')
  && !path.split('/').some(part => part.startsWith('.') || ['_headers', '_redirects', '_worker.js'].includes(part));
const assetRequestHeaders = ['accept', 'accept-encoding', 'if-none-match', 'if-modified-since', 'range', 'if-range'];

function headersFor(origin, source) {
  const headers = new Headers(source);
  headers.delete('Set-Cookie');
  headers.set('Cache-Control', 'private, no-store');
  headers.set('CDN-Cache-Control', 'no-store');
  headers.set('Cloudflare-CDN-Cache-Control', 'no-store');
  headers.set('X-Content-Type-Options', 'nosniff');
  headers.set('Referrer-Policy', 'no-referrer');
  headers.set('X-Frame-Options', 'DENY');
  headers.set('Permissions-Policy', 'camera=(), microphone=(), geolocation=(), payment=()');
  headers.set('Content-Security-Policy', origin
    ? `default-src 'none'; script-src 'self' https://challenges.cloudflare.com; style-src 'self' 'unsafe-inline'; img-src 'self'; font-src 'self'; connect-src 'self' ${origin.replace(/^https:/, 'wss:')} https://challenges.cloudflare.com; frame-src https://challenges.cloudflare.com; base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'none'`
    : "default-src 'none'; frame-ancestors 'none'");
  return headers;
}

function failure(request, status, error, extraHeaders) {
  const headers = headersFor(null, extraHeaders);
  headers.set('Content-Type', 'application/json; charset=utf-8');
  return new Response(request.method === 'HEAD' ? null : JSON.stringify({ error,
    detail: status === 503 ? 'The private workspace is unavailable or not configured.' : 'This request is not allowed.' }), { status, headers });
}

function siteOrigin(env) {
  let origin;
  try { origin = new URL(env?.APP_ORIGIN); } catch { throw new SecurityError('site_unconfigured', 503); }
  if (origin.protocol !== 'https:' || origin.origin !== env.APP_ORIGIN || origin.username || origin.password
    || !env.ASSETS || typeof env.ASSETS.fetch !== 'function') throw new SecurityError('site_unconfigured', 503);
  return origin.origin;
}

async function discard(response) {
  try { await response?.body?.cancel(); } catch { /* No rejected asset body is sent to the client. */ }
}

/** Test seams are function arguments only; production always uses the strict Access verifier. */
export function createSiteHandler({ accessKeys, apiHandler = createHandler({ accessKeys }) } = {}) {
  return async function fetchSite(request, env, ctx) {
    const url = new URL(request.url);
    // Preserve the API's original request, authentication order, cookies and 101 response.
    if (apiPath(url.pathname)) return apiHandler(request, env, ctx);
    let asset;
    try {
      const origin = siteOrigin(env);
      if (url.origin !== origin || (request.headers.has('Origin') && request.headers.get('Origin') !== origin)) {
        return failure(request, 403, 'host_or_origin_denied');
      }
      if (!['GET', 'HEAD'].includes(request.method)) return failure(request, 405, 'method_not_allowed', { Allow: 'GET, HEAD' });
      const identity = await verifyAccess(request, env, accessKeys);
      if (!safePath(url.pathname)) return failure(request, 404, 'not_found');
      if (request.headers.has('Upgrade')) return failure(request, 400, 'upgrade_not_allowed');

      if (url.pathname === '/runtime-config.json') {
        const headers = headersFor(origin, { 'Content-Type': 'application/json; charset=utf-8' });
        return new Response(request.method === 'HEAD' ? null : JSON.stringify({ syntheticPreview: false, access: 'cloudflare-access' }), { headers });
      }

      // Static storage needs no identity, app cookies, service credentials or referrer.
      const headers = new Headers();
      for (const name of assetRequestHeaders) if (request.headers.has(name)) headers.set(name, request.headers.get(name));
      const assetUrl = new URL(url);
      if (assetUrl.pathname === '/') assetUrl.pathname = '/index.html';
      asset = await env.ASSETS.fetch(new Request(assetUrl, { method: request.method, headers, redirect: 'manual' }));
      if (!(asset instanceof Response) || asset.redirected || asset.status >= 500) throw new SecurityError('assets_unavailable', 503);
      if (identity.exp * 1000 <= Date.now()) throw new SecurityError('access_denied', 403);
      const responseHeaders = headersFor(origin, asset.headers);
      if (asset.status >= 300 && asset.status < 400 && asset.status !== 304) {
        const location = asset.headers.get('Location');
        if (!location || location.length > 2048 || ![301, 302, 303, 307, 308].includes(asset.status)) throw new SecurityError('assets_unavailable', 503);
        const target = new URL(location, url);
        if (target.origin !== origin || target.username || target.password || !safePath(target.pathname) || apiPath(target.pathname)) {
          throw new SecurityError('assets_unavailable', 503);
        }
        responseHeaders.set('Location', target.pathname + target.search + target.hash);
        await discard(asset);
        return new Response(null, { status: asset.status, headers: responseHeaders });
      }
      if (request.method === 'HEAD') await discard(asset);
      return new Response(request.method === 'HEAD' ? null : asset.body, { status: asset.status, headers: responseHeaders });
    } catch (error) {
      await discard(asset);
      return failure(request, error instanceof SecurityError && error.code === 'access_denied' ? 403 : 503,
        error instanceof SecurityError && error.code === 'access_denied' ? 'access_denied' : 'site_unavailable');
    }
  };
}

// Requires ASSETS=../app/dist, run_worker_first=true, html_handling='none',
// and not_found_handling='none'. Only the exact root maps to index.html.
// Keep the existing account maintenance trigger and Durable Object export intact.
export default { fetch: createSiteHandler(), scheduled: accountWorker.scheduled };
