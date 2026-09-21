import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { randomToken, digestToken, normalizeEmail } from '../security.mjs';

const literal = value => `'${value.replaceAll("'", "''")}'`;
const EMAIL_TIMEOUT_MS = 30000;
const EMAIL_RESPONSE_LIMIT = 16384;
const PRIVATE_JSON_LIMIT = 128 * 1024;
const USAGE = 'Usage: node scripts/seed-invite.mjs --settings /private/bootstrap.local.json --apply --local|--remote [--email (remote only)]';

function accountMappings(value) {
  const accounts = typeof value === 'string' ? JSON.parse(value) : value;
  if (!Array.isArray(accounts) || accounts.length !== 2) throw new Error('Invalid private bootstrap settings.');
  const normalized = accounts.map(value => ({ email: normalizeEmail(value?.email), handle: value?.handle }));
  if (new Set(normalized.map(value => value.email)).size !== 2 || new Set(normalized.map(value => value.handle)).size !== 2
    || normalized.some(value => !['deandre', 'ali'].includes(value.handle))) throw new Error('Exactly the two approved account mappings are required.');
  return normalized.sort((a, b) => a.handle.localeCompare(b.handle));
}

function appOrigin(value) {
  const app = new URL(value);
  if (app.protocol !== 'https:' || app.origin !== value || app.username || app.password) throw new Error('Supply the reviewed exact HTTPS app origin.');
  return app;
}

/** No password or Access subject is seeded. The invited human supplies both later. */
export function prepareBootstrap(accounts, now = Math.floor(Date.now() / 1000)) {
  if (!Number.isSafeInteger(now) || now < 1 || !Number.isSafeInteger(now + 86400)) throw new Error('Invalid private bootstrap settings.');
  const normalized = accountMappings(accounts);
  const email = normalized.find(value => value.handle === 'deandre').email;
  const token = randomToken(), hash = digestToken(token), expiresAt = now + 86400;
  const sql = `INSERT INTO invites(token_hash,email,handle,role,issued_by,expires_at,created_at)
SELECT ${literal(hash)},${literal(email)},'deandre','owner',NULL,${expiresAt},${now}
WHERE NOT EXISTS (SELECT 1 FROM users)
  AND NOT EXISTS (SELECT 1 FROM invites WHERE accepted_at IS NULL AND expires_at > ${now});
`;
  const confirmationSql = `SELECT token_hash,email,handle,role,expires_at FROM invites WHERE token_hash=${literal(hash)};`;
  return { token, hash, email, expiresAt, sql, confirmationSql };
}

export function verifyBootstrapResult(result, prepared) {
  if (!Array.isArray(result) || !result.length || result.some(value => value?.success !== true || !Array.isArray(value.results))) throw new Error('Bootstrap result was not confirmed; do not retry blindly.');
  const rows = result.flatMap(value => value.results);
  const matched = rows.filter(row => row.token_hash === prepared.hash && row.email === prepared.email
    && row.handle === 'deandre' && row.role === 'owner' && row.expires_at === prepared.expiresAt);
  if (rows.length !== 1 || matched.length !== 1) throw new Error('Bootstrap refused: an account or an active invitation already exists, or the result was not confirmed.');
}

/** Check only the explicitly selected private configuration; no credential discovery. */
function emailSettings(settings, config, apiToken) {
  const app = appOrigin(settings.APP_ORIGIN);
  const accounts = accountMappings(settings.INVITE_ACCOUNTS);
  const sender = normalizeEmail(settings.EMAIL_FROM);
  const accountId = settings.CLOUDFLARE_ACCOUNT_ID;
  const databases = config?.d1_databases;
  const emailBindings = config?.send_email;
  if (sender !== settings.EMAIL_FROM || !/^[a-f0-9]{32}$/.test(accountId ?? '')
    || !/^[A-Za-z0-9][A-Za-z0-9_-]{0,62}$/.test(settings.database ?? '')
    || config.account_id !== accountId || config.vars?.APP_ORIGIN !== settings.APP_ORIGIN
    || config.vars?.EMAIL_FROM !== sender
    || JSON.stringify(accountMappings(config.vars?.INVITE_ACCOUNTS)) !== JSON.stringify(accounts)
    || !Array.isArray(databases) || databases.length !== 1 || databases[0].binding !== 'DB'
    || databases[0].database_name !== settings.database
    || !/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(databases[0].database_id ?? '')
    || databases[0].database_id === '00000000-0000-0000-0000-000000000000'
    || (settings.databaseId !== undefined && settings.databaseId !== databases[0].database_id)
    || !Array.isArray(emailBindings) || emailBindings.length !== 1 || emailBindings[0].name !== 'EMAIL'
    || !Array.isArray(emailBindings[0].allowed_sender_addresses) || emailBindings[0].allowed_sender_addresses.length !== 1
    || emailBindings[0].allowed_sender_addresses[0] !== sender
    || typeof apiToken !== 'string' || apiToken.length < 20 || apiToken.length > 4096
    || !/^[A-Za-z0-9._~+/-]+={0,2}$/.test(apiToken)) throw new Error('Invalid private email bootstrap settings.');
  return { app, accounts, sender, accountId };
}

