import assert from 'node:assert/strict';
import { test } from 'node:test';
import { D1Harness } from './d1-harness.mjs';
import { prepareBootstrap, verifyBootstrapResult, bootstrapByEmail, runBootstrapCli } from '../scripts/seed-invite.mjs';
import { digestToken } from '../security.mjs';

const accounts = [{email:'owner@example.test',handle:'deandre'},{email:'member@example.test',handle:'ali'}];
const execute = (db, prepared) => {
  db.exec(prepared.sql);
  return [{success:true,results:db.database.prepare('SELECT token_hash,email,handle,role,expires_at FROM invites WHERE token_hash=?').all(prepared.hash).map(row=>({...row}))}];
};
test('bootstrap inserts only an expiring owner invite digest and confirms the exact inserted row', t => {
  const db = new D1Harness(); t.after(()=>db.close());
  const prepared = prepareBootstrap(accounts, 1000);
  assert.equal(prepared.hash, digestToken(prepared.token)); assert.ok(!prepared.sql.includes(prepared.token));
  verifyBootstrapResult(execute(db,prepared),prepared);
  const invite = {...db.database.prepare('SELECT * FROM invites').get()};
  assert.equal(invite.role,'owner'); assert.equal(invite.handle,'deandre'); assert.equal(invite.expires_at,87400); assert.equal(invite.issued_by,null);
  assert.equal(db.database.prepare('SELECT count(*) AS n FROM users').get().n,0);
  const second=prepareBootstrap(accounts,1001);
  assert.throws(()=>verifyBootstrapResult(execute(db,second),second),/refused/);
  assert.equal(db.database.prepare('SELECT count(*) AS n FROM invites').get().n,1);
});
test('expired pending invite can be replaced but an existing account prevents bootstrap', t => {
  const db = new D1Harness(); t.after(()=>db.close());
  execute(db,prepareBootstrap(accounts,1000));
  const replacement=prepareBootstrap(accounts,87401); verifyBootstrapResult(execute(db,replacement),replacement);
  db.database.prepare("INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,created_at,updated_at) VALUES('u','owner@example.test','deandre','Owner','owner','subject','identity@example.test','pbkdf2-sha256',600000,'salt','hash',1,1)").run();
  const afterUser=prepareBootstrap(accounts,180000);
  assert.throws(()=>verifyBootstrapResult(execute(db,afterUser),afterUser),/refused/);
});
test('bootstrap validates the two private mappings and escapes an allowed apostrophe in the address', t => {
  const db = new D1Harness(); t.after(()=>db.close());
  for (const invalid of [[],accounts.slice(0,1),[accounts[0],accounts[0]],[accounts[0],{email:accounts[0].email,handle:'ali'}]]) assert.throws(()=>prepareBootstrap(invalid));
  const prepared=prepareBootstrap([{email:"o'owner@example.test",handle:'deandre'},accounts[1]],1000);
  verifyBootstrapResult(execute(db,prepared),prepared);
  assert.equal(db.database.prepare('SELECT email FROM invites').get().email,"o'owner@example.test");
  assert.throws(()=>verifyBootstrapResult([{success:false}],prepared),/not confirmed/);
});

