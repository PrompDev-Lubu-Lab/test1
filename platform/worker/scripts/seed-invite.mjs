import { readFile, writeFile, unlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { randomToken, digestToken, normalizeEmail } from '../security.mjs';

const literal = value => `'${value.replaceAll("'", "''")}'`;

/** No password or Access subject is seeded. The invited human supplies both later. */
export function prepareBootstrap(accounts, now = Math.floor(Date.now() / 1000)) {
  if (!Array.isArray(accounts) || accounts.length !== 2 || !Number.isSafeInteger(now) || now < 1) throw new Error('Invalid private bootstrap settings.');
  const normalized = accounts.map(value => ({ email: normalizeEmail(value.email), handle: value.handle }));
  if (new Set(normalized.map(value => value.email)).size !== 2 || new Set(normalized.map(value => value.handle)).size !== 2
    || normalized.some(value => !['deandre', 'ali'].includes(value.handle))) throw new Error('Exactly the two approved account mappings are required.');
  const email = normalized.find(value => value.handle === 'deandre').email;
  const token = randomToken(), hash = digestToken(token), expiresAt = now + 86400;
  const sql = `INSERT INTO invites(token_hash,email,handle,role,issued_by,expires_at,created_at)
SELECT ${literal(hash)},${literal(email)},'deandre','owner',NULL,${expiresAt},${now}
WHERE NOT EXISTS (SELECT 1 FROM users)
  AND NOT EXISTS (SELECT 1 FROM invites WHERE accepted_at IS NULL AND expires_at > ${now});
SELECT token_hash,email,handle,role,expires_at FROM invites WHERE token_hash=${literal(hash)};
`;
  return { token, hash, email, expiresAt, sql };
}

export function verifyBootstrapResult(result, prepared) {
  if (!Array.isArray(result) || result.some(value => value.success === false)) throw new Error('Bootstrap result was not confirmed; do not retry blindly.');
  const rows = result.flatMap(value => Array.isArray(value.results) ? value.results : []);
  const matched = rows.filter(row => row.token_hash === prepared.hash && row.email === prepared.email
    && row.handle === 'deandre' && row.role === 'owner' && row.expires_at === prepared.expiresAt);
  if (matched.length !== 1) throw new Error('Bootstrap refused: an account or an active invitation already exists, or the result was not confirmed.');
}

async function executeWrangler(settings, sqlFile, mode) {
  if (!/^[A-Za-z0-9][A-Za-z0-9_-]{0,62}$/.test(settings.database ?? '') || !settings.wrangler || !settings.wranglerConfig) throw new Error('Supply the exact database and installed Wrangler/config paths.');
  const args = [resolve(settings.wrangler), 'd1', 'execute', settings.database, mode, '--config', resolve(settings.wranglerConfig), '--file', sqlFile, '--json', '--yes'];
  return new Promise((accept, reject) => {
    const child = spawn(process.execPath, args, { shell: false, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '', bytes = 0, failed = false;
    const timer = setTimeout(() => { failed = true; child.kill(); reject(new Error('Bootstrap timed out; inspect the database before retrying.')); }, 60000);
    child.stdout.on('data', chunk => {
      bytes += chunk.length;
      if (bytes > 1024 * 1024) { failed = true; child.kill(); reject(new Error('Unexpected bootstrap output; inspect the database before retrying.')); }
      else output += chunk;
    });
    child.stderr.on('data', () => {}); // Never echo private config, SQL or provider diagnostics.
    child.on('error', () => { clearTimeout(timer); reject(new Error('Could not start the installed Wrangler.')); });
    child.on('close', code => {
      clearTimeout(timer);
      if (failed) return;
      if (code !== 0) return reject(new Error('Wrangler did not confirm bootstrap; inspect the database before retrying.'));
      try { accept(JSON.parse(output)); } catch { reject(new Error('Bootstrap returned unreadable confirmation; inspect the database before retrying.')); }
    });
  });
}

async function main() {
  const args = process.argv.slice(2);
  if (args.length !== 4 || args[0] !== '--settings' || args[2] !== '--apply' || !['--local', '--remote'].includes(args[3])) {
    throw new Error('Usage: node scripts/seed-invite.mjs --settings /private/bootstrap.local.json --apply --local|--remote');
  }
  // The link is a credential: refuse CI/log capture or redirected output.
  if (!process.stdout.isTTY || !process.stdin.isTTY) throw new Error('Run this operator-only command in an interactive terminal; the one-use link must not enter logs.');
  const settings = JSON.parse(await readFile(resolve(args[1]), 'utf8'));
  const app = new URL(settings.APP_ORIGIN);
  if (app.protocol !== 'https:' || app.origin !== settings.APP_ORIGIN || app.username || app.password) throw new Error('Supply the reviewed exact HTTPS app origin.');
  const accounts = typeof settings.INVITE_ACCOUNTS === 'string' ? JSON.parse(settings.INVITE_ACCOUNTS) : settings.INVITE_ACCOUNTS;
  const prepared = prepareBootstrap(accounts);
  const sqlFile = join(tmpdir(), `platform-bootstrap-${crypto.randomUUID()}.sql`);
  try {
    // The temporary query contains only the digest, never the invite credential.
    await writeFile(sqlFile, prepared.sql, { flag: 'wx', mode: 0o600 });
    verifyBootstrapResult(await executeWrangler(settings, sqlFile, args[3]), prepared);
    app.hash = `invite=${prepared.token}`;
    process.stdout.write(`Owner invitation confirmed. Expires ${new Date(prepared.expiresAt * 1000).toISOString()}.\nKeep the following one-use link private; it is shown once.\n${app.href}\n`);
  } finally { await unlink(sqlFile).catch(() => {}); }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
}
