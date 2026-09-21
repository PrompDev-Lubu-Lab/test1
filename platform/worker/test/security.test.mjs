import assert from 'node:assert/strict';
import { createHash, pbkdf2Sync } from 'node:crypto';
import { test } from 'node:test';
import { pbkdf2 } from '@noble/hashes/pbkdf2.js';
import { sha256 } from '@noble/hashes/sha2.js';
import { createLocalJWKSet, exportJWK, generateKeyPair, SignJWT } from 'jose';
import {
  createKdfBudget, digestToken, equalDigest, normalizeEmail, PASSWORD_ITERATIONS,
  randomToken, readJsonLimited, SecurityError, verifyAccess, verifyTurnstile,
} from '../security.mjs';

const encode = value => new TextEncoder().encode(value);
const errorCode = (code, status) => error => error instanceof SecurityError && error.code === code && error.status === status;

// Published known answers, not derived from the implementation under test:
// https://www.rfc-editor.org/rfc/rfc7914.html#section-11 (PBKDF2-HMAC-SHA256).
test('pinned PBKDF2 matches both RFC 7914 section 11 SHA256 vectors', () => {
  const vectors = [
    ['passwd', 'salt', 1, '55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783'],
    ['Password', 'NaCl', 80_000, '4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d'],
  ];
  for (const [password, salt, iterations, expected] of vectors) {
    assert.equal(Buffer.from(pbkdf2(sha256, encode(password), encode(salt), { c: iterations, dkLen: 64 })).toString('hex'), expected);
  }
});

test('production 600000-iteration verification matches independent Node outputs', () => {
  const cases = [
    ['twelvechars!', Buffer.from('000102030405060708090a0b0c0d0e0f', 'hex')],
    ['a long password crosses the HMAC block boundary '.repeat(2), Buffer.from('fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0', 'hex')],
    ['密碼🔑ma\0il-words🌙', Buffer.from('006173616c7400ff00656d6265646465', 'hex')],
    ['  spaces stay  ', Buffer.alloc(16, 0x37)],
  ];
  for (const [password, salt] of cases) {
    assert.equal(salt.byteLength, 16);
    const record = { scheme: 'pbkdf2-sha256', iterations: 600_000, salt: salt.toString('hex'),
      hash: pbkdf2Sync(Buffer.from(password, 'utf8'), salt, 600_000, 32, 'sha256').toString('hex') };
    assert.equal(createKdfBudget().verify(password, record), true);
  }
});

test('hash records have fresh 16-byte salts, fixed work and the native expected result', () => {
  const password = 'a valid sample passphrase';
  const first = createKdfBudget().hash(password);
  const second = createKdfBudget().hash(password);
  assert.equal(first.scheme, 'pbkdf2-sha256');
  assert.equal(first.iterations, PASSWORD_ITERATIONS);
  assert.match(first.salt, /^[0-9a-f]{32}$/);
  assert.match(first.hash, /^[0-9a-f]{64}$/);
  assert.notEqual(first.salt, second.salt);
  assert.notEqual(first.hash, second.hash);
  assert.equal(first.hash, pbkdf2Sync(password, Buffer.from(first.salt, 'hex'), 600_000, 32, 'sha256').toString('hex'));
  assert.equal(createKdfBudget().verify('a different valid passphrase', first), false);
});

test('unknown accounts and unsupported records consume the single request KDF budget', () => {
  for (const record of [null, { scheme: 'pbkdf2-sha256', iterations: 100_000, salt: '00'.repeat(16), hash: '00'.repeat(32) }]) {
    const budget = createKdfBudget();
    assert.equal(budget.verify('unknown account sample', record), false);
    assert.throws(() => budget.hash('another sample password'), errorCode('kdf_budget_exhausted', 500));
    assert.throws(() => budget.verify('another sample password', null), errorCode('kdf_budget_exhausted', 500));
  }
});

