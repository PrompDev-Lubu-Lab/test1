import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';
import { AUTH_LIMITS, AuthLimiter, createLimiterState, limiterTransition } from '../limiter.mjs';

const digest = value => createHash('sha256').update(value).digest('hex');
const key = index => digest(`synthetic-key-${index}`);
let nextId = 1;
const id = () => `00000000-0000-4000-8000-${(nextId++).toString(16).padStart(12, '0')}`;
function scenario() {
  let state = createLimiterState();
  return {
    get state() { return state; },
    ingress(ip, now = 1000) {
      const output = limiterTransition(state, { type: 'ingress', ipKey: key(ip) }, now);
      state = output.state;
      return output.result;
    },
    reserve(ip, account, purpose = 'login', now = 1000) {
      const output = limiterTransition(state, { type: 'reserve', ipKey: key(ip), accountKey: key(account), purpose }, now, id());
      state = output.state;
      return output.result;
    },
    result(reservationId, success, now = 1000) {
      const output = limiterTransition(state, { type: 'result', reservationId, success }, now);
      state = output.state;
      return output;
    },
    cleanup(now) {
      state = limiterTransition(state, { type: 'cleanup' }, now).state;
    },
  };
}

test('one account cannot evade its window by switching IPs', () => {
  const run = scenario();
  for (let index = 0; index < 10; index++) assert.equal(run.reserve(`ip-${index}`, 'account').allowed, true);
  assert.deepEqual(run.reserve('new-ip', 'account'), { allowed: false, reservationId: null, retryAfter: 600 });
});

test('success clears only the account window, retaining IP and global reservations', () => {
  const run = scenario();
  for (let index = 0; index < 10; index++) {
    const reservation = run.reserve('same-ip', 'account');
    assert.equal(reservation.allowed, true);
    run.result(reservation.reservationId, index === 9);
  }
  assert.equal(run.reserve('same-ip', 'different-account').allowed, false);
  assert.equal(run.reserve('fresh-ip', 'account').allowed, true);
  assert.equal(run.state.global.length, 11);
  assert.equal(run.state.ips[`kdf:${key('same-ip')}`].length, 10);
});

test('the tenth wrong login locks for ten minutes after that failure', () => {
  const run = scenario();
  const start = 1000;
  let tenthTime;
  for (let index = 0; index < 10; index++) {
    tenthTime = start + index * 50_000;
    const reservation = run.reserve(`ip-${index}`, 'account', 'login', tenthTime);
    assert.equal(reservation.allowed, true);
    run.result(reservation.reservationId, false, tenthTime);
  }
  // The first attempt has left its sliding window, but the explicit lock remains.
  const stillLocked = run.reserve('new-ip', 'account', 'login', start + 600_001);
  assert.equal(stillLocked.allowed, false);
  assert.ok(stillLocked.retryAfter > 400);
  assert.equal(run.reserve('new-ip', 'account', 'login', tenthTime + 600_001).allowed, true);
});

test('recovery has separate account and IP windows and does not charge the KDF global cap', () => {
  const run = scenario();
  for (let index = 0; index < 10; index++) {
    const reservation = run.reserve('exhausted-ip', 'account');
    run.result(reservation.reservationId, false);
  }
  assert.equal(run.reserve('fresh-ip', 'account', 'login').allowed, false);
  const recoveries = [];
  for (let index = 0; index < 10; index++) {
    const recovery = run.reserve('exhausted-ip', 'account', 'recovery');
    assert.equal(recovery.allowed, true);
    recoveries.push(recovery.reservationId);
  }
  assert.equal(run.reserve('fresh-ip', 'account', 'recovery').allowed, false);
  run.result(recoveries[0], true);
  assert.equal(run.reserve('exhausted-ip', 'account', 'recovery').allowed, false);
  assert.equal(run.reserve('another-ip', 'account', 'login').allowed, false);
  assert.equal(run.reserve('another-ip', 'account', 'recovery').allowed, true);
  assert.equal(run.state.global.length, 10);
  assert.equal(run.state.accounts[`recovery:${key('account')}`].lockedUntil, 0);
});

test('the KDF IP limit aggregates every password purpose', () => {
  const run = scenario();
  const purposes = ['login', 'signup', 'reauth', 'reset', 'password'];
  for (let index = 0; index < 10; index++) {
    assert.equal(run.reserve('ip', `account-${index}`, purposes[index % purposes.length]).allowed, true);
  }
  for (const purpose of purposes) assert.equal(run.reserve('ip', 'new-account', purpose).allowed, false);
  assert.equal(run.reserve('ip', 'new-account', 'recovery').allowed, true);
  assert.equal(run.reserve('ip', 'new-account', 'write').allowed, true);
});

