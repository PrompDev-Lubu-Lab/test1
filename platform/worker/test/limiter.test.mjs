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
  assert.equal(run.state.ips[key('same-ip')].length, 10);
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

test('recovery has its own account window while retaining shared IP/global caps', () => {
  const run = scenario();
  for (let index = 0; index < 10; index++) {
    const reservation = run.reserve('exhausted-ip', 'account');
    run.result(reservation.reservationId, false);
  }
  assert.equal(run.reserve('fresh-ip', 'account', 'login').allowed, false);
  assert.equal(run.reserve('exhausted-ip', 'account', 'recovery').allowed, false);
  const recovery = run.reserve('fresh-ip', 'account', 'recovery');
  assert.equal(recovery.allowed, true);
  run.result(recovery.reservationId, true);
  assert.equal(run.reserve('another-ip', 'account', 'login').allowed, false);
  assert.equal(run.reserve('another-ip', 'account', 'recovery').allowed, true);
});

test('the IP limit aggregates purposes and cannot be reset by selecting recovery', () => {
  const run = scenario();
  const purposes = ['login', 'signup', 'reauth', 'recovery'];
  for (let index = 0; index < 10; index++) {
    assert.equal(run.reserve('ip', `account-${index}`, purposes[index % 4]).allowed, true);
  }
  assert.equal(run.reserve('ip', 'new-account', 'recovery').allowed, false);
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
      assert.equal(Object.hasOwn(run.state.ips, key(`ip-${index}`)), false);
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

test('concurrent reservations atomically reserve both account and IP limits', async () => {
  const storage = new StorageHarness();
  const limiter = new AuthLimiter({ storage }, {});
  const responses = await Promise.all(Array.from({ length: 25 }, (_, index) => limiter.fetch(http('/reserve', {
    ipKey: key(`ip-${index}`), accountKey: key('same-account'), purpose: 'login',
  }))));
  assert.equal(responses.filter(response => response.status === 200).length, 10);
  assert.equal(responses.filter(response => response.status === 429).length, 15);
  assert.equal(storage.transactions, 25);
  const state = storage.values.get('auth-limits:v1');
  assert.equal(state.global.length, 10);
  assert.equal(Object.keys(state.ips).length, 10);
  assert.equal(Object.keys(state.reservations).length, 10);
  assert.ok(storage.alarmAt > Date.now());
  for (const response of responses.filter(response => response.status === 429)) {
    assert.equal((await response.json()).reservationId, null);
    assert.ok(Number(response.headers.get('retry-after')) > 0);
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
  assert.equal((await limiter.fetch(http('/unknown', valid))).status, 404);
  assert.equal((await limiter.fetch(http('/reserve', undefined, 'GET'))).status, 405);
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
