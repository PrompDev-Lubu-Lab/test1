import { readJsonLimited, SecurityError } from './security.mjs';

export const AUTH_LIMITS = Object.freeze({
  attempts: 10,
  windowMs: 600_000,
  globalAttempts: 60,
  globalWindowMs: 60_000,
  writeAttempts: 120,
  ingressAttempts: 120,
  ingressGlobalAttempts: 600,
  ingressWindowMs: 60_000,
  reservationMs: 120_000,
  maxStateBytes: 96 * 1024,
});
const STATE_KEY = 'auth-limits:v2';
const kdfPurposes = new Set(['login', 'signup', 'reauth', 'reset', 'password']);
const purposes = new Set([...kdfPurposes, 'recovery', 'write']);
const digestPattern = /^[0-9a-f]{64}$/;
const reservationPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const encoder = new TextEncoder();

export function createLimiterState() {
  return { version: 2, clock: 0, ingress: { global: [], ips: {} }, global: [], ips: {}, accounts: {}, reservations: {} };
}

function validateFields(command, fields) {
  if (!command || typeof command !== 'object' || Array.isArray(command)
    || Object.keys(command).some(key => !fields.includes(key))) {
    throw new SecurityError('invalid_limiter_request', 400);
  }
}

function prune(state, now) {
  state.ingress.global = state.ingress.global.filter(time => time > now - AUTH_LIMITS.ingressWindowMs);
  for (const [key, times] of Object.entries(state.ingress.ips)) {
    const recent = times.filter(time => time > now - AUTH_LIMITS.ingressWindowMs);
    if (recent.length) state.ingress.ips[key] = recent;
    else delete state.ingress.ips[key];
  }
  state.global = state.global.filter(time => time > now - AUTH_LIMITS.globalWindowMs);
  for (const [id, reservation] of Object.entries(state.reservations)) {
    if (reservation.at + AUTH_LIMITS.reservationMs <= now) delete state.reservations[id];
  }
  const referenced = new Set(Object.values(state.reservations).map(value => value.account));
  for (const [key, times] of Object.entries(state.ips)) {
    const recent = times.filter(time => time > now - AUTH_LIMITS.windowMs);
    if (recent.length) state.ips[key] = recent;
    else delete state.ips[key];
  }
  for (const [key, account] of Object.entries(state.accounts)) {
    account.times = account.times.filter(time => time > now - AUTH_LIMITS.windowMs);
    account.failures = account.failures.filter(time => time > now - AUTH_LIMITS.windowMs);
    if (account.lockedUntil <= now) account.lockedUntil = 0;
    if (!account.times.length && !account.failures.length && !account.lockedUntil && !referenced.has(key)) {
      delete state.accounts[key];
    }
  }
  return state;
}

function blocked(retryMs, reservation = true) {
  return {
    status: 429,
    result: { allowed: false, ...(reservation ? { reservationId: null } : {}), retryAfter: Math.max(1, Math.ceil(retryMs / 1000)) },
  };
}

function capacityRetry(state, now) {
  const expirations = [now + AUTH_LIMITS.windowMs];
  for (const times of Object.values(state.ips)) {
    if (times.length) expirations.push(times[0] + AUTH_LIMITS.windowMs);
  }
  for (const times of Object.values(state.ingress.ips)) {
    if (times.length) expirations.push(times[0] + AUTH_LIMITS.ingressWindowMs);
  }
  if (state.ingress.global.length) expirations.push(state.ingress.global[0] + AUTH_LIMITS.ingressWindowMs);
  for (const entry of Object.values(state.reservations)) expirations.push(entry.at + AUTH_LIMITS.reservationMs);
  return Math.max(1, Math.min(...expirations) - now);
}

/**
 * Pure transition for deterministic tests. Production time and reservation IDs come
 * only from AuthLimiter below; neither can be supplied through its HTTP interface.
 */
