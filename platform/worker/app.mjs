import { APP_NAME } from '../app/public/config.js';
import { AccountStore } from './store.mjs';
import { avatarReady, uploadAvatar } from './avatar.mjs';
import { hexToBytes } from '@noble/hashes/utils.js';
import { createKdfBudget, verifyAccess, verifyTurnstile, randomToken, digestToken, normalizeEmail, readJsonLimited, equalDigest } from './security.mjs';
export { AuthLimiter } from './limiter.mjs';

export const TERMS_VERSION = '2026-09-21-v1';
export const TERMS_TEXT = 'This application displays saved bot outputs and supports project collaboration. It does not place trades, alter risk limits, enable live mode or write run data. Synthetic and historical results do not establish future performance. Live operation requires a separate human decision and server configuration. Use only your invited account and keep private account information out of the shared project board.';
const COOKIE = '__Host-platform-session';
const PASSWORD_ROUTES = new Map([['/auth/signup', 'signup'], ['/auth/login', 'login'], ['/auth/reset', 'reset'], ['/auth/reauth', 'reauth'], ['/me/password', 'password']]);
const RECOVERY_ROUTES = new Set(['/auth/forgot', '/auth/resend', '/auth/verify', '/me/email', '/me/email/verify', '/admin/invites']);
const epoch = () => Math.floor(Date.now() / 1000);
class HttpError extends Error { constructor(status, code, message) { super(message); Object.assign(this, { status, code }); } }
const deny = (status, code, text) => { throw new HttpError(status, code, text); };
const cookieValue = token => `${COOKIE}=${token}; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=604800`;
const clearCookie = () => `${COOKIE}=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0`;
const publicUser = user => ({ id: user.id, email: user.email, handle: user.handle, display_name: user.display_name, role: user.role, verified: user.verified_at !== null });
async function userView(store, user) {
  const avatar = await store.avatar(user.id);
  return { ...publicUser(user), ...(avatar ? { avatar: { url: `/api/avatars/${encodeURIComponent(user.id)}`, width: 128, height: 128 } } : {}) };
}
const passwordRecord = user => user ? ({ scheme: user.password_scheme, iterations: user.password_iterations, salt: user.password_salt, hash: user.password_hash }) : null;
function reply(value, status = 200, headers = {}) {
  return Response.json(value, { status, headers: { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", ...headers } });
}
function nameValue(value) {
  if (typeof value !== 'string' || value.trim().length < 1 || [...value.trim()].length > 80 || /[\p{Cc}\p{Cf}]/u.test(value)) deny(400, 'invalid_name', 'Use a display name between 1 and 80 characters.');
  return value.trim();
}
function tokenValue(value) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_-]{32,128}$/.test(value)) deny(400, 'invalid_token', 'This token is invalid or expired.');
  return value;
}
function configured(env) {
  let origin;
  try { origin = new URL(env.APP_ORIGIN); } catch { deny(503, 'configuration_required', 'Account access has not been configured.'); }
  if (origin.protocol !== 'https:' || origin.origin !== env.APP_ORIGIN || origin.username || origin.password || !env.DB || !env.AUTH_LIMITER || !env.ACCESS_TEAM_DOMAIN || !env.ACCESS_AUDIENCE || typeof env.RATE_KEY_SECRET !== 'string' || env.RATE_KEY_SECRET.length < 32) deny(503, 'configuration_required', 'Account access has not been configured.');
  return origin;
}
function sessionToken(request) {
  const auth = request.headers.get('Authorization');
  if (auth?.startsWith('Bearer ')) return tokenValue(auth.slice(7));
  const cookies = (request.headers.get('Cookie') ?? '').split(';').map(value => value.trim()).filter(value => value.startsWith(COOKIE + '='));
  if (cookies.length !== 1) deny(401, 'login_required', 'Sign in to continue.');
  return tokenValue(cookies[0].slice(COOKIE.length + 1));
}
async function limiterCall(env, route, body) {
  const stub = env.AUTH_LIMITER.get(env.AUTH_LIMITER.idFromName('global-auth-v1'));
  let response;
  try { response = await stub.fetch(`https://auth-limiter.internal${route}`, { method: 'POST', body: JSON.stringify(body), headers: { 'Content-Type': 'application/json' } }); }
  catch { deny(503, 'limiter_unavailable', 'Authentication is temporarily unavailable.'); }
  const result = await response.json();
  if (response.status === 429) { const error = new HttpError(429, 'rate_limited', 'Too many attempts. Try again later.'); error.retryAfter = result.retryAfter; throw error; }
  if (!response.ok) deny(503, 'limiter_unavailable', 'Authentication is temporarily unavailable.');
  return result;
}
function clientIp(request) {
  const ip = request.headers.get('CF-Connecting-IP');
  if (!ip || ip.length > 64 || !/^[0-9a-fA-F:.]+$/.test(ip)) deny(503, 'client_context_missing', 'Trusted client context is unavailable.');
  return ip;
}
async function ingress(request, env) {
  const ipKey = digestToken(`${env.RATE_KEY_SECRET}:ip:${clientIp(request)}`);
  const result = await limiterCall(env, '/ingress', { ipKey });
  if (!result.allowed) deny(429, 'rate_limited', 'Too many attempts. Try again later.');
}
async function reserve(request, env, email, purpose) {
  const ip = clientIp(request);
  const [ipKey, accountKey] = await Promise.all([digestToken(`${env.RATE_KEY_SECRET}:ip:${ip}`), digestToken(`${env.RATE_KEY_SECRET}:account:${email}`)]);
  const result = await limiterCall(env, '/reserve', { ipKey, accountKey, purpose });
  if (!result.allowed || !result.reservationId) deny(429, 'rate_limited', 'Too many attempts. Try again later.');
  return { id: result.reservationId, ip };
}
async function sendToken(env, kind, email, token) {
  if (!env.EMAIL?.send || typeof env.EMAIL_FROM !== 'string' || !/^[^\s<>@]+@[^\s<>@]+$/.test(env.EMAIL_FROM)) deny(503, 'email_unavailable', 'Account email is not configured.');
  const link = new URL(env.APP_ORIGIN);
  // Fragment tokens are not sent as URL query strings or HTTP referrers.
  link.hash = `${kind}=${encodeURIComponent(token)}`;
  const subject = kind === 'reset' ? 'Reset your password' : kind === 'invite' ? 'Your private workspace invitation' : 'Verify your email';
  const text = `${subject} for ${APP_NAME}.\n\nOpen this link: ${link.href}\n\nThis link expires and can be used once. If you did not request it, ignore this message.`;
  await env.EMAIL.send({ from: { email: env.EMAIL_FROM, name: APP_NAME }, to: email, subject, text, html: `<p>${subject} for your private workspace.</p><p><a href="${link.href}">Continue securely</a></p><p>This link expires and can be used once.</p>` });
}

