import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';
import { AccountStore } from '../store.mjs';
import { D1Harness } from './d1-harness.mjs';

const hash = text => createHash('sha256').update(text).digest('hex');
// These are shaped synthetic storage records. Cryptographic correctness is tested separately.
const record = name => ({ scheme: 'pbkdf2-sha256', iterations: 600000, salt: hash(`salt-${name}`).slice(0, 32), hash: hash(`password-${name}`) });
const subject = id => `${id}-synthetic-access-sub`;
function fixture(t) {
  const db = new D1Harness();
  t.after(() => db.close());
  return { db, store: new AccountStore(db) };
}

test('active sessions are capped per user and one-session signout releases capacity',async t=>{
  const {db,store}=fixture(t); await seed(db);
  const guards=[]; for(let index=0;index<20;index++) guards.push(await session(store,'owner',`capacity-${index}`));
  const user=await store.userById('owner');
  const attempt=()=>store.newSession({hash:hash('capacity-overflow'),csrfHash:hash('overflow-csrf'),user,subject:subject('owner'),accessEmail:user.access_email,now:1001});
  assert.equal(await attempt(),false);
  assert.equal(await store.revoke('owner',guards[0].sessionHash,false,1001,guards[0]),true);
  assert.equal(await attempt(),true);
});
async function seed(db, id = 'owner', { role = id === 'owner' ? 'owner' : 'member', verified = true } = {}) {
  const password = record(id);
  await db.prepare(`INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,verified_at,created_at,updated_at)
    VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)`).bind(id, `${id}@example.test`, id === 'owner' ? 'deandre' : 'ali', id, role, subject(id), `${id}-access@example.test`, password.scheme, password.iterations, password.salt, password.hash, verified ? 900 : null, 900, 900).run();
}
async function session(store, id = 'owner', label = 'main', now = 1000) {
  const user = await store.userById(id);
  const sessionHash = hash(`session-${id}-${label}`);
  assert.equal(await store.newSession({ hash: sessionHash, csrfHash: hash(`csrf-${id}-${label}`), user, subject: subject(id), accessEmail: user.access_email, now }), true);
  return { sessionHash, subject: subject(id), version: user.session_version };
}
async function issue(store, id, purpose, tokenName, now = 1000, options = {}) {
  const tokenHash = hash(tokenName);
  const user = await store.userById(id);
  assert.equal(await store.issueToken(tokenHash, id, purpose, now, purpose === 'password-change' ? 120 : 1800,
    { subject: subject(id), expectedVersion: user.session_version, ...options }), true);
  return tokenHash;
}

