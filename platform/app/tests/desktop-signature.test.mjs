import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import signature from '../desktop/signature.cjs';

const { verifyWindowsInstaller } = signature;
const publisherNames = ['Example Publisher'];
const certificateThumbprints = ['A'.repeat(40)];
const sha512 = bytes => createHash('sha512').update(bytes).digest('base64');
async function fixture(t, filename = 'setup.exe') {
  const temporary = await fs.realpath(os.tmpdir());
  const top = await fs.mkdtemp(path.join(temporary, 'platform-signature-'));
  const root = path.join(top, 'cache');
  await fs.mkdir(root);
  const file = path.join(root, filename);
  await fs.writeFile(file, Buffer.from('MZ synthetic unsigned installer fixture; never executable.'));
  t.after(async () => {
    const resolved = await fs.realpath(top);
    if (!resolved.startsWith(temporary + path.sep) || !path.basename(resolved).startsWith('platform-signature-')) throw new Error('Unsafe test cleanup target');
    await fs.rm(resolved, { recursive: true, force: true });
  });
  return { top, root, file, config: { path: file, allowedRoot: root, publisherNames, certificateThumbprints } };
}
async function report(request, extra = {}) {
  const input = JSON.parse(request.input);
  const content = await fs.readFile(input.path);
  return { version: 1, nonce: input.nonce, path: input.path, status: 'Valid', publisher: publisherNames[0],
    thumbprint: certificateThumbprints[0], timestampThumbprint: 'B'.repeat(40),
    sha256: createHash('sha256').update(content).digest('hex'), sha512: sha512(content), size: String(content.byteLength), ...extra };
}
const successful = async request => ({ code: 0, stdout: JSON.stringify(await report(request)), stderr: '' });
async function denied(config) {
  const result = await verifyWindowsInstaller(config);
  assert.equal(typeof result, 'string');
  assert.ok(result.length < 200);
  if (typeof config?.path === 'string' && config.path) assert.equal(result.includes(config.path), false);
  return result;
}

test('valid matching attestation permits only the exact contained file and normalized approved thumbprint', async t => {
  const { config } = await fixture(t);
  assert.equal(await verifyWindowsInstaller({ ...config, certificateThumbprints: ['aa '.repeat(20).trim()], execute: successful }), null);
});

test('the expected release digest is optional but must match the attested held-file snapshot when supplied', async t => {
  const { file, config } = await fixture(t);
  const content = await fs.readFile(file), expectedSha512 = sha512(content);
  assert.equal(await verifyWindowsInstaller({ ...config, expectedSha512, expectedSize: content.byteLength, execute: successful }), null);
  await denied({ ...config, expectedSha512, execute: async request => ({
    code: 0, stdout: JSON.stringify(await report(request, { sha512: sha512('another approved installer') })), stderr: ''
  }) });
});

test('a supplied release size must be a matching positive integer of at most one GiB', async t => {
  const { file, config } = await fixture(t);
  const size = (await fs.stat(file)).size;
  let calls = 0;
  const execute = async request => { calls++; return successful(request); };
  for (const expectedSize of [null, false, 0, -1, 1.5, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1,
    1073741825, String(size), [], {}, size - 1, size + 1]) await denied({ ...config, expectedSize, execute });
  assert.equal(calls, 0, 'malformed and mismatching sizes are refused before native attestation');
  assert.equal(await verifyWindowsInstaller({ ...config, expectedSize: size, execute }), null);
  assert.equal(calls, 1);
});

test('a supplied release digest must be canonical SHA-512 base64 before any verifier is invoked', async t => {
  const { file, config } = await fixture(t);
  const digest = sha512(await fs.readFile(file));
  let calls = 0;
  const execute = async request => { calls++; return successful(request); };
  for (const expectedSha512 of [null, false, 512, '', 'f'.repeat(128), digest.slice(0, -2), digest + '\n',
    digest.slice(0, -2) + '=', 'A'.repeat(85) + 'B==']) await denied({ ...config, expectedSha512, execute });
  assert.equal(calls, 0);
});

test('another approved signed artifact cannot replace the expected release between verifier calls', async t => {
  const { file, config } = await fixture(t);
  const expectedSha512 = sha512(await fs.readFile(file));
  assert.equal(await verifyWindowsInstaller({ ...config, expectedSha512, execute: successful }), null);
  await fs.writeFile(file, 'MZ another release with a synthetically approved signature.');
  assert.equal(await verifyWindowsInstaller({ ...config, execute: successful }), null);
  let calls = 0;
  await denied({ ...config, expectedSha512, execute: async request => { calls++; return successful(request); } });
  assert.equal(calls, 0, 'a different release is rejected before native attestation');
});

test('the expected release cannot change during attestation even with an approved signer report', async t => {
  for (const reportChangedFile of [false, true]) {
    const { file, config } = await fixture(t);
    const content = await fs.readFile(file), expectedSha512 = sha512(content);
    await denied({ ...config, expectedSha512, expectedSize: content.byteLength, execute: async request => {
      const original = await report(request);
      await fs.writeFile(file, 'MZ another synthetically signed release during attestation.');
      return { code: 0, stdout: JSON.stringify(reportChangedFile ? await report(request) : original), stderr: '' };
    } });
  }
});