test('password bounds count Unicode code points without lossy replacement or trimming', () => {
  const maximum = '🔑'.repeat(128); // 128 code points, 512 UTF-8 bytes, 256 UTF-16 units.
  const record = createKdfBudget().hash(maximum);
  assert.equal(record.hash, pbkdf2Sync(maximum, Buffer.from(record.salt, 'hex'), 600_000, 32, 'sha256').toString('hex'));
  for (const invalid of [null, 123, '', 'short', 'x'.repeat(129), '🔑'.repeat(129), '\ud800valid-length-input']) {
    assert.throws(() => createKdfBudget().hash(invalid), errorCode('invalid_password', 400));
  }
});

test('fixed-length digest equality rejects malformed lengths and unequal bytes', () => {
  const a = new Uint8Array(32).fill(19);
  const b = new Uint8Array(a);
  assert.equal(equalDigest(a, b), true);
  b[31] ^= 1;
  assert.equal(equalDigest(a, b), false);
  assert.equal(equalDigest(a, a.slice(0, 31)), false);
  assert.equal(equalDigest('0'.repeat(32), a), false);
});

test('random tokens carry 256 bits and only their stable SHA256 digest is persisted', () => {
  const tokens = Array.from({ length: 8 }, randomToken);
  assert.equal(new Set(tokens).size, tokens.length);
  for (const token of tokens) {
    assert.match(token, /^[A-Za-z0-9_-]{43}$/);
    assert.equal(Buffer.from(token, 'base64url').byteLength, 32);
    assert.equal(digestToken(token), createHash('sha256').update(token).digest('hex'));
  }
  assert.throws(() => digestToken(''), errorCode('invalid_token', 400));
});

test('email normalization is explicit and rejects ambiguous or malformed addr-specs', () => {
  assert.equal(normalizeEmail('  MEMBER+test@Example.TEST '), 'member+test@example.test');
  for (const invalid of [null, 'no-at-sign', 'a@b@c', '.a@example.test', 'a..b@example.test', 'a@example..test', 'a@-example.test', 'A <a@example.test>', 'a\n@example.test']) {
    assert.throws(() => normalizeEmail(invalid), errorCode('invalid_email', 400));
  }
});

const env = { ACCESS_TEAM_DOMAIN: 'https://access-team.example.test', ACCESS_AUDIENCE: 'application-audience' };
const rsa = await generateKeyPair('RS256');
const publicJwk = { ...await exportJWK(rsa.publicKey), kid: 'synthetic-rsa', alg: 'RS256', use: 'sig' };
const localKeys = createLocalJWKSet({ keys: [publicJwk] });
function assertionRequest(token, extraHeaders = {}) {
  return new Request('https://app.example.test/api/me', { headers: { 'cf-access-jwt-assertion': token, ...extraHeaders } });
}
async function token(overrides = {}, options = {}) {
  const now = Math.floor(Date.now() / 1000);
  const payload = { iss: env.ACCESS_TEAM_DOMAIN, aud: [env.ACCESS_AUDIENCE], sub: 'synthetic-human-id',
    email: 'member@example.test', type: 'app', iat: now, nbf: now - 1, exp: now + 600, ...overrides };
  for (const key of options.omit ?? []) delete payload[key];
  return new SignJWT(payload).setProtectedHeader({ alg: options.alg ?? 'RS256', kid: options.kid ?? 'synthetic-rsa' })
    .sign(options.key ?? rsa.privateKey);
}