function outcome(status, prepared, invitationConfirmed = false, sendAttempted = false) {
  return Object.freeze({
    status, invitation_confirmed: invitationConfirmed, send_attempted: sendAttempted,
    inbox_receipt_verified: false, automatic_retry: false,
    ...(prepared ? { invite_digest_prefix: prepared.hash.slice(0, 16) } : {})
  });
}

async function boundedJson(response) {
  if (!response.body || Number(response.headers.get('content-length')) > EMAIL_RESPONSE_LIMIT) throw new Error('Unconfirmed response.');
  const reader = response.body.getReader();
  const chunks = [];
  let size = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > EMAIL_RESPONSE_LIMIT) throw new Error('Unconfirmed response.');
      chunks.push(value);
    }
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks, size)));
  } finally { void reader.cancel().catch(() => {}); }
}

function deliveryState(value, recipient) {
  if (value?.success === false && value.result == null) return 'email_rejected';
  if (value?.success !== true || !value.result || (value.errors !== undefined && (!Array.isArray(value.errors) || value.errors.length))) return 'email_uncertain';
  const buckets = ['delivered', 'queued', 'permanent_bounces', 'suppressed_recipients'];
  let selected;
  for (const bucket of buckets) {
    const list = value.result[bucket] ?? (bucket === 'suppressed_recipients' ? [] : undefined);
    if (!Array.isArray(list) || list.length > 1 || list.some(address => address !== recipient)) return 'email_uncertain';
    if (list.length) {
      if (selected) return 'email_uncertain';
      selected = bucket;
    }
  }
  // "Delivered" is provider-reported recipient-server acceptance, never inbox receipt.
  return selected === 'delivered' ? 'email_delivered' : selected === 'queued' ? 'email_queued'
    : selected ? 'email_rejected' : 'email_uncertain';
}

/** The token enters an HTTP header only here. Exactly one request, without retries. */
async function sendInvitation({ accountId, sender, app }, prepared, apiToken, fetchImpl) {
  app.hash = `invite=${prepared.token}`;
  const url = app.href;
  const escapeHtml = value => value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&#39;');
  const body = JSON.stringify({
    from: { address: sender, name: 'Private workspace' }, to: [prepared.email],
    subject: 'Your private workspace setup invitation',
    text: `This is the requested setup test and first-owner invitation. Open this private, single-use link to finish setting up your account:\n\n${url}\n\nIt expires in 24 hours. Do not forward this message.`,
    html: `<p>This is the requested setup test and first-owner invitation.</p><p><a href="${escapeHtml(url)}">Set up your private workspace account</a></p><p>This private, single-use link expires in 24 hours. Do not forward this message.</p>`
  });
  const controller = new AbortController();
  let timer;
  const timeout = new Promise(resolveTimeout => {
    timer = setTimeout(() => { controller.abort(); resolveTimeout('email_uncertain'); }, EMAIL_TIMEOUT_MS);
  });
  const request = (async () => {
    try {
      const response = await fetchImpl(`https://api.cloudflare.com/client/v4/accounts/${accountId}/email/sending/send`, {
        method: 'POST', redirect: 'manual', cache: 'no-store', signal: controller.signal,
        headers: { authorization: `Bearer ${apiToken}`, 'content-type': 'application/json' }, body
      });
      if (!response.ok) {
        void response.body?.cancel().catch(() => {});
        return [400, 401, 403, 404, 405, 413, 415, 422, 429].includes(response.status) ? 'email_rejected' : 'email_uncertain';
      }
      return deliveryState(await boundedJson(response), prepared.email);
    } catch { return 'email_uncertain'; }
  })();
  try { return await Promise.race([request, timeout]); }
  finally { clearTimeout(timer); controller.abort(); }
}

/** No raw credential, address, provider response or message ID leaves this result. */
export async function bootstrapByEmail(settings, { wranglerConfig, apiToken, executeSql, fetchImpl = globalThis.fetch, now = Math.floor(Date.now() / 1000) } = {}) {
  let selected, prepared;
  try {
    selected = emailSettings(settings, wranglerConfig, apiToken);
    if (typeof executeSql !== 'function' || typeof fetchImpl !== 'function') throw new Error('Invalid executor.');
    prepared = prepareBootstrap(selected.accounts, now);
  } catch { return outcome('bootstrap_invalid'); }
  let result;
  try { result = await executeSql(prepared.sql, prepared.confirmationSql); }
  catch { return outcome('bootstrap_unconfirmed', prepared); }
  try { verifyBootstrapResult(result, prepared); }
  catch {
    const refused = Array.isArray(result) && result.length > 0
      && result.every(value => value?.success === true && Array.isArray(value.results) && value.results.length === 0);
    return outcome(refused ? 'bootstrap_refused' : 'bootstrap_unconfirmed', prepared);
  }
  const status = await sendInvitation(selected, prepared, apiToken, fetchImpl);
  // Keep the invitation even after rejection/uncertainty: reruns must hit the guard.
  return outcome(status, prepared, true, true);
}

