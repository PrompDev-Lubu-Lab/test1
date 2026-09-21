import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';
import { BOARD_ROSTER, BOARD_STATUSES, boardReady, readBoard, mutateBoard } from '../board.mjs';

const env = Object.freeze({ BOARD_READY: 'staging', BOARD_BRANCH: 'board/staging', BOARD_REPO: 'example-owner/example-project',
  BOARD_GITHUB_TOKEN: 'github_pat_synthetic_test_token_never_a_real_secret' });
const now = Date.UTC(2026, 8, 21, 14, 5, 30);
let sequence = 1;
const uuid = () => `00000000-0000-4000-8000-${(sequence++).toString(16).padStart(12, '0')}`;
const blobSha = text => createHash('sha1').update(`blob ${Buffer.byteLength(text)}\0`).update(text).digest('hex');
const paths = { tasks: 'board/tasks.json', notes: 'board/notes.md' };
const projectTasks = await readFile(new URL('../../../board/tasks.json', import.meta.url), 'utf8');
const projectNotes = await readFile(new URL('../../../board/notes.md', import.meta.url), 'utf8');
const task = (number, overrides = {}) => ({ id: `T-${String(number).padStart(3, '0')}`, title: `Complete item ${number}`,
  status: 'todo', priority: 'normal', assigned_by: 'deandre-fable', assigned_to: null, assigned_on: '2026-09-20', due: null,
  area: 'platform', depends_on: [], instructions: 'Keep the existing implementation and report the result.',
  log: [{ on: '2026-09-20', by: 'deandre-fable', text: 'created', inherited_log_field: ['retain', 1] }], ...overrides });
const seed = () => ({ $schema_note: 'Preserve this existing metadata.', inherited: { nested: [1, { keep: true }] },
  tasks: [task(1, { inherited_task_field: { keep: ['all', 'values'] } }), task(3, { status: 'done' })] });

class GithubHarness {
  calls = [];
  putCount = 0;
  getCount = 0;
  documents = new Map();
  beforePut = null;
  constructor(tasks = JSON.stringify(seed(), null, 2) + '\n', notes = '# Notes\r\n\r\nExisting text remains byte-for-byte.\r\n') {
    this.set('tasks', tasks);
    this.set('notes', notes);
  }
  set(kind, text) { this.documents.set(paths[kind], { text, sha: blobSha(text) }); }
  get(kind) { return this.documents.get(paths[kind]); }
  parsed() { return JSON.parse(this.get('tasks').text); }
  editTasks(edit) { const document = this.parsed(); edit(document); this.set('tasks', JSON.stringify(document, null, 2) + '\n'); }
  fetch = async (input, options) => {
    const url = new URL(input);
    this.calls.push({ url, ...options });
    assert.equal(url.origin, 'https://api.github.com');
    assert.equal(options.redirect, 'error');
    assert.equal(options.headers.Authorization, `Bearer ${env.BOARD_GITHUB_TOKEN}`);
    assert.equal(options.headers['X-GitHub-Api-Version'], '2026-03-10');
    assert.ok(options.signal instanceof AbortSignal);
    const path = url.pathname.replace('/repos/example-owner/example-project/contents/', '');
    assert.ok(Object.values(paths).includes(path));
    if (options.method === 'GET') {
      this.getCount++;
      assert.equal(url.searchParams.get('ref'), 'board/staging');
      const current = this.documents.get(path);
      return Response.json({ type: 'file', path, sha: current.sha, size: Buffer.byteLength(current.text), encoding: 'base64',
        content: Buffer.from(current.text).toString('base64').replace(/.{60}/g, '$&\n'),
        download_url: 'https://untrusted.example.test/never-follow', git_url: 'https://untrusted.example.test/never-follow' });
    }
    assert.equal(options.method, 'PUT');
    this.putCount++;
    const body = JSON.parse(options.body);
    assert.deepEqual(Object.keys(body).sort(), ['branch', 'content', 'message', 'sha']);
    assert.equal(body.branch, 'board/staging');
    assert.match(body.message, /^board: (?:T-\d+ (?:todo|in_progress|blocked|review|done|dropped)|note) \((?:deandre|ali)\)$/);
    if (this.beforePut) await this.beforePut(this, path, this.putCount);
    if (body.sha !== this.documents.get(path).sha) return Response.json({ message: 'provider error must remain private' }, { status: 409 });
    const text = Buffer.from(body.content, 'base64').toString('utf8');
    const sha = blobSha(text);
    this.documents.set(path, { text, sha });
    return Response.json({ content: { path, sha, type: 'file' }, commit: { sha: 'c'.repeat(40) } });
  };
}
function creation(harness, overrides = {}) {
  return { action: 'create', expected_sha: harness.get('tasks').sha, operation_id: uuid(),
    task: { title: 'Build the next capability', area: 'platform', instructions: 'Use the reviewed scope.', ...overrides } };
}
function update(harness, changes, extra = {}) {
  const target = harness.parsed().tasks[0];
  return { action: 'update', expected_sha: harness.get('tasks').sha, operation_id: uuid(), task_id: target.id,
    changes, base: Object.fromEntries(Object.keys(changes).filter(key => key !== 'status').map(key => [key, target[key]])), ...extra };
}
function note(harness, text = 'A plain note referring to T-001.') {
  return { expected_sha: harness.get('notes').sha, operation_id: uuid(), to: ['all'], text };
}
const options = harness => ({ fetchImpl: harness.fetch, now, authorize:async()=>true });
const denies = (promise, code = 'board_invalid_request', status = 400) => assert.rejects(promise, { code, status });