test('real signed human Access assertions require issuer, audience, expiry and identity', async () => {
  const assertion = await token();
  const verified = await verifyAccess(assertionRequest(assertion), env, localKeys);
  assert.deepEqual({sub:verified.sub,email:verified.email}, { sub: 'synthetic-human-id', email: 'member@example.test' });
  assert.ok(Number.isSafeInteger(verified.exp) && verified.exp > Date.now()/1000);
  const claims = [
    { iss: 'https://other-team.example.test' }, { iss: `${env.ACCESS_TEAM_DOMAIN}/` },
    { aud: ['different-application'] }, { exp: 1 }, { nbf: Math.floor(Date.now() / 1000) + 600 },
    { sub: '' }, { sub: '  ' }, { sub: 42 }, { email: '' }, { email: 'not-an-email' },
    { type: 'org' }, { common_name: 'synthetic-service.access' },
    { service_token_id: 'synthetic-service' }, { service_token_status: true },
  ];
  for (const override of claims) {
    await assert.rejects(verifyAccess(assertionRequest(await token(override)), env, localKeys), errorCode('access_denied', 403));
  }
  for (const missing of ['exp', 'sub', 'email', 'type', 'iss', 'aud']) {
    await assert.rejects(verifyAccess(assertionRequest(await token({}, { omit: [missing] })), env, localKeys), errorCode('access_denied', 403));
  }
});

test('Access rejects service tokens, alternate algorithms, tampering and unavailable keys', async () => {
  const service = await token({ sub: '', common_name: 'synthetic-service.access' }, { omit: ['email'] });
  await assert.rejects(verifyAccess(assertionRequest(service), env, localKeys), errorCode('access_denied', 403));
  const hmac = await token({}, { alg: 'HS256', key: new Uint8Array(32).fill(13) });
  await assert.rejects(verifyAccess(assertionRequest(hmac), env, localKeys), errorCode('access_denied', 403));
  const otherRsa = await generateKeyPair('RS384');
  const rs384 = await token({}, { alg: 'RS384', key: otherRsa.privateKey });
  await assert.rejects(verifyAccess(assertionRequest(rs384), env, localKeys), errorCode('access_denied', 403));
  const valid = await token();
  const parts = valid.split('.');
  parts[1] = Buffer.from(JSON.stringify({ ...JSON.parse(Buffer.from(parts[1], 'base64url')), sub: 'attacker-id' })).toString('base64url');
  await assert.rejects(verifyAccess(assertionRequest(parts.join('.')), env, localKeys), errorCode('access_denied', 403));
  await assert.rejects(verifyAccess(assertionRequest(valid), env, async () => { throw new Error('unavailable'); }), errorCode('access_denied', 403));
  await assert.rejects(verifyAccess(new Request('https://app.example.test/api/me', { headers: { 'cf-access-authenticated-user-email': 'member@example.test' } }), env, localKeys), errorCode('access_denied', 403));
});

test('Access fails closed for incomplete or unsafe issuer configuration', async () => {
  const request = assertionRequest(await token());
  for (const badEnv of [{}, { ...env, ACCESS_AUDIENCE: '' }, { ...env, ACCESS_AUDIENCE: ' padded ' },
    ...['http://access-team.example.test', 'https://name:pass@access-team.example.test', 'https://access-team.example.test/path', 'https://access-team.example.test/?query=1', 'https://access-team.example.test/#fragment'].map(team => ({ ...env, ACCESS_TEAM_DOMAIN: team }))]) {
    await assert.rejects(verifyAccess(request, badEnv, localKeys), errorCode('access_unconfigured', 503));
  }
});

test('production path fetches the exact HTTPS JWKS endpoint and reuses its bounded cache', async t => {
  const productionEnv = { ...env, ACCESS_TEAM_DOMAIN: 'https://jwks-fetch.example.test' };
  let calls = 0;
  t.mock.method(globalThis, 'fetch', async input => {
    calls++;
    assert.equal(String(input), `${productionEnv.ACCESS_TEAM_DOMAIN}/cdn-cgi/access/certs`);
    return Response.json({ keys: [publicJwk] });
  });
  const request = assertionRequest(await token({ iss: productionEnv.ACCESS_TEAM_DOMAIN }));
  // No resolver injection: JOSE's actual remote-key retrieval runs here.
  assert.equal((await verifyAccess(request, productionEnv)).sub, 'synthetic-human-id');
  assert.equal((await verifyAccess(request, productionEnv)).sub, 'synthetic-human-id');
  assert.equal(calls, 1);
});

