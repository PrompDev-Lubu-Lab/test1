'use strict';

const fs = require('node:fs/promises');
const pathTools = require('node:path');
const { createHash, randomBytes } = require('node:crypto');
const { spawn } = require('node:child_process');

const TIMEOUT_MS = 15000, OUTPUT_BYTES = 16384, MAX_FILE_BYTES = 2n * 1024n * 1024n * 1024n;
const DENIED = 'Installer signature verification failed. Installation is disabled.';
const THUMBPRINT = /^[A-F0-9]{40}$/;
// Fixed code only. Neither the path nor publisher configuration enters shell code.
const SCRIPT = String.raw`
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$PSModuleAutoLoadingPreference = 'None'
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false, $true)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$stream = $null
$hasher = $null
$releaseHasher = $null
try {
  Import-Module ($PSHOME + '\Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
  Import-Module ($PSHOME + '\Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
  $request = Microsoft.PowerShell.Utility\ConvertFrom-Json -InputObject ([Console]::In.ReadToEnd())
  $target = [System.IO.Path]::GetFullPath([string]$request.path)
  $drive = [System.IO.DriveInfo]::new([System.IO.Path]::GetPathRoot($target))
  if ($drive.DriveType -ne [System.IO.DriveType]::Fixed) { throw 'denied' }
  $item = [System.IO.FileInfo]::new($target)
  if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or ($item.Attributes -band [System.IO.FileAttributes]::Directory) -ne 0) { throw 'denied' }
  $directory = $item.Directory
  while ($null -ne $directory) {
    if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'denied' }
    $directory = $directory.Parent
  }
  $stream = [System.IO.File]::Open($target, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
  $signature = Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $target -ErrorAction Stop
  if ($signature.Status.ToString() -ne 'Valid' -or $null -eq $signature.SignerCertificate -or $null -eq $signature.TimeStamperCertificate) { throw 'denied' }
  $hasher = [System.Security.Cryptography.SHA256]::Create()
  $digest = [System.BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
  $stream.Position = 0
  $releaseHasher = [System.Security.Cryptography.SHA512]::Create()
  $releaseDigest = [System.Convert]::ToBase64String($releaseHasher.ComputeHash($stream))
  $result = @{
    version = 1; nonce = [string]$request.nonce; path = $target; status = $signature.Status.ToString()
    publisher = $signature.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    thumbprint = $signature.SignerCertificate.Thumbprint
    timestampThumbprint = $signature.TimeStamperCertificate.Thumbprint
    sha256 = $digest; sha512 = $releaseDigest; size = [string]$stream.Length
  }
  [Console]::Write((Microsoft.PowerShell.Utility\ConvertTo-Json -InputObject $result -Compress))
} catch {
  [Console]::Write('{"error":"verification_failed"}')
  exit 1
} finally {
  if ($null -ne $hasher) { $hasher.Dispose() }
  if ($null -ne $releaseHasher) { $releaseHasher.Dispose() }
  if ($null -ne $stream) { $stream.Dispose() }
}
`;
const ARGUMENTS = Object.freeze(['-NoLogo', '-NoProfile', '-NonInteractive', '-InputFormat', 'Text', '-OutputFormat', 'Text',
  '-EncodedCommand', Buffer.from(SCRIPT, 'utf16le').toString('base64')]);

