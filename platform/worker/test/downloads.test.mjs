import assert from 'node:assert/strict';
import { test } from 'node:test';
import { downloadsReady, readDownloadCatalog, serveRelease } from '../downloads.mjs';
import { SecurityError } from '../security.mjs';

const ORIGIN = 'https://app.example.test';
const PREFIX = '/api/updates/windows/x64/';
const STORE = 'releases/windows/x64/';
const FILE = 'clawdie-platform-1.2.3-win-x64.exe';
const encoder = new TextEncoder();
const authorized = { authorize: async () => undefined };
const safeError = (code, status) => error => error instanceof SecurityError && error.code === code && error.status === status;
const catalog = (overrides = {}) => ({
  version: '1.2.3', file: FILE, size: 42, sha512: Buffer.alloc(64, 7).toString('base64'),
  released_at: '2026-09-21T02:03:04.000Z', notes: 'Verified release.\nShared project notes.', ...overrides,
});

function harness(records = {}) {
  const entries = new Map();
  const calls = [];
  let cancels = 0;
  const fixture = {
    calls, entries,
    get cancels() { return cancels; },
    set(name, contents, override = {}) {
      const bytes = typeof contents === 'string' ? encoder.encode(contents) : contents;
      const key = STORE + name;
      entries.set(key, {
        key, size: bytes.byteLength, etag: 'etag-first', httpEtag: '"etag-first"', version: 'version-first',
        bytes, ...override,
      });
    },
    bucket: {
      async head(key) {
        calls.push(['head', key]);
        if (fixture.beforeHead) await fixture.beforeHead(key);
        if (fixture.headError) throw fixture.headError;
        const entry = entries.get(key);
        if (!entry) return null;
        const { bytes, ...meta } = entry;
        return { ...meta };
      },
      async get(key, options) {
        calls.push(['get', key, options]);
        if (fixture.beforeGet) await fixture.beforeGet(key);
        if (fixture.getError) throw fixture.getError;
        const entry = entries.get(key);
        if (!entry) return null;
        const { bytes, ...meta } = entry;
        if (options?.onlyIf?.etagMatches !== entry.etag) return meta;
        const chunks = fixture.chunks ?? [bytes];
        let index = 0;
        const body = new ReadableStream({
          pull(controller) {
            if (fixture.streamError) { controller.error(fixture.streamError); return; }
            if (index >= chunks.length) controller.close();
            else controller.enqueue(chunks[index++]);
          },
          cancel() { cancels++; },
        }, { highWaterMark: 0 });
        return { ...meta, ...(fixture.getMetadata ?? {}), body };
      },
    },
  };
  for (const [name, contents] of Object.entries(records)) fixture.set(name, contents);
  fixture.env = { DOWNLOADS_READY: 'verified', APP_ORIGIN: ORIGIN, RELEASES: fixture.bucket };
  return fixture;
}

// Plain request shapes intentionally preserve raw traversal/encoding. WHATWG Request
// normalizes literal dot segments before a Worker can observe the URL.
function request(suffix = FILE, options = {}) {
  return { method: 'GET', url: ORIGIN + PREFIX + suffix, headers: new Headers(), ...options };
}

test('release readiness requires an explicit verified flag, exact HTTPS origin and both R2 read methods', () => {
  const f = harness();
  assert.equal(downloadsReady(f.env), true);
  for (const APP_ORIGIN of [undefined, '', 'http://app.example.test', ORIGIN + '/', ORIGIN + '/nested', ORIGIN + '?x=1', ORIGIN + '#x', 'https://user:pass@app.example.test', 'https://APP.example.test', 'https://app.example.test:443']) {
    assert.equal(downloadsReady({ ...f.env, APP_ORIGIN }), false, String(APP_ORIGIN));
  }
  for (const DOWNLOADS_READY of [undefined, true, 'true', 'VERIFIED']) assert.equal(downloadsReady({ ...f.env, DOWNLOADS_READY }), false);
  for (const RELEASES of [undefined, {}, { get() {} }, { head() {} }]) assert.equal(downloadsReady({ ...f.env, RELEASES }), false);
  assert.equal(downloadsReady(undefined), false);
  assert.deepEqual(f.calls, []);
});