test('simultaneous invite redemption creates one account, verification token and audit event', async t => {
  const { db, store } = fixture(t);
  const invite = hash('invite');
  await db.prepare('INSERT INTO invites(token_hash,email,handle,role,expires_at,created_at) VALUES(?,?,?,?,?,?)')
    .bind(invite, 'owner@example.test', 'deandre', 'owner', 10000, 1000).run();
  const outcomes = await Promise.all(['first', 'second'].map(id => store.createInvitedUser({ id, email: 'owner@example.test', displayName: 'Owner',
    subject: subject('owner'), accessEmail: 'different-access@example.test', record: record('new'), inviteHash: invite,
    verificationHash: hash(`verify-${id}`), now: 1000 })));
  assert.deepEqual(outcomes, [true, false]);
  assert.equal((await db.prepare('SELECT count(*) AS n FROM users').first()).n, 1);
  assert.equal((await db.prepare('SELECT count(*) AS n FROM verification_tokens').first()).n, 1);
  assert.equal((await db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='signup'").first()).n, 1);
  assert.equal((await store.userById('first')).access_email, 'different-access@example.test');
});

test('unique Access subjects roll back the entire losing invitation transaction', async t => {
  const { db, store } = fixture(t);
  for (const [name, handle, role] of [['owner', 'deandre', 'owner'], ['member', 'ali', 'member']]) {
    await db.prepare('INSERT INTO invites(token_hash,email,handle,role,expires_at,created_at) VALUES(?,?,?,?,?,?)')
      .bind(hash(`invite-${name}`), `${name}@example.test`, handle, role, 10000, 1000).run();
  }
  const outcomes = await Promise.allSettled(['owner', 'member'].map(id => store.createInvitedUser({ id, email: `${id}@example.test`, displayName: id,
    subject: 'same-synthetic-subject', accessEmail: 'access@example.test', record: record(id), inviteHash: hash(`invite-${id}`),
    verificationHash: hash(`verification-${id}`), now: 1000 })));
  assert.equal(outcomes.filter(value => value.status === 'fulfilled' && value.value).length, 1);
  assert.equal(outcomes.filter(value => value.status === 'rejected').length, 1);
  assert.equal((await db.prepare('SELECT count(*) AS n FROM users').first()).n, 1);
  assert.equal((await db.prepare('SELECT accepted_at FROM invites WHERE token_hash=?').bind(hash('invite-member')).first()).accepted_at, null);
});

test('verification is one-use even when redemptions arrive together', async t => {
  const { db, store } = fixture(t);
  await seed(db, 'owner', { verified: false });
  const token = await issue(store, 'owner', 'verify', 'verify');
  assert.deepEqual(await Promise.all([store.verifyEmail(token, subject('owner'), 1001), store.verifyEmail(token, subject('owner'), 1001)]), [true, false]);
  assert.equal((await store.userById('owner')).verified_at, 1001);
  assert.equal((await db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='email-verification'").first()).n, 1);
});

test('failed token issuance neither invalidates an existing token nor audits a fictitious issue', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const first = await issue(store, 'owner', 'reset', 'first');
  assert.equal(await store.issueToken(hash('denied'), 'owner', 'reset', 1001, 1800, { subject: 'wrong-subject', expectedVersion: 0 }), false);
  assert.ok(await store.token(first, 'reset', subject('owner'), 1002));
  assert.equal((await db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='credential-link'").first()).n, 1);
  const second = await issue(store, 'owner', 'reset', 'second', 1003);
  assert.equal(await store.token(first, 'reset', subject('owner'), 1004), null);
  assert.ok(await store.token(second, 'reset', subject('owner'), 1004));
});

test('password-change proofs require a live bound session and expire at 120 seconds', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const guard = await session(store);
  await assert.rejects(store.issueToken(hash('too-long'), 'owner', 'password-change', 1000, 121, { subject: guard.subject, expectedVersion: guard.version, session: guard.sessionHash }));
  const proof = await issue(store, 'owner', 'password-change', 'proof', 1000, { session: guard.sessionHash });
  assert.ok(await store.token(proof, 'password-change', guard.subject, 1119, guard.sessionHash));
  assert.equal(await store.token(proof, 'reset', guard.subject, 1001), null);
  assert.equal(await store.token(proof, 'password-change', 'wrong-subject', 1001, guard.sessionHash), null);
  assert.equal(await store.token(proof, 'password-change', guard.subject, 1001, hash('another-session')), null);
  assert.equal(await store.setPassword(proof, 'password-change', guard.subject, guard.sessionHash, record('new'), 1120), false);
  const second = await issue(store, 'owner', 'password-change', 'second-proof', 1121, { session: guard.sessionHash });
  await db.prepare('UPDATE sessions SET revoked_at=? WHERE token_hash=?').bind(1122, guard.sessionHash).run();
  assert.equal(await store.setPassword(second, 'password-change', guard.subject, guard.sessionHash, record('new'), 1123), false);
});

test('a password reset wins over an already-read old password-change proof', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const guard = await session(store);
  const proof = await issue(store, 'owner', 'password-change', 'proof', 1000, { session: guard.sessionHash });
  assert.ok(await store.token(proof, 'password-change', guard.subject, 1001, guard.sessionHash));
  const reset = await issue(store, 'owner', 'reset', 'reset', 1002);
  const latest = record('latest-password');
  assert.equal(await store.setPassword(reset, 'reset', guard.subject, null, latest, 1003), true);
  assert.equal(await store.setPassword(proof, 'password-change', guard.subject, guard.sessionHash, record('stale-password'), 1004), false);
  const user = await store.userById('owner');
  assert.equal(user.password_hash, latest.hash);
  assert.equal(user.session_version, 1);
  assert.equal(await store.session(guard.sessionHash, guard.subject, 1004), null);
  assert.notEqual((await db.prepare('SELECT used_at FROM verification_tokens WHERE token_hash=?').bind(proof).first()).used_at, null);
});

test('parallel reset redemption has one winner and invalidates every outstanding proof/session', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const guard = await session(store);
  const other = await session(store, 'owner', 'other');
  const emailToken = await issue(store, 'owner', 'email-change', 'email', 1000, { session: guard.sessionHash, email: 'new@example.test' });
  const reset = await issue(store, 'owner', 'reset', 'reset');
  assert.deepEqual(await Promise.all([
    store.setPassword(reset, 'reset', guard.subject, null, record('winner'), 1001),
    store.setPassword(reset, 'reset', guard.subject, null, record('loser'), 1001),
  ]), [true, false]);
  assert.equal((await store.userById('owner')).password_hash, record('winner').hash);
  assert.equal(await store.session(other.sessionHash, other.subject, 1002), null);
  assert.equal(await store.token(emailToken, 'email-change', guard.subject, 1002), null);
  assert.equal((await db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='reset' AND outcome='completed'").first()).n, 1);
});

test('email-change confirmation needs its token and Access subject, not an existing app session', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const guard = await session(store);
  const pending = await issue(store, 'owner', 'email-change', 'email-change', 1000, { session: guard.sessionHash, email: 'new@example.test' });
  assert.equal(await store.revoke('owner', guard.sessionHash, false, 1001, guard), true);
  assert.ok(await store.token(pending, 'email-change', guard.subject, 1002));
  assert.equal(await store.changeEmail(pending, guard.subject, 1002), true);
  assert.equal((await store.userById('owner')).email, 'new@example.test');
  assert.equal((await store.userById('owner')).session_version, 1);
  assert.equal(await store.changeEmail(pending, guard.subject, 1003), false);
});

test('conflicting email confirmation has no partial token or account mutation', async t => {
  const { db, store } = fixture(t);
  await seed(db); await seed(db, 'member');
  const guard = await session(store);
  const pending = await issue(store, 'owner', 'email-change', 'conflict', 1000, { session: guard.sessionHash, email: 'member@example.test' });
  assert.equal(await store.changeEmail(pending, guard.subject, 1001), false);
  assert.ok(await store.token(pending, 'email-change', guard.subject, 1002));
  assert.equal((await store.userById('owner')).email, 'owner@example.test');
  assert.equal((await store.userById('owner')).session_version, 0);
});

test('stale login snapshots cannot create sessions or alter Access email metadata', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const old = await store.userById('owner');
  await db.prepare('UPDATE users SET session_version=session_version+1 WHERE id=?').bind('owner').run();
  assert.equal(await store.newSession({ hash: hash('stale-login'), csrfHash: hash('csrf'), user: old, subject: old.access_sub, accessEmail: 'stale-access@example.test', now: 1001 }), false);
  assert.equal((await store.userById('owner')).access_email, old.access_email);
  assert.equal((await db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='access-email'").first()).n, 0);
});

test('revoked sessions cannot perform profile, terms, invite, owner-read or role writes', async t => {
  const { db, store } = fixture(t);
  await seed(db); await seed(db, 'member');
  const guard = await session(store);
  assert.equal(await store.revoke('owner', guard.sessionHash, true, 1001, guard), true);
  assert.equal(await store.profile('owner', 'stale name', 1002, guard), false);
  assert.equal(await store.acceptTerms('owner', 'v1', hash('terms'), 1002, guard), null);
  assert.equal(await store.createInvite(hash('stale-invite'), 'member@example.test', 'ali', 'member', 'owner', 1002, guard), false);
  assert.equal(await store.updateUser('owner', 'member', { role: 'owner', disabled: false, expected_version: 0 }, 1002, guard), false);
  await assert.rejects(store.userList('owner', 1002, guard), error => error.status === 403);
  await assert.rejects(store.auditList('owner', 1002, guard), error => error.status === 403);
  assert.equal((await db.prepare("SELECT count(*) AS n FROM audit_events WHERE action='invite'").first()).n, 0);
});

test('stale target versions cannot overwrite a newer owner decision', async t => {
  const { db, store } = fixture(t);
  await seed(db); await seed(db, 'member');
  const guard = await session(store);
  assert.equal(await store.updateUser('owner', 'member', { role: 'member', disabled: true, expected_version: 0 }, 1001, guard), true);
  assert.equal(await store.updateUser('owner', 'member', { role: 'owner', disabled: false, expected_version: 0 }, 1002, guard), false);
  const member = await store.userById('member');
  assert.equal(member.disabled_at, 1001);
  assert.equal(member.role, 'member');
  assert.equal(member.session_version, 1);
  assert.equal((await store.userList('owner', 1002, guard)).find(user => user.id === 'member').version, 1);
});

test('an unverified owner account cannot justify removing the last usable owner', async t => {
  const { db, store } = fixture(t);
  await seed(db); await seed(db, 'member', { role: 'owner', verified: false });
  const guard = await session(store);
  assert.equal(await store.updateUser('owner', 'owner', { role: 'member', disabled: false, expected_version: 0 }, 1001, guard), false);
  assert.equal(await store.updateUser('owner', 'owner', { role: 'owner', disabled: true, expected_version: 0 }, 1001, guard), false);
});

test('parallel owners cannot demote each other into a zero-owner state', async t => {
  const { db, store } = fixture(t);
  await seed(db); await seed(db, 'member', { role: 'owner' });
  const first = await session(store), second = await session(store, 'member');
  assert.deepEqual(await Promise.all([
    store.updateUser('owner', 'member', { role: 'member', disabled: false, expected_version: 0 }, 1001, first),
    store.updateUser('member', 'owner', { role: 'member', disabled: false, expected_version: 0 }, 1001, second),
  ]), [true, false]);
  assert.equal((await db.prepare("SELECT count(*) AS n FROM users WHERE role='owner' AND verified_at IS NOT NULL AND disabled_at IS NULL").first()).n, 1);
});

test('a stale logout-all cannot revoke a newer version session through a coincidental counter', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  const oldGuard = await session(store);
  // Version checks already revoke old sessions even if physical row tidy-up is deferred.
  await db.prepare('UPDATE users SET session_version=session_version+1 WHERE id=?').bind('owner').run();
  const freshGuard = await session(store, 'owner', 'fresh', 1001);
  assert.equal(await store.revoke('owner', oldGuard.sessionHash, true, 1002, oldGuard), false);
  assert.ok(await store.session(freshGuard.sessionHash, freshGuard.subject, 1003));
  assert.equal((await store.userById('owner')).session_version, 1);
});

test('cleanup removes bounded expired credential batches while retaining active records and audit', async t => {
  const { db, store } = fixture(t);
  await seed(db);
  for (let index = 0; index < 5; index++) {
    await db.prepare('INSERT INTO verification_tokens(token_hash,user_id,purpose,user_version,expires_at,created_at) VALUES(?,?,?,?,?,?)')
      .bind(hash(`old-${index}`), 'owner', 'reset', 0, 2000 + index, 1000).run();
  }
  const active = await issue(store, 'owner', 'verify', 'active', 100000);
  const guard = await session(store, 'owner', 'active', 100000);
  const beforeAudit = (await db.prepare('SELECT count(*) AS n FROM audit_events').first()).n;
  const results = await store.cleanup(100000, 2);
  assert.equal(results[0].meta.changes, 2);
  assert.equal((await db.prepare('SELECT count(*) AS n FROM verification_tokens').first()).n, 4);
  assert.ok(await store.token(active, 'verify', guard.subject, 100001));
  assert.ok(await store.session(guard.sessionHash, guard.subject, 100001));
  assert.equal((await db.prepare('SELECT count(*) AS n FROM audit_events').first()).n, beforeAudit);
});
