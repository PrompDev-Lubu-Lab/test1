import { timingSafeEqual } from 'node:crypto';
import { pbkdf2 } from '@noble/hashes/pbkdf2.js';
import { sha256 } from '@noble/hashes/sha2.js';
import { bytesToHex, hexToBytes } from '@noble/hashes/utils.js';
import { createRemoteJWKSet, jwtVerify } from 'jose';

export const PASSWORD_ITERATIONS = 600_000;
const PASSWORD_BYTES = 32;
const SALT_BYTES = 16;
const encoder = new TextEncoder();
const dummySalt = new Uint8Array(SALT_BYTES).fill(0x93);
const dummyHash = new Uint8Array(PASSWORD_BYTES);
const remoteKeySets = new Map();
const MAX_CACHED_ISSUERS = 4;

/** Safe public error; callers must not serialize the underlying provider error. */
export class SecurityError extends Error {
  constructor(code, status, message = code) {
    super(message);
    this.name = 'SecurityError';
    this.code = code;
    this.status = status;
  }
}

function passwordBytes(password) {
  if (typeof password !== 'string' || password.length > 256) {
    throw new SecurityError('invalid_password', 400);
  }
  let count = 0;
  for (const point of password) {
    const value = point.codePointAt(0);
    // Reject ill-formed UTF-16 rather than silently encoding a replacement character.
    if (++count > 128 || (value >= 0xd800 && value <= 0xdfff)) {
      throw new SecurityError('invalid_password', 400);
    }
  }
  const bytes = encoder.encode(password);
  if (count < 12 || bytes.byteLength > 512) {
    bytes.fill(0);
    throw new SecurityError('invalid_password', 400);
  }
  return bytes;
}

function validPasswordRecord(record) {
  return record !== null && typeof record === 'object'
    && record.scheme === 'pbkdf2-sha256'
    && record.iterations === PASSWORD_ITERATIONS
    && typeof record.salt === 'string' && /^[0-9a-f]{32}$/.test(record.salt)
    && typeof record.hash === 'string' && /^[0-9a-f]{64}$/.test(record.hash);
}

/** Both inputs are fixed-size digests. Content comparison uses the native primitive. */
export function equalDigest(left, right) {
  if (!(left instanceof Uint8Array) || !(right instanceof Uint8Array)
    || left.byteLength !== PASSWORD_BYTES || right.byteLength !== PASSWORD_BYTES) {
    return false;
  }
  return timingSafeEqual(left, right);
}

/** Construct once per request, after the shared limiter reservation. Never cache it. */
export function createKdfBudget() {
  let used = false;
  function derive(password, salt) {
    const bytes = passwordBytes(password);
    if (used) {
      bytes.fill(0);
      throw new SecurityError('kdf_budget_exhausted', 500);
    }
    used = true;
    try {
      return pbkdf2(sha256, bytes, salt, { c: PASSWORD_ITERATIONS, dkLen: PASSWORD_BYTES });
    } finally {
      bytes.fill(0);
    }
  }
  return Object.freeze({
    hash(password) {
      const salt = crypto.getRandomValues(new Uint8Array(SALT_BYTES));
      const hash = derive(password, salt);
      try {
        return { scheme: 'pbkdf2-sha256', iterations: PASSWORD_ITERATIONS, salt: bytesToHex(salt), hash: bytesToHex(hash) };
      } finally {
        hash.fill(0);
        salt.fill(0);
      }
    },
    verify(password, record) {
      const valid = validPasswordRecord(record);
      const salt = valid ? hexToBytes(record.salt) : dummySalt;
      const expected = valid ? hexToBytes(record.hash) : dummyHash;
      const actual = derive(password, salt);
      try {
        // Unknown accounts and unsupported/malformed stored records still spend one KDF.
        const matches = equalDigest(actual, expected);
        return valid && matches;
      } finally {
        actual.fill(0);
        if (valid) { salt.fill(0); expected.fill(0); }
      }
    },
  });
}

/** 256 random bits, URL-safe, with no padding. */
export function randomToken() {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return btoa(String.fromCharCode(...bytes)).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
}

export function digestToken(token) {
  if (typeof token !== 'string' || token.length === 0 || token.length > 2048) {
    throw new SecurityError('invalid_token', 400);
  }
  return bytesToHex(sha256(encoder.encode(token)));
}

/** Supported addresses are ordinary, unquoted ASCII addr-specs; no identity inference. */
export function normalizeEmail(text) {
  if (typeof text !== 'string' || text.length > 320) throw new SecurityError('invalid_email', 400);
  const email = text.trim().toLowerCase();
  const parts = email.split('@');
  if (email.length > 254 || parts.length !== 2) throw new SecurityError('invalid_email', 400);
  const [local, domain] = parts;
  if (local.length < 1 || local.length > 64 || !/^[a-z0-9.!#$%&'*+/=?^_`{|}~-]+$/.test(local)
    || local.startsWith('.') || local.endsWith('.') || local.includes('..')
    || domain.length < 1 || domain.length > 253
    || domain.split('.').some(label => !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label))) {
    throw new SecurityError('invalid_email', 400);
  }
  return email;
}

function accessConfig(env) {
  const team = env?.ACCESS_TEAM_DOMAIN;
  const audience = env?.ACCESS_AUDIENCE;
  if (typeof team !== 'string' || team.length > 512 || typeof audience !== 'string'
    || !audience || audience.length > 512 || audience.trim() !== audience) {
    throw new SecurityError('access_unconfigured', 503);
  }
  let url;
  try { url = new URL(team); } catch { throw new SecurityError('access_unconfigured', 503); }
  if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash
    || url.pathname !== '/' || team.trim() !== team) {
    throw new SecurityError('access_unconfigured', 503);
  }
  return { issuer: url.origin, audience };
}

