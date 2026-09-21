import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, mkdtemp, mkdir, writeFile, rm, symlink, rename } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { fileURLToPath } from 'node:url';
import { createReadApi, heartbeatFrom, parseCsv } from '../server.mjs';
import { WebSocket } from 'ws';

const fixtureRoot = fileURLToPath(new URL('../../fixtures/synthetic/', import.meta.url));
const runs = ['ma_synthetic_2024-03-15_2024-03-17_78e97a61', 'bah_synthetic_2024-03-15_2024-03-17_f2e66a79'];
const instance = 'paper-ma_synthetic';
const read = (id, file) => readFile(path.join(fixtureRoot, id, file), 'utf8');
async function setup(t, options = {}) {
  const api = await createReadApi({ root: fixtureRoot, synthetic: true, ...options });
  const base = await api.listen(0);
  t.after(() => api.close());
  return { api, base, get: endpoint => fetch(base + endpoint) };
}

test('all run summary/metrics/config fields and decimal strings equal both fixture runs', async t => {
  const { get } = await setup(t);
  const index = await (await get('/runs')).json();
  assert.deepEqual(index.map(row => row.run_id).sort(), [...runs].sort());
  for (const id of runs) {
    const response = await get(`/runs/${id}`);
    assert.equal(response.headers.get('x-data-source'), 'synthetic-fixtures');
    assert.equal(response.headers.get('cache-control'), 'no-store');
    assert.deepEqual(await response.json(), { summary: JSON.parse(await read(id, 'summary.json')), metrics: JSON.parse(await read(id, 'metrics.json')), config: await read(id, 'config.txt') });
  }
});

test('each CSV route preserves exact source columns and values; downsampling keeps endpoints', async t => {
  const { get } = await setup(t);
  for (const id of runs) for (const file of ['equity', 'fills', 'orders', 'metrics', 'round_trips']) {
    const expected = parseCsv(await read(id, `${file}.csv`));
    assert.deepEqual(await (await get(`/runs/${id}/${file}`)).json(), expected);
  }
  const all = parseCsv(await read(runs[0], 'equity.csv'));
  const selected = await (await get(`/runs/${runs[0]}/equity?every=200`)).json();
  assert.deepEqual(selected[0], all[0]); assert.deepEqual(selected.at(-1), all.at(-1));
  const page = await get(`/runs/${runs[0]}/equity?limit=2&after=1`);
  assert.equal(page.headers.get('x-next-after'), '3'); assert.equal(page.headers.get('x-has-more'), 'true');
  assert.deepEqual(await page.json(), all.slice(1, 3));
});

test('instance status/state/prometheus/journal and persisted validation are verbatim', async t => {
  const at = Date.parse('2026-09-21T01:15:30Z');
  const { get } = await setup(t, { now: () => at });
  const list = await (await get('/instances')).json();
  assert.equal(list.length, 1); assert.equal(list[0].id, instance); assert.equal(list[0].heartbeat.age_seconds, 30);
  for (const artifact of ['status', 'state']) assert.equal(await (await get(`/instances/${instance}/${artifact}`)).text(), await read(instance, `${artifact}.json`));
  assert.equal(await (await get(`/instances/${instance}/metrics.prom`)).text(), await read(instance, 'metrics.prom'));
  const journal = await get(`/instances/${instance}/journal`);
  assert.match(journal.headers.get('content-type'), /^application\/x-ndjson/);
  assert.equal(await journal.text(), await read(instance, 'journal.jsonl'));
  assert.equal(await (await get(`/runs/${runs[0]}/validation`)).text(), await read(runs[0], 'validation.json'));
  assert.equal((await get(`/runs/${runs[1]}/validation`)).status, 404);
  assert.equal(await (await get(`/runs/${runs[0]}/report`)).text(), await read(runs[0], 'report.txt'));
});

test('read-only methods, origin/host checks, traversal and query boundaries deny access', async t => {
  const { get, base } = await setup(t);
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE']) assert.equal((await fetch(base + '/runs', { method })).status, 405);
  assert.equal((await fetch(base + '/runs', { headers: { Origin: 'https://outside.example' } })).status, 403);
  const forgedHostStatus = await new Promise((resolve, reject) => {
    const request = http.get(base + '/runs', { headers: { Host: 'attacker.example' } }, response => { response.resume(); resolve(response.statusCode); });
    request.on('error', reject);
  });
  assert.equal(forgedHostStatus, 403);
  for (const route of ['/runs/%2e%2e%2fREADME.md', '/runs/a%5cb/status', `/runs/${runs[0]}/equity?every=0`, `/runs/${runs[0]}/equity?limit=10001`, `/runs/${runs[0]}/equity?after=-1`, `/runs/${runs[0]}/equity?every=1&every=2`]) assert.equal((await get(route)).status, 400, route);
  for (const route of ['/board/tasks', '/trade', '/runs/unknown', `/runs/${runs[0]}/config.txt`]) assert.equal((await get(route)).status, 404, route);
});

