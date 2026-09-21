import { SecurityError } from './security.mjs';

const LIFETIMES = Object.freeze({ verify: 86400, reset: 1800, 'email-change': 86400, 'password-change': 120 });
const hashPattern = /^[0-9a-f]{64}$/;
function validHash(value) { return typeof value === 'string' && hashPattern.test(value); }
function validVersion(value) { return Number.isSafeInteger(value) && value >= 0; }
function requireGuard(guard) {
  if (!guard || !validHash(guard.sessionHash) || typeof guard.subject !== 'string' || !guard.subject || !validVersion(guard.version)) {
    throw new SecurityError('session_guard_required', 403);
  }
}
function recordArgs(record) {
  if (record?.scheme !== 'pbkdf2-sha256' || record.iterations !== 600000 || !/^[0-9a-f]{32}$/.test(record.salt ?? '') || !validHash(record.hash)) {
    throw new SecurityError('invalid_password_record', 500);
  }
  return [record.scheme, record.iterations, record.salt, record.hash];
}
const activeSession = (owner = false) => `EXISTS(SELECT 1 FROM sessions guarded JOIN users actor ON actor.id=guarded.user_id
  WHERE guarded.token_hash=? AND guarded.user_id=? AND guarded.access_sub=? AND actor.access_sub=?
  AND guarded.version=actor.session_version AND actor.session_version=? AND guarded.revoked_at IS NULL
  AND guarded.expires_at>? AND actor.verified_at IS NOT NULL AND actor.disabled_at IS NULL${owner ? " AND actor.role='owner'" : ''})`;
function guardArgs(userId, now, guard) {
  requireGuard(guard);
  return [guard.sessionHash, userId, guard.subject, guard.subject, guard.version, now];
}
const tokenEligible = `SELECT t.user_id FROM verification_tokens t JOIN users u ON u.id=t.user_id
  WHERE t.token_hash=? AND t.purpose=? AND t.used_at IS NULL AND t.expires_at>? AND u.access_sub=?
  AND u.disabled_at IS NULL AND t.user_version=u.session_version
  AND (t.purpose<>'password-change' OR (t.session_hash=? AND EXISTS(SELECT 1 FROM sessions proof_session
    WHERE proof_session.token_hash=t.session_hash AND proof_session.user_id=u.id AND proof_session.access_sub=u.access_sub
    AND proof_session.version=u.session_version AND proof_session.revoked_at IS NULL AND proof_session.expires_at>?)))`;