function jsonRequest(body, headers = {}) {
  return new Request('https://app.example.test/api/auth/login', { method: 'POST', body,
    headers: { 'content-type': 'application/json', ...headers } });
}

test('JSON limits measure real UTF8 bytes even with absent or false Content-Length', async () => {
  assert.deepEqual(await readJsonLimited(jsonRequest('{"value":"🔑"}')), { value: '🔑' });
  const oversized = JSON.stringify({ value: '🔑'.repeat(12) });
  await assert.rejects(readJsonLimited(jsonRequest(oversized, { 'content-length': '1' }), 32), errorCode('body_too_large', 413));
  await assert.rejects(readJsonLimited(jsonRequest(oversized), 32), errorCode('body_too_large', 413));
  await assert.rejects(readJsonLimited(jsonRequest('{}', { 'content-length': '9000' })), errorCode('body_too_large', 413));
});

test('JSON parsing rejects wrong types, malformed encodings and oversized streams', async () => {
  for (const body of ['not json', 'null', '[]', '42']) {
    await assert.rejects(readJsonLimited(jsonRequest(body)), errorCode('invalid_json', 400));
  }
  await assert.rejects(readJsonLimited(jsonRequest('{}', { 'content-type': 'text/plain' })), errorCode('json_required', 415));
  await assert.rejects(readJsonLimited(jsonRequest(new Uint8Array([0xc3, 0x28]))), errorCode('invalid_json', 400));
  let cancelled = false;
  const stream = new ReadableStream({
    pull(controller) { controller.enqueue(new Uint8Array(33)); },
    cancel() { cancelled = true; },
  });
  const request = new Request('https://app.example.test/api/auth/login', { method: 'POST', body: stream, duplex: 'half', headers: { 'content-type': 'application/json' } });
  await assert.rejects(readJsonLimited(request, 32), errorCode('body_too_large', 413));
  assert.equal(cancelled, true);
});

const turnstileEnv = { TURNSTILE_SECRET: 'synthetic-turnstile-secret', TURNSTILE_HOSTNAME: 'app.example.test' };
test('Turnstile sends a bounded verification request and requires the exact hostname/action', async t => {
  let result = { success: true, hostname: 'app.example.test', action: 'login' };
  t.mock.method(globalThis, 'fetch', async (input, options) => {
    assert.equal(input, 'https://challenges.cloudflare.com/turnstile/v0/siteverify');
    assert.equal(options.method, 'POST');
    assert.equal(options.redirect, 'error');
    assert.deepEqual(JSON.parse(options.body), { secret: 'synthetic-turnstile-secret', response: 'synthetic-token', remoteip: '192.0.2.1' });
    return Response.json(result);
  });
  assert.equal(await verifyTurnstile('synthetic-token', '192.0.2.1', turnstileEnv, 'login'), true);
  for (const bad of [{ success: false }, { hostname: 'other.example.test' }, { action: 'signup' }, { success: 'true' }]) {
    result = { success: true, hostname: 'app.example.test', action: 'login', ...bad };
    await assert.rejects(verifyTurnstile('synthetic-token', '192.0.2.1', turnstileEnv, 'login'), errorCode('turnstile_failed', 403));
  }
});

test('Turnstile configuration and provider failures never enable a bypass', async t => {
  await assert.rejects(verifyTurnstile('synthetic-token', '192.0.2.1', {}, 'login'), errorCode('turnstile_unconfigured', 503));
  await assert.rejects(verifyTurnstile('', '192.0.2.1', turnstileEnv, 'login'), errorCode('turnstile_failed', 403));
  t.mock.method(globalThis, 'fetch', async () => new Response('unavailable', { status: 503 }));
  await assert.rejects(verifyTurnstile('synthetic-token', '192.0.2.1', turnstileEnv, 'login'), errorCode('turnstile_unavailable', 503));
});