test('each non-login password purpose keeps its account cap and clears it only on success', () => {
  for (const purpose of ['signup', 'reauth', 'reset', 'password']) {
    const run = scenario();
    const reservations = [];
    for (let index = 0; index < 10; index++) {
      const reservation = run.reserve(`ip-${index}`, 'account', purpose);
      assert.equal(reservation.allowed, true);
      reservations.push(reservation.reservationId);
    }
    assert.equal(run.reserve('new-ip', 'account', purpose).allowed, false);
    run.result(reservations[0], false);
    assert.equal(run.reserve('new-ip', 'account', purpose).allowed, false);
    run.result(reservations[1], true);
    assert.equal(run.reserve('new-ip', 'account', purpose).allowed, true);
    assert.equal(run.state.accounts[`${purpose}:${key('account')}`].lockedUntil, 0);
    assert.equal(run.state.global.length, 11);
  }
});

test('the global limit remains 60/minute even if every account succeeds', () => {
  const run = scenario();
  for (let index = 0; index < 60; index++) {
    const reservation = run.reserve(`ip-${index}`, `account-${index}`);
    assert.equal(reservation.allowed, true);
    run.result(reservation.reservationId, true);
  }
  assert.deepEqual(run.reserve('ip-61', 'account-61'), { allowed: false, reservationId: null, retryAfter: 60 });
  assert.equal(run.reserve('ip-61', 'account-61', 'login', 61_001).allowed, true);
});

test('write and recovery traffic neither consumes nor is blocked by the KDF global ceiling', () => {
  const run = scenario();
  for (let index = 0; index < 60; index++) {
    for (const purpose of ['write', 'recovery']) {
      const reservation = run.reserve(`ip-${index}`, `account-${index}`, purpose);
      assert.equal(reservation.allowed, true);
      run.result(reservation.reservationId, true);
    }
  }
  assert.equal(run.state.global.length, 0);
  const purposes = ['login', 'signup', 'reauth', 'reset', 'password'];
  for (let index = 0; index < 60; index++) {
    const reservation = run.reserve(`kdf-ip-${index}`, `kdf-account-${index}`, purposes[index % purposes.length]);
    assert.equal(reservation.allowed, true);
    run.result(reservation.reservationId, true);
  }
  assert.equal(run.reserve('fresh-ip', 'fresh-account', 'password').allowed, false);
  assert.equal(run.reserve('fresh-ip', 'fresh-account', 'write').allowed, true);
  assert.equal(run.reserve('fresh-ip', 'fresh-account', 'recovery').allowed, true);
  assert.equal(run.state.global.length, 60);
});

test('120 counted writes cannot bypass account or IP windows by reporting success', () => {
  const run = scenario();
  for (let index = 0; index < 120; index++) {
    const reservation = run.reserve('ip', 'account', 'write');
    assert.equal(reservation.allowed, true);
    assert.equal(run.result(reservation.reservationId, true).status, 200);
  }
  assert.equal(run.reserve('ip', 'different-account', 'write').allowed, false);
  assert.equal(run.reserve('different-ip', 'account', 'write').allowed, false);
  assert.equal(run.reserve('ip', 'account', 'login').allowed, true);
  assert.equal(run.reserve('ip', 'account', 'recovery').allowed, true);
  const account = run.state.accounts[`write:${key('account')}`];
  assert.equal(account.times.length, 120);
  assert.deepEqual(account.failures, []);
  assert.equal(account.lockedUntil, 0);
  assert.equal(run.reserve('ip', 'account', 'write', 601_000).allowed, true);
});

test('failed writes never create a lock or extend their sliding window', () => {
  const run = scenario();
  for (let index = 0; index < 120; index++) {
    const reservation = run.reserve('ip', 'account', 'write');
    run.result(reservation.reservationId, false, 2000);
  }
  assert.equal(run.state.accounts[`write:${key('account')}`].lockedUntil, 0);
  assert.deepEqual(run.state.accounts[`write:${key('account')}`].failures, []);
  assert.equal(run.reserve('ip', 'account', 'write', 602_000).allowed, true);
});

test('ingress charges only short-lived hashed-IP/global counters with no account or result state', () => {
  const run = scenario();
  for (let index = 0; index < 120; index++) assert.deepEqual(run.ingress('ip'), { allowed: true, retryAfter: 0 });
  assert.deepEqual(run.ingress('ip'), { allowed: false, retryAfter: 60 });
  assert.equal(run.state.ingress.global.length, 120);
  assert.equal(run.state.ingress.ips[key('ip')].length, 120);
  assert.deepEqual(run.state.global, []);
  assert.deepEqual(run.state.ips, {});
  assert.deepEqual(run.state.accounts, {});
  assert.deepEqual(run.state.reservations, {});
  assert.equal(run.reserve('ip', 'account').allowed, true);
  assert.equal(run.ingress('other-ip').allowed, true);
});