export class AccountStore {
  constructor(db) { this.db = db; }
  statement(sql, ...values) { return this.db.prepare(sql).bind(...values); }
  userByEmail(email) { return this.statement('SELECT * FROM users WHERE email = ?', email).first(); }
  userById(id) { return this.statement('SELECT * FROM users WHERE id = ?', id).first(); }
  invite(hash, email, now) { return this.statement('SELECT * FROM invites WHERE token_hash=? AND email=? AND accepted_at IS NULL AND expires_at>?', hash, email, now).first(); }
  audit(actor, action, outcome, now, object = null, reason = null) {
    return this.statement('INSERT INTO audit_events(at,actor_id,action,outcome,object_id,reason) VALUES(?,?,?,?,?,?)', now, actor, action, outcome, object, reason).run();
  }
  async createInvitedUser({ id, email, displayName, subject, accessEmail, record, inviteHash, verificationHash, now }) {
    const results = await this.db.batch([
      this.statement(`INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,created_at,updated_at)
        SELECT ?, email, handle, ?, role, ?, ?, ?, ?, ?, ?, ?, ? FROM invites
        WHERE token_hash=? AND email=? AND accepted_at IS NULL AND expires_at>? AND (SELECT count(*) FROM users WHERE disabled_at IS NULL) < 2`,
      id, displayName, subject, accessEmail, ...recordArgs(record), now, now, inviteHash, email, now),
      this.statement('UPDATE invites SET accepted_at=? WHERE token_hash=? AND EXISTS(SELECT 1 FROM users WHERE id=?)', now, inviteHash, id),
      this.statement(`INSERT INTO verification_tokens(token_hash,user_id,purpose,user_version,expires_at,created_at) SELECT ?,id,'verify',session_version,?,? FROM users WHERE id=?`, verificationHash, now + 86400, now, id),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome,object_id) SELECT ?,id,'signup','created',id FROM users WHERE id=?`, now, id)
    ]);
    return results[0].meta.changes === 1;
  }
  async createInvite(hash, email, handle, role, actor, now, guard) {
    const authorization = guardArgs(actor, now, guard);
    const results = await this.db.batch([
      this.statement(`INSERT INTO invites(token_hash,email,handle,role,issued_by,expires_at,created_at) SELECT ?,?,?,?,?,?,? WHERE ${activeSession(true)}`, hash, email, handle, role, actor, now + 86400, now, ...authorization),
      this.statement('UPDATE invites SET accepted_at=? WHERE email=? AND token_hash<>? AND accepted_at IS NULL AND EXISTS(SELECT 1 FROM invites WHERE token_hash=?)', now, email, hash, hash),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome) SELECT ?,?,'invite','issued' WHERE EXISTS(SELECT 1 FROM invites WHERE token_hash=?)`, now, actor, hash)
    ]);
    return results[0].meta.changes === 1;
  }
  async issueToken(hash, userId, purpose, now, lifetime, { email = null, session = null, subject, expectedVersion } = {}) {
    if (!validHash(hash) || !Object.hasOwn(LIFETIMES, purpose) || !Number.isSafeInteger(lifetime) || lifetime < 1 || lifetime > LIFETIMES[purpose]
      || !validVersion(expectedVersion) || typeof subject !== 'string' || !subject) throw new SecurityError('invalid_token_issuance', 400);
    const sessionRequired = ['password-change', 'email-change'].includes(purpose);
    if ((sessionRequired && !validHash(session)) || (!sessionRequired && session !== null)
      || (purpose === 'email-change' ? typeof email !== 'string' || !email : email !== null)) throw new SecurityError('invalid_token_issuance', 400);
    const guard = { sessionHash: session, subject, version: expectedVersion };
    const suffix = sessionRequired ? ` AND ${activeSession()}` : '';
    const extra = sessionRequired ? guardArgs(userId, now, guard) : [];
    const results = await this.db.batch([
      this.statement(`INSERT INTO verification_tokens(token_hash,user_id,purpose,pending_email,session_hash,user_version,expires_at,created_at)
        SELECT ?,id,?,?,?,session_version,?,? FROM users WHERE id=? AND access_sub=? AND session_version=? AND disabled_at IS NULL${suffix}`,
      hash, purpose, email, session, now + lifetime, now, userId, subject, expectedVersion, ...extra),
      this.statement(`UPDATE verification_tokens SET used_at=? WHERE user_id=? AND purpose=? AND token_hash<>? AND used_at IS NULL
        AND EXISTS(SELECT 1 FROM verification_tokens WHERE token_hash=?)`, now, userId, purpose, hash, hash),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome,object_id) SELECT ?,user_id,'credential-link','issued',purpose FROM verification_tokens WHERE token_hash=?`, now, hash)
    ]);
    return results[0].meta.changes === 1;
  }
  token(hash, purpose, subject, now, session = null) {
    return this.statement(`SELECT t.*,u.email,u.access_sub,u.session_version FROM verification_tokens t JOIN users u ON u.id=t.user_id
      WHERE t.token_hash=? AND t.user_id IN (${tokenEligible})`, hash, hash, purpose, now, subject, session, now).first();
  }
  consumeToken(hash, purpose, subject, session, now, marker, extra = '') {
    return this.statement(`UPDATE verification_tokens SET used_at=?,consume_marker=? WHERE token_hash=? AND user_id IN (${tokenEligible}) ${extra}`,
      now, marker, hash, hash, purpose, now, subject, session, now);
  }
  async verifyEmail(hash, subject, now) {
    const marker = crypto.randomUUID();
    const results = await this.db.batch([
      this.consumeToken(hash, 'verify', subject, null, now, marker),
      this.statement(`UPDATE users SET verified_at=COALESCE(verified_at,?),updated_at=? WHERE id IN(SELECT user_id FROM verification_tokens WHERE consume_marker=?)`, now, now, marker),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome) SELECT ?,user_id,'email-verification','completed' FROM verification_tokens WHERE consume_marker=?`, now, marker)
    ]);
    return results[0].meta.changes === 1;
  }
  async setPassword(hash, purpose, subject, session, record, now) {
    if (!['reset', 'password-change'].includes(purpose) || (purpose === 'reset' ? session !== null : !validHash(session))) throw new SecurityError('invalid_token_purpose', 400);
    const marker = crypto.randomUUID();
    const target = 'SELECT user_id FROM verification_tokens WHERE consume_marker=?';
    const results = await this.db.batch([
      this.consumeToken(hash, purpose, subject, session, now, marker),
      this.statement(`UPDATE users SET password_scheme=?,password_iterations=?,password_salt=?,password_hash=?,session_version=session_version+1,updated_at=? WHERE id IN(${target})`, ...recordArgs(record), now, marker),
      this.statement(`UPDATE sessions SET revoked_at=COALESCE(revoked_at,?) WHERE user_id IN(${target})`, now, marker),
      this.statement(`UPDATE verification_tokens SET used_at=COALESCE(used_at,?) WHERE user_id IN(${target})`, now, marker),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome) SELECT ?,user_id,?,'completed' FROM verification_tokens WHERE consume_marker=?`, now, purpose, marker)
    ]);
    return results[0].meta.changes === 1;
  }
  async changeEmail(hash, subject, now) {
    const marker = crypto.randomUUID();
    const target = 'SELECT user_id FROM verification_tokens WHERE consume_marker=?';
    const results = await this.db.batch([
      this.consumeToken(hash, 'email-change', subject, null, now, marker,
        'AND pending_email IS NOT NULL AND NOT EXISTS(SELECT 1 FROM users conflicting WHERE conflicting.email=verification_tokens.pending_email AND conflicting.id<>verification_tokens.user_id)'),
      this.statement(`UPDATE users SET email=(SELECT pending_email FROM verification_tokens WHERE consume_marker=?),verified_at=?,session_version=session_version+1,updated_at=? WHERE id IN(${target})`, marker, now, now, marker),
      this.statement(`UPDATE sessions SET revoked_at=COALESCE(revoked_at,?) WHERE user_id IN(${target})`, now, marker),
      this.statement(`UPDATE verification_tokens SET used_at=COALESCE(used_at,?) WHERE user_id IN(${target})`, now, marker),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome) SELECT ?,user_id,'email-change','completed' FROM verification_tokens WHERE consume_marker=?`, now, marker)
    ]);
    return results[0].meta.changes === 1;
  }
  async newSession({ hash, csrfHash, user, subject, accessEmail, now }) {
    const results = await this.db.batch([
      this.statement(`INSERT INTO sessions(token_hash,user_id,version,access_sub,csrf_hash,created_at,expires_at)
        SELECT ?,id,session_version,access_sub,?,?,? FROM users WHERE id=? AND verified_at IS NOT NULL AND disabled_at IS NULL AND access_sub=? AND password_hash=? AND session_version=?
        AND (SELECT count(*) FROM sessions active WHERE active.user_id=users.id AND active.version=users.session_version AND active.revoked_at IS NULL AND active.expires_at>?)<20`,
      hash, csrfHash, now, now + 604800, user.id, subject, user.password_hash, user.session_version, now),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome) SELECT ?,id,'access-email','changed' FROM users WHERE id=? AND access_email<>? AND EXISTS(SELECT 1 FROM sessions WHERE token_hash=?)`, now, user.id, accessEmail, hash),
      this.statement('UPDATE users SET access_email=?,updated_at=? WHERE id=? AND EXISTS(SELECT 1 FROM sessions WHERE token_hash=?)', accessEmail, now, user.id, hash)
    ]);
    return results[0].meta.changes === 1;
  }
  session(hash, subject, now) {
    return this.statement(`SELECT u.*,s.token_hash AS session_hash,s.csrf_hash,s.expires_at AS session_expires_at FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.access_sub=? AND u.access_sub=? AND s.version=u.session_version AND s.revoked_at IS NULL AND s.expires_at>? AND u.verified_at IS NOT NULL AND u.disabled_at IS NULL`, hash, subject, subject, now).first();
  }
  async revoke(userId, hash, all, now, suppliedGuard) {
    const guard = { ...suppliedGuard, sessionHash: hash };
    const authorization = guardArgs(userId, now, guard);
    const marker = crypto.randomUUID();
    const claim = this.statement(`UPDATE sessions SET revoked_at=?,revocation_marker=? WHERE token_hash=? AND user_id=? AND ${activeSession()}`, now, marker, hash, userId, ...authorization);
    if (all) {
      const results = await this.db.batch([
        claim,
        this.statement('UPDATE users SET session_version=session_version+1,updated_at=? WHERE id IN(SELECT user_id FROM sessions WHERE revocation_marker=?)', now, marker),
        this.statement('UPDATE sessions SET revoked_at=COALESCE(revoked_at,?) WHERE user_id IN(SELECT user_id FROM sessions WHERE revocation_marker=?)', now, marker),
        this.statement('UPDATE verification_tokens SET used_at=COALESCE(used_at,?) WHERE user_id IN(SELECT user_id FROM sessions WHERE revocation_marker=?)', now, marker)
      ]);
      return results[0].meta.changes === 1;
    }
    return (await claim.run()).meta.changes === 1;
  }
  terms(user, version) { return this.statement('SELECT version,content_hash,accepted_at FROM terms_acceptances WHERE user_id=? AND version=?', user, version).first(); }
  async acceptTerms(user, version, contentHash, now, guard) {
    const results = await this.db.batch([
      this.statement(`INSERT OR IGNORE INTO terms_acceptances(user_id,version,content_hash,accepted_at) SELECT ?,?,?,? WHERE ${activeSession()}`, user, version, contentHash, now, ...guardArgs(user, now, guard)),
      this.statement(`SELECT version,content_hash,accepted_at FROM terms_acceptances WHERE user_id=? AND version=? AND ${activeSession()}`, user, version, ...guardArgs(user, now, guard))
    ]);
    return results[1].results[0] ?? null;
  }
  async profile(user, name, now, guard) {
    return (await this.statement(`UPDATE users SET display_name=?,updated_at=? WHERE id=? AND ${activeSession()}`, name, now, user, ...guardArgs(user, now, guard)).run()).meta.changes === 1;
  }
  avatar(user) {return this.statement('SELECT * FROM avatars WHERE user_id=?',user).first();}
  async saveAvatar(user, key, contentHash, expectedKey, now, guard) {
    const result=await this.db.batch([
      this.statement(`INSERT INTO avatars(user_id,object_key,mime_type,width,height,content_hash,updated_at)
        SELECT ?,?,'image/webp',128,128,?,? WHERE ${activeSession()}
        AND ((? IS NULL AND NOT EXISTS(SELECT 1 FROM avatars WHERE user_id=?)) OR EXISTS(SELECT 1 FROM avatars WHERE user_id=? AND object_key=?))
        ON CONFLICT(user_id) DO UPDATE SET object_key=excluded.object_key,content_hash=excluded.content_hash,updated_at=excluded.updated_at`,
      user,key,contentHash,now,...guardArgs(user,now,guard),expectedKey,user,user,expectedKey),
      this.statement("INSERT INTO audit_events(at,actor_id,action,outcome) SELECT ?,?,'avatar','changed' WHERE changes()=1",now,user)
    ]);
    return result[0].meta.changes===1;
  }
  async updateUser(actor, target, { role, disabled, expected_version }, now, guard) {
    if (!['owner', 'member'].includes(role) || typeof disabled !== 'boolean' || !validVersion(expected_version)) throw new SecurityError('invalid_user_change', 400);
    const results = await this.db.batch([
      this.statement(`UPDATE users SET role=?,disabled_at=?,session_version=session_version+1,updated_at=? WHERE id=? AND session_version=? AND ${activeSession(true)}
        AND NOT(role='owner' AND disabled_at IS NULL AND verified_at IS NOT NULL AND (?<>'owner' OR ?=1)
        AND (SELECT count(*) FROM users WHERE role='owner' AND disabled_at IS NULL AND verified_at IS NOT NULL)<=1)`,
      role, disabled ? now : null, now, target, expected_version, ...guardArgs(actor, now, guard), role, disabled ? 1 : 0),
      this.statement(`INSERT INTO audit_events(at,actor_id,action,outcome,object_id) SELECT ?,?,'user-access','changed',? WHERE changes()=1`, now, actor, target),
      // Revocation is enforced by the new user version even before this tidy-up.
      this.statement(`UPDATE sessions SET revoked_at=COALESCE(revoked_at,?) WHERE user_id=? AND version<>(SELECT session_version FROM users WHERE id=?)`, now, target, target),
      this.statement(`UPDATE verification_tokens SET used_at=COALESCE(used_at,?) WHERE user_id=? AND user_version<>(SELECT session_version FROM users WHERE id=?)`, now, target, target)
    ]);
    return results[0].meta.changes === 1;
  }
  async ownerRead(actor, now, guard, query) {
    const authorization = guardArgs(actor, now, guard);
    const results = await this.db.batch([
      this.statement(`SELECT 1 AS allowed WHERE ${activeSession(true)}`, ...authorization),
      this.statement(query, ...authorization)
    ]);
    if (!results[0].results.length) throw new SecurityError('owner_required', 403);
    return results[1].results;
  }
  userList(actor, now, guard) {
    return this.ownerRead(actor, now, guard, `SELECT id,email,handle,display_name,role,verified_at,disabled_at,created_at,session_version AS version FROM users WHERE ${activeSession(true)} ORDER BY created_at`);
  }
  auditList(actor, now, guard) {
    return this.ownerRead(actor, now, guard, `SELECT * FROM audit_events WHERE ${activeSession(true)} ORDER BY id DESC LIMIT 100`);
  }
  /** Call on a bounded maintenance cadence; audit retention is an explicit separate policy. */
  async cleanup(now, limit = 200) {
    if (!Number.isSafeInteger(now) || !Number.isSafeInteger(limit) || limit < 1 || limit > 1000) throw new SecurityError('invalid_cleanup', 500);
    const cutoff = now - 86400;
    return this.db.batch([
      this.statement(`DELETE FROM verification_tokens WHERE token_hash IN(SELECT token_hash FROM verification_tokens WHERE expires_at<=? OR used_at<=? ORDER BY expires_at LIMIT ?)`, cutoff, cutoff, limit),
      this.statement(`DELETE FROM sessions WHERE token_hash IN(SELECT token_hash FROM sessions WHERE expires_at<=? OR revoked_at<=? ORDER BY expires_at LIMIT ?)`, cutoff, cutoff, limit),
      this.statement(`DELETE FROM invites WHERE token_hash IN(SELECT token_hash FROM invites WHERE expires_at<=? OR accepted_at<=? ORDER BY expires_at LIMIT ?)`, cutoff, cutoff, limit)
    ]);
  }
}