function reject() { throw new Error(DENIED); }
function checkSignal(signal) { if (signal.aborted) reject(); }
function canonicalSha512(value) {
  return typeof value === 'string' && /^[A-Za-z0-9+/]{86}==$/.test(value)
    && Buffer.from(value, 'base64').toString('base64') === value;
}
function thumbprint(value) {
  if (typeof value !== 'string' || !/^[A-Fa-f0-9 ]{40,80}$/.test(value)) return null;
  const normalized = value.replace(/ /g, '').toUpperCase();
  return THUMBPRINT.test(normalized) ? normalized : null;
}
function samePath(left, right) { return process.platform === 'win32' ? left.toLowerCase() === right.toLowerCase() : left === right; }
function localAbsolute(value) {
  if (typeof value !== 'string' || value.length > 32767 || !value.isWellFormed() || /[\u0000-\u001f\u007f]/.test(value) || !pathTools.isAbsolute(value)) reject();
  if (process.platform === 'win32') {
    if (!/^[A-Za-z]:[\\/]/.test(value) || /[:<>"|?*]/.test(value.slice(2)) || value.split(/[\\/]/).slice(1).some(part => /[. ]$/.test(part))) reject();
  }
  return pathTools.resolve(value);
}
function contained(root, file) {
  const relative = pathTools.relative(root, file);
  return Boolean(relative) && !pathTools.isAbsolute(relative) && relative !== '..' && !relative.startsWith(`..${pathTools.sep}`);
}
function identity(stat, file = false) {
  if (stat.isSymbolicLink() || (file ? !stat.isFile() || stat.nlink !== 1n || stat.size <= 0n || stat.size > MAX_FILE_BYTES : !stat.isDirectory()) || stat.ino === 0n) reject();
  return file ? [stat.dev, stat.ino, stat.nlink, stat.size, stat.mtimeNs, stat.ctimeNs, stat.birthtimeNs].join(':') : `${stat.dev}:${stat.ino}`;
}
async function snapshot(root, file, signal) {
  checkSignal(signal);
  const realRoot = await fs.realpath(root), realFile = await fs.realpath(file);
  if (!samePath(root, realRoot) || !samePath(file, realFile) || !contained(realRoot, realFile)) reject();
  const fileStat = await fs.lstat(file, { bigint: true });
  const result = { file: identity(fileStat, true), size: fileStat.size.toString(), directories: [] };
  let directory = pathTools.dirname(file);
  for (let count = 0;; count++) {
    if (count > 128) reject();
    result.directories.push([directory, identity(await fs.lstat(directory, { bigint: true }))]);
    if (samePath(directory, root)) break;
    const parent = pathTools.dirname(directory);
    if (parent === directory) reject();
    directory = parent;
  }
  checkSignal(signal);
  return result;
}
async function hashFile(handle, signal) {
  checkSignal(signal);
  const sha256 = createHash('sha256'), sha512 = createHash('sha512');
  for await (const bytes of handle.createReadStream({ autoClose: false, start: 0, signal })) {
    sha256.update(bytes); sha512.update(bytes);
  }
  checkSignal(signal);
  return { sha256: sha256.digest('hex'), sha512: sha512.digest('base64') };
}
function abortable(work, signal) {
  return new Promise((resolve, rejectPromise) => {
    const abort = () => rejectPromise(new Error(DENIED));
    if (signal.aborted) { abort(); return; }
    signal.addEventListener('abort', abort, { once: true });
    Promise.resolve(work).then(resolve, rejectPromise).finally(() => signal.removeEventListener('abort', abort));
  });
}
function executeSystem({ executable, args, input, signal }) {
  return new Promise((resolve, rejectPromise) => {
    let child, finished = false, size = 0;
    const stdout = [], stderr = [];
    const finish = (error, result) => {
      if (finished) return;
      finished = true; signal.removeEventListener('abort', abort);
      if (error) { try { child?.kill(); } catch {} rejectPromise(new Error(DENIED)); }
      else resolve(result);
    };
    const abort = () => finish(new Error(DENIED));
    if (signal.aborted) { abort(); return; }
    try { child = spawn(executable, args, { windowsHide: true, shell: false, stdio: ['pipe', 'pipe', 'pipe'] }); }
    catch { abort(); return; }
    signal.addEventListener('abort', abort, { once: true });
    for (const [stream, target] of [[child.stdout, stdout], [child.stderr, stderr]]) {
      stream.on('data', bytes => {
        if (finished) return;
        size += bytes.byteLength;
        if (size > OUTPUT_BYTES) abort(); else target.push(bytes);
      });
      stream.on('error', abort);
    }
    child.on('error', abort);
    child.on('close', (code, closedSignal) => finish(closedSignal ? new Error(DENIED) : null,
      { code, stdout: Buffer.concat(stdout), stderr: Buffer.concat(stderr) }));
    child.stdin.on('error', abort);
    child.stdin.end(Buffer.from(input, 'utf8'));
  });
}
function outputBytes(value) {
  if (typeof value === 'string') return Buffer.from(value, 'utf8');
  if (Buffer.isBuffer(value) || value instanceof Uint8Array) return Buffer.from(value);
  reject();
}
function parseReport(bytes) {
  const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  const result = JSON.parse(text), keys = new Set();
  // The fixed script emits a flat object. Duplicate keys are malformed output,
  // even if JSON.parse would otherwise retain a later, more permissive value.
  for (const token of text.matchAll(/"(?:[^"\\]|\\.)*"/gs)) {
    if (!/^\s*:/.test(text.slice(token.index + token[0].length))) continue;
    const key = JSON.parse(token[0]);
    if (keys.has(key)) reject();
    keys.add(key);
  }
  return result;
}

/** No fallback: only null permits installation. Expected digest/size bind a release; execute is a test seam. */
async function verifyWindowsInstaller(options = {}) {
  let handle, timeout;
  const controller = new AbortController();
  try {
    if (!options || typeof options !== 'object' || Array.isArray(options)) reject();
    const { path, publisherNames, certificateThumbprints, allowedRoot, expectedSha512, expectedSize, execute } = options;
    if (execute !== undefined && typeof execute !== 'function') reject();
    if (!execute && process.platform !== 'win32') reject();
    if (expectedSha512 !== undefined && !canonicalSha512(expectedSha512)) reject();
    if (expectedSize !== undefined && (!Number.isSafeInteger(expectedSize) || expectedSize <= 0 || expectedSize > 1073741824)) reject();
    if (!Array.isArray(publisherNames) || !publisherNames.length || publisherNames.length > 10
      || publisherNames.some(name => typeof name !== 'string' || !name.isWellFormed() || !name.trim() || name !== name.trim() || name.length > 256 || /[\p{Cc}\p{Cf}]/u.test(name))) reject();
    if (!Array.isArray(certificateThumbprints) || !certificateThumbprints.length || certificateThumbprints.length > 10) reject();
    const approved = certificateThumbprints.map(thumbprint);
    if (approved.some(value => value === null)) reject();
    const root = localAbsolute(allowedRoot), file = localAbsolute(path);
    if (!contained(root, file) || !/\.(exe|msi)$/i.test(file)) reject();
    timeout = setTimeout(() => controller.abort(), TIMEOUT_MS);
    const before = await snapshot(root, file, controller.signal);
    if (expectedSize !== undefined && before.size !== String(expectedSize)) reject();
    handle = await fs.open(file, 'r');
    if (identity(await handle.stat({ bigint: true }), true) !== before.file) reject();
    const hashes = await hashFile(handle, controller.signal);
    if (expectedSha512 !== undefined && hashes.sha512 !== expectedSha512) reject();
    if (JSON.stringify(await snapshot(root, file, controller.signal)) !== JSON.stringify(before)) reject();
    const nonce = randomBytes(24).toString('hex');
    const systemRoot = process.env.SystemRoot;
    if (!execute && (typeof systemRoot !== 'string' || !/^[A-Za-z]:\\Windows$/i.test(systemRoot))) reject();
    const executable = pathTools.win32.join(systemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
    checkSignal(controller.signal);
    const result = await abortable((execute ?? executeSystem)({ executable, args: [...ARGUMENTS],
      input: JSON.stringify({ path: file, nonce }), timeoutMs: TIMEOUT_MS, maxOutputBytes: OUTPUT_BYTES,
      windowsHide: true, shell: false, signal: controller.signal }), controller.signal);
    if (!result || result.code !== 0 || Object.keys(result).some(key => !['code', 'stdout', 'stderr'].includes(key))) reject();
    const stdout = outputBytes(result.stdout), stderr = outputBytes(result.stderr);
    if (!stdout.length || stdout.byteLength + stderr.byteLength > OUTPUT_BYTES || stderr.toString('utf8').trim()) reject();
    const report = parseReport(stdout);
    const keys = ['version', 'nonce', 'path', 'status', 'publisher', 'thumbprint', 'timestampThumbprint', 'sha256', 'sha512', 'size'];
    if (!report || typeof report !== 'object' || Array.isArray(report) || Object.keys(report).length !== keys.length
      || Object.keys(report).some(key => !keys.includes(key)) || report.version !== 1 || report.nonce !== nonce
      || typeof report.path !== 'string' || !samePath(report.path, file) || report.status !== 'Valid'
      || !publisherNames.includes(report.publisher) || !approved.includes(thumbprint(report.thumbprint))
      || !THUMBPRINT.test(report.timestampThumbprint ?? '') || report.sha256 !== hashes.sha256
      || report.sha512 !== hashes.sha512 || report.size !== before.size) reject();
    const afterHashes = await hashFile(handle, controller.signal);
    const afterStat = await handle.stat({ bigint: true });
    if (afterHashes.sha256 !== hashes.sha256 || afterHashes.sha512 !== hashes.sha512
      || (expectedSha512 !== undefined && afterHashes.sha512 !== expectedSha512)
      || (expectedSize !== undefined && afterStat.size !== BigInt(expectedSize))
      || identity(afterStat, true) !== before.file
      || JSON.stringify(await snapshot(root, file, controller.signal)) !== JSON.stringify(before)) reject();
    checkSignal(controller.signal);
    await handle.close(); handle = null;
    checkSignal(controller.signal);
    return null;
  } catch { return DENIED; }
  finally {
    clearTimeout(timeout);
    try { await handle?.close(); } catch { /* A failed verification never authorizes installation. */ }
  }
}

module.exports = { verifyWindowsInstaller };
