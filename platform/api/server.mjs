import http from 'node:http';
import { constants } from 'node:fs';
import { open, realpath, lstat, readdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer, WebSocket } from 'ws';

const ID = /^[A-Za-z0-9][A-Za-z0-9_.-]{0,159}$/;
const JSON_FILES = { status: 'status.json', state: 'state.json', drift: 'drift.json', validation: 'validation.json' };
const CSV_FILES = { equity: 'equity.csv', fills: 'fills.csv', orders: 'orders.csv', metrics: 'metrics.csv', round_trips: 'round_trips.csv' };
const TEXT_FILES = { report: 'report.txt', revalidation: 'revalidation.txt', 'metrics.prom': 'metrics.prom' };
const MAX_FILE = 16 * 1024 * 1024;
const MAX_ROWS = 10000;
class ApiError extends Error {
  constructor(status, code, detail) { super(detail); Object.assign(this, { status, code }); }
}
const fail = (status, code, detail) => { throw new ApiError(status, code, detail); };
const rawJson = text => { try { JSON.parse(text); return text; } catch { return fail(503, 'source_incomplete', 'The source file is not complete JSON. Retry shortly.'); } };
const json = value => JSON.stringify(value);
const isWithin = (base, candidate) => { const relative = path.relative(base, candidate); return relative !== '' && relative !== '..' && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative); };

export function heartbeatFrom(text, now = Date.now()) {
  const match = /^(\S+)\s+([^\r\n]+)\s*$/.exec(text.trim());
  if (!match || !Number.isFinite(Date.parse(match[1]))) return fail(503, 'source_incomplete', 'The heartbeat is not a valid timestamp and status.');
  return { time: match[1], status: match[2], age_seconds: Math.max(0, (now - Date.parse(match[1])) / 1000) };
}

// CSV quoting is parsed without splitting quoted commas/newlines; values remain exact strings.
export function parseCsv(text) {
  const records = []; let row = [], field = '', quoted = false, closed = false;
  for (let i = 0; i < text.length; i++) {
    const char = text[i];
    if (quoted) {
      if (char === '"' && text[i + 1] === '"') { field += '"'; i++; }
      else if (char === '"') { quoted = false; closed = true; }
      else field += char;
    } else if (char === '"' && !field && !closed) quoted = true;
    else if (char === ',') { row.push(field); field = ''; closed = false; }
    else if (char === '\n' || char === '\r') {
      if (char === '\r' && text[i + 1] === '\n') i++;
      row.push(field); if (row.some(value => value !== '')) records.push(row);
      row = []; field = ''; closed = false;
    } else if (closed) return fail(503, 'source_incomplete', 'The CSV contains an invalid quoted field.');
    else field += char;
  }
  if (quoted) return fail(503, 'source_incomplete', 'The CSV contains an incomplete quoted field.');
  if (field || row.length) { row.push(field); records.push(row); }
  if (!records.length) return [];
  const headers = records.shift();
  if (new Set(headers).size !== headers.length || headers.some(key => !/^[A-Za-z_][A-Za-z0-9_]*$/.test(key))) return fail(503, 'source_incomplete', 'The CSV header is invalid.');
  return records.map(values => {
    if (values.length !== headers.length) return fail(503, 'source_incomplete', 'The CSV row has an unexpected number of fields.');
    return Object.fromEntries(headers.map((key, i) => [key, values[i]]));
  });
}

function boundedInteger(params, key, fallback, max) {
  const value = params.get(key);
  if (value === null) return fallback;
  if (!/^\d+$/.test(value) || !Number.isSafeInteger(Number(value)) || Number(value) > max) return fail(400, 'invalid_query', `Invalid ${key} value.`);
  return Number(value);
}