test('closed configuration permits only the operator-selected repository and staging branch', async () => {
  assert.equal(boardReady(env), true);
  for (const invalid of [{}, { ...env, BOARD_READY: undefined }, { ...env, BOARD_BRANCH: 'main' },
    { ...env, BOARD_GITHUB_TOKEN: 'ghp_classic_token_not_accepted' }, { ...env, BOARD_REPO: '../escape/project' },
    { ...env, BOARD_REPO: 'example-owner/..' }, { ...env, BOARD_BRANCH: 'board/staging?other=1' },
    { ...env, BOARD_READY: 'verified', BOARD_BRANCH: 'a/../main' }]) {
    assert.equal(boardReady(invalid), false);
    await denies(readBoard(invalid, 'tasks', { fetchImpl: () => assert.fail('No request may be made') }), 'board_unavailable', 503);
  }
  assert.equal(boardReady({ ...env, BOARD_READY: 'verified', BOARD_BRANCH: 'main' }), true);
  assert.ok(BOARD_ROSTER.includes('ali-fable'));
  assert.ok(BOARD_STATUSES.includes('dropped'));
});

test('actual project documents read through Contents JSON without following returned URLs', async () => {
  const github = new GithubHarness(projectTasks, projectNotes);
  assert.deepEqual(await readBoard(env, 'tasks', options(github)), { sha: blobSha(projectTasks), tasks: JSON.parse(projectTasks).tasks });
  assert.deepEqual(await readBoard(env, 'notes', options(github)), { sha: blobSha(projectNotes), notes: projectNotes });
  assert.equal(github.calls.length, 2);
});

test('actor, path, branch, author and server-owned field injection are rejected before GitHub calls', async () => {
  const github = new GithubHarness();
  for (const actor of ['deandre-fable', 'ali-fable', 'intruder', { handle: 'deandre' }, null]) {
    await denies(mutateBoard(env, 'tasks', actor, creation(github), options(github)), 'board_actor_denied', 403);
  }
  for (const kind of ['../tasks', 'board/tasks.json', 'README.md', 'constructor']) {
    await denies(readBoard(env, kind, options(github)));
    await denies(mutateBoard(env, kind, 'deandre', creation(github), options(github)));
  }
  for (const extra of [{ repo: 'someone/else' }, { path: 'README.md' }, { branch: 'main' }, { author: 'ali' }, { message: 'arbitrary commit' }]) {
    await denies(mutateBoard(env, 'tasks', 'deandre', { ...creation(github), ...extra }, options(github)));
  }
  for (const extra of [{ id: 'T-999' }, { assigned_by: 'ali' }, { assigned_on: '2020-01-01' }, { log: [] }, { status: 'done' }]) {
    await denies(mutateBoard(env, 'tasks', 'deandre', creation(github, extra), options(github)));
  }
  await denies(mutateBoard(env, 'tasks', 'deandre', { ...creation(github), action: 'delete' }, options(github)));
  assert.equal(github.calls.length, 0);
});