export function createHandler({ accessKeys } = {}) {
  return async function fetchHandler(request, env) {
    const store = env.DB ? new AccountStore(env.DB) : null;
    const now = epoch();
    let reservation = null, succeeded = false, actor = null, route = '/';
    const finish = (value, status = 200, headers = {}) => {
      succeeded = status >= 200 && status < 300;
      return reply(value, status, headers);
    };
    try {
      configured(env);
      const url = new URL(request.url);
      if (url.origin !== env.APP_ORIGIN) deny(403, 'host_denied', 'This hostname is not enabled.');
      route = url.pathname.startsWith('/api/') ? url.pathname.slice(4) : url.pathname;
      const mutation = !['GET', 'HEAD'].includes(request.method);
      if (!['GET', 'POST', 'PATCH', 'PUT'].includes(request.method)) deny(405, 'method_not_allowed', 'Unsupported request method.');
      if (request.headers.get('Origin') && request.headers.get('Origin') !== env.APP_ORIGIN) deny(403, 'origin_denied', 'This origin is not allowed.');
      let body = null;
      if (mutation) {
        if (request.headers.get('Origin') !== env.APP_ORIGIN || request.headers.get('Sec-Fetch-Site') === 'cross-site') deny(403, 'origin_denied', 'This origin is not allowed.');
        if (route !== '/me/avatar') {
          if (request.headers.get('Content-Type')?.split(';')[0].trim().toLowerCase() !== 'application/json') deny(415, 'json_required', 'Send a JSON request.');
          body = await readJsonLimited(request, 8192);
          if (!body || Array.isArray(body) || typeof body !== 'object') deny(400, 'invalid_body', 'Send a JSON object.');
        }
      }
      const passwordRoute = request.method === 'POST' && PASSWORD_ROUTES.has(route);
      const recoveryRoute = request.method === 'POST' && RECOVERY_ROUTES.has(route);
      let email;
      if (passwordRoute || recoveryRoute) {
        email = normalizeEmail(body.email);
        // Bound external verification without charging an unverified named account.
        await ingress(request, env);
      }
      const access = await verifyAccess(request, env, accessKeys);
      if (passwordRoute || recoveryRoute) reservation = await reserve(request, env, email, PASSWORD_ROUTES.get(route) ?? 'recovery');
      if (reservation) await verifyTurnstile(body.turnstile, reservation.ip, env, route.split('/').at(-1));
      const budget = createKdfBudget();

      if (request.method === 'GET' && route === '/config') {
        if (typeof env.TURNSTILE_SITE_KEY !== 'string' || !/^[A-Za-z0-9_-]{1,100}$/.test(env.TURNSTILE_SITE_KEY)) deny(503, 'configuration_required', 'Account access has not been configured.');
        return finish({ account_service: true, turnstile_site_key: env.TURNSTILE_SITE_KEY, features: { avatars: avatarReady(env), board: false, downloads: false } });
      }

      if (request.method === 'POST' && route === '/auth/signup') {
        const inviteHash = await digestToken(tokenValue(body.invite));
        const invite = await store.invite(inviteHash, email, now);
        if (!invite) deny(400, 'invalid_invite', 'This invitation is invalid or expired.');
        const displayName = nameValue(body.display_name);
        const record = await budget.hash(body.password);
        const verification = randomToken(), id = crypto.randomUUID();
        let created;
        try { created = await store.createInvitedUser({ id, email, displayName, subject: access.sub, accessEmail: access.email, record, inviteHash, verificationHash: await digestToken(verification), now: epoch() }); }
        catch { deny(409, 'invite_conflict', 'This invitation or identity has already been used.'); }
        if (!created) deny(409, 'invite_conflict', 'This invitation or identity has already been used.');
        actor = id;
        await sendToken(env, 'verify', email, verification);
        return finish({ verification_required: true, email_accepted_by_provider: true }, 201);
      }
      if (request.method === 'POST' && route === '/auth/verify') {
        const hash = await digestToken(tokenValue(body.token));
        const pending = await store.token(hash, 'verify', access.sub, epoch());
        if (!pending || pending.email !== email || !await store.verifyEmail(hash, access.sub, epoch())) deny(400, 'invalid_token', 'This token is invalid or expired.');
        return finish({ verified: true });
      }
      if (request.method === 'POST' && route === '/auth/login') {
        const user = await store.userByEmail(email);
        const candidate = user?.access_sub === access.sub && user.disabled_at === null ? user : null;
        const correct = await budget.verify(body.password, passwordRecord(candidate));
        if (!correct || !candidate || candidate.verified_at === null) deny(401, 'invalid_login', 'Check your sign-in details and email verification.');
        const token = randomToken(), hash = await digestToken(token), csrf = await digestToken(`csrf:${token}`);
        if (!await store.newSession({ hash, csrfHash: await digestToken(csrf), user, subject: access.sub, accessEmail: access.email, now: epoch() })) deny(401, 'invalid_login', 'Account state changed. Sign in again.');
        actor = user.id;
        await store.audit(actor, 'login', 'completed', now);
        return finish({ user: await userView(store,user), csrf, terms_required: !await store.terms(user.id, TERMS_VERSION) }, 200, { 'Set-Cookie': cookieValue(token) });
      }
      if (request.method === 'POST' && ['/auth/forgot', '/auth/resend'].includes(route)) {
        const user = await store.userByEmail(email);
        if (user?.access_sub === access.sub && user.disabled_at === null) {
          const purpose = route === '/auth/forgot' ? 'reset' : 'verify';
          const token = randomToken();
          if (!await store.issueToken(await digestToken(token), user.id, purpose, epoch(), purpose === 'reset' ? 1800 : 86400, { subject: access.sub, expectedVersion: user.session_version })) deny(409, 'account_changed', 'Account state changed. Try again.');
          await sendToken(env, purpose, user.email, token);
        }
        await store.audit(null, 'account-email-request', 'accepted', now);
        return finish({ message: 'If the account is eligible, an email will be sent.' });
      }
      if (request.method === 'POST' && route === '/auth/reset') {
        const hash = await digestToken(tokenValue(body.token));
        const token = await store.token(hash, 'reset', access.sub, epoch());
        if (!token || token.email !== email) deny(400, 'invalid_token', 'This token is invalid or expired.');
        const record = await budget.hash(body.password);
        if (!await store.setPassword(hash, 'reset', access.sub, null, record, epoch())) deny(400, 'invalid_token', 'This token is invalid or expired.');
        actor = token.user_id;
        return finish({ reset: true, sessions_revoked: true }, 200, { 'Set-Cookie': clearCookie() });
      }
      if (request.method === 'POST' && route === '/me/email/verify') {
        const hash = await digestToken(tokenValue(body.token));
        const pending = await store.token(hash, 'email-change', access.sub, epoch());
        if (!pending || pending.email !== email || !await store.changeEmail(hash, access.sub, epoch())) deny(400, 'invalid_token', 'This token is invalid, expired or conflicts with an account.');
        return finish({ changed: true, sessions_revoked: true }, 200, { 'Set-Cookie': clearCookie() });
      }

      const rawSession = sessionToken(request), sessionHash = await digestToken(rawSession);
      const user = await store.session(sessionHash, access.sub, epoch());
      if (!user) deny(401, 'login_required', 'Your session has expired. Sign in again.');
      actor = user.id;
      const guard = { sessionHash, subject: access.sub, version: user.session_version };
      const csrf = await digestToken(`csrf:${rawSession}`);
      const suppliedCsrf = request.headers.get('X-CSRF-Token');
      if (mutation && (!suppliedCsrf || !/^[a-f0-9]{64}$/.test(suppliedCsrf) || !/^[a-f0-9]{64}$/.test(user.csrf_hash)
        || !equalDigest(hexToBytes(digestToken(suppliedCsrf)), hexToBytes(user.csrf_hash)))) deny(403, 'csrf_denied', 'Refresh your session before making this change.');
      // Bound authenticated writes too. Polling reads do not consume this allowance.
      // A valid one-use revocation must remain possible even after login quotas fill.
      if (mutation && !reservation && !['/auth/logout','/auth/logout-all'].includes(route)) reservation = await reserve(request, env, `user:${user.id}`, 'write');
      if (request.method === 'GET' && route === '/me') return finish({ user: await userView(store,user), csrf, terms_required: !await store.terms(user.id, TERMS_VERSION) });
      if (request.method === 'GET' && route === '/terms') return finish({ version: TERMS_VERSION, text: TERMS_TEXT, content_hash: await digestToken(TERMS_TEXT) });
      if (request.method === 'POST' && route === '/terms/accept') {
        if (body.version !== TERMS_VERSION) deny(409, 'terms_changed', 'Read the current terms before accepting.');
        const acceptance = await store.acceptTerms(user.id, TERMS_VERSION, await digestToken(TERMS_TEXT), epoch(), guard);
        if (!acceptance) deny(401, 'login_required', 'Account state changed. Sign in again.');
        await store.audit(actor, 'terms', 'accepted', now, TERMS_VERSION);
        return finish(acceptance);
      }
      if (request.method === 'POST' && ['/auth/logout', '/auth/logout-all'].includes(route)) {
        if (!await store.revoke(user.id, sessionHash, route === '/auth/logout-all', epoch(), guard)) deny(401, 'login_required', 'Account state changed. Sign in again.');
        await store.audit(actor, 'logout', 'completed', now);
        return finish({ signed_out: true }, 200, { 'Set-Cookie': clearCookie() });
      }
      if (!await store.terms(user.id, TERMS_VERSION)) deny(403, 'terms_required', 'Accept the current terms to enter the workspace.');
      if (request.method === 'PATCH' && route === '/me') {
        if (Object.keys(body).some(key => key !== 'display_name')) deny(400, 'invalid_profile', 'Only the display name can be changed here.');
        if (!await store.profile(user.id, nameValue(body.display_name), epoch(), guard)) deny(401, 'login_required', 'Account state changed. Sign in again.');
        await store.audit(actor, 'profile', 'changed', now);
        return finish({ user: await userView(store,await store.userById(user.id)) });
      }
      if (request.method === 'POST' && route === '/auth/reauth') {
        if (email !== user.email || !await budget.verify(body.password, passwordRecord(user))) deny(401, 'invalid_login', 'Check your sign-in details.');
        const proof = randomToken();
        if (!await store.issueToken(await digestToken(proof), user.id, 'password-change', epoch(), 120, { session: sessionHash, subject: access.sub, expectedVersion: user.session_version })) deny(401, 'login_required', 'Account state changed. Sign in again.');
        await store.audit(actor, 'password-reauth', 'completed', now);
        return finish({ proof, expires_in: 120 });
      }
      if (request.method === 'POST' && route === '/me/password') {
        const hash = await digestToken(tokenValue(body.proof));
        const proof = await store.token(hash, 'password-change', access.sub, epoch(), sessionHash);
        if (!proof || proof.user_id !== user.id || email !== user.email) deny(400, 'invalid_token', 'Confirm your current password again.');
        const record = await budget.hash(body.password);
        if (!await store.setPassword(hash, 'password-change', access.sub, sessionHash, record, epoch())) deny(400, 'invalid_token', 'Confirm your current password again.');
        return finish({ changed: true, sessions_revoked: true }, 200, { 'Set-Cookie': clearCookie() });
      }
      if (request.method === 'POST' && route === '/me/email') {
        if (email !== user.email) deny(400, 'account_mismatch', 'Use your current account address.');
        const newEmail = normalizeEmail(body.new_email);
        const token = randomToken();
        if (!await store.issueToken(await digestToken(token), user.id, 'email-change', epoch(), 86400, { email: newEmail, session: sessionHash, subject: access.sub, expectedVersion: user.session_version })) deny(401, 'login_required', 'Account state changed. Sign in again.');
        await sendToken(env, 'email-change', newEmail, token);
        await store.audit(actor, 'email-change', 'requested', now);
        return finish({ verification_required: true });
      }

      if (route.startsWith('/admin/')) {
        if (user.role !== 'owner') deny(403, 'owner_required', 'Only the owner can perform this action.');
        if (request.method === 'GET' && route === '/admin/users') return finish(await store.userList(user.id, epoch(), guard));
        if (request.method === 'GET' && route === '/admin/audit') return finish(await store.auditList(user.id, epoch(), guard));
        if (request.method === 'POST' && route === '/admin/invites') {
          const inviteEmail = normalizeEmail(body.email);
          const allowed = JSON.parse(env.INVITE_ACCOUNTS ?? '[]');
          const account = Array.isArray(allowed) ? allowed.find(account => account.email === inviteEmail && ['deandre','ali'].includes(account.handle)) : null;
          if (!account || allowed.length !== 2 || new Set(allowed.map(account => account.handle)).size !== 2 || !['owner', 'member'].includes(body.role)) deny(403, 'invite_not_allowed', 'This address or role is outside the configured invitations.');
          const token = randomToken();
          if (!await store.createInvite(await digestToken(token), inviteEmail, account.handle, body.role, user.id, epoch(), guard)) deny(403, 'owner_required', 'Only the owner can issue invitations.');
          await sendToken(env, 'invite', inviteEmail, token);
          return finish({ invited: true, email_accepted_by_provider: true }, 201);
        }
        if (request.method === 'PATCH' && /^\/admin\/users\/[A-Za-z0-9-]{1,80}$/.test(route)) {
          if (!['owner', 'member'].includes(body.role) || typeof body.disabled !== 'boolean' || !Number.isSafeInteger(body.expected_version) || body.expected_version < 0 || Object.keys(body).some(key => !['role', 'disabled', 'expected_version'].includes(key))) deny(400, 'invalid_user_change', 'Supply the role, disabled state and expected version.');
          if (!await store.updateUser(user.id, route.split('/').at(-1), body, epoch(), guard)) deny(409, 'owner_or_user_conflict', 'The account changed or the final owner would be removed. Refresh before trying again.');
          return finish({ updated: true, old_sessions_revoked: true });
        }
        deny(503, 'admin_feature_pending', 'This administration capability is not enabled.');
      }
      if (route === '/me/avatar' && request.method === 'PUT') return finish(await uploadAvatar(request,env,store,user,guard));
      if (route.startsWith('/avatars/') && request.method === 'GET') {
        const target=route.slice('/avatars/'.length);
        if(!/^[A-Za-z0-9-]{1,80}$/.test(target) || !env.AVATARS) deny(404,'not_found','Avatar unavailable.');
        const record=await store.avatar(target);
        const object=record ? await env.AVATARS.get(record.object_key) : null;
        if(!object) deny(404,'not_found','Avatar unavailable.');
        return new Response(object.body,{headers:{'Content-Type':'image/webp','Cache-Control':'private, no-store','X-Content-Type-Options':'nosniff','Content-Security-Policy':"default-src 'none'"}});
      }
      if (route.startsWith('/board/')) deny(503, 'board_binding_required', 'The authenticated board connection is not configured.');
      if (request.method === 'GET' && /^\/(runs|instances)(\/|$)/.test(route)) {
        let origin;
        try { origin = new URL(env.RUN_ORIGIN); } catch { deny(503, 'origin_unavailable', 'The run service is not configured.'); }
        if (origin.protocol !== 'https:' || origin.origin !== env.RUN_ORIGIN || origin.username || origin.password || !env.ORIGIN_CLIENT_ID || !env.ORIGIN_CLIENT_SECRET) deny(503, 'origin_unavailable', 'The run service is not configured.');
        if (route.includes('..') || /%2f|%5c|%2e|\\/i.test(route)) deny(400, 'invalid_path', 'Invalid run path.');
        const upstream = await fetch(new URL(route + url.search, origin), { redirect: 'manual', headers: { 'CF-Access-Client-Id': env.ORIGIN_CLIENT_ID, 'CF-Access-Client-Secret': env.ORIGIN_CLIENT_SECRET }, signal: AbortSignal.timeout(10000) });
        if (upstream.status >= 300 && upstream.status < 400) deny(503, 'origin_unavailable', 'The protected run service is unavailable.');
        const headers = new Headers({ 'Content-Type': upstream.headers.get('Content-Type') ?? 'application/json', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
        for (const name of ['X-Data-Source', 'X-Next-After', 'X-Next-Offset', 'X-Has-More', 'X-Result-Warning']) if (upstream.headers.has(name)) headers.set(name, upstream.headers.get(name));
        return new Response(upstream.body, { status: upstream.status, headers });
      }
      if (route === '/events') deny(503, 'event_session_pending', 'The authenticated event stream is not enabled.');
      deny(404, 'not_found', 'Unknown platform route.');
    } catch (error) {
      const status = Number.isInteger(error.status) && error.status >= 400 && error.status <= 599 ? error.status : 503;
      const code = typeof error.code === 'string' && /^[a-z_]{1,60}$/.test(error.code) ? error.code : 'service_unavailable';
      // Invalid transport/JWT floods and repeated 429s cannot create unbounded D1 writes.
      // Every admitted auth failure is still recorded, including wrong passwords.
      if (store && reservation) try { await store.audit(actor, route.startsWith('/auth/') ? 'authentication' : 'request', 'denied', now, null, code); } catch { /* Never reveal failed SQL or request contents. */ }
      return reply({ error: code, detail: status === 503 ? 'This capability is temporarily unavailable or not configured.' : error.message }, status, error.retryAfter ? { 'Retry-After': String(error.retryAfter) } : {});
    } finally {
      if (reservation) try { await limiterCall(env, '/result', { reservationId: reservation.id, success: succeeded }); } catch { /* Reservation remains counted if reporting fails. */ }
    }
  };
}
export default {
  fetch: createHandler(),
  async scheduled(_event, env) {
    if (!env.DB) return;
    await new AccountStore(env.DB).cleanup(epoch(), 1000);
  }
};