async function executeWrangler(settings, sql, mode) {
  if (!/^[A-Za-z0-9][A-Za-z0-9_-]{0,62}$/.test(settings.database ?? '') || !settings.wrangler || !settings.wranglerConfig
    || !['--local', '--remote'].includes(mode)) throw new Error('Supply the exact database and installed Wrangler/config paths.');
  const args = [resolve(settings.wrangler), 'd1', 'execute', settings.database, mode, '--config', resolve(settings.wranglerConfig), '--command', sql, '--json', '--yes'];
  return new Promise((accept, reject) => {
    const environment = { ...process.env };
    delete environment.BOOTSTRAP_EMAIL_API_TOKEN;
    // D1 prints --json through logger.log; lower inherited levels suppress it.
    environment.WRANGLER_LOG = 'log';
    const child = spawn(process.execPath, args, { env: environment, shell: false, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
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

/** Short commands avoid --file import progress on stdout; confirmation stays separate. */
export async function executeBootstrapSql(settings, sql, confirmationSql, mode, { runWrangler = executeWrangler } = {}) {
  // SQL contains the digest and fixed account metadata, never the raw invite token.
  const inserted = await runWrangler(settings, sql, mode);
  if (!Array.isArray(inserted) || !inserted.length || inserted.some(value => value?.success !== true)) {
    throw new Error('Bootstrap insert was not confirmed; inspect the database before retrying.');
  }
  return await runWrangler(settings, confirmationSql, mode);
}

async function loadPrivateJson(path) {
  const bytes = await readFile(resolve(path));
  if (bytes.byteLength > PRIVATE_JSON_LIMIT) throw new Error('Invalid private settings.');
  return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
}

/** Dependency injection is for local tests; the CLI has no alternative-send option. */
export async function runBootstrapCli(args, {
  stdinIsTTY = process.stdin.isTTY, stdoutIsTTY = process.stdout.isTTY, environment = process.env,
  loadJson = loadPrivateJson, executeSql, fetchImpl = globalThis.fetch,
  writeOut = value => process.stdout.write(value), writeError = value => process.stderr.write(value)
} = {}) {
  const email = args.length === 5 && args[4] === '--email';
  if ((!email && args.length !== 4) || args[0] !== '--settings' || !args[1] || args[2] !== '--apply'
    || !['--local', '--remote'].includes(args[3]) || (email && args[3] !== '--remote')) {
    writeError(`${USAGE}\n`); return 1;
  }
  // The old mode still refuses CI/log capture or redirected output.
  if (!email && (!stdoutIsTTY || !stdinIsTTY)) {
    writeError('Run this operator-only command in an interactive terminal; the one-use link must not enter logs.\n'); return 1;
  }
  const apiToken = email ? environment.BOOTSTRAP_EMAIL_API_TOKEN : undefined;
  if (email) delete environment.BOOTSTRAP_EMAIL_API_TOKEN;
  try {
    const settings = await loadJson(args[1]);
    const executor = (sql, confirmationSql) => executeSql ? executeSql(sql, confirmationSql)
      : executeBootstrapSql(settings, sql, confirmationSql, args[3]);
    if (email) {
      const wranglerConfig = await loadJson(settings.wranglerConfig);
      const result = await bootstrapByEmail(settings, { wranglerConfig, apiToken, executeSql: executor, fetchImpl });
      writeOut(`${JSON.stringify(result)}\n`);
      return ['email_delivered', 'email_queued'].includes(result.status) ? 0
        : result.status === 'email_rejected' ? 2 : ['email_uncertain', 'bootstrap_unconfirmed'].includes(result.status) ? 3 : 1;
    }
    const app = appOrigin(settings.APP_ORIGIN);
    const prepared = prepareBootstrap(settings.INVITE_ACCOUNTS);
    verifyBootstrapResult(await executor(prepared.sql, prepared.confirmationSql), prepared);
    app.hash = `invite=${prepared.token}`;
    writeOut(`Owner invitation confirmed. Expires ${new Date(prepared.expiresAt * 1000).toISOString()}.\nKeep the following one-use link private; it is shown once.\n${app.href}\n`);
    return 0;
  } catch {
    // JSON, filesystem and provider exceptions can contain credentials or addresses.
    writeError('Bootstrap could not be confirmed; inspect the private settings and database before retrying.\n');
    return 3;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  process.exitCode = await runBootstrapCli(process.argv.slice(2));
}