test('create allocates max+1 and supplies dates, attribution and append-only trace metadata', async () => {
  const github = new GithubHarness();
  const before = github.parsed();
  const body = creation(github, { assigned_to: 'ali-fable', priority: 'high', due: '2026-09-28', depends_on: ['T-003'] });
  const result = await mutateBoard(env, 'tasks', 'ali', body, options(github));
  assert.equal(result.task_id, 'T-004');
  assert.equal(result.replayed, false);
  assert.equal(result.commit_sha, 'c'.repeat(40));
  const after = github.parsed();
  assert.deepEqual(after.tasks.slice(0, 2), before.tasks);
  assert.deepEqual(after.inherited, before.inherited);
  assert.equal(after.$schema_note, before.$schema_note);
  const created = after.tasks.at(-1);
  assert.equal(created.assigned_by, 'ali');
  assert.equal(created.assigned_on, '2026-09-21');
  assert.equal(created.status, 'todo');
  assert.deepEqual(created.log[0], { on: '2026-09-21', by: 'ali', text: 'created', operation_id: body.operation_id,
    operation_hash: created.log[0].operation_hash });
  assert.match(created.log[0].operation_hash, /^[a-f0-9]{64}$/);
});

test('schema validates enums, UTC dates, handles and dependency identifiers', async () => {
  for (const invalid of [{ title: '' }, { title: 'x'.repeat(161) }, { priority: 'urgent' }, { assigned_to: 'someone' },
    { due: '2026-02-30' }, { due: 'tomorrow' }, { area: 'exchange' }, { instructions: '\u0000' },
    { instructions: '\ud800' }, { depends_on: ['T-003', 'T-003'] }, { depends_on: ['T-3'] }]) {
    const github = new GithubHarness();
    await denies(mutateBoard(env, 'tasks', 'deandre', creation(github, invalid), options(github)));
    assert.equal(github.calls.length, 0);
  }
  for (const dependencies of [['T-999'], ['T-004']]) {
    const github = new GithubHarness();
    await denies(mutateBoard(env, 'tasks', 'deandre', creation(github, { depends_on: dependencies }), options(github)));
    assert.equal(github.putCount, 0);
  }
});

test('updates preserve inherited fields and every prior log while attributing reassignment', async () => {
  const github = new GithubHarness();
  const before = github.parsed();
  const body = update(github, { assigned_to: 'ali', status: 'in_progress', title: 'Start the planned work' }, { log: 'Scope is confirmed.' });
  await mutateBoard(env, 'tasks', 'deandre', body, options(github));
  const after = github.parsed();
  assert.deepEqual(after.tasks[0].log.slice(0, -1), before.tasks[0].log);
  assert.deepEqual(after.tasks[0].inherited_task_field, before.tasks[0].inherited_task_field);
  assert.deepEqual(after.tasks[1], before.tasks[1]);
  assert.equal(after.tasks[0].assigned_by, 'deandre');
  assert.equal(after.tasks[0].assigned_on, '2026-09-21');
  assert.equal(after.tasks[0].status, 'in_progress');
  assert.match(after.tasks[0].log.at(-1).text, /^status: todo → in_progress; Scope is confirmed\./);
});

test('dependencies cannot form a cycle or advance an unfinished dependency; dropping needs a reason', async () => {
  const github = new GithubHarness();
  github.editTasks(document => { document.tasks[1].depends_on = ['T-001']; });
  await denies(mutateBoard(env, 'tasks', 'deandre', update(github, { depends_on: ['T-003'] }), options(github)));
  github.editTasks(document => { document.tasks[1].depends_on = []; document.tasks[1].status = 'todo'; document.tasks[0].depends_on = ['T-003']; });
  await denies(mutateBoard(env, 'tasks', 'deandre', update(github, { status: 'done' }), options(github)), 'board_dependency_pending', 409);
  await denies(mutateBoard(env, 'tasks', 'deandre', update(github, { status: 'dropped' }), options(github)));
  await mutateBoard(env, 'tasks', 'deandre', update(github, { status: 'dropped' }, { log: 'No longer required.' }), options(github));
  assert.equal(github.parsed().tasks[0].status, 'dropped');
  assert.equal(github.parsed().tasks.length, 2);
});

