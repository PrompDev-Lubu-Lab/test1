import { SecurityError } from './security.mjs';

const STORAGE_PREFIX = 'releases/windows/x64/';
const DOWNLOAD_PREFIX = '/api/updates/windows/x64/';
const MAX_CATALOG_BYTES = 32 * 1024;
const MAX_MANIFEST_BYTES = 64 * 1024;
const MAX_INSTALLER_BYTES = 1024 ** 3;
const VERSION = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
const encoder = new TextEncoder();

function httpsOrigin(value) {
  if (typeof value !== 'string') return false;
  try {
    const url = new URL(value);
    return url.protocol === 'https:' && url.origin === value && !url.username && !url.password;
  } catch { return false; }
}

export function downloadsReady(env) {
  return env?.DOWNLOADS_READY === 'verified' && httpsOrigin(env.APP_ORIGIN)
    && typeof env.RELEASES?.head === 'function' && typeof env.RELEASES?.get === 'function';
}

function unavailable() { return new SecurityError('download_unavailable', 503); }
function changed() { return new SecurityError('release_changed', 409); }

async function authorize(options) {
  if (typeof options?.authorize !== 'function') throw new SecurityError('login_required', 401);
  try {
    if (await options.authorize() === false) throw new SecurityError('login_required', 401);
  } catch (error) {
    if (error instanceof SecurityError) throw error;
    throw new SecurityError('download_authorization_unavailable', 503);
  }
}

function requireReady(env) {
  if (!downloadsReady(env)) throw new SecurityError('downloads_unavailable', 503);
}

function validVersion(value) {
  return typeof value === 'string' && value.length <= 48 && VERSION.test(value)
    && value.split('.').every(part => Number.isSafeInteger(Number(part)));
}

function installerName(version) { return `clawdie-platform-${version}-win-x64.exe`; }

function releasePath(request, env) {
  if (request.method !== 'GET' && request.method !== 'HEAD') throw new SecurityError('method_not_allowed', 405);
  // Match the raw supplied URL. Never normalize/decode an arbitrary path into an R2 key.
  // A client may already have normalized its URL; even then only these two shapes enter R2.
  const prefix = env.APP_ORIGIN + DOWNLOAD_PREFIX;
  if (typeof request.url !== 'string' || !request.url.startsWith(prefix)) throw new SecurityError('release_not_found', 404);
  const suffix = request.url.slice(prefix.length);
  if (/^latest\.yml(?:\?noCache=[A-Za-z0-9._-]{1,128})?$/.test(suffix)) {
    return { name: 'latest.yml', max: MAX_MANIFEST_BYTES, manifest: true };
  }
  const match = /^clawdie-platform-(.+)-win-x64\.exe$/.exec(suffix);
  if (!match || !validVersion(match[1]) || suffix !== installerName(match[1])) {
    throw new SecurityError('release_not_found', 404);
  }
  return { name: suffix, max: MAX_INSTALLER_BYTES, manifest: false };
}

function metadata(object, key, max) {
  if (!object || object.key !== key || !Number.isSafeInteger(object.size) || object.size < 1 || object.size > max
    || typeof object.etag !== 'string' || !/^[\x21\x23-\x5b\x5d-\x7e]{1,256}$/.test(object.etag)
    || object.httpEtag !== `"${object.etag}"`
    || typeof object.version !== 'string' || object.version.length < 1 || object.version.length > 1024) {
    throw unavailable();
  }
  return object;
}

async function cancelBody(object) {
  try { if (typeof object?.body?.cancel === 'function') await object.body.cancel(); } catch { /* best effort */ }
}

async function headObject(env, key, max, options) {
  let object;
  try { object = await env.RELEASES.head(key); } catch { throw unavailable(); }
  await authorize(options);
  if (object === null) throw new SecurityError('release_not_found', 404);
  return metadata(object, key, max);
}

async function getObject(env, head, max, options) {
  let object;
  try { object = await env.RELEASES.get(head.key, { onlyIf: { etagMatches: head.etag } }); }
  catch { throw unavailable(); }
  try {
    await authorize(options);
    // R2 returns a bodyless R2Object when onlyIf fails, and null when it is missing.
    if (!object || !object.body || typeof object.body.getReader !== 'function') throw changed();
    metadata(object, head.key, max);
    if (object.etag !== head.etag || object.version !== head.version || object.size !== head.size) throw changed();
    return object;
  } catch (error) { await cancelBody(object); throw error; }
}

async function readSmallObject(object, max) {
  const reader = object.body.getReader();
  let length = 0;
  const chunks = [];
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array) || value.byteLength > Math.min(max, object.size) - length) throw unavailable();
      length += value.byteLength;
      chunks.push(value);
    }
    if (length !== object.size) throw unavailable();
    const bytes = new Uint8Array(length);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    return bytes;
  } catch {
    try { await reader.cancel(); } catch { /* best effort */ }
    throw unavailable();
  } finally { reader.releaseLock(); }
}