test('legacy unsafe integer journals pass through exactly; incomplete tail is not published', async t => {
  const root = await mkdtemp(path.join(tmpdir(), 'platform-api-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, 'paper-example'));
  await writeFile(path.join(root, 'paper-example', 'heartbeat'), '2026-09-21T01:15:00Z paper healthy armed\n');
  const first = '{"time":1789949100000000001,"price":"0.00000001"}\n';
  await writeFile(path.join(root, 'paper-example', 'journal.jsonl'), first + '{"time":1789949100000000002');
  const { get } = await setup(t, { root });
  assert.equal(await (await get('/instances/paper-example/journal')).text(), first);
  assert.equal(await (await get('/instances/paper-example/journal?after=1')).text(), '');
});

test('source symlinks and oversized files cannot be read', async t => {
  const root = await mkdtemp(path.join(tmpdir(), 'platform-api-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, 'example'));
  await writeFile(path.join(root, 'example', 'summary.json'), '{"extra":"' + 'x'.repeat(200) + '"}');
  const { get } = await setup(t, { root, maxFileBytes: 100 });
  assert.equal((await get('/runs/example')).status, 413);
  // Junctions work without Windows Developer Mode; Unix uses a directory symlink.
  await symlink(path.join(fixtureRoot, runs[0]), path.join(root, 'linked'), process.platform === 'win32' ? 'junction' : 'dir');
  assert.equal((await get('/runs/linked')).status, 404);
});

test('polling detects atomic heartbeat replacement; age crosses the stale threshold', async t => {
  const root = await mkdtemp(path.join(tmpdir(), 'platform-api-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const folder = path.join(root, 'paper-example'); await mkdir(folder);
  await writeFile(path.join(folder, 'heartbeat'), '2026-09-21T01:15:00Z paper healthy armed\n');
  const { api, base } = await setup(t, { root, pollMs: 60000 });
  const ws = new WebSocket(base.replace('http:', 'ws:') + '/events');
  await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); });
  t.after(() => ws.terminate());
  await api.poll();
  const message = new Promise((resolve, reject) => { const timeout = setTimeout(() => reject(new Error('No heartbeat update')), 3000); ws.once('message', bytes => { clearTimeout(timeout); resolve(JSON.parse(bytes)); }); });
  await writeFile(path.join(folder, 'heartbeat.tmp'), '2026-09-21T01:15:05Z stopped\n');
  await rename(path.join(folder, 'heartbeat.tmp'), path.join(folder, 'heartbeat'));
  await api.poll();
  assert.deepEqual(await message, { kind: 'heartbeat', instance: 'paper-example', time: '2026-09-21T01:15:05Z', status: 'stopped' });
  assert.equal(heartbeatFrom('2026-09-21T01:15:00Z paper healthy armed', Date.parse('2026-09-21T01:15:45Z')).age_seconds, 45);
});

test('CSV quoted commas, quotes and multiline fields decode without monetary conversion', () => {
  assert.deepEqual(parseCsv('price,reason\r\n0.000001,"a, ""quote""\nnext"\r\n'), [{ price: '0.000001', reason: 'a, "quote"\nnext' }]);
  assert.throws(() => parseCsv('a,b\n1,"partial'));
});

test('incomplete instance metadata returns a source error rather than malformed JSON', async t => {
  const root = await mkdtemp(path.join(tmpdir(), 'platform-api-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(path.join(root, 'paper-example'));
  await writeFile(path.join(root, 'paper-example', 'heartbeat'), '2026-09-21T01:15:00Z paper healthy armed\n');
  await writeFile(path.join(root, 'paper-example', 'status.json'), '{"equity":"1"}');
  const { get } = await setup(t, { root });
  const response = await get('/instances');
  assert.equal(response.status, 503);
  assert.equal((await response.json()).error, 'source_incomplete');
});