function remoteKeys(issuer) {
  const existing = remoteKeySets.get(issuer);
  if (existing) return existing;
  if (remoteKeySets.size >= MAX_CACHED_ISSUERS) remoteKeySets.delete(remoteKeySets.keys().next().value);
  const keys = createRemoteJWKSet(new URL('/cdn-cgi/access/certs', issuer), {
    cacheMaxAge: 300_000,
    cooldownDuration: 30_000,
    timeoutDuration: 5_000,
  });
  remoteKeySets.set(issuer, keys);
  return keys;
}

/**
 * Verify the edge-injected assertion, never an unsigned email header.
 * The optional resolver is an explicit test seam for real signed JWTs. No env bypass exists.
 * Binding the returned subject to an invited/current app user belongs to the route layer.
 */
export async function verifyAccess(request, env, testKeyResolver) {
  const { issuer, audience } = accessConfig(env);
  if (testKeyResolver !== undefined && typeof testKeyResolver !== 'function') {
    throw new SecurityError('invalid_key_resolver', 500);
  }
  const token = request.headers.get('cf-access-jwt-assertion');
  if (!token || token.length > 16_384) throw new SecurityError('access_denied', 403);
  try {
    const { payload } = await jwtVerify(token, testKeyResolver ?? remoteKeys(issuer), {
      algorithms: ['RS256'], issuer, audience,
      requiredClaims: ['exp', 'sub', 'email', 'type'],
      clockTolerance: 0,
    });
    if (payload.type !== 'app' || !Number.isSafeInteger(payload.exp) || !Number.isSafeInteger(payload.exp * 1000) || typeof payload.sub !== 'string' || !payload.sub.trim()
      || payload.sub.length > 256 || payload.sub.trim() !== payload.sub
      || typeof payload.email !== 'string' || !payload.email.trim()
      || Object.hasOwn(payload, 'common_name') || Object.hasOwn(payload, 'service_token_id')
      || payload.service_token_status === true) {
      throw new SecurityError('access_denied', 403);
    }
    return Object.freeze({ sub: payload.sub, email: normalizeEmail(payload.email), exp: payload.exp });
  } catch {
    throw new SecurityError('access_denied', 403);
  }
}

/** Bound the actual byte stream; Content-Length alone is neither required nor trusted. */
export async function readJsonLimited(request, maxBytes = 8192) {
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 || maxBytes > 2 * 1024 * 1024) {
    throw new SecurityError('invalid_body_limit', 500);
  }
  const contentType = request.headers.get('content-type') ?? '';
  if (!/^application\/json(?:\s*;[^\r\n]*)?$/i.test(contentType)) {
    throw new SecurityError('json_required', 415);
  }
  const declared = request.headers.get('content-length');
  if (declared !== null && (!/^\d+$/.test(declared) || !Number.isSafeInteger(Number(declared)))) {
    throw new SecurityError('invalid_json', 400);
  }
  if (declared !== null && Number(declared) > maxBytes) {
    await request.body?.cancel().catch(() => {});
    throw new SecurityError('body_too_large', 413);
  }
  if (!request.body) throw new SecurityError('invalid_json', 400);
  const reader = request.body.getReader();
  const chunks = [];
  let size = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > maxBytes) {
        await reader.cancel().catch(() => {});
        throw new SecurityError('body_too_large', 413);
      }
      chunks.push(value);
    }
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    const json = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    if (json === null || typeof json !== 'object' || Array.isArray(json)) throw new Error('object required');
    return json;
  } catch (error) {
    if (error instanceof SecurityError) throw error;
    throw new SecurityError('invalid_json', 400);
  } finally {
    reader.releaseLock();
  }
}

/** Call only after the atomic rate reservation. Provider acceptance is single-use. */
export async function verifyTurnstile(token, ip, env, action) {
  const secret = env?.TURNSTILE_SECRET;
  const hostname = env?.TURNSTILE_HOSTNAME;
  if (typeof secret !== 'string' || !secret || secret.length > 2048
    || typeof hostname !== 'string' || !/^[a-z0-9.-]{1,253}$/.test(hostname)
    || hostname.includes('..') || hostname.startsWith('.') || hostname.endsWith('.')) {
    throw new SecurityError('turnstile_unconfigured', 503);
  }
  if (typeof token !== 'string' || !token || token.length > 2048
    || typeof ip !== 'string' || !/^[0-9a-fA-F:.]{1,45}$/.test(ip)
    || typeof action !== 'string' || !/^[a-zA-Z0-9_-]{1,32}$/.test(action)) {
    throw new SecurityError('turnstile_failed', 403);
  }
  let result;
  try {
    const response = await fetch('https://challenges.cloudflare.com/turnstile/v0/siteverify', {
      method: 'POST', redirect: 'error', signal: AbortSignal.timeout(5000),
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ secret, response: token, remoteip: ip }),
    });
    if (!response.ok) throw new Error('provider unavailable');
    result = await readJsonLimited(response, 16_384);
  } catch {
    throw new SecurityError('turnstile_unavailable', 503);
  }
  if (result.success !== true || result.hostname !== hostname || result.action !== action) {
    throw new SecurityError('turnstile_failed', 403);
  }
  return true;
}
