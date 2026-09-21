PRAGMA foreign_keys = ON;
CREATE TABLE users (
  id TEXT PRIMARY KEY,
  email TEXT NOT NULL UNIQUE,
  handle TEXT NOT NULL UNIQUE CHECK(handle IN ('deandre','ali')),
  display_name TEXT NOT NULL,
  role TEXT NOT NULL CHECK(role IN ('owner','member')),
  access_sub TEXT NOT NULL UNIQUE,
  access_email TEXT NOT NULL,
  password_scheme TEXT NOT NULL,
  password_iterations INTEGER NOT NULL CHECK(password_iterations >= 600000),
  password_salt TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  verified_at INTEGER,
  disabled_at INTEGER,
  session_version INTEGER NOT NULL DEFAULT 0 CHECK(session_version >= 0),
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE invites (
  token_hash TEXT PRIMARY KEY,
  email TEXT NOT NULL,
  handle TEXT NOT NULL CHECK(handle IN ('deandre','ali')),
  role TEXT NOT NULL CHECK(role IN ('owner','member')),
  issued_by TEXT,
  expires_at INTEGER NOT NULL,
  accepted_at INTEGER,
  created_at INTEGER NOT NULL
);
CREATE INDEX invites_email ON invites(email);
CREATE INDEX invites_expiry ON invites(expires_at);
CREATE TABLE sessions (
  token_hash TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id),
  version INTEGER NOT NULL,
  access_sub TEXT NOT NULL,
  csrf_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  revoked_at INTEGER,
  revocation_marker TEXT UNIQUE
);
CREATE INDEX sessions_user ON sessions(user_id);
CREATE INDEX sessions_expiry ON sessions(expires_at);
CREATE INDEX sessions_revoked ON sessions(revoked_at);
CREATE TABLE verification_tokens (
  token_hash TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id),
  purpose TEXT NOT NULL CHECK(purpose IN ('verify','reset','email-change','password-change')),
  pending_email TEXT,
  session_hash TEXT,
  user_version INTEGER NOT NULL CHECK(user_version >= 0),
  expires_at INTEGER NOT NULL,
  used_at INTEGER,
  consume_marker TEXT UNIQUE,
  created_at INTEGER NOT NULL,
  CHECK((purpose IN ('verify','reset') AND session_hash IS NULL AND pending_email IS NULL)
    OR (purpose='password-change' AND session_hash IS NOT NULL AND pending_email IS NULL)
    OR (purpose='email-change' AND session_hash IS NOT NULL AND pending_email IS NOT NULL))
);
CREATE INDEX verification_user ON verification_tokens(user_id, purpose);
CREATE INDEX verification_expiry ON verification_tokens(expires_at);
CREATE INDEX verification_used ON verification_tokens(used_at);
CREATE TABLE terms_acceptances (
  user_id TEXT NOT NULL REFERENCES users(id),
  version TEXT NOT NULL,
  content_hash TEXT NOT NULL,
  accepted_at INTEGER NOT NULL,
  PRIMARY KEY(user_id, version)
);
CREATE TABLE avatars (
  user_id TEXT PRIMARY KEY REFERENCES users(id),
  object_key TEXT NOT NULL UNIQUE,
  mime_type TEXT NOT NULL CHECK(mime_type = 'image/webp'),
  width INTEGER NOT NULL CHECK(width = 128),
  height INTEGER NOT NULL CHECK(height = 128),
  content_hash TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE audit_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  at INTEGER NOT NULL,
  actor_id TEXT,
  action TEXT NOT NULL,
  outcome TEXT NOT NULL,
  object_id TEXT,
  reason TEXT
);
CREATE INDEX audit_time ON audit_events(at);
CREATE TABLE settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  version INTEGER NOT NULL DEFAULT 1,
  updated_at INTEGER NOT NULL
);
