import assert from 'node:assert/strict';
import { test } from 'node:test';
import { generateKeyPair, SignJWT } from 'jose';
import site, { createSiteHandler, AuthLimiter } from '../site.mjs';
import accountWorker, { AuthLimiter as AccountLimiter } from '../app.mjs';

const origin = 'https://app.example.test', issuer = 'https://team.example.test', audience = 'site-test-audience';
const keys = await generateKeyPair('RS256');
async function assertion(claims = {}, { algorithm = 'RS256', key = keys.privateKey, omit = [] } = {}) {
  const now = Math.floor(Date.now() / 1000);
  const payload = { iss: issuer, aud: audience, sub: 'invited-human', email: 'human@example.test', type: 'app', iat: now, nbf: now - 1, exp: now + 600, ...claims };
  for (const name of omit) delete payload[name];
  return new SignJWT(payload).setProtectedHeader({ alg: algorithm }).sign(key);
}
function fixture(assetFetch = async () => new Response('protected asset', { headers: { 'Content-Type': 'text/javascript' } })) {
  const requests = [];
  const env = { APP_ORIGIN: origin, ACCESS_TEAM_DOMAIN: issuer, ACCESS_AUDIENCE: audience,
    ASSETS: { async fetch(request) { requests.push(request); return assetFetch(request); } } };
  const fetch = createSiteHandler({ accessKeys: async () => keys.publicKey });
  const request = (path = '/app.js', { token, headers, ...options } = {}) => new Request(origin + path,
    { ...options, headers: { ...(token ? { 'cf-access-jwt-assertion': token } : {}), ...headers } });
  return { env, requests, fetch, request };
}
function privateHeaders(response) {
  assert.equal(response.headers.get('Cache-Control'), 'private, no-store');
  assert.equal(response.headers.get('CDN-Cache-Control'), 'no-store');
  assert.equal(response.headers.get('Cloudflare-CDN-Cache-Control'), 'no-store');
  assert.equal(response.headers.get('X-Content-Type-Options'), 'nosniff');
  assert.equal(response.headers.get('Referrer-Policy'), 'no-referrer');
  assert.equal(response.headers.get('X-Frame-Options'), 'DENY');
  assert.match(response.headers.get('Content-Security-Policy'), /frame-ancestors 'none'/);
}

test('HTML, scripts, styles, fonts and legal assets require a real human Access assertion', async () => {
  const f = fixture(), token = await assertion();
  for (const path of ['/', '/index.html', '/app.js', '/styles.css', '/assets/caseforge/fonts/Ubuntu-Regular.ttf', '/licenses/CASE-FORGE-MIT.txt', '/source-manifest.json']) {
    const before = f.requests.length;
    const denied = await f.fetch(f.request(path), f.env);
    assert.equal(denied.status, 403); privateHeaders(denied);
    assert.equal(f.requests.length, before);
    const accepted = await f.fetch(f.request(path, { token }), f.env);
    assert.equal(accepted.status, 200); assert.equal(f.requests.length, before + 1);
    assert.equal(new URL(f.requests.at(-1).url).pathname, path === '/' ? '/index.html' : path);
    assert.equal(await accepted.text(), 'protected asset'); privateHeaders(accepted);
    assert.match(accepted.headers.get('Content-Security-Policy'), /connect-src 'self' wss:\/\/app\.example\.test https:\/\/challenges\.cloudflare\.com/);
  }
});

test('wrong issuer, audience, expiry, human claims and signature never reach static storage', async () => {
  const f = fixture();
  const badClaims = [{ iss: 'https://other.example.test' }, { aud: 'different-app' }, { exp: 1 },
    { nbf: Math.floor(Date.now() / 1000) + 600 }, { sub: '' }, { email: '' }, { type: 'service' },
    { common_name: 'synthetic-service' }, { service_token_id: 'synthetic-service' }];
  const tokens = await Promise.all(badClaims.map(value => assertion(value)));
  tokens.push(await assertion({}, { omit: ['exp'] }), await assertion({}, { algorithm: 'HS256', key: new Uint8Array(32).fill(4) }), 'not.a.signature');
  for (const token of tokens) assert.equal((await f.fetch(f.request('/app.js', { token }), f.env)).status, 403);
  assert.equal(f.requests.length, 0);
});

test('missing assets, malformed origin and missing Access configuration fail closed', async () => {
  const f = fixture(), token = await assertion();
  for (const changed of [{ ASSETS: undefined }, { ASSETS: {} }, { APP_ORIGIN: '' }, { APP_ORIGIN: 'http://app.example.test' },
    { APP_ORIGIN: origin + '/' }, { APP_ORIGIN: origin + '/path' }, { ACCESS_TEAM_DOMAIN: '' },
    { ACCESS_TEAM_DOMAIN: 'http://team.example.test' }, { ACCESS_AUDIENCE: '' }]) {
    const response = await f.fetch(f.request('/', { token }), { ...f.env, ...changed });
    assert.equal(response.status, 503); privateHeaders(response);
  }
  assert.equal(f.requests.length, 0);
  assert.equal((await site.fetch(f.request('/app.js', { headers: { 'cf-access-authenticated-user-email': 'human@example.test' } }),
    { ...f.env, ACCESS_BYPASS: true, SYNTHETIC_PREVIEW: true })).status, 403);
});