test('missing or denied authorization never touches R2, and disabled delivery stays closed', async () => {
  const f = harness({ [FILE]: 'exe', 'catalog.json': JSON.stringify(catalog()) });
  for (const fn of [options => serveRelease(request(), f.env, options), options => readDownloadCatalog(f.env, options)]) {
    await assert.rejects(fn(), safeError('login_required', 401));
    await assert.rejects(fn({ authorize: async () => false }), safeError('login_required', 401));
    await assert.rejects(fn({ authorize: async () => { throw new SecurityError('access_denied', 403); } }), safeError('access_denied', 403));
    await assert.rejects(fn({ authorize: async () => { throw new Error('private identity provider response'); } }), safeError('download_authorization_unavailable', 503));
  }
  await assert.rejects(serveRelease(request(), { ...f.env, DOWNLOADS_READY: undefined }, authorized), safeError('downloads_unavailable', 503));
  await assert.rejects(readDownloadCatalog({ ...f.env, APP_ORIGIN: 'http://app.example.test' }, authorized), safeError('downloads_unavailable', 503));
  assert.deepEqual(f.calls, []);
});

test('only literal release paths, canonical versions, GET/HEAD and latest noCache are accepted', async () => {
  const f = harness({ [FILE]: 'exe', 'latest.yml': 'version: 1.2.3\n' });
  const invalid = [
    '', 'catalog.json', '../latest.yml', './latest.yml', 'nested/../latest.yml', '%2e%2e/latest.yml',
    'latest%2eyml', 'latest.yml/', 'latest.yml#x', 'latest.yml?x=1', 'latest.yml?noCache=',
    'latest.yml?noCache=a&noCache=b', 'latest.yml?noCache=a&token=secret', 'latest.yml?noCache=' + 'a'.repeat(129),
    'latest.yml?noCache=a%2fb', 'latest.yml\\other', 'latest.yml.sig', FILE + '.blockmap', FILE + '?noCache=1',
    FILE + '?X-Amz-Signature=x', FILE + '#hash', 'clawdie-platform-01.2.3-win-x64.exe',
    'clawdie-platform-1.2.3-beta-win-x64.exe', 'clawdie-platform-1.2.3/../../private-win-x64.exe',
    'clawdie-platform-1.2.3%2fprivate-win-x64.exe', 'clawdie-platform-1.2.3%5cprivate-win-x64.exe',
    'clawdie-platform-9007199254740992.0.0-win-x64.exe', 'clawdie-platform-1.2.3-win-arm64.exe',
  ];
  for (const suffix of invalid) await assert.rejects(serveRelease(request(suffix), f.env, authorized), safeError('release_not_found', 404), suffix);
  for (const url of ['https://foreign.example.test' + PREFIX + FILE, ORIGIN + '.attacker.test' + PREFIX + FILE, ORIGIN + '/a/../' + PREFIX.slice(1) + FILE]) {
    await assert.rejects(serveRelease(request(FILE, { url }), f.env, authorized), safeError('release_not_found', 404));
  }
  for (const method of ['POST', 'PUT', 'DELETE', 'OPTIONS', 'get']) {
    await assert.rejects(serveRelease(request(FILE, { method }), f.env, authorized), safeError('method_not_allowed', 405));
  }
  assert.deepEqual(f.calls, []);
  assert.equal(await (await serveRelease(request('latest.yml?noCache=a0_1-2.3'), f.env, authorized)).text(), 'version: 1.2.3\n');
  assert.deepEqual(f.calls.map(call => call[1]), [STORE + 'latest.yml', STORE + 'latest.yml']);
});