test('ingress global admission is 600/minute across IPs and expires without clearing KDF usage', () => {
  const run = scenario();
  const reservation = run.reserve('authenticated-ip', 'account');
  run.result(reservation.reservationId, false);
  for (let index = 0; index < 600; index++) assert.equal(run.ingress(`ip-${index % 5}`).allowed, true);
  assert.deepEqual(run.ingress('fresh-ip'), { allowed: false, retryAfter: 60 });
  assert.equal(run.ingress('fresh-ip', 60_999).allowed, false);
  assert.equal(run.ingress('fresh-ip', 61_000).allowed, true);
  assert.equal(run.state.ingress.global.length, 1);
  assert.equal(Object.keys(run.state.ingress.ips).length, 1);
  assert.equal(run.state.ips[`kdf:${key('authenticated-ip')}`].length, 1);
  assert.equal(run.state.accounts[`login:${key('account')}`].failures.length, 1);
});

test('unreported attempts remain charged after their result reservations expire', () => {
  const run = scenario();
  let reservation;
  for (let index = 0; index < 10; index++) reservation = run.reserve('ip', `account-${index}`);
  assert.equal(run.result(reservation.reservationId, true, 121_001).status, 409);
  assert.equal(run.reserve('ip', 'another-account', 'login', 121_001).allowed, false);
  assert.equal(Object.keys(run.state.reservations).length, 0);
  assert.equal(run.reserve('ip', 'another-account', 'login', 601_001).allowed, true);
});

test('reservation outcomes are consumed once and cannot be replayed to clear failures', () => {
  const run = scenario();
  const reservation = run.reserve('ip', 'account');
  assert.equal(run.result(reservation.reservationId, false).status, 200);
  assert.equal(run.result(reservation.reservationId, true).status, 409);
  assert.equal(run.state.accounts[`login:${key('account')}`].failures.length, 1);
});

test('a late old success cannot erase attempts made after a newer successful login', () => {
  const run = scenario();
  const first = run.reserve('first-ip', 'account');
  const late = run.reserve('late-ip', 'account');
  run.result(first.reservationId, true);
  const current = run.reserve('current-ip', 'account');
  run.result(current.reservationId, false);
  run.result(late.reservationId, true);
  for (let index = 0; index < 9; index++) {
    const reservation = run.reserve(`next-ip-${index}`, 'account');
    assert.equal(reservation.allowed, true);
    run.result(reservation.reservationId, false);
  }
  assert.equal(run.reserve('after-ten-ip', 'account').allowed, false);
});

test('clock rollback cannot reopen an active window', () => {
  const run = scenario();
  for (let index = 0; index < 10; index++) run.reserve('ip', 'account', 'login', 10_000);
  assert.equal(run.reserve('ip', 'account', 'login', 1).allowed, false);
  assert.equal(run.state.clock, 10_000);
});

test('rotating keys cannot grow storage without bound or evict active limits', () => {
  const run = scenario();
  let blockedAt = null;
  for (let index = 0; index < 1000; index++) {
    const now = 1000 + index * 1001; // Below 60/minute; distinct IP/account every time.
    const output = run.reserve(`ip-${index}`, `account-${index}`, 'login', now);
    assert.ok(Buffer.byteLength(JSON.stringify(run.state)) <= AUTH_LIMITS.maxStateBytes);
    if (!output.allowed) {
      blockedAt = now;
      assert.ok(run.state.global.length < 60);
      assert.equal(Object.hasOwn(run.state.ips, `kdf:${key(`ip-${index}`)}`), false);
      break;
    }
  }
  assert.notEqual(blockedAt, null);
  assert.ok(Object.keys(run.state.ips).length > 0);
  run.cleanup(blockedAt + 1_200_000);
  assert.deepEqual(run.state.global, []);
  assert.deepEqual(run.state.ips, {});
  assert.deepEqual(run.state.accounts, {});
  assert.deepEqual(run.state.reservations, {});
  assert.equal(run.reserve('new-ip', 'new-account', 'login', blockedAt + 1_200_001).allowed, true);
});