test('text updates require exact base fields and cannot replace history or introduce unknown fields', async () => {
  const github = new GithubHarness();
  for (const body of [update(github, { title: 'A new title' }, { base: {} }),
    update(github, { status: 'invalid' }), update(github, { log: [] }),
    update(github, { title: 'A new title' }, { base: { title: 'wrong prior title' } })]) {
    await assert.rejects(mutateBoard(env, 'tasks', 'deandre', body, options(github)), error => [400, 409].includes(error.status));
  }
  assert.equal(github.putCount, 0);
});

test('one 409 re-read preserves concurrent tasks, fields and logs while applying newest status', async () => {
  const github = new GithubHarness();
  const body = update(github, { status: 'review' }, { log: 'Ready for review.' });
  let concurrent;
  github.beforePut = (store, _path, count) => {
    if (count !== 1) return;
    store.editTasks(document => {
      document.tasks[0].title = 'Concurrent title must remain';
      document.tasks[0].status = 'in_progress';
      document.tasks[0].log.push({ on: '2026-09-21', by: 'ali', text: 'Work started elsewhere.', external: true });
      document.tasks.push(task(8));
    });
    concurrent = store.parsed();
  };
  await mutateBoard(env, 'tasks', 'deandre', body, options(github));
  const after = github.parsed();
  assert.equal(github.getCount, 2);
  assert.equal(github.putCount, 2);
  assert.equal(after.tasks[0].title, 'Concurrent title must remain');
  assert.equal(after.tasks[0].status, 'review');
  assert.deepEqual(after.tasks[0].log.slice(0, -1), concurrent.tasks[0].log);
  assert.deepEqual(after.tasks.slice(1), concurrent.tasks.slice(1));
});

test('concurrent changes to the same text field return409 without a replacement PUT', async () => {
  const github = new GithubHarness();
  const body = update(github, { instructions: 'My proposed replacement.' });
  github.beforePut = (store, _path, count) => {
    if (count === 1) store.editTasks(document => { document.tasks[0].instructions = 'A concurrent, independently written instruction.'; });
  };
  await denies(mutateBoard(env, 'tasks', 'deandre', body, options(github)), 'board_conflict', 409);
  assert.equal(github.putCount, 1);
  assert.equal(github.parsed().tasks[0].instructions, 'A concurrent, independently written instruction.');
});

test('a stale expected SHA gets one fresh merge attempt and a second conflict stops', async () => {
  const github = new GithubHarness();
  const body = update(github, { title: 'Safe change to untouched title' });
  github.editTasks(document => { document.tasks[1].priority = 'high'; });
  await mutateBoard(env, 'tasks', 'deandre', body, options(github));
  assert.equal(github.getCount, 2);
  assert.equal(github.putCount, 1);
  assert.equal(github.parsed().tasks[1].priority, 'high');
  const conflicting = update(github, { status: 'in_progress' });
  github.beforePut = store => store.editTasks(document => { document.external_revision = (document.external_revision ?? 0) + 1; });
  const initialPuts = github.putCount;
  await denies(mutateBoard(env, 'tasks', 'deandre', conflicting, options(github)), 'board_conflict', 409);
  assert.equal(github.putCount - initialPuts, 2);
});

test('concurrent creates reallocate IDs against the latest document and preserve both operations', async () => {
  const github = new GithubHarness();
  const first = creation(github, { title: 'First independent task' });
  const second = creation(github, { title: 'Second independent task' });
  const results = await Promise.all([
    mutateBoard(env, 'tasks', 'deandre', first, options(github)),
    mutateBoard(env, 'tasks', 'ali', second, options(github)),
  ]);
  assert.deepEqual(results.map(result => result.task_id).sort(), ['T-004', 'T-005']);
  assert.equal(github.parsed().tasks.length, 4);
  assert.deepEqual(github.parsed().tasks.slice(-2).map(value => value.title).sort(), ['First independent task', 'Second independent task']);
});