test('all Range and If-Range requests are explicitly unsupported before R2 I/O', async () => {
  const f = harness({ [FILE]: 'exe', 'latest.yml': 'version: 1.2.3\n' });
  for (const headers of [{ Range: 'bytes=0-1' }, { Range: 'bytes=0-1,3-4' }, { Range: 'bytes=-2' }, { Range: 'bytes=1-' }, { Range: '' }, { 'If-Range': '"etag-first"' }]) {
    for (const suffix of [FILE, 'latest.yml']) await assert.rejects(serveRelease(request(suffix, { headers: new Headers(headers) }), f.env, authorized), safeError('release_range_unsupported', 416));
  }
  assert.deepEqual(f.calls, []);
});

test('installer bytes are streamed with safe headers and an etag-conditioned exact R2 key', async () => {
  const bytes = new Uint8Array([0, 1, 2, 3, 254, 255]);
  const f = harness({ [FILE]: bytes });
  f.chunks = [bytes.slice(0, 2), bytes.slice(2)];
  let checks = 0;
  const response = await serveRelease(request(), f.env, { authorize: async () => { checks++; } });
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()), bytes);
  assert.equal(response.status, 200);
  assert.equal(checks, 3);
  assert.deepEqual(f.calls, [['head', STORE + FILE], ['get', STORE + FILE, { onlyIf: { etagMatches: 'etag-first' } }]]);
  assert.equal(response.headers.get('Content-Type'), 'application/octet-stream');
  assert.equal(response.headers.get('Content-Disposition'), `attachment; filename="${FILE}"`);
  assert.equal(response.headers.get('Content-Length'), '6');
  assert.equal(response.headers.get('ETag'), '"etag-first"');
  assert.match(response.headers.get('Cache-Control'), /private.*no-store/);
  assert.equal(response.headers.get('Accept-Ranges'), 'none');
  assert.equal(response.headers.get('X-Content-Type-Options'), 'nosniff');
  assert.match(response.headers.get('Content-Security-Policy'), /default-src 'none'/);
  assert.equal(response.headers.get('Cross-Origin-Resource-Policy'), 'same-origin');
  for (const header of ['Location', 'Set-Cookie', 'Access-Control-Allow-Origin']) assert.equal(response.headers.get(header), null);
});

test('manifest preserves exact bytes and HEAD never retrieves or returns an object body', async () => {
  const manifest = 'version: 1.2.3\r\nfiles:\r\n  - url: ' + FILE + '\r\n';
  const f = harness({ 'latest.yml': manifest, [FILE]: 'exe' });
  let checks = 0;
  const response = await serveRelease(request('latest.yml'), f.env, { authorize: async () => { checks++; } });
  assert.equal(await response.text(), manifest);
  assert.equal(response.headers.get('Content-Type'), 'application/yaml');
  assert.equal(response.headers.get('Content-Disposition'), null);
  assert.equal(checks, 4);
  f.calls.length = 0;
  for (const suffix of [FILE, 'latest.yml?noCache=123']) {
    const head = await serveRelease(request(suffix, { method: 'HEAD' }), f.env, authorized);
    assert.equal(head.body, null);
    assert.equal(await head.text(), '');
    assert.equal(head.headers.get('ETag'), '"etag-first"');
  }
  assert.deepEqual(f.calls, [['head', STORE + FILE], ['head', STORE + 'latest.yml']]);
});