test('alternate hosts and foreign Origin headers do not reach the asset binding', async () => {
  const f = fixture(), token = await assertion();
  for (const target of ['https://alternate.example.test/app.js', 'http://app.example.test/app.js']) {
    assert.equal((await f.fetch(new Request(target, { headers: { 'cf-access-jwt-assertion': token } }), f.env)).status, 403);
  }
  assert.equal((await f.fetch(f.request('/app.js', { token, headers: { Origin: 'https://foreign.example.test' } }), f.env)).status, 403);
  assert.equal(f.requests.length, 0);
});

test('static requests preserve asset validators but strip authentication and credential headers', async () => {
  const f = fixture(), token = await assertion();
  const response = await f.fetch(f.request('/app.js?v=1', { token, headers: {
    Cookie: '__Host-platform-session=private', Authorization: 'Bearer private', 'CF-Access-Client-Secret': 'private',
    Referer: origin + '/private', 'If-None-Match': 'asset-version', Range: 'bytes=0-5', Accept: 'text/javascript'
  } }), f.env);
  assert.equal(response.status, 200);
  const forwarded = f.requests[0];
  assert.equal(forwarded.url, origin + '/app.js?v=1'); assert.equal(forwarded.redirect, 'manual');
  assert.equal(forwarded.headers.get('If-None-Match'), 'asset-version'); assert.equal(forwarded.headers.get('Range'), 'bytes=0-5');
  for (const name of ['Cookie', 'Authorization', 'cf-access-jwt-assertion', 'CF-Access-Client-Secret', 'Referer']) assert.equal(forwarded.headers.has(name), false);
});

test('asset streams and response metadata are preserved without buffering or cookies', async () => {
  const source = new Response(new ReadableStream({ start(controller) { controller.enqueue(new Uint8Array([0, 17, 255])); controller.close(); } }),
    { status: 206, headers: { 'Content-Type': 'font/ttf', 'Content-Range': 'bytes 0-2/3', ETag: 'font-v1', 'Set-Cookie': 'not-static=1', 'Cache-Control': 'public, max-age=86400' } });
  const f = fixture(async () => source), token = await assertion();
  const response = await f.fetch(f.request('/assets/font.ttf', { token }), f.env);
  assert.equal(response.body, source.body); assert.equal(response.status, 206);
  assert.equal(response.headers.get('Content-Range'), 'bytes 0-2/3'); assert.equal(response.headers.get('ETag'), 'font-v1');
  assert.equal(response.headers.has('Set-Cookie'), false); privateHeaders(response);
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()), new Uint8Array([0, 17, 255]));
});

test('HEAD is protected and bodyless, unsupported methods and static upgrades cannot reach assets', async () => {
  const f = fixture(), token = await assertion();
  const head = await f.fetch(f.request('/app.js', { method: 'HEAD', token }), f.env);
  assert.equal(head.status, 200); assert.equal(head.body, null); assert.equal(f.requests[0].method, 'HEAD');
  const deniedHead = await f.fetch(f.request('/app.js', { method: 'HEAD' }), f.env);
  assert.equal(deniedHead.status, 403); assert.equal(deniedHead.body, null);
  for (const method of ['POST', 'PUT', 'DELETE', 'OPTIONS']) {
    const response = await f.fetch(f.request('/app.js', { token, method }), f.env);
    assert.equal(response.status, 405); assert.equal(response.headers.get('Allow'), 'GET, HEAD');
  }
  assert.equal((await f.fetch(f.request('/app.js', { token, headers: { Upgrade: 'websocket' } }), f.env)).status, 400);
  assert.equal(f.requests.length, 1);
});

test('private control files and encoded path aliases cannot be served', async () => {
  const f = fixture(), token = await assertion();
  for (const path of ['/.env', '/.git/config', '/_headers', '/_redirects', '/_worker.js', '/api%2fme', '/assets/%00font.ttf', '/assets//font.ttf']) {
    assert.equal((await f.fetch(f.request(path, { token }), f.env)).status, 404);
  }
  assert.equal(f.requests.length, 0);
});

test('runtime configuration always disables synthetic entry and is Access protected', async () => {
  const f = fixture(async () => Response.json({ syntheticPreview: true })), token = await assertion();
  assert.equal((await f.fetch(f.request('/runtime-config.json'), f.env)).status, 403);
  const response = await f.fetch(f.request('/runtime-config.json', { token }), f.env);
  assert.deepEqual(await response.json(), { syntheticPreview: false, access: 'cloudflare-access' }); privateHeaders(response);
  assert.equal((await f.fetch(f.request('/runtime-config.json', { method: 'HEAD', token }), f.env)).body, null);
  assert.equal(f.requests.length, 0);
});

