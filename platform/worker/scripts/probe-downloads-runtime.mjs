import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { join, resolve } from 'node:path';
import { writeFile } from 'node:fs/promises';

// An existing pinned local toolchain is required. No installation or deployment occurs.
if (!process.env.PROBE_RUNTIME_ROOT) throw new Error('Set PROBE_RUNTIME_ROOT to the directory containing the installed Miniflare/esbuild toolchain.');
const require = createRequire(join(resolve(process.env.PROBE_RUNTIME_ROOT), 'package.json'));
const { Miniflare, convertV4MiniflareOptions } = require('miniflare');
const { build } = require('esbuild');
const bundled = await build({
  entryPoints: [fileURLToPath(new URL('../probes/downloads-runtime.mjs', import.meta.url))],
  bundle: true, format: 'esm', platform: 'browser', target: 'es2022',
  external: ['node:*'], write: false, logLevel: 'silent',
});
const origin = 'https://app.example.test';
const compatibilityDate = '2026-09-21';
const compatibilityFlags = ['nodejs_compat'];
const workerName = 'downloads-probe';
const options = {
  host: '127.0.0.1', port: 0, cf: false,
  workers: [{
    name: workerName, modules: true, script: bundled.outputFiles[0].text,
    compatibilityDate, compatibilityFlags,
    bindings: { APP_ORIGIN: origin, DOWNLOADS_READY: 'verified' },
    r2Buckets: { RELEASES: 'synthetic-downloads-probe' },
  }],
};
const mf = new Miniflare(convertV4MiniflareOptions ? convertV4MiniflareOptions(options) : options);
const file = 'clawdie-platform-1.2.3-win-x64.exe';
const apiPrefix = '/api/updates/windows/x64/';
const storagePrefix = 'releases/windows/x64/';
const installer = Uint8Array.from({ length: 128 * 1024 + 37 }, (_, i) => i % 251);
const digest = createHash('sha512').update(installer).digest('base64');
const manifest = `version: 1.2.3\r\nfiles:\r\n  - url: ${file}\r\n    sha512: ${digest}\r\n    size: ${installer.byteLength}\r\npath: ${file}\r\nsha512: ${digest}\r\n`;
const catalog = {
  version: '1.2.3', file, size: installer.byteLength, sha512: digest,
  released_at: '2026-09-21T00:00:00.000Z', notes: 'Local synthetic runtime probe only.',
};
const dispatch = (path, init = {}) => {
  const headers = new Headers(init.headers);
  if (!headers.has('X-Probe-Authorization')) headers.set('X-Probe-Authorization', 'synthetic-allow');
  return mf.dispatchFetch(origin + path, { ...init, headers });
};
async function rejection(path, status, code, init) {
  const response = await dispatch(path, init);
  assert.equal(response.status, status);
  assert.deepEqual(await response.json(), { error: code });
}
function safeHeaders(response, type) {
  assert.equal(response.headers.get('Content-Type'), type);
  assert.match(response.headers.get('Cache-Control'), /private.*no-store/);
  assert.equal(response.headers.get('X-Content-Type-Options'), 'nosniff');
  assert.match(response.headers.get('Content-Security-Policy'), /default-src 'none'/);
  assert.equal(response.headers.get('Accept-Ranges'), 'none');
  assert.equal(response.headers.get('Location'), null);
}
function observedLength(response, expected) {
  const actual = response.headers.get('Content-Length');
  assert.equal(actual, String(expected));
  return { expected, observed: actual === null ? null : Number(actual), preserved: actual === String(expected) };
}