test('authorization is repeated after metadata and buffered content, and denied bodies are cancelled', async () => {
  for (const denyAt of [2, 3]) {
    const f = harness({ [FILE]: 'exe' });
    let checks = 0;
    await assert.rejects(serveRelease(request(), f.env, { authorize: async () => { if (++checks === denyAt) throw new SecurityError('session_expired', 401); } }), safeError('session_expired', 401));
    assert.equal(checks, denyAt);
    assert.equal(f.calls.length, denyAt - 1);
    assert.equal(f.cancels, denyAt === 3 ? 1 : 0);
  }
  for (const kind of ['manifest', 'catalog']) {
    const f = harness({ 'latest.yml': 'version: 1.2.3\n', 'catalog.json': JSON.stringify(catalog()) });
    let checks = 0;
    const options = { authorize: async () => { if (++checks === 4) throw new SecurityError('terms_required', 403); } };
    await assert.rejects(kind === 'manifest' ? serveRelease(request('latest.yml'), f.env, options) : readDownloadCatalog(f.env, options), safeError('terms_required', 403));
    assert.equal(checks, 4);
  }
});

test('changed or vanished objects are never served after the metadata snapshot', async () => {
  for (const mode of ['etag', 'version', 'size', 'missing']) {
    const f = harness({ [FILE]: 'exe' });
    f.beforeGet = async key => {
      const entry = f.entries.get(key);
      if (mode === 'missing') f.entries.delete(key);
      if (mode === 'etag') { entry.etag = 'etag-new'; entry.httpEtag = '"etag-new"'; }
      if (mode === 'version') entry.version = 'version-new';
      if (mode === 'size') entry.size += 1;
    };
    await assert.rejects(serveRelease(request(), f.env, authorized), safeError('release_changed', 409), mode);
    assert.equal(f.cancels, ['version', 'size'].includes(mode) ? 1 : 0);
  }
});

test('missing objects, invalid metadata and provider failures have safe bounded errors', async () => {
  const missing = harness();
  await assert.rejects(serveRelease(request(), missing.env, authorized), safeError('release_not_found', 404));
  await assert.rejects(readDownloadCatalog(missing.env, authorized), safeError('release_not_found', 404));
  for (const override of [{ size: 0 }, { size: -1 }, { size: 1.1 }, { size: 1024 ** 3 + 1 }, { key: 'private/object' }, { etag: 'bad\r\nheader' }, { httpEtag: 'bad' }, { version: '' }]) {
    const f = harness(); f.set(FILE, 'exe', override);
    await assert.rejects(serveRelease(request(), f.env, authorized), safeError('download_unavailable', 503));
    assert.equal(f.calls.length, 1);
  }
  const exact = harness(); exact.set(FILE, 'exe', { size: 1024 ** 3 });
  assert.equal((await serveRelease(request(FILE, { method: 'HEAD' }), exact.env, authorized)).headers.get('Content-Length'), String(1024 ** 3));
  for (const field of ['headError', 'getError']) {
    const f = harness({ [FILE]: 'exe' }); f[field] = new Error('private bucket identifier and provider secret');
    await assert.rejects(serveRelease(request(), f.env, authorized), error => safeError('download_unavailable', 503)(error) && !error.message.includes('private'));
  }
  const unsafeGet = harness({ [FILE]: 'exe' }); unsafeGet.getMetadata = { key: 'private/object' };
  await assert.rejects(serveRelease(request(), unsafeGet.env, authorized), safeError('download_unavailable', 503));
  assert.equal(unsafeGet.cancels, 1);
});

test('buffered objects enforce metadata caps and actual stream length independently', async () => {
  for (const [name, max] of [['catalog.json', 32 * 1024], ['latest.yml', 64 * 1024]]) {
    const invoke = f => name === 'catalog.json' ? readDownloadCatalog(f.env, authorized) : serveRelease(request(name), f.env, authorized);
    const oversized = harness(); oversized.set(name, 'x', { size: max + 1 });
    await assert.rejects(invoke(oversized), safeError('download_unavailable', 503));
    assert.equal(oversized.calls.length, 1);
    for (const size of [2, 4]) {
      const f = harness(); f.set(name, 'abc', { size });
      await assert.rejects(invoke(f), safeError('download_unavailable', 503));
      if (size === 2) assert.equal(f.cancels, 1);
    }
  }
});