function validTimestamp(value) {
  if (typeof value !== 'string' || value.length > 35) return false;
  const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d{1,3})?(?:Z|([+-])(\d{2}):(\d{2}))$/.exec(value);
  if (!match || !Number.isFinite(Date.parse(value))) return false;
  const [, year, month, day, hour, minute, second, , zoneHour, zoneMinute] = match;
  const yearNumber = Number(year), monthNumber = Number(month), dayNumber = Number(day);
  const leap = yearNumber % 4 === 0 && (yearNumber % 100 !== 0 || yearNumber % 400 === 0);
  const days = [31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
  return monthNumber >= 1 && monthNumber <= 12 && dayNumber >= 1 && dayNumber <= days[monthNumber - 1]
    && Number(hour) <= 23 && Number(minute) <= 59 && Number(second) <= 59
    && (zoneHour === undefined || (Number(zoneHour) <= 23 && Number(zoneMinute) <= 59));
}

function validDigest(value) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9+/]{86}==$/.test(value)) return false;
  try { const bytes = atob(value); return bytes.length === 64 && btoa(bytes) === value; }
  catch { return false; }
}

function validNotes(value) {
  return typeof value === 'string' && value.length <= 6000 && value.isWellFormed()
    && !/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(value) && encoder.encode(value).byteLength <= 6000;
}

function catalogRecord(value) {
  const keys = ['version', 'file', 'size', 'sha512', 'released_at', 'notes'];
  if (!value || typeof value !== 'object' || Array.isArray(value)
    || Object.keys(value).some(key => !keys.includes(key))
    || !validVersion(value.version) || value.file !== installerName(value.version)
    || !Number.isSafeInteger(value.size) || value.size < 1 || value.size > MAX_INSTALLER_BYTES
    || !validDigest(value.sha512) || !validTimestamp(value.released_at)
    || (Object.hasOwn(value, 'notes') && !validNotes(value.notes))) throw unavailable();
  return {
    version: value.version, file: value.file, size: value.size, sha512: value.sha512,
    released_at: value.released_at, url: DOWNLOAD_PREFIX + value.file,
    ...(Object.hasOwn(value, 'notes') ? { notes: value.notes } : {}),
  };
}

/** The callback must freshly check Access, the current application session and terms. */
export async function readDownloadCatalog(env, options) {
  await authorize(options);
  requireReady(env);
  const head = await headObject(env, STORAGE_PREFIX + 'catalog.json', MAX_CATALOG_BYTES, options);
  const object = await getObject(env, head, MAX_CATALOG_BYTES, options);
  const bytes = await readSmallObject(object, MAX_CATALOG_BYTES);
  await authorize(options);
  try { return catalogRecord(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes))); }
  catch { throw unavailable(); }
}

function responseHeaders(object, path) {
  const headers = new Headers({
    'Cache-Control': 'private, no-store, max-age=0',
    'Pragma': 'no-cache',
    'X-Content-Type-Options': 'nosniff',
    'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'; sandbox",
    'X-Frame-Options': 'DENY',
    'Referrer-Policy': 'no-referrer',
    'Cross-Origin-Resource-Policy': 'same-origin',
    'Accept-Ranges': 'none',
    'Content-Length': String(object.size),
    'ETag': object.httpEtag,
    'Content-Type': path.manifest ? 'application/yaml' : 'application/octet-stream',
  });
  if (!path.manifest) headers.set('Content-Disposition', `attachment; filename="${path.name}"`);
  return headers;
}

function installerStream(object) {
  const reader = object.body.getReader();
  let sent = 0, finished = false;
  function release() { if (!finished) { finished = true; reader.releaseLock(); } }
  return new ReadableStream({
    async pull(controller) {
      try {
        const { done, value } = await reader.read();
        if (done) {
          if (sent !== object.size) throw unavailable();
          release(); controller.close(); return;
        }
        if (!(value instanceof Uint8Array) || value.byteLength > object.size - sent) throw unavailable();
        sent += value.byteLength;
        controller.enqueue(value);
      } catch {
        try { await reader.cancel(); } catch { /* best effort */ }
        release(); controller.error(unavailable());
      }
    },
    async cancel(reason) { try { await reader.cancel(reason); } finally { release(); } },
  }, { highWaterMark: 0 });
}

/**
 * Protected, full-download-only delivery. Configure electron-updater with
 * disableDifferentialDownload=true. Ranges and If-Range are deliberately unsupported;
 * adding them later needs conditional R2 reads and byte-exact Content-Range handling.
 * Authorization is rechecked after each R2 metadata await, before any bytes are returned.
 * Existing streams are bounded, but are not a continuous session-revocation monitor.
 */
export async function serveRelease(request, env, options) {
  await authorize(options);
  requireReady(env);
  const path = releasePath(request, env);
  if (request.headers.has('Range') || request.headers.has('If-Range')) {
    throw new SecurityError('release_range_unsupported', 416);
  }
  const head = await headObject(env, STORAGE_PREFIX + path.name, path.max, options);
  if (request.method === 'HEAD') return new Response(null, { headers: responseHeaders(head, path) });
  const object = await getObject(env, head, path.max, options);
  if (path.manifest) {
    const bytes = await readSmallObject(object, path.max);
    await authorize(options);
    return new Response(bytes, { headers: responseHeaders(object, path) });
  }
  const bounded = installerStream(object);
  // workerd derives Content-Length from the body, ignoring a manually set header.
  // Keep the bounded Node test stream and use the native fixed-length body at the edge.
  const body = typeof FixedLengthStream === 'function'
    ? bounded.pipeThrough(new FixedLengthStream(object.size)) : bounded;
  return new Response(body, { headers: responseHeaders(object, path) });
}