test('the fixed verifier command carries a hostile-looking filename only through stdin JSON', async t => {
  const { config } = await fixture(t, "O'Reilly $([never-run]) `literal [x].exe");
  let calls = 0;
  const execute = async request => {
    calls++;
    assert.equal(request.windowsHide, true);
    assert.equal(request.shell, false);
    assert.equal(request.timeoutMs, 30000);
    assert.equal(request.maxOutputBytes, 16384);
    assert.ok(request.signal instanceof AbortSignal);
    assert.match(request.executable, /^[A-Za-z]:\\Windows\\System32\\WindowsPowerShell\\v1\.0\\powershell\.exe$/i);
    assert.ok(request.args.includes('-NoProfile'));
    assert.ok(request.args.includes('-NonInteractive'));
    assert.ok(!request.args.some(value => value.includes(config.path)));
    const script = Buffer.from(request.args.at(-1), 'base64').toString('utf16le');
    assert.ok(!script.includes(config.path));
    assert.match(script, /Get-AuthenticodeSignature -LiteralPath \$target/);
    assert.match(script, /FileShare\]::Read/);
    assert.match(script, /SHA512\]::Create\(\)/);
    assert.match(script, /TimeStamperCertificate/);
    assert.match(script, /X509NameType\]::SimpleName, \$false/);
    assert.equal(JSON.parse(request.input).path, config.path);
    return successful(request);
  };
  assert.equal(await verifyWindowsInstaller({ ...config, execute }), null);
  assert.equal(calls, 1);
});

test('missing or malformed policy never starts a verifier or permits a fallback', async t => {
  const { config } = await fixture(t);
  let calls = 0;
  const execute = () => { calls++; return null; };
  for (const changes of [{ publisherNames: [] }, { publisherNames: null }, { publisherNames: [' '] },
    { publisherNames: [' Example Publisher'] }, { publisherNames: ['bad\u0000name'] },
    { certificateThumbprints: [] }, { certificateThumbprints: ['not a thumbprint'] },
    { certificateThumbprints: ['A'.repeat(64)] }, { allowedRoot: '' }, { path: '' },
    { path: 'relative.exe' }, { path: 'https://untrusted.example.test/setup.exe' }]) {
    await denied({ ...config, execute, ...changes });
  }
  await denied(null);
  await denied({ ...config, execute: true });
  assert.equal(calls, 0);
});

test('outside and prefix-sibling paths, missing files, directories and empty files fail closed', async t => {
  const { top, root, config } = await fixture(t);
  const outside = path.join(top, 'outside.exe');
  const sibling = root + '-other';
  await fs.mkdir(sibling);
  await fs.writeFile(outside, 'unsigned');
  await fs.writeFile(path.join(sibling, 'setup.exe'), 'unsigned');
  await fs.writeFile(path.join(root, 'empty.exe'), '');
  let calls = 0;
  const execute = () => { calls++; return null; };
  for (const value of [outside, path.join(sibling, 'setup.exe'), path.join(root, 'missing.exe'), root, path.join(root, 'empty.exe')]) {
    await denied({ ...config, path: value, execute });
  }
  await denied({ ...config, allowedRoot: config.path, execute });
  assert.equal(calls, 0);
});

test('hard-linked installers are refused before verification', async t => {
  const { root, file, config } = await fixture(t);
  await fs.link(file, path.join(root, 'second.exe'));
  let calls = 0;
  await denied({ ...config, execute: () => { calls++; return null; } });
  assert.equal(calls, 0);
});

test('a symlink installer is refused before verification', async t => {
  const { root, file, config } = await fixture(t);
  const link = path.join(root, 'linked.exe');
  try { await fs.symlink(file, link, 'file'); }
  catch (error) { if (['EPERM', 'EACCES'].includes(error.code)) { t.skip('File symlink creation requires Windows permission'); return; } throw error; }
  let calls = 0;
  await denied({ ...config, path: link, execute: () => { calls++; return null; } });
  assert.equal(calls, 0);
});

test('a directory junction cannot escape or substitute for the configured root', async t => {
  const { top, root, file, config } = await fixture(t);
  const elsewhere = path.join(top, 'elsewhere');
  await fs.mkdir(elsewhere);
  await fs.writeFile(path.join(elsewhere, 'setup.exe'), 'outside fixture');
  const junction = path.join(root, 'linked');
  await fs.symlink(elsewhere, junction, process.platform === 'win32' ? 'junction' : 'dir');
  await denied({ ...config, path: path.join(junction, 'setup.exe'), execute: successful });
  const aliasedRoot = path.join(top, 'root-alias');
  await fs.symlink(root, aliasedRoot, process.platform === 'win32' ? 'junction' : 'dir');
  await denied({ ...config, allowedRoot: aliasedRoot, path: path.join(aliasedRoot, path.basename(file)), execute: successful });
});