const apiToken = 'synthetic-oauth-token-never-output';
const settings = {
  INVITE_ACCOUNTS: JSON.stringify(accounts), APP_ORIGIN: 'https://app.example.test',
  CLOUDFLARE_ACCOUNT_ID: 'a'.repeat(32), EMAIL_FROM: 'noreply@example.test',
  database: 'workspace-accounts', wrangler: '/private/wrangler.mjs', wranglerConfig: '/private/deployment.json'
};
const config = {
  account_id: settings.CLOUDFLARE_ACCOUNT_ID,
  vars: { APP_ORIGIN: settings.APP_ORIGIN, INVITE_ACCOUNTS: settings.INVITE_ACCOUNTS, EMAIL_FROM: settings.EMAIL_FROM },
  d1_databases: [{ binding: 'DB', database_name: settings.database, database_id: '12345678-abcd-4321-abcd-123456789abc' }],
  send_email: [{ name: 'EMAIL', allowed_sender_addresses: [settings.EMAIL_FROM] }]
};
const sqlExecutor = db => sql => {
  db.exec(sql);
  const select = sql.slice(sql.lastIndexOf('\nSELECT token_hash') + 1);
  return [{ success: true, results: db.database.prepare(select).all().map(row => ({ ...row })) }];
};
const providerBody = (bucket = 'delivered', recipient = accounts[0].email) => ({
  success: true, errors: [], messages: [], result: {
    delivered: [], queued: [], permanent_bounces: [], suppressed_recipients: [],
    message_id: '<private-provider-id@example.test>', [bucket]: [recipient]
  }
});
const provider = bucket => Response.json(providerBody(bucket));
function setup(t) {
  const db = new D1Harness(); t.after(() => db.close());
  return { db, options: { wranglerConfig: structuredClone(config), apiToken, executeSql: sqlExecutor(db), now: 1000 } };
}
function assertSanitized(value, privateValues = []) {
  const output = typeof value === 'string' ? value : JSON.stringify(value);
  for (const secret of [apiToken, settings.EMAIL_FROM, accounts[0].email, accounts[1].email,
    settings.APP_ORIGIN, 'private-provider-id', ...privateValues]) assert.equal(output.includes(secret), false, `Private value appeared in output: ${secret.slice(0, 3)}`);
  assert.equal(output.includes('#invite='), false);
}