test('operation retries are idempotent and reject different actors, actions or target fields', async () => {
  const github = new GithubHarness();
  const body = creation(github);
  const result = await mutateBoard(env, 'tasks', 'deandre', body, options(github));
  const replayed = await mutateBoard(env, 'tasks', 'deandre', body, options(github));
  assert.equal(replayed.task_id, result.task_id);
  assert.equal(replayed.replayed, true);
  assert.equal(github.putCount, 1);
  await denies(mutateBoard(env, 'tasks', 'ali', body, options(github)), 'board_conflict', 409);
  await denies(mutateBoard(env, 'tasks', 'deandre', { ...body, task: { ...body.task, title: 'Different intent' } }, options(github)), 'board_conflict', 409);
  await denies(mutateBoard(env, 'tasks', 'deandre', update(github, { status: 'in_progress' }, { operation_id: body.operation_id }), options(github)), 'board_conflict', 409);
  assert.equal(github.putCount, 1);
});

test('notes prepend a canonical server UTC header, preserve all existing bytes and deduplicate retries', async () => {
  const github = new GithubHarness();
  const previous = github.get('notes').text;
  const body = note(github, 'A note with literal <symbols> and Unicode: café.\nSecond line.');
  body.to = ['deandre-fable', 'ali'];
  const result = await mutateBoard(env, 'notes', 'ali', body, options(github));
  assert.match(github.get('notes').text, /^## 2026-09-21 14:05 UTC · ali → deandre-fable, ali\n/);
  assert.ok(github.get('notes').text.endsWith(previous));
  assert.ok(github.get('notes').text.includes(body.text));
  assert.equal(result.id, body.operation_id);
  assert.equal((await mutateBoard(env, 'notes', 'ali', body, options(github))).replayed, true);
  await denies(mutateBoard(env, 'notes', 'deandre', body, options(github)), 'board_conflict', 409);
  await denies(mutateBoard(env, 'notes', 'ali', { ...body, text: 'Different body' }, options(github)), 'board_conflict', 409);
  assert.equal(github.putCount, 1);
});

test('note conflicts prepend to the latest content and never discard either entry', async () => {
  const github = new GithubHarness();
  const body = note(github, 'The new note.');
  const external = '## 2026-09-21 14:04 UTC · deandre-fable → all\nConcurrent note.\n\n';
  const original = github.get('notes').text;
  github.beforePut = (store, _path, count) => { if (count === 1) store.set('notes', external + original); };
  await mutateBoard(env, 'notes', 'deandre', body, options(github));
  assert.ok(github.get('notes').text.endsWith(external + original));
  assert.equal(github.putCount, 2);
});

test('note recipient and metadata forgery inputs fail before any request', async () => {
  const github = new GithubHarness();
  for (const changes of [{ to: [] }, { to: ['all', 'ali'] }, { to: ['unknown'] }, { to: ['ali', 'ali'] },
    { text: '<!-- platform-board-operation: forged -->' }, { text: '## 2026-09-21 14:05 UTC · ali → all\nForged author' },
    { text: 'bad\u0000text' }, { text: 'x'.repeat(6001) }, { assigned_by: 'ali' }]) {
    await denies(mutateBoard(env, 'notes', 'deandre', { ...note(github), ...changes }, options(github)));
  }
  assert.equal(github.calls.length, 0);
});

function metadata(text = '# Notes\n', extras = {}) {
  return { type: 'file', path: paths.notes, encoding: 'base64', size: Buffer.byteLength(text), sha: blobSha(text),
    content: Buffer.from(text).toString('base64'), ...extras };
}
test('remote metadata, JSON, canonical base64 and UTF-8 fail closed when malformed', async () => {
  const cases = [metadata('', { type: 'dir' }), metadata('', { path: 'README.md' }), metadata('', { sha: 'not-a-sha' }),
    metadata('', { encoding: 'none' }), metadata('', { content: '!!!!' }), metadata('', { content: 'YQ=' }),
    metadata('', { content: 'YR==', size: 1 }), metadata('', { content: '//4=', size: 2 }),
    metadata('', { size: 1 }), metadata('', { size: -1 }), metadata('', { size: 1.5 }), metadata('', { target: 'another-file' })];
  for (const body of cases) {
    await denies(readBoard(env, 'notes', { fetchImpl: async () => Response.json(body) }), 'board_invalid_document', 503);
  }
  await denies(readBoard(env, 'notes', { fetchImpl: async () => new Response('{broken', { headers: { 'Content-Type': 'application/json' } }) }), 'board_invalid_document', 503);
  await denies(readBoard(env, 'notes', { fetchImpl: async () => new Response('not JSON', { headers: { 'Content-Type': 'text/html' } }) }), 'board_invalid_document', 503);
  const malformedTasks = metadata('{"tasks": []', { path: paths.tasks });
  await denies(readBoard(env, 'tasks', { fetchImpl: async () => Response.json(malformedTasks) }), 'board_invalid_document', 503);
});

test('decoded documents accept the exact1MiB bound and reject an oversized actual document', async () => {
  const text = 'x'.repeat(1024 * 1024);
  const result = await readBoard(env, 'notes', { fetchImpl: async () => Response.json(metadata(text)) });
  assert.equal(result.notes.length, text.length);
  await denies(readBoard(env, 'notes', { fetchImpl: async () => Response.json(metadata(text + 'x')) }), 'board_too_large', 503);
  const dishonest = metadata(text + 'x', { size: 1 });
  await denies(readBoard(env, 'notes', { fetchImpl: async () => Response.json(dishonest) }), 'board_too_large', 503);
});

test('actual streamed JSON is bounded even with a dishonest Content-Length and canceled early', async () => {
  let canceled = false;
  let reads = 0;
  const fetchImpl = async () => new Response(new ReadableStream({
    pull(controller) { reads++; controller.enqueue(new Uint8Array(128 * 1024).fill(32)); },
    cancel() { canceled = true; },
  }), { headers: { 'Content-Type': 'application/json', 'Content-Length': '2' } });
  await denies(readBoard(env, 'notes', { fetchImpl }), 'board_too_large', 503);
  assert.equal(canceled, true);
  assert.ok(reads <= 15);
});

test('a write cannot grow an accepted board beyond its document bound', async () => {
  const github = new GithubHarness(undefined, 'x'.repeat(1024 * 1024));
  await denies(mutateBoard(env, 'notes', 'ali', note(github), options(github)), 'board_too_large', 503);
  assert.equal(github.putCount, 0);
});

test('provider redirects, errors and transport exceptions are sanitized without exposing secrets', async () => {
  for (const status of [301, 302, 401, 403, 404, 422, 429, 500]) {
    let calls = 0;
    const fetchImpl = async () => { calls++; return new Response(`private ${env.BOARD_GITHUB_TOKEN}`, { status, headers: { Location: 'https://untrusted.example.test/' } }); };
    await assert.rejects(readBoard(env, 'notes', { fetchImpl }), error => error.status === 503 && !error.message.includes(env.BOARD_GITHUB_TOKEN));
    assert.equal(calls, 1);
  }
  await denies(readBoard(env, 'notes', { fetchImpl: async () => { throw new Error(env.BOARD_GITHUB_TOKEN); } }), 'board_unavailable', 503);
});

test('an uncertain successful PUT can be retried safely using its operation marker', async () => {
  const github = new GithubHarness();
  const body = creation(github);
  const uncertain = async (url, request) => {
    const response = await github.fetch(url, request);
    if (request.method === 'PUT') throw new Error('Connection failed after the commit.');
    return response;
  };
  await denies(mutateBoard(env, 'tasks', 'deandre', body, { fetchImpl: uncertain, now,authorize:async()=>true }), 'board_unavailable', 503);
  const retry = await mutateBoard(env, 'tasks', 'deandre', body, options(github));
  assert.equal(retry.replayed, true);
  assert.equal(github.parsed().tasks.length, 3);
  assert.equal(github.putCount, 1);
});


test('an account revoked during the remote read cannot commit using earlier authority',async()=>{
  const github=new GithubHarness();let checks=0;
  await denies(mutateBoard(env,'tasks','deandre',creation(github),{...options(github),authorize:async()=>++checks===1}),'login_required',401);
  assert.equal(github.getCount,1);assert.equal(github.putCount,0);
  await denies(mutateBoard(env,'tasks','deandre',creation(github),{fetchImpl:github.fetch}),'board_actor_denied',403);
});