export function limiterTransition(previous, command, now, newReservationId) {
  if (!Number.isSafeInteger(now) || now < 0) throw new SecurityError('invalid_limiter_clock', 500);
  const state = structuredClone(previous ?? createLimiterState());
  if (state.version !== 2) throw new SecurityError('limiter_unavailable', 503);
  if (!command || typeof command !== 'object' || Array.isArray(command)) throw new SecurityError('invalid_limiter_request', 400);
  now = Math.max(now, state.clock);
  state.clock = now;
  prune(state, now);

  if (command.type === 'cleanup') {
    validateFields(command, ['type']);
    return { state, status: 200, result: { cleaned: true } };
  }

  if (command.type === 'ingress') {
    validateFields(command, ['type', 'ipKey']);
    if (typeof command.ipKey !== 'string' || !digestPattern.test(command.ipKey)) throw new SecurityError('invalid_limiter_request', 400);
    const ipTimes = state.ingress.ips[command.ipKey] ?? [];
    const delays = [];
    if (ipTimes.length >= AUTH_LIMITS.ingressAttempts) delays.push(ipTimes[0] + AUTH_LIMITS.ingressWindowMs - now);
    if (state.ingress.global.length >= AUTH_LIMITS.ingressGlobalAttempts) delays.push(state.ingress.global[0] + AUTH_LIMITS.ingressWindowMs - now);
    if (delays.length) return { state, ...blocked(Math.max(...delays), false) };

    // Unverified callers only charge short-lived IP/global admission counters.
    // They cannot select, reserve, fail or lock any named account.
    const candidate = structuredClone(state);
    candidate.ingress.global.push(now);
    candidate.ingress.ips[command.ipKey] = [...ipTimes, now];
    if (encoder.encode(JSON.stringify(candidate)).byteLength > AUTH_LIMITS.maxStateBytes) {
      return { state, ...blocked(capacityRetry(state, now), false) };
    }
    return { state: candidate, status: 200, result: { allowed: true, retryAfter: 0 } };
  }

  if (command.type === 'reserve') {
    validateFields(command, ['type', 'ipKey', 'accountKey', 'purpose']);
    const { ipKey, accountKey, purpose } = command;
    if (typeof ipKey !== 'string' || !digestPattern.test(ipKey)
      || typeof accountKey !== 'string' || !digestPattern.test(accountKey) || !purposes.has(purpose)
      || typeof newReservationId !== 'string' || !reservationPattern.test(newReservationId)
      || Object.hasOwn(state.reservations, newReservationId)) {
      throw new SecurityError('invalid_limiter_request', 400);
    }
    const key = `${purpose}:${accountKey}`;
    const rateClass = kdfPurposes.has(purpose) ? 'kdf' : purpose;
    const ipBucket = `${rateClass}:${ipKey}`;
    const attempts = purpose === 'write' ? AUTH_LIMITS.writeAttempts : AUTH_LIMITS.attempts;
    const ipTimes = state.ips[ipBucket] ?? [];
    const account = state.accounts[key] ?? { times: [], failures: [], lockedUntil: 0, epoch: newReservationId };
    const delays = [];
    if (ipTimes.length >= attempts) delays.push(ipTimes[0] + AUTH_LIMITS.windowMs - now);
    if (account.times.length >= attempts) delays.push(account.times[0] + AUTH_LIMITS.windowMs - now);
    if (purpose === 'login' && account.lockedUntil > now) delays.push(account.lockedUntil - now);
    if (rateClass === 'kdf' && state.global.length >= AUTH_LIMITS.globalAttempts) delays.push(state.global[0] + AUTH_LIMITS.globalWindowMs - now);
    if (delays.length) return { state, ...blocked(Math.max(...delays)) };

    // Keep the pre-reservation state for an atomic capacity rejection. Do not evict
    // someone else's active limits when a caller rotates account/IP identifiers.
    const candidate = structuredClone(state);
    if (rateClass === 'kdf') candidate.global.push(now);
    candidate.ips[ipBucket] = [...ipTimes, now];
    candidate.accounts[key] = { ...account, times: [...account.times, now] };
    candidate.reservations[newReservationId] = { account: key, purpose, at: now, epoch: account.epoch };
    if (encoder.encode(JSON.stringify(candidate)).byteLength > AUTH_LIMITS.maxStateBytes) {
      return { state, ...blocked(capacityRetry(state, now)) };
    }
    return { state: candidate, status: 200, result: { allowed: true, reservationId: newReservationId, retryAfter: 0 } };
  }

  if (command.type === 'result') {
    validateFields(command, ['type', 'reservationId', 'success']);
    const { reservationId, success } = command;
    if (typeof reservationId !== 'string' || !reservationPattern.test(reservationId) || typeof success !== 'boolean') {
      throw new SecurityError('invalid_limiter_request', 400);
    }
    const reservation = state.reservations[reservationId];
    if (!reservation) return { state, status: 409, result: { error: 'reservation_unavailable' } };
    delete state.reservations[reservationId];
    const account = state.accounts[reservation.account];
    // A late result cannot reset or poison a newer account epoch. Counted writes
    // are never cleared by success; IP and KDF-global charges are never removed.
    if (account && account.epoch === reservation.epoch) {
      if (success && reservation.purpose !== 'write') {
        account.times = [];
        account.failures = [];
        account.lockedUntil = 0;
        account.epoch = `completed:${reservationId}`;
      } else if (reservation.purpose === 'login') {
        account.failures.push(now);
        if (account.failures.length >= AUTH_LIMITS.attempts) account.lockedUntil = now + AUTH_LIMITS.windowMs;
      }
    }
    prune(state, now);
    return { state, status: 200, result: { recorded: true } };
  }
  throw new SecurityError('invalid_limiter_request', 400);
}