test('static redirects stay on the configured origin and never redirect to API handlers', async () => {
  const token = await assertion();
  for (const location of ['https://external.example.test/asset', '//external.example.test/asset', '/api/me', '/_headers']) {
    const f = fixture(async () => new Response('discarded', { status: 302, headers: { Location: location } }));
    const response = await f.fetch(f.request('/index.html', { token }), f.env);
    assert.equal(response.status, 503); assert.equal(response.headers.has('Location'), false);
  }
  const f = fixture(async () => new Response(null, { status: 308, headers: { Location: origin + '/' } }));
  const response = await f.fetch(f.request('/index.html', { token }), f.env);
  assert.equal(response.status, 308); assert.equal(response.headers.get('Location'), '/'); privateHeaders(response);
});

test('missing files keep their 404 while unavailable assets expose only a sanitized failure', async () => {
  const token = await assertion();
  const missing = fixture(async () => new Response('not found', { status: 404 }));
  const response = await missing.fetch(missing.request('/missing.js', { token }), missing.env);
  assert.equal(response.status, 404); assert.equal(await response.text(), 'not found');
  for (const fetch of [async () => { throw new Error('private-binding-details'); }, async () => new Response('private-provider-error', { status: 503 })]) {
    const f = fixture(fetch), denied = await f.fetch(f.request('/app.js', { token }), f.env);
    assert.equal(denied.status, 503); assert.doesNotMatch(await denied.text(), /private-binding|private-provider/);
  }
});

test('Access expiry during asset lookup prevents the response body from being served', async t => {
  const time = Date.now(), expires = Math.floor(time / 1000) + 60, token = await assertion({ exp: expires });
  t.mock.timers.enable({ apis: ['Date'], now: time });
  const f = fixture(async () => { t.mock.timers.setTime(expires * 1000); return new Response('expired asset'); });
  const response = await f.fetch(f.request('/app.js', { token }), f.env);
  assert.equal(response.status, 403); assert.doesNotMatch(await response.text(), /expired asset/);
});

test('API and WebSocket requests delegate the exact request/context and response without static fallback', async () => {
  const f = fixture(), ctx = { marker: 'execution-context' }, received = [];
  const upgrade = { status: 101, webSocket: { marker: 'socket' } };
  const handler = createSiteHandler({ apiHandler: async (request, env, context) => {
    received.push({ request, env, context }); return request.headers.has('Upgrade') ? upgrade : new Response('api', { status: 401, headers: { 'Set-Cookie': 'session=revoked' } });
  } });
  for (const path of ['/api', '/api/config', '/api/updates/windows/x64/latest.yml', '/api/events']) {
    const request = f.request(path, { headers: path.endsWith('events') ? { Upgrade: 'websocket' } : {} });
    const response = await handler(request, f.env, ctx);
    assert.equal(received.at(-1).request, request); assert.equal(received.at(-1).env, f.env); assert.equal(received.at(-1).context, ctx);
    if (path.endsWith('events')) assert.equal(response, upgrade);
    else { assert.equal(response.status, 401); assert.equal(response.headers.get('Set-Cookie'), 'session=revoked'); }
  }
  assert.equal(f.requests.length, 0);
});

test('unprefixed handler names are static 404s and cannot invoke an API or SPA fallback', async () => {
  const f = fixture(async () => new Response('not found', { status: 404 })), token = await assertion();
  let delegated = 0;
  const handler = createSiteHandler({ accessKeys: async () => keys.publicKey, apiHandler: async () => { delegated++; return new Response('api'); } });
  for (const path of ['/events', '/me', '/downloads', '/unknown-route', '/apilookalike']) {
    const before = f.requests.length;
    const denied = await handler(f.request(path), f.env);
    assert.equal(denied.status, 403); assert.equal(f.requests.length, before); privateHeaders(denied);
    const response = await handler(f.request(path, { token }), f.env);
    assert.equal(response.status, 404); assert.equal(await response.text(), 'not found'); privateHeaders(response);
    assert.equal(new URL(f.requests.at(-1).url).pathname, path);
  }
  assert.equal(delegated, 0);
});

test('the default API path retains the existing account authentication and feature gates', async () => {
  const f = fixture(), token = await assertion();
  const env = { ...f.env, DB: {}, AUTH_LIMITER: {}, RATE_KEY_SECRET: 'r'.repeat(32), TURNSTILE_SITE_KEY: 'synthetic-site-key' };
  assert.equal((await f.fetch(f.request('/api/config'), env)).status, 403);
  const response = await f.fetch(f.request('/api/config', { token }), env);
  assert.equal(response.status, 200); assert.equal((await response.json()).account_service, true);
  assert.equal(f.requests.length, 0);
  assert.equal(site.scheduled, accountWorker.scheduled); assert.equal(AuthLimiter, AccountLimiter);
});