test('email bootstrap inserts and confirms only a digest before exactly one bounded fixed-recipient send', async t => {
  const { db, options } = setup(t);
  let sends = 0, invitation;
  const result = await bootstrapByEmail(settings, { ...options, fetchImpl: async (url, request) => {
    sends++;
    assert.equal(url, `https://api.cloudflare.com/client/v4/accounts/${settings.CLOUDFLARE_ACCOUNT_ID}/email/sending/send`);
    assert.equal(request.method, 'POST'); assert.equal(request.redirect, 'manual'); assert.equal(request.cache, 'no-store');
    assert.equal(request.headers.authorization, `Bearer ${apiToken}`);
    assert.equal(request.signal.aborted, false);
    const body = JSON.parse(request.body);
    assert.deepEqual(body.to, [accounts[0].email]);
    assert.deepEqual(body.from, { address: settings.EMAIL_FROM, name: 'Private workspace' });
    assert.equal(body.cc, undefined); assert.equal(body.bcc, undefined); assert.equal(body.attachments, undefined);
    invitation = new URL(body.text.match(/https:\/\/\S+/)[0]);
    assert.equal(invitation.origin, settings.APP_ORIGIN); assert.equal(invitation.pathname, '/'); assert.equal(invitation.search, '');
    assert.match(invitation.hash, /^#invite=[A-Za-z0-9_-]{43}$/);
    assert.ok(body.html.includes(invitation.href));
    const row = db.database.prepare('SELECT * FROM invites').get();
    assert.equal(row.token_hash, digestToken(invitation.hash.slice('#invite='.length)));
    assert.equal(row.email, accounts[0].email); assert.equal(row.handle, 'deandre'); assert.equal(row.role, 'owner');
    assert.equal(db.database.prepare('SELECT COUNT(*) AS n FROM users').get().n, 0);
    assert.equal(row.expires_at, 87400); assert.equal(row.accepted_at, null);
    return provider('delivered');
  } });
  assert.equal(sends, 1); assert.equal(result.status, 'email_delivered');
  assert.equal(result.invitation_confirmed, true); assert.equal(result.send_attempted, true);
  assert.equal(result.inbox_receipt_verified, false); assert.equal(result.automatic_retry, false);
  assert.equal(result.invite_digest_prefix, db.database.prepare('SELECT token_hash FROM invites').get().token_hash.slice(0, 16));
  assertSanitized(result, [invitation.href, invitation.hash.slice('#invite='.length)]);
});

test('email response states separate recipient-server acceptance, queue, bounce and suppression', async t => {
  for (const [bucket, expected] of [['delivered', 'email_delivered'], ['queued', 'email_queued'], ['permanent_bounces', 'email_rejected'], ['suppressed_recipients', 'email_rejected']]) {
    const { options } = setup(t);
    const result = await bootstrapByEmail(settings, { ...options, fetchImpl: async () => provider(bucket) });
    assert.equal(result.status, expected); assert.equal(result.inbox_receipt_verified, false); assertSanitized(result);
  }
});

test('an active invite or any existing account refuses bootstrap without sending', async t => {
  for (const existing of ['invite', 'account']) {
    const { db, options } = setup(t);
    if (existing === 'invite') execute(db, prepareBootstrap(accounts, 999));
    else db.exec("INSERT INTO users(id,email,handle,display_name,role,access_sub,access_email,password_scheme,password_iterations,password_salt,password_hash,created_at,updated_at) VALUES('u','owner@example.test','deandre','Owner','owner','subject','identity@example.test','pbkdf2-sha256',600000,'salt','hash',1,1)");
    let sent = false;
    const result = await bootstrapByEmail(settings, { ...options, fetchImpl: async () => { sent = true; return provider(); } });
    assert.equal(result.status, 'bootstrap_refused'); assert.equal(sent, false);
    assert.equal(result.send_attempted, false); assert.equal(result.invitation_confirmed, false); assertSanitized(result);
  }
});

test('unconfirmed D1 writes never send, even if SQL may have succeeded', async t => {
  const cases = [undefined, [], [{ success: false, results: [] }], [{ success: true }], [{ success: true, results: [{ token_hash: 'wrong' }] }]];
  for (const confirmation of cases) {
    const { options } = setup(t); let sends = 0;
    const result = await bootstrapByEmail(settings, { ...options, executeSql: async () => confirmation,
      fetchImpl: async () => { sends++; return provider(); } });
    assert.equal(result.status, 'bootstrap_unconfirmed'); assert.equal(sends, 0); assertSanitized(result);
  }
  const { db, options } = setup(t);
  const result = await bootstrapByEmail(settings, { ...options, executeSql: async sql => { sqlExecutor(db)(sql); throw new Error(`${apiToken} private provider diagnostic`); },
    fetchImpl: async () => { assert.fail('Unconfirmed D1 must not send'); } });
  assert.equal(result.status, 'bootstrap_unconfirmed'); assert.equal(db.database.prepare('SELECT COUNT(*) AS n FROM invites').get().n, 1);
  assertSanitized(result, ['private provider diagnostic']);
});

test('ambiguous and rejected sends retain the invitation so reruns cannot resend blindly', async t => {
  for (const reply of ['network', 'server', 'malformed', 'rejected']) {
    const { db, options } = setup(t); let sends = 0;
    const fetchImpl = async () => {
      sends++;
      if (reply === 'network') throw new Error(`Network diagnostics with ${apiToken}`);
      if (reply === 'server') return new Response('Provider private diagnostic', { status: 500 });
      if (reply === 'malformed') return new Response('not JSON');
      return new Response('Provider private diagnostic', { status: 403 });
    };
    const first = await bootstrapByEmail(settings, { ...options, fetchImpl });
    assert.equal(first.status, reply === 'rejected' ? 'email_rejected' : 'email_uncertain');
    assert.equal(first.invitation_confirmed, true); assert.equal(first.send_attempted, true);
    const second = await bootstrapByEmail(settings, { ...options, fetchImpl });
    assert.equal(second.status, 'bootstrap_refused'); assert.equal(sends, 1);
    assert.equal(db.database.prepare('SELECT COUNT(*) AS n FROM invites WHERE accepted_at IS NULL').get().n, 1);
    assertSanitized(first, ['Provider private diagnostic']); assertSanitized(second);
  }
});

test('concurrent bootstrap contenders share the atomic empty-account and active-invite guard', async t => {
  const { db, options } = setup(t); let sends = 0;
  const fetchImpl = async () => { sends++; return provider('queued'); };
  const results = await Promise.all([bootstrapByEmail(settings, { ...options, fetchImpl }), bootstrapByEmail(settings, { ...options, fetchImpl })]);
  assert.deepEqual(results.map(value => value.status).sort(), ['bootstrap_refused', 'email_queued']);
  assert.equal(sends, 1); assert.equal(db.database.prepare('SELECT COUNT(*) AS n FROM invites').get().n, 1);
});

test('missing, malformed, contradictory or unexpected recipient outcomes are uncertain', async t => {
  const wrong = providerBody('delivered', 'other@example.test');
  const duplicate = providerBody(); duplicate.result.queued = [accounts[0].email];
  const repeated = providerBody(); repeated.result.delivered.push(accounts[0].email);
  const malformed = providerBody(); malformed.result.queued = 'not-an-array';
  const missing = providerBody(); delete missing.result.permanent_bounces;
  const conflicting = providerBody(); conflicting.errors = [{ message: apiToken }];
  const empty = providerBody(); empty.result.delivered = [];
  const deniedWithResult = providerBody(); deniedWithResult.success = false;
  for (const body of [wrong, duplicate, repeated, malformed, missing, conflicting, empty, deniedWithResult, {}, null]) {
    const { options } = setup(t);
    const result = await bootstrapByEmail(settings, { ...options, fetchImpl: async () => Response.json(body) });
    assert.equal(result.status, 'email_uncertain'); assertSanitized(result, ['other@example.test']);
  }
});

test('redirects, excessive output, invalid UTF-8 and JSON cannot disclose private provider data', async t => {
  const replies = [
    () => new Response(null, { status: 302, headers: { location: 'https://other.example.test/collect' } }),
    () => new Response('x'.repeat(16385)),
    () => new Response('{}', { headers: { 'content-length': '16385' } }),
    () => new Response(new Uint8Array([0xff, 0xfe])),
    () => new Response(`malformed JSON ${apiToken}`)
  ];
  for (const reply of replies) {
    const { options } = setup(t); let sends = 0;
    const result = await bootstrapByEmail(settings, { ...options, fetchImpl: async (_url, request) => {
      sends++; assert.equal(request.redirect, 'manual'); return reply();
    } });
    assert.equal(result.status, 'email_uncertain'); assert.equal(sends, 1); assertSanitized(result);
  }
  const { options } = setup(t);
  const result = await bootstrapByEmail(settings, { ...options,
    fetchImpl: async () => Response.json({ success: false, result: null, errors: [{ message: apiToken }] }) });
  assert.equal(result.status, 'email_rejected'); assertSanitized(result);
});

test('email mode validates account, sender, origin, mappings, database and selected config before SQL', async () => {
  const invalid = [
    { setting: { CLOUDFLARE_ACCOUNT_ID: '../wrong' } }, { setting: { EMAIL_FROM: 'Name <sender@example.test>' } },
    { setting: { APP_ORIGIN: 'https://app.example.test/' } }, { setting: { APP_ORIGIN: 'http://app.example.test' } },
    { setting: { INVITE_ACCOUNTS: JSON.stringify([accounts[0], accounts[0]]) } }, { setting: { database: 'wrong-db' } },
    { setting: { databaseId: '98765432-abcd-4321-abcd-123456789abc' } },
    { config: { account_id: 'b'.repeat(32) } }, { config: { vars: { ...config.vars, APP_ORIGIN: 'https://different.example.test' } } },
    { config: { vars: { ...config.vars, EMAIL_FROM: 'different@example.test' } } },
    { config: { vars: { ...config.vars, INVITE_ACCOUNTS: JSON.stringify([{ ...accounts[0], email: 'wrong@example.test' }, accounts[1]]) } } },
    { config: { d1_databases: [{ ...config.d1_databases[0], binding: 'OTHER' }] } },
    { config: { d1_databases: [{ ...config.d1_databases[0], database_id: '00000000-0000-0000-0000-000000000000' }] } },
    { config: { d1_databases: [...config.d1_databases, config.d1_databases[0]] } },
    { config: { send_email: [{ name: 'EMAIL', allowed_sender_addresses: [settings.EMAIL_FROM, 'other@example.test'] }] } },
    { token: '' }, { token: 'token-with-newline\n' }, { token: 'x'.repeat(4097) }
  ];
  for (const value of invalid) {
    const result = await bootstrapByEmail({ ...settings, ...value.setting }, {
      wranglerConfig: { ...config, ...value.config }, apiToken: value.token ?? apiToken,
      executeSql: async () => assert.fail('Invalid config must not write'), fetchImpl: async () => assert.fail('Invalid config must not send')
    });
    assert.equal(result.status, 'bootstrap_invalid'); assert.equal(result.send_attempted, false); assertSanitized(result);
  }
});

test('send timeout is bounded, aborts the sole request and keeps the guard active', async t => {
  const { db, options } = setup(t);
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let started; const ready = new Promise(resolve => { started = resolve; });
  let signal;
  const pending = bootstrapByEmail(settings, { ...options, fetchImpl: (_url, request) => {
    signal = request.signal; started(); return new Promise(() => {});
  } });
  await ready; t.mock.timers.tick(30000);
  const result = await pending;
  assert.equal(result.status, 'email_uncertain'); assert.equal(signal.aborted, true);
  assert.equal(db.database.prepare('SELECT COUNT(*) AS n FROM invites WHERE accepted_at IS NULL').get().n, 1);
});

test('CLI preserves the interactive link guard and permits --email only with explicit --remote', async () => {
  const failures = [
    ['--settings', '/private/settings.json', '--apply', '--remote'],
    ['--settings', '/private/settings.json', '--apply', '--local'],
    ['--settings', '/private/settings.json', '--apply', '--local', '--email'],
    ['--settings', '/private/settings.json', '--apply', '--remote', '--unknown']
  ];
  for (const args of failures) {
    let output = '';
    const code = await runBootstrapCli(args, { stdinIsTTY: false, stdoutIsTTY: false,
      loadJson: async () => assert.fail('Must refuse before reading private settings'), writeOut: value => { output += value; }, writeError: value => { output += value; } });
    assert.equal(code, 1); assertSanitized(output);
  }
});

test('noninteractive email CLI captures then removes the token and outputs sanitized status only', async t => {
  const { db } = setup(t); let output = '', error = '', sentToken, link;
  const environment = { BOOTSTRAP_EMAIL_API_TOKEN: apiToken };
  const code = await runBootstrapCli(['--settings', '/private/settings.json', '--apply', '--remote', '--email'], {
    stdinIsTTY: false, stdoutIsTTY: false, environment,
    loadJson: async path => {
      assert.equal(environment.BOOTSTRAP_EMAIL_API_TOKEN, undefined);
      return path === '/private/settings.json' ? settings : config;
    }, executeSql: sqlExecutor(db),
    fetchImpl: async (_url, request) => {
      sentToken = request.headers.authorization;
      link = JSON.parse(request.body).text.match(/https:\/\/\S+/)[0];
      return provider('queued');
    }, writeOut: value => { output += value; }, writeError: value => { error += value; }
  });
  assert.equal(code, 0); assert.equal(error, ''); assert.equal(sentToken, `Bearer ${apiToken}`);
  assert.equal(JSON.parse(output).status, 'email_queued'); assertSanitized(output, [link]);
});

test('CLI failures never echo filesystem, JSON, SQL or provider exception messages', async () => {
  let output = '';
  const environment = { BOOTSTRAP_EMAIL_API_TOKEN: apiToken };
  const code = await runBootstrapCli(['--settings', '/private/settings.json', '--apply', '--remote', '--email'], {
    environment, loadJson: async () => { throw new Error(`${apiToken} ${accounts[0].email} https://app.example.test/#invite=raw-secret`); },
    writeOut: value => { output += value; }, writeError: value => { output += value; }
  });
  assert.equal(code, 3); assert.equal(environment.BOOTSTRAP_EMAIL_API_TOKEN, undefined); assertSanitized(output, ['raw-secret']);
});

test('original interactive CLI still shows the confirmed one-use link only to its TTY', async t => {
  const { db } = setup(t); let output = '';
  const code = await runBootstrapCli(['--settings', '/private/settings.json', '--apply', '--local'], {
    stdinIsTTY: true, stdoutIsTTY: true, loadJson: async () => settings,
    executeSql: sqlExecutor(db), writeOut: value => { output += value; }, writeError: () => assert.fail('Unexpected failure')
  });
  assert.equal(code, 0); assert.match(output, /Owner invitation confirmed/);
  const link = new URL(output.match(/https:\/\/\S+/)[0]);
  assert.equal(digestToken(link.hash.slice('#invite='.length)), db.database.prepare('SELECT token_hash FROM invites').get().token_hash);
  assert.equal((output.match(/#invite=/g) ?? []).length, 1);
});