export async function createReadApi({ root, synthetic = false, now = Date.now, pollMs = 2000, allowedOrigins = ['http://127.0.0.1:8787', 'http://localhost:8787'], allowedHosts = [], maxFileBytes = MAX_FILE } = {}) {
  if (!root) throw new Error('RUNS_ROOT is required; use --fixtures for an explicit synthetic preview.');
  const base = await realpath(path.resolve(root));
  if (!(await lstat(base)).isDirectory()) throw new Error('RUNS_ROOT must be a directory.');
  let port = 0, lastSnapshot = null, polling = false;

  async function read(id, file, optional = false) {
    if (id && (!ID.test(id) || id === '.' || id === '..')) return fail(400, 'invalid_id', 'Invalid run or instance identifier.');
    let handle;
    try {
      const directory = id ? path.join(base, id) : base;
      const directoryStat = await lstat(directory);
      if (!directoryStat.isDirectory() || directoryStat.isSymbolicLink()) return fail(404, 'not_found', 'Unknown run or instance.');
      const target = path.join(directory, file);
      const info = await lstat(target);
      if (!info.isFile() || info.isSymbolicLink()) return fail(404, 'not_found', 'The requested artifact is unavailable.');
      if (!isWithin(base, await realpath(target))) return fail(404, 'not_found', 'The requested artifact is unavailable.');
      handle = await open(target, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
      const opened = await handle.stat();
      if (!opened.isFile() || opened.dev !== info.dev || opened.ino !== info.ino) return fail(503, 'source_changed', 'The source changed during reading. Retry shortly.');
      if (opened.size > maxFileBytes) return fail(413, 'artifact_too_large', 'This artifact exceeds the configured read limit.');
      // Read a bounded buffer even if an append-only file grows after stat().
      const bytes = Buffer.alloc(Math.min(maxFileBytes + 1, opened.size + 1));
      const result = await handle.read(bytes, 0, bytes.length, 0);
      if (result.bytesRead > maxFileBytes) return fail(413, 'artifact_too_large', 'This artifact exceeds the configured read limit.');
      return bytes.subarray(0, result.bytesRead).toString('utf8');
    } catch (error) {
      if (error.code === 'ENOENT' || error.code === 'ENOTDIR') {
        if (optional) return null;
        return fail(404, 'not_found', 'The requested artifact is unavailable.');
      }
      throw error;
    } finally { await handle?.close(); }
  }

  async function ids() {
    const entries = await readdir(base, { withFileTypes: true });
    const directories = entries.filter(entry => entry.isDirectory() && !entry.isSymbolicLink() && ID.test(entry.name));
    if (directories.length > 500) return fail(413, 'too_many_runs', 'The configured root exceeds the supported run count.');
    return directories.map(entry => entry.name).sort();
  }

  function respond(res, status, body, type = 'application/json; charset=utf-8', extra = {}, head = false) {
    res.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", 'X-Data-Source': synthetic ? 'synthetic-fixtures' : 'bot-artifacts', ...extra });
    res.end(head ? undefined : body);
  }

  async function handler(req, res) {
    const head = req.method === 'HEAD';
    try {
      const host = req.headers.host ?? '';
      if (![`127.0.0.1:${port}`, `localhost:${port}`, ...allowedHosts].includes(host)) return fail(403, 'invalid_host', 'This host is not allowed.');
      if (req.headers.origin && !allowedOrigins.includes(req.headers.origin)) return fail(403, 'invalid_origin', 'This origin is not allowed.');
      if (req.method !== 'GET' && !head) return fail(405, 'read_only', 'This service supports read-only requests.');
      if (req.url.includes('\\') || /%(?:2e|2f|5c|00)/i.test(req.url.split('?')[0]) || /(?:^|\/)\.\.?($|\/)/.test(req.url.split('?')[0])) return fail(400, 'invalid_path', 'Invalid artifact path.');
      const url = new URL(req.url, `http://${host}`);
      const parts = url.pathname.split('/').filter(Boolean).map(part => decodeURIComponent(part));
      if (parts.some(part => part === '.' || part === '..')) return fail(400, 'invalid_path', 'Invalid artifact path.');
      const [kind, id, artifact] = parts;
      if (parts.length > 3 || !['runs', 'instances'].includes(kind)) return fail(404, 'not_found', 'Unknown read API route.');
      const permitted = artifact === 'journal' ? ['after', 'limit'] : artifact === 'equity' ? ['every', 'after', 'limit'] : artifact === 'metrics' ? ['name', 'after', 'limit'] : artifact in CSV_FILES ? ['after', 'limit'] : [];
      for (const key of url.searchParams.keys()) if (!permitted.includes(key) || url.searchParams.getAll(key).length !== 1) return fail(400, 'invalid_query', 'Unsupported or duplicate query parameter.');
      if (id && !ID.test(id)) return fail(400, 'invalid_id', 'Invalid run or instance identifier.');

      if (!id) {
        if (kind === 'runs') {
          const index = await read('', 'index.csv', true);
          if (index !== null) return respond(res, 200, json(parseCsv(index)), undefined, {}, head);
          const summaries = [];
          for (const entry of await ids()) {
            const summary = await read(entry, 'summary.json', true);
            if (summary !== null) summaries.push(rawJson(summary));
          }
          return respond(res, 200, `[${summaries.join(',')}]`, undefined, {}, head);
        }
        const instances = [];
        for (const entry of await ids()) {
          const heartbeat = await read(entry, 'heartbeat', true);
          if (heartbeat === null) continue;
          const source = rawJson(await read(entry, 'status.json'));
          const status = JSON.parse(source);
          if (!status || Array.isArray(status) || typeof status.mode !== 'string' || typeof status.label !== 'string') return fail(503, 'source_incomplete', 'The instance status is missing its mode or label.');
          instances.push(`{"id":${json(entry)},"mode":${json(status.mode)},"label":${json(status.label)},"heartbeat":${json(heartbeatFrom(heartbeat, now()))},"status":${source}}`);
        }
        return respond(res, 200, `[${instances.join(',')}]`, undefined, {}, head);
      }

      if (kind === 'instances') {
        await read(id, 'heartbeat');
        if (artifact === 'journal') {
          const source = await read(id, 'journal.jsonl');
          const lines = source.match(/[^\n]*\n|[^\n]+$/g) ?? [];
          // Ignore only an incomplete final line while the bot is appending.
          if (lines.length && !lines.at(-1).endsWith('\n')) lines.pop();
          const after = boundedInteger(url.searchParams, 'after', 0, 1000000000);
          const limit = boundedInteger(url.searchParams, 'limit', MAX_ROWS, MAX_ROWS);
          if (!limit) return fail(400, 'invalid_query', 'The limit must be positive.');
          const selected = lines.slice(after, after + limit);
          for (const line of selected) rawJson(line);
          const next = after + selected.length;
          return respond(res, 200, selected.join(''), 'application/x-ndjson; charset=utf-8', { 'X-Next-After': String(next), 'X-Has-More': String(next < lines.length) }, head);
        }
        if (!['status', 'state', 'metrics.prom'].includes(artifact)) return fail(404, 'not_found', 'Unknown instance artifact.');
      } else {
        if (!artifact) {
          const summary = rawJson(await read(id, 'summary.json'));
          const metrics = await read(id, 'metrics.json', true);
          const config = await read(id, 'config.txt');
          return respond(res, 200, `{"summary":${summary},"metrics":${metrics === null ? 'null' : rawJson(metrics)},"config":${json(config)}}`, undefined, {}, head);
        }
        // Run artifact routes accept an instance directory too, as the contract specifies.
        if (await read(id, 'summary.json', true) === null && await read(id, 'heartbeat', true) === null) return fail(404, 'not_found', 'Unknown run or instance.');
      }

      if (Object.hasOwn(JSON_FILES, artifact)) return respond(res, 200, rawJson(await read(id, JSON_FILES[artifact])), undefined, {}, head);
      if (Object.hasOwn(TEXT_FILES, artifact)) return respond(res, 200, await read(id, TEXT_FILES[artifact]), 'text/plain; charset=utf-8', {}, head);
      if (Object.hasOwn(CSV_FILES, artifact) && kind === 'runs') {
        let rows = parseCsv(await read(id, CSV_FILES[artifact]));
        if (artifact === 'metrics' && url.searchParams.has('name')) rows = rows.filter(row => row.name === url.searchParams.get('name'));
        if (artifact === 'equity') {
          const every = boundedInteger(url.searchParams, 'every', 1, 1000000);
          if (!every) return fail(400, 'invalid_query', 'The every value must be positive.');
          rows = rows.filter((_, index) => index === 0 || index === rows.length - 1 || index % every === 0);
        }
        const after = boundedInteger(url.searchParams, 'after', 0, 1000000000);
        const limit = boundedInteger(url.searchParams, 'limit', MAX_ROWS, MAX_ROWS);
        if (!limit) return fail(400, 'invalid_query', 'The limit must be positive.');
        const selected = rows.slice(after, after + limit), next = after + selected.length;
        return respond(res, 200, json(selected), undefined, { 'X-Next-After': String(next), 'X-Has-More': String(next < rows.length) }, head);
      }
      return fail(404, 'not_found', 'Unknown artifact.');
    } catch (error) {
      if (error instanceof URIError) error = new ApiError(400, 'invalid_path', 'Invalid artifact path encoding.');
      const known = error instanceof ApiError;
      respond(res, known ? error.status : 503, json({ error: known ? error.code : 'source_unavailable', detail: known ? error.message : 'The artifact source is unavailable.' }), undefined, error.status === 405 ? { Allow: 'GET, HEAD' } : {}, head);
    }
  }

  const server = http.createServer({ maxHeaderSize: 8192, requestTimeout: 10000, headersTimeout: 5000 }, handler);
  const wss = new WebSocketServer({ noServer: true, maxPayload: 1024, perMessageDeflate: false });
  server.on('upgrade', (request, socket, head) => {
    const host = request.headers.host ?? '';
    if (request.url !== '/events' || ![`127.0.0.1:${port}`, `localhost:${port}`, ...allowedHosts].includes(host) || (request.headers.origin && !allowedOrigins.includes(request.headers.origin)) || wss.clients.size >= 32) {
      socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n'); return;
    }
    wss.handleUpgrade(request, socket, head, client => { client.on('message', () => client.close(1008, 'Read-only stream')); client.on('error', () => {}); wss.emit('connection', client); });
  });
  function broadcast(message) {
    for (const client of wss.clients) {
      if (client.bufferedAmount > 1024 * 1024) client.close(1013, 'Slow consumer');
      else if (client.readyState === WebSocket.OPEN) client.send(message);
    }
  }
  async function snapshot() {
    const next = new Map();
    for (const id of await ids()) for (const file of ['heartbeat', 'status.json', 'state.json', 'journal.jsonl', 'summary.json']) {
      try { const stat = await lstat(path.join(base, id, file), { bigint: true }); if (stat.isFile() && !stat.isSymbolicLink()) next.set(`${id}/${file}`, `${stat.mtimeNs}:${stat.size}:${stat.ino}`); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    return next;
  }
  async function poll() {
    if (polling || !wss.clients.size) return;
    polling = true;
    try {
      const next = await snapshot();
      if (lastSnapshot) for (const [key, signature] of next) {
        if (lastSnapshot.get(key) === signature) continue;
        const [id, file] = key.split('/');
        try {
          if (file === 'heartbeat') { const { time, status } = heartbeatFrom(await read(id, file), now()); broadcast(json({ kind: 'heartbeat', instance: id, time, status })); }
          else if (file === 'status.json') broadcast(`{"kind":"status","instance":${json(id)},"status":${rawJson(await read(id, file))}}`);
          else if (file === 'state.json') broadcast(json({ kind: 'state', instance: id }));
          else if (file === 'journal.jsonl') broadcast(json({ kind: 'journal', instance: id, lines: (await read(id, file)).split('\n').length - 1 }));
          else broadcast(json({ kind: 'run', run_id: id, event: lastSnapshot.has(key) ? 'updated' : 'created' }));
        } catch { /* Transient atomic file replacement: retry the next poll. */ next.delete(key); }
      }
      lastSnapshot = next;
    } finally { polling = false; }
  }
  const timer = setInterval(() => poll().catch(() => {}), Math.max(100, pollMs)); timer.unref();
  return {
    server, poll,
    async listen(listenPort = 8788, host = '127.0.0.1') {
      await new Promise((resolve, reject) => { server.once('error', reject); server.listen(listenPort, host, resolve); });
      port = server.address().port; return `http://127.0.0.1:${port}`;
    },
    async close() { clearInterval(timer); for (const client of wss.clients) client.terminate(); wss.close(); server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const synthetic = process.argv.includes('--fixtures');
  const root = synthetic ? fileURLToPath(new URL('../fixtures/synthetic/', import.meta.url)) : process.env.RUNS_ROOT;
  const api = await createReadApi({ root, synthetic, allowedHosts: (process.env.ALLOWED_HOSTS ?? '').split(',').filter(Boolean), allowedOrigins: process.env.ALLOWED_ORIGINS ? process.env.ALLOWED_ORIGINS.split(',') : undefined });
  const url = await api.listen(Number(process.env.PORT ?? 8788), process.env.BIND_HOST ?? '127.0.0.1');
  console.log(`Read-only API: ${url} (${synthetic ? 'SYNTHETIC FIXTURES — not market data' : 'configured bot artifact root'})`);
  for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, async () => { await api.close(); process.exit(0); });
}