function hasEntries(state) {
  return state.ingress.global.length > 0 || Object.keys(state.ingress.ips).length > 0
    || state.global.length > 0 || Object.keys(state.ips).length > 0
    || Object.keys(state.accounts).length > 0 || Object.keys(state.reservations).length > 0;
}

/** Bind one named instance globally: this is not an independent instance per IP. */
export class AuthLimiter {
  constructor(ctx, env) {
    this.ctx = ctx;
  }

  async apply(command) {
    // Generate once outside the callback because storage can retry a transaction.
    const reservationId = command.type === 'reserve' ? crypto.randomUUID() : undefined;
    const now = Date.now();
    const output = await this.ctx.storage.transaction(async tx => {
      const result = limiterTransition(await tx.get(STATE_KEY), command, now, reservationId);
      if (hasEntries(result.state)) await tx.put(STATE_KEY, result.state);
      else await tx.delete(STATE_KEY);
      return result;
    });
    if (hasEntries(output.state)) await this.ctx.storage.setAlarm(Date.now() + 60_000);
    return output;
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (!['/ingress', '/reserve', '/result'].includes(url.pathname)) {
      return Response.json({ error: 'not_found' }, { status: 404 });
    }
    if (request.method !== 'POST') {
      return Response.json({ error: 'method_not_allowed' }, { status: 405, headers: { allow: 'POST' } });
    }
    try {
      const body = await readJsonLimited(request, 1024);
      // A client-supplied clock or operation must never reach the pure transition.
      const fields = url.pathname === '/ingress' ? ['ipKey']
        : url.pathname === '/reserve' ? ['ipKey', 'accountKey', 'purpose'] : ['reservationId', 'success'];
      validateFields(body, fields);
      const output = await this.apply({ ...body, type: url.pathname.slice(1) });
      const headers = { 'cache-control': 'no-store' };
      if (output.status === 429) headers['retry-after'] = String(output.result.retryAfter);
      return Response.json(output.result, { status: output.status, headers });
    } catch (error) {
      const status = error instanceof SecurityError ? error.status : 503;
      const code = error instanceof SecurityError ? error.code : 'limiter_unavailable';
      return Response.json({ error: code }, { status, headers: { 'cache-control': 'no-store' } });
    }
  }

  async alarm() {
    await this.apply({ type: 'cleanup' });
  }
}