test('ingress rotation rejects capacity atomically without evicting existing account limits', () => {
  const run = scenario();
  for (let index = 0; index < 10; index++) {
    const reservation = run.reserve(`login-ip-${index}`, 'locked-account');
    run.result(reservation.reservationId, false);
  }
  let writes = 0;
  for (; writes < 2000; writes++) {
    if (!run.reserve(`write-ip-${writes}`, `write-account-${writes}`, 'write').allowed) break;
  }
  assert.ok(writes > 0 && writes < 2000);
  const accountCount = Object.keys(run.state.accounts).length;
  let admitted = 0;
  for (let index = 0; index < 600; index++) {
    const output = run.ingress(`ingress-ip-${index}`);
    assert.ok(Buffer.byteLength(JSON.stringify(run.state)) <= AUTH_LIMITS.maxStateBytes);
    if (!output.allowed) break;
    admitted++;
  }
  assert.ok(admitted < 600);
  assert.equal(Object.keys(run.state.accounts).length, accountCount);
  assert.equal(run.state.accounts[`login:${key('locked-account')}`].lockedUntil, 601_000);
  assert.equal(run.state.ingress.global.length, admitted);
  assert.equal(Object.hasOwn(run.state.ingress.ips, key(`ingress-ip-${admitted}`)), false);
  run.cleanup(601_000);
  assert.deepEqual(run.state.ingress, { global: [], ips: {} });
  assert.deepEqual(run.state.accounts, {});
  assert.equal(run.ingress('fresh-ip', 601_001).allowed, true);
});

/** A serial, rollback-capable transaction harness; no fake time is accepted by HTTP. */
class StorageHarness {
  values = new Map();
  queue = Promise.resolve();
  alarmAt = null;
  transactions = 0;
  async transaction(operation) {
    const execution = this.queue.then(async () => {
      this.transactions++;
      const next = structuredClone(this.values);
      const output = await operation({
        get: async key => structuredClone(next.get(key)),
        put: async (key, value) => { next.set(key, structuredClone(value)); },
        delete: async key => next.delete(key),
      });
      this.values = next;
      return output;
    });
    this.queue = execution.catch(() => {});
    return execution;
  }
  async setAlarm(at) { this.alarmAt = at; }
}
function http(path, body, method = 'POST') {
  return new Request(`https://internal-limiter.example.test${path}`, {
    method, ...(method === 'GET' ? {} : { body: JSON.stringify(body) }),
    headers: { 'content-type': 'application/json' },
  });
}

test('concurrent reservations atomically reserve account and IP limits in every rate class', async () => {
  for (const [purpose, limit] of [['login', 10], ['recovery', 10], ['write', 120]]) {
    const storage = new StorageHarness();
    const limiter = new AuthLimiter({ storage }, {});
    const responses = await Promise.all(Array.from({ length: limit + 15 }, (_, index) => limiter.fetch(http('/reserve', {
      ipKey: key(`ip-${index}`), accountKey: key('same-account'), purpose,
    }))));
    assert.equal(responses.filter(response => response.status === 200).length, limit);
    assert.equal(responses.filter(response => response.status === 429).length, 15);
    assert.equal(storage.transactions, limit + 15);
    const state = storage.values.get('auth-limits:v2');
    assert.equal(state.global.length, purpose === 'login' ? limit : 0);
    assert.equal(Object.keys(state.ips).length, limit);
    assert.equal(Object.keys(state.reservations).length, limit);
    assert.ok(storage.alarmAt > Date.now());
    for (const response of responses.filter(response => response.status === 429)) {
      assert.equal((await response.json()).reservationId, null);
      assert.ok(Number(response.headers.get('retry-after')) > 0);
    }
  }
});

test('concurrent ingress atomically admits at most 120/IP and 600 globally without named state', async () => {
  for (const [ipCount, limit] of [[1, 120], [6, 600]]) {
    const storage = new StorageHarness();
    const limiter = new AuthLimiter({ storage }, {});
    const responses = await Promise.all(Array.from({ length: limit + 30 }, (_, index) => limiter.fetch(http('/ingress', {
      ipKey: key(`ip-${index % ipCount}`),
    }))));
    assert.equal(responses.filter(response => response.status === 200).length, limit);
    assert.equal(responses.filter(response => response.status === 429).length, 30);
    const state = storage.values.get('auth-limits:v2');
    assert.equal(state.ingress.global.length, limit);
    for (const times of Object.values(state.ingress.ips)) assert.ok(times.length <= 120);
    assert.deepEqual(state.global, []);
    assert.deepEqual(state.ips, {});
    assert.deepEqual(state.accounts, {});
    assert.deepEqual(state.reservations, {});
    assert.ok(Buffer.byteLength(JSON.stringify(state)) <= AUTH_LIMITS.maxStateBytes);
    for (const response of responses) {
      const result = await response.json();
      assert.equal(Object.hasOwn(result, 'reservationId'), false);
      if (response.status === 429) assert.ok(Number(response.headers.get('retry-after')) > 0);
    }
  }
});