test('invalid Authenticode status, absent timestamp, publisher mismatch and thumbprint mismatch deny installation', async t => {
  const { config } = await fixture(t);
  for (const changed of [{ status: 'NotSigned' }, { status: 'UnknownError' }, { status: 'HashMismatch' }, { status: 0 },
    { timestampThumbprint: null }, { timestampThumbprint: '' }, { timestampThumbprint: 'not a certificate' },
    { publisher: 'Other Publisher' }, { publisher: 'example publisher' }, { publisher: null },
    { thumbprint: 'C'.repeat(40) }, { thumbprint: null }, { sha256: 'f'.repeat(64) }, { sha512: null }, { size: 1 }]) {
    await denied({ ...config, execute: async request => ({ code: 0, stdout: JSON.stringify(await report(request, changed)), stderr: '' }) });
  }
});

test('nonce, exact returned path and output schema are mandatory', async t => {
  const { config } = await fixture(t);
  for (const changed of [{ nonce: 'wrong' }, { path: config.path + '.other' }, { version: 2 }, { extra: true }, { size: '00057' }]) {
    await denied({ ...config, execute: async request => ({ code: 0, stdout: JSON.stringify(await report(request, changed)), stderr: '' }) });
  }
  await denied({ ...config, execute: async request => {
    const value = await report(request); delete value.timestampThumbprint;
    return { code: 0, stdout: JSON.stringify(value), stderr: '' };
  } });
});

test('malformed JSON, duplicate keys, non-UTF8 output, oversized output and stderr all fail closed', async t => {
  const { config } = await fixture(t);
  const corruptions = [() => '{broken', () => 'null', () => '[]', () => '', () => Buffer.from([0xff, 0xfe]),
    () => ' '.repeat(16385), value => '{"status":"NotSigned",' + JSON.stringify(value).slice(1)];
  for (const corrupt of corruptions) {
    await denied({ ...config, execute: async request => ({ code: 0, stdout: corrupt(await report(request)), stderr: '' }) });
  }
  await denied({ ...config, execute: async request => ({ code: 0, stdout: JSON.stringify(await report(request)), stderr: 'Verifier warning' }) });
  await denied({ ...config, execute: async request => ({ code: 0, stdout: JSON.stringify(await report(request)), stderr: ' '.repeat(16384) }) });
});

test('missing tools, nonzero exit status and malformed executor outcomes never use a fallback', async t => {
  const { config } = await fixture(t);
  for (const execute of [async () => { throw new Error('ENOENT private verifier details'); }, async () => null,
    async request => ({ code: 1, stdout: JSON.stringify(await report(request)), stderr: '' }),
    async request => ({ code: 0, stdout: JSON.stringify(await report(request)), stderr: '', timedOut: true }),
    async () => ({ code: 0, stdout: { status: 'Valid' }, stderr: '' })]) await denied({ ...config, execute });
});

test('an executor that hangs is aborted at thirty seconds and the file handle is released', async t => {
  const { config, file } = await fixture(t);
  let signal, started;
  const entered = new Promise(resolve => { started = resolve; });
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const result = verifyWindowsInstaller({ ...config, execute: request => {
    signal = request.signal; started(); return new Promise(() => {});
  } });
  await entered;
  t.mock.timers.tick(29999);
  assert.equal(signal.aborted, false);
  t.mock.timers.tick(1);
  assert.equal(typeof await result, 'string');
  assert.equal(signal.aborted, true);
  t.mock.timers.reset();
  await fs.unlink(file);
});

test('file replacement, in-place modification and removal during verification are denied', async t => {
  for (const mutation of [async file => { await fs.rename(file, file + '.old'); await fs.writeFile(file, 'replacement installer'); },
    async file => { await fs.writeFile(file, 'MZ altered bytes of the same untrusted installer fixture.'); },
    async file => { await fs.unlink(file); }]) {
    const { file, config } = await fixture(t);
    await denied({ ...config, execute: async request => {
      const original = await report(request); await mutation(file);
      return { code: 0, stdout: JSON.stringify(original), stderr: '' };
    } });
  }
});

test('a cache directory replacement is rejected even when the replacement bytes match', async t => {
  const { root, file, config } = await fixture(t);
  await denied({ ...config, execute: async request => {
    const original = await report(request), bytes = await fs.readFile(file);
    await fs.rename(root, root + '.old'); await fs.mkdir(root); await fs.writeFile(file, bytes);
    return { code: 0, stdout: JSON.stringify(original), stderr: '' };
  } });
});

test('every invocation rechecks the installer; a previous valid result is never cached', async t => {
  const { config } = await fixture(t);
  let calls = 0;
  const execute = async request => ({ code: 0, stdout: JSON.stringify(await report(request, { status: ++calls === 1 ? 'Valid' : 'HashMismatch' })), stderr: '' });
  assert.equal(await verifyWindowsInstaller({ ...config, execute }), null);
  await denied({ ...config, execute });
  assert.equal(calls, 2);
});

test('Windows native verification denies the deliberately unsigned local fixture', { skip: process.platform !== 'win32' }, async t => {
  const { config } = await fixture(t);
  await denied(config);
});