test('installer stream aborts overlong, truncated or failing object bodies without buffering them', async () => {
  for (const size of [2, 4]) {
    const f = harness(); f.set(FILE, 'abc', { size });
    const response = await serveRelease(request(), f.env, authorized);
    await assert.rejects(response.arrayBuffer(), safeError('download_unavailable', 503));
    if (size === 2) assert.equal(f.cancels, 1);
  }
  const f = harness({ [FILE]: 'exe' }); f.streamError = new Error('private provider diagnostic');
  const response = await serveRelease(request(), f.env, authorized);
  await assert.rejects(response.arrayBuffer(), error => safeError('download_unavailable', 503)(error) && !error.message.includes('private'));
  const cancelled = harness({ [FILE]: 'exe' });
  await (await serveRelease(request(), cancelled.env, authorized)).body.cancel();
  assert.equal(cancelled.cancels, 1);
});

test('catalog returns only validated metadata and the fixed app-relative URL', async () => {
  const source = catalog();
  const f = harness({ 'catalog.json': JSON.stringify(source) });
  let checks = 0;
  const result = await readDownloadCatalog(f.env, { authorize: async () => { checks++; } });
  assert.deepEqual(result, { ...source, url: PREFIX + FILE });
  assert.equal(checks, 4);
  assert.deepEqual(f.calls, [['head', STORE + 'catalog.json'], ['get', STORE + 'catalog.json', { onlyIf: { etagMatches: 'etag-first' } }]]);
  for (const released_at of ['2024-02-29T23:59:59Z', '2026-09-21T02:03:04.1+10:00']) {
    const item = catalog({ released_at }); delete item.notes;
    const valid = harness({ 'catalog.json': JSON.stringify(item) });
    assert.deepEqual(await readDownloadCatalog(valid.env, authorized), { ...item, url: PREFIX + FILE });
  }
});

test('catalog rejects unknown fields, unsafe names, invalid checksums, dates and excessive notes', async () => {
  const invalid = [
    null, [], 'catalog', { ...catalog(), unexpected: true }, { ...catalog(), url: 'https://foreign.example.test/file' },
    catalog({ version: '01.2.3' }), catalog({ version: '1.2.3-beta' }), catalog({ file: '../' + FILE }),
    catalog({ file: 'clawdie-platform-1.2.4-win-x64.exe' }), catalog({ size: '42' }), catalog({ size: 0 }),
    catalog({ size: 1024 ** 3 + 1 }), catalog({ sha512: Buffer.alloc(63).toString('base64') }),
    catalog({ sha512: Buffer.alloc(65).toString('base64') }), catalog({ sha512: 'A'.repeat(85) + 'B==' }),
    catalog({ released_at: 'yesterday' }), catalog({ released_at: '2026-02-29T00:00:00Z' }),
    catalog({ released_at: '2026-04-31T00:00:00Z' }), catalog({ released_at: '2026-09-21T24:00:00Z' }),
    catalog({ released_at: '2026-09-21' }), catalog({ released_at: '2026-09-21T01:02:03+24:00' }),
    catalog({ notes: '<script>Plain text is allowed.</script>\u0000' }), catalog({ notes: 'x'.repeat(6001) }),
    catalog({ notes: 'é'.repeat(3001) }), catalog({ notes: '\ud800' }), catalog({ notes: null }),
  ];
  const missing = catalog(); delete missing.sha512; invalid.push(missing);
  for (const source of invalid) {
    const f = harness({ 'catalog.json': JSON.stringify(source) });
    await assert.rejects(readDownloadCatalog(f.env, authorized), safeError('download_unavailable', 503));
  }
  for (const contents of ['{broken json', new Uint8Array([0xff, 0xfe])]) {
    const f = harness({ 'catalog.json': contents });
    await assert.rejects(readDownloadCatalog(f.env, authorized), safeError('download_unavailable', 503));
  }
});