try {
  // This is Miniflare's installed getR2Bucket API, backed by local workerd R2 storage.
  const bucket = await mf.getR2Bucket('RELEASES', workerName);
  await bucket.put(storagePrefix + file, installer);
  await bucket.put(storagePrefix + 'latest.yml', manifest);
  await bucket.put(storagePrefix + 'catalog.json', JSON.stringify(catalog));

  const installerResponse = await dispatch(apiPrefix + file);
  assert.equal(installerResponse.status, 200);
  safeHeaders(installerResponse, 'application/octet-stream');
  assert.equal(installerResponse.headers.get('Content-Disposition'), `attachment; filename="${file}"`);
  const installerLength = observedLength(installerResponse, installer.byteLength);
  assert.deepEqual(new Uint8Array(await installerResponse.arrayBuffer()), installer);

  const manifestResponse = await dispatch(apiPrefix + 'latest.yml?noCache=synthetic-123');
  assert.equal(manifestResponse.status, 200);
  safeHeaders(manifestResponse, 'application/yaml');
  const manifestLength = observedLength(manifestResponse, Buffer.byteLength(manifest));
  assert.equal(await manifestResponse.text(), manifest);

  const headResponse = await dispatch(apiPrefix + file, { method: 'HEAD' });
  assert.equal(headResponse.status, 200);
  safeHeaders(headResponse, 'application/octet-stream');
  const headLength = observedLength(headResponse, installer.byteLength);
  assert.equal(headResponse.body, null);
  assert.equal((await headResponse.arrayBuffer()).byteLength, 0);
  const manifestHead = await dispatch(apiPrefix + 'latest.yml', { method: 'HEAD' });
  assert.equal(manifestHead.status, 200);
  const manifestHeadLength = observedLength(manifestHead, Buffer.byteLength(manifest));
  assert.equal(manifestHead.body, null);
  assert.equal((await manifestHead.arrayBuffer()).byteLength, 0);

  const catalogResponse = await dispatch('/api/downloads');
  assert.equal(catalogResponse.status, 200);
  assert.deepEqual(await catalogResponse.json(), { ...catalog, url: apiPrefix + file });
  await rejection(apiPrefix + 'catalog.json', 404, 'release_not_found');
  await rejection(apiPrefix + file, 401, 'login_required', { headers: { 'X-Probe-Authorization': 'denied' } });
  await rejection(apiPrefix + file, 401, 'login_required', { headers: { 'X-Probe-Authorization': 'omit' } });
  await rejection('/api/downloads', 401, 'login_required', { headers: { 'X-Probe-Authorization': 'omit' } });
  await rejection(apiPrefix + file, 503, 'downloads_unavailable', { headers: { 'X-Probe-Disable': 'true' } });
  await rejection('/api/downloads', 503, 'downloads_unavailable', { headers: { 'X-Probe-Disable': 'true' } });
  await rejection(apiPrefix + file, 416, 'release_range_unsupported', { headers: { Range: 'bytes=0-10' } });
  await rejection(apiPrefix + file, 416, 'release_range_unsupported', { headers: { Range: 'bytes=0-1,3-4' } });
  await rejection(apiPrefix + file, 416, 'release_range_unsupported', { headers: { 'If-Range': '"synthetic"' } });
  await rejection(apiPrefix + file, 405, 'method_not_allowed', { method: 'POST' });
  await rejection(apiPrefix + 'clawdie-platform-9.9.9-win-x64.exe', 404, 'release_not_found');
  await rejection(apiPrefix + 'clawdie-platform-1.2.3%2fprivate-win-x64.exe', 404, 'release_not_found');
  await rejection(apiPrefix + file + '?token=synthetic', 404, 'release_not_found');

  const result = {
    checked_at: new Date().toISOString(),
    scope: 'Local workerd with a real Miniflare R2 binding and synthetic release bytes. Explicit harness authorization only; no real Access, D1 sessions, accounts, production R2, public deployment, signing or updater installation was tested.',
    node: process.version,
    miniflare: require('miniflare/package.json').version,
    workerd: require('workerd/package.json').version,
    esbuild: require('esbuild/package.json').version,
    runtime: { compatibility_date: compatibilityDate, compatibility_flags: compatibilityFlags, host: '127.0.0.1', cf: false, persistent_storage_configured: false },
    checks: {
      actual_r2_binding: true, installer_byte_exact: true, manifest_byte_exact: true,
      installer_head_no_body: true, manifest_head_no_body: true, private_no_store: true,
      safe_response_headers: true, catalog_metadata_and_relative_path: true, catalog_not_downloadable: true,
      disabled_gate_rejected: true, harness_authorization_denial_rejected: true, missing_authorize_callback_rejected: true,
      single_and_multi_ranges_rejected_416: true, if_range_rejected_416: true,
      write_method_rejected: true, missing_file_rejected: true, encoded_separator_rejected: true, installer_query_rejected: true,
    },
    content_length: { installer_get: installerLength, installer_head: headLength, manifest_get: manifestLength, manifest_head: manifestHeadLength },
    limitations: [
      'Local harness checks are not evidence of real Access or account authorization.',
      'The first delivery implementation deliberately supports complete downloads only.',
      'No signed executable, electron-updater client, live Cloudflare edge or cache was exercised.',
    ],
  };
  await writeFile(new URL('../../docs/downloads-runtime-results.json', import.meta.url), JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result, null, 2));
} finally { await mf.dispose(); }