test('the HTTP limiter rejects raw identifiers, clocks, unknown operations and methods', async () => {
  const storage = new StorageHarness();
  const limiter = new AuthLimiter({ storage }, {});
  const valid = { ipKey: key('ip'), accountKey: key('account'), purpose: 'login' };
  for (const body of [{ ...valid, ipKey: '192.0.2.1' }, { ...valid, accountKey: 'member@example.test' },
    { ...valid, now: 0 }, { ...valid, type: 'cleanup' }, { ...valid, password: 'never stored' }, { ...valid, purpose: 'anything' }]) {
    assert.equal((await limiter.fetch(http('/reserve', body))).status, 400);
  }
  for (const body of [{ ipKey: '192.0.2.1' }, { ipKey: key('ip'), accountKey: key('account') },
    { ipKey: key('ip'), purpose: 'login' }, { ipKey: key('ip'), now: 0 },
    { ipKey: key('ip'), type: 'reserve' }, { ipKey: key('ip'), success: false }, {}]) {
    assert.equal((await limiter.fetch(http('/ingress', body))).status, 400);
  }
  assert.equal((await limiter.fetch(http('/unknown', valid))).status, 404);
  assert.equal((await limiter.fetch(http('/reserve', undefined, 'GET'))).status, 405);
  assert.equal((await limiter.fetch(http('/ingress', undefined, 'GET'))).status, 405);
  assert.equal(storage.values.size, 0);
});

test('HTTP result handling is one-use and storage failure denies reservations', async () => {
  const limiter = new AuthLimiter({ storage: new StorageHarness() }, {});
  const reservation = await (await limiter.fetch(http('/reserve', { ipKey: key('ip'), accountKey: key('account'), purpose: 'login' }))).json();
  assert.ok(reservation.reservationId);
  assert.equal((await limiter.fetch(http('/result', { reservationId: reservation.reservationId, success: true }))).status, 200);
  assert.equal((await limiter.fetch(http('/result', { reservationId: reservation.reservationId, success: true }))).status, 409);
  const failed = new AuthLimiter({ storage: { transaction: async () => { throw new Error('unavailable'); } } }, {});
  const response = await failed.fetch(http('/reserve', { ipKey: key('ip'), accountKey: key('account'), purpose: 'login' }));
  assert.equal(response.status, 503);
  assert.deepEqual(await response.json(), { error: 'limiter_unavailable' });
  const ingress = await failed.fetch(http('/ingress', { ipKey: key('ip') }));
  assert.equal(ingress.status, 503);
  assert.deepEqual(await ingress.json(), { error: 'limiter_unavailable' });
});

test('the alarm removes expired state and does not keep scheduling after it is empty', async t => {
  let now = 1000;
  t.mock.method(Date, 'now', () => now);
  const storage = new StorageHarness();
  const limiter = new AuthLimiter({ storage }, {});
  await limiter.fetch(http('/reserve', { ipKey: key('ip'), accountKey: key('account'), purpose: 'login' }));
  assert.equal(storage.values.size, 1);
  now = 601_001;
  storage.alarmAt = null; // The runtime consumes the scheduled alarm before invoking it.
  await limiter.alarm();
  assert.equal(storage.values.size, 0);
  assert.equal(storage.alarmAt, null);
});

test('ingress-only alarms expire admission counters without extending their lifetime', async t => {
  let now = 1000;
  t.mock.method(Date, 'now', () => now);
  const storage = new StorageHarness();
  const limiter = new AuthLimiter({ storage }, {});
  await limiter.fetch(http('/ingress', { ipKey: key('ip') }));
  assert.equal(storage.values.size, 1);
  assert.equal(storage.alarmAt, 61_000);
  now = 61_000;
  storage.alarmAt = null;
  await limiter.alarm();
  assert.equal(storage.values.size, 0);
  assert.equal(storage.alarmAt, null);
});

test('old persisted versions and invalid pure commands fail closed', () => {
  assert.throws(() => limiterTransition({ ...createLimiterState(), version: 1 }, { type: 'ingress', ipKey: key('ip') }, 1000), { code: 'limiter_unavailable', status: 503 });
  for (const command of [null, [], { type: 'cleanup', now: 0 }, { type: 'ingress', accountKey: key('account') }]) {
    assert.throws(() => limiterTransition(createLimiterState(), command, 1000), { code: 'invalid_limiter_request', status: 400 });
  }
});
