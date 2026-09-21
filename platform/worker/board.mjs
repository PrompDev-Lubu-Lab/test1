import { digestToken, SecurityError } from './security.mjs';

export const BOARD_ROSTER = Object.freeze(['deandre', 'deandre-fable', 'deandre-gpt6', 'ali', 'ali-fable']);
export const BOARD_STATUSES = Object.freeze(['todo', 'in_progress', 'blocked', 'review', 'done', 'dropped']);
export const BOARD_PRIORITIES = Object.freeze(['high', 'normal', 'low']);
export const BOARD_AREAS = Object.freeze(['bot', 'platform', 'research', 'ops', 'board']);
const HUMAN_ACTORS = new Set(['deandre', 'ali']);
const PATHS = Object.freeze({ tasks: 'board/tasks.json', notes: 'board/notes.md' });
const API_VERSION = '2026-03-10';
const DOCUMENT_BYTES = 1024 * 1024;
const ENVELOPE_BYTES = 1536 * 1024;
const SHA = /^[0-9a-f]{40}$/;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const EDITABLE = ['title', 'status', 'priority', 'assigned_to', 'due', 'area', 'depends_on', 'instructions'];
const NOTE_MARKER = '<!-- platform-board-operation:';
const encoder = new TextEncoder();
const fail = (code = 'board_invalid_request', status = 400) => {
  throw new SecurityError(code, status, {
    board_invalid_request: 'Use valid board fields and the current board version.',
    board_conflict: 'The board changed. Refresh it and review your changes.',
    board_dependency_pending: 'Complete the task dependencies before advancing its status.',
    board_unavailable: 'The project board is unavailable or not configured.',
    board_too_large: 'The board document exceeds its size limit.',
    board_actor_denied: 'A verified human account is required to change the board.',
    board_invalid_document: 'The stored board could not be validated.',
  }[code]);
};

function object(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
function fields(value, allowed) {
  if (!object(value) || Object.keys(value).some(key => !allowed.includes(key))) fail();
}
function validBranch(branch) {
  return typeof branch === 'string' && branch.length <= 200 && /^[A-Za-z0-9][A-Za-z0-9._/-]*$/.test(branch)
    && !branch.includes('..') && !branch.includes('//') && !branch.endsWith('/')
    && branch.split('/').every(part => !part.startsWith('.') && !part.endsWith('.') && !part.endsWith('.lock'));
}
function configuration(env) {
  if (!env || !['staging', 'verified'].includes(env.BOARD_READY)
    || typeof env.BOARD_REPO !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9-]{0,38}\/[A-Za-z0-9_.-]{1,100}$/.test(env.BOARD_REPO)
    || ['.', '..'].includes(env.BOARD_REPO.split('/')[1]) || !validBranch(env.BOARD_BRANCH)
    || (env.BOARD_READY === 'staging' && env.BOARD_BRANCH !== 'board/staging')
    || typeof env.BOARD_GITHUB_TOKEN !== 'string' || !/^github_pat_[A-Za-z0-9_]{20,255}$/.test(env.BOARD_GITHUB_TOKEN)) {
    fail('board_unavailable', 503);
  }
  return { repo: env.BOARD_REPO, branch: env.BOARD_BRANCH, token: env.BOARD_GITHUB_TOKEN };
}
export function boardReady(env) {
  try { configuration(env); return true; } catch { return false; }
}
function boardPath(kind) {
  if (!Object.hasOwn(PATHS, kind)) fail();
  return PATHS[kind];
}
function validDate(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value)) return false;
  const date = new Date(`${value}T00:00:00.000Z`);
  return Number.isFinite(date.getTime()) && date.toISOString().slice(0, 10) === value;
}
function taskNumber(value) {
  if (typeof value !== 'string' || !/^T-\d{3,9}$/.test(value)) return null;
  const number = Number(value.slice(2));
  return number > 0 && `T-${String(number).padStart(3, '0')}` === value ? number : null;
}
function plainText(value, maxBytes = DOCUMENT_BYTES, allowEmpty = false) {
  return typeof value === 'string' && value.isWellFormed() && (allowEmpty || value.trim().length > 0)
    && encoder.encode(value).byteLength <= maxBytes && !/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/u.test(value);
}
function inputText(value, maxBytes) {
  if (!plainText(value, maxBytes)) fail();
  return value.replace(/\r\n?/g, '\n').trim();
}
function fieldValid(key, value, incoming = false) {
  switch (key) {
    case 'title': return plainText(value, incoming ? 640 : DOCUMENT_BYTES) && (!incoming || [...value].length <= 160) && !/[\r\n\t]/.test(value);
    case 'status': return BOARD_STATUSES.includes(value);
    case 'priority': return BOARD_PRIORITIES.includes(value);
    case 'assigned_to': return value === null || BOARD_ROSTER.includes(value);
    case 'due': return value === null || validDate(value);
    case 'area': return BOARD_AREAS.includes(value);
    case 'depends_on': return Array.isArray(value) && value.length <= (incoming ? 100 : 2000)
      && value.every(id => taskNumber(id) !== null) && new Set(value).size === value.length;
    case 'instructions': return plainText(value, incoming ? 6000 : DOCUMENT_BYTES);
    default: return false;
  }
}
function validateTasks(document) {
  if (!object(document) || !Array.isArray(document.tasks) || document.tasks.length > 2000) fail('board_invalid_document', 503);
  const byId = new Map();
  for (const task of document.tasks) {
    if (!object(task) || taskNumber(task.id) === null || byId.has(task.id)
      || EDITABLE.some(key => !fieldValid(key, task[key])) || !BOARD_ROSTER.includes(task.assigned_by)
      || !validDate(task.assigned_on) || !Array.isArray(task.log)
      || task.log.some(line => !object(line) || !validDate(line.on) || !BOARD_ROSTER.includes(line.by) || !plainText(line.text))) {
      fail('board_invalid_document', 503);
    }
    byId.set(task.id, task);
  }
  for (const task of document.tasks) {
    if (task.depends_on.some(id => id === task.id || !byId.has(id))) fail('board_invalid_document', 503);
  }
  return byId;
}
function dependenciesValid(tasks, targetId, dependencies) {
  const byId = new Map(tasks.map(task => [task.id, task]));
  if (dependencies.some(id => id === targetId || !byId.has(id))) fail();
  const visited = new Set();
  const pending = [...dependencies];
  while (pending.length) {
    const id = pending.pop();
    if (id === targetId) fail();
    if (visited.has(id)) continue;
    visited.add(id);
    pending.push(...(byId.get(id)?.depends_on ?? []));
  }
}
function stable(value) {
  if (Array.isArray(value)) return value.map(stable);
  if (object(value)) return Object.fromEntries(Object.keys(value).sort().map(key => [key, stable(value[key])]));
  return value;
}
function equal(left, right) { return JSON.stringify(stable(left)) === JSON.stringify(stable(right)); }
function normalizeOperation(kind, body) {
  if (!object(body) || !SHA.test(body.expected_sha ?? '') || !UUID.test(body.operation_id ?? '')) fail();
  if (kind === 'notes') {
    fields(body, ['expected_sha', 'operation_id', 'to', 'text']);
    if (!Array.isArray(body.to) || !body.to.length || body.to.length > BOARD_ROSTER.length
      || body.to.some(handle => handle !== 'all' && !BOARD_ROSTER.includes(handle))
      || new Set(body.to).size !== body.to.length || (body.to.includes('all') && body.to.length !== 1)) fail();
    const text = inputText(body.text, 6000);
    // Metadata is server-owned; users cannot forge a replay marker or note author.
    if (text.includes(NOTE_MARKER) || /^## \d{4}-\d{2}-\d{2} \d{2}:\d{2} UTC · /m.test(text)) fail();
    return { action: 'note', to: [...body.to], text };
  }
  if (body.action === 'create') {
    fields(body, ['action', 'expected_sha', 'operation_id', 'task']);
    fields(body.task, EDITABLE.filter(key => key !== 'status'));
    const task = { priority: 'normal', assigned_to: null, due: null, depends_on: [], ...body.task };
    if (Object.keys(task).some(key => !fieldValid(key, task[key], true)) || !Object.hasOwn(task, 'title')
      || !Object.hasOwn(task, 'area') || !Object.hasOwn(task, 'instructions')) fail();
    task.title = task.title.trim();
    task.instructions = inputText(task.instructions, 6000);
    return { action: 'create', task };
  }
  fields(body, ['action', 'expected_sha', 'operation_id', 'task_id', 'changes', 'base', 'log']);
  if (body.action !== 'update' || taskNumber(body.task_id) === null) fail();
  fields(body.changes, EDITABLE);
  const changes = { ...body.changes };
  if (Object.keys(changes).some(key => !fieldValid(key, changes[key], true))) fail();
  const compared = Object.keys(changes).filter(key => key !== 'status');
  fields(body.base ?? {}, compared);
  if (compared.some(key => !Object.hasOwn(body.base ?? {}, key) || !fieldValid(key, body.base[key]))) fail();
  if (Object.hasOwn(changes, 'title')) changes.title = changes.title.trim();
  if (Object.hasOwn(changes, 'instructions')) changes.instructions = inputText(changes.instructions, 6000);
  const log = body.log === undefined ? null : inputText(body.log, 2000);
  if ((!Object.keys(changes).length && !log) || (changes.status === 'dropped' && !log)) fail();
  return { action: 'update', task_id: body.task_id, changes, base: body.base ?? {}, log };
}

async function discard(response) {
  try { await response.body?.cancel(); } catch { /* Provider response bodies never become user-visible errors. */ }
}
async function responseJson(response) {
  if (!response.headers.get('Content-Type')?.toLowerCase().startsWith('application/json')) {
    await discard(response); fail('board_invalid_document', 503);
  }
  if (!response.body) fail('board_invalid_document', 503);
  const reader = response.body.getReader();
  const chunks = [];
  let bytes = 0;
  try {
    for (;;) {
      const next = await reader.read();
      if (next.done) break;
      bytes += next.value.byteLength;
      if (bytes > ENVELOPE_BYTES) { await reader.cancel(); fail('board_too_large', 503); }
      chunks.push(next.value);
    }
    const buffer = new Uint8Array(bytes);
    let offset = 0;
    for (const chunk of chunks) { buffer.set(chunk, offset); offset += chunk.byteLength; }
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(buffer));
  } catch (error) {
    if (error instanceof SecurityError) throw error;
    fail('board_invalid_document', 503);
  } finally { reader.releaseLock(); }
}
function encoded(bytes) {
  let binary = '';
  for (let offset = 0; offset < bytes.length; offset += 8192) binary += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
  return btoa(binary);
}
function decoded(value) {
  if (typeof value !== 'string' || /[^A-Za-z0-9+/=\r\n]/.test(value)) fail('board_invalid_document', 503);
  const compact = value.replace(/[\r\n]/g, '');
  if (compact.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(compact)) fail('board_invalid_document', 503);
  let binary;
  try { binary = atob(compact); } catch { fail('board_invalid_document', 503); }
  if (binary.length > DOCUMENT_BYTES) fail('board_too_large', 503);
  if (btoa(binary) !== compact) fail('board_invalid_document', 503);
  return Uint8Array.from(binary, char => char.charCodeAt(0));
}
async function github(config, path, method, fetchImpl, body) {
  const url = new URL(`https://api.github.com/repos/${config.repo}/contents/${path}`);
  if (method === 'GET') url.searchParams.set('ref', config.branch);
  let response;
  try {
    response = await fetchImpl(url.href, {
      method, redirect: 'error', signal: AbortSignal.timeout(10_000),
      headers: { Accept: 'application/vnd.github+json', Authorization: `Bearer ${config.token}`,
        'X-GitHub-Api-Version': API_VERSION, 'User-Agent': 'platform-board-worker', ...(body ? { 'Content-Type': 'application/json' } : {}) },
      ...(body ? { body: JSON.stringify(body) } : {}),
    });
  } catch { fail('board_unavailable', 503); }
  if (response.status === 409 && method === 'PUT') { await discard(response); return null; }
  if (response.status !== 200) { await discard(response); fail('board_unavailable', 503); }
  return responseJson(response);
}
async function readDocument(config, kind, fetchImpl) {
  const path = boardPath(kind);
  const result = await github(config, path, 'GET', fetchImpl);
  if (!object(result) || result.type !== 'file' || result.path !== path || result.encoding !== 'base64'
    || !SHA.test(result.sha ?? '') || !Number.isSafeInteger(result.size) || result.size < 0
    || result.target !== undefined || result.submodule_git_url !== undefined) fail('board_invalid_document', 503);
  if (result.size > DOCUMENT_BYTES) fail('board_too_large', 503);
  const bytes = decoded(result.content);
  if (bytes.byteLength !== result.size) fail('board_invalid_document', 503);
  let text, document;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    if (kind === 'tasks') document = JSON.parse(text);
  } catch { fail('board_invalid_document', 503); }
  if (kind === 'tasks') validateTasks(document);
  else if (!plainText(text, DOCUMENT_BYTES, true)) fail('board_invalid_document', 503);
  return { sha: result.sha, text, document };
}
export async function readBoard(env, kind, { fetchImpl = fetch } = {}) {
  boardPath(kind);
  const current = await readDocument(configuration(env), kind, fetchImpl);
  return kind === 'tasks' ? { sha: current.sha, tasks: current.document.tasks } : { sha: current.sha, notes: current.text };
}

function replay(current, kind, actor, operationId, operationHash) {
  const matches = [];
  if (kind === 'tasks') {
    for (const task of current.document.tasks) {
      for (const line of task.log) {
        if (line.operation_id === operationId) matches.push({ actor: line.by, hash: line.operation_hash, id: task.id });
      }
    }
  } else {
    const pattern = /^<!-- platform-board-operation: ([0-9a-f-]{36}) ([a-z-]+) ([0-9a-f]{64}) -->\r?$/gm;
    for (const match of current.text.matchAll(pattern)) {
      if (match[1] === operationId) matches.push({ actor: match[2], hash: match[3], id: operationId });
    }
  }
  if (!matches.length) return null;
  if (matches.length !== 1 || matches[0].actor !== actor || matches[0].hash !== operationHash) fail('board_conflict', 409);
  return { sha: current.sha, [kind === 'tasks' ? 'task_id' : 'id']: matches[0].id, operation_id: operationId, replayed: true };
}
function changedDocument(current, kind, actor, operation, operationId, operationHash, timestamp) {
  const date = timestamp.slice(0, 10);
  if (kind === 'notes') {
    const header = `## ${timestamp.slice(0, 16).replace('T', ' ')} UTC · ${actor} → ${operation.to.join(', ')}`;
    const text = `${header}\n${NOTE_MARKER} ${operationId} ${actor} ${operationHash} -->\n${operation.text}\n\n${current.text}`;
    return { text, id: operationId, message: `board: note (${actor})` };
  }
  const document = structuredClone(current.document);
  let task, text;
  if (operation.action === 'create') {
    const number = Math.max(0, ...document.tasks.map(task => taskNumber(task.id))) + 1;
    if (number > 999_999_999 || document.tasks.length >= 2000) fail('board_too_large', 503);
    task = { id: `T-${String(number).padStart(3, '0')}`, ...operation.task, status: 'todo', assigned_by: actor, assigned_on: date, log: [] };
    dependenciesValid(document.tasks, task.id, task.depends_on);
    document.tasks.push(task);
    text = 'created';
  } else {
    task = document.tasks.find(task => task.id === operation.task_id);
    if (!task) fail('board_conflict', 409);
    for (const key of Object.keys(operation.changes).filter(key => key !== 'status')) {
      if (!equal(task[key], operation.base[key])) fail('board_conflict', 409);
    }
    const oldStatus = task.status;
    const newDependencies = operation.changes.depends_on ?? task.depends_on;
    dependenciesValid(document.tasks, task.id, newDependencies);
    const newStatus = operation.changes.status ?? oldStatus;
    if (newStatus !== oldStatus && ['in_progress', 'review', 'done'].includes(newStatus)
      && newDependencies.some(id => document.tasks.find(item => item.id === id).status !== 'done')) fail('board_dependency_pending', 409);
    const assigned = Object.hasOwn(operation.changes, 'assigned_to') && operation.changes.assigned_to !== task.assigned_to;
    Object.assign(task, operation.changes);
    if (assigned) { task.assigned_by = actor; task.assigned_on = date; }
    text = newStatus !== oldStatus ? `status: ${oldStatus} → ${newStatus}`
      : Object.keys(operation.changes).length ? `updated: ${Object.keys(operation.changes).join(', ')}` : 'note';
    if (operation.log) text += `; ${operation.log}`;
  }
  task.log.push({ on: date, by: actor, text, operation_id: operationId, operation_hash: operationHash });
  return { text: JSON.stringify(document, null, 2) + '\n', task_id: task.id, message: `board: ${task.id} ${task.status} (${actor})` };
}
export async function mutateBoard(env, kind, actor, body, { fetchImpl = fetch, now = Date.now(), authorize } = {}) {
  const path = boardPath(kind), config = configuration(env);
  if (!HUMAN_ACTORS.has(actor)) fail('board_actor_denied', 403);
  if(typeof authorize!=='function')fail('board_actor_denied',403);
  async function currentAuthority() {
    let allowed;
    try {allowed=await authorize();} catch {fail('board_unavailable',503);}
    if(allowed!==true)throw new SecurityError('login_required',401,'Your account session changed. Sign in again.');
  }
  await currentAuthority();
  const operation = normalizeOperation(kind, body);
  if (!Number.isSafeInteger(now) || now < 0 || now > 253_402_300_799_999) fail();
  const timestamp = new Date(now).toISOString();
  const operationHash = digestToken(JSON.stringify(stable({ kind, actor, operation })));
  let current = await readDocument(config, kind, fetchImpl);
  for (let attempt = 0; attempt < 2; attempt++) {
    const completed = replay(current, kind, actor, body.operation_id, operationHash);
    if (completed) return completed;
    if (attempt === 0 && current.sha !== body.expected_sha) {
      // A stale caller snapshot consumes the same single conflict retry as a PUT
      // race. Re-read once before merging; a further conflict requires a refresh.
      current = await readDocument(config, kind, fetchImpl);
      continue;
    }
    // expected_sha identifies the caller's snapshot. If stale, append-only actions
    // and status changes merge; other fields must still equal the supplied base.
    const changed = changedDocument(current, kind, actor, operation, body.operation_id, operationHash, timestamp);
    const bytes = encoder.encode(changed.text);
    if (bytes.byteLength > DOCUMENT_BYTES) fail('board_too_large', 503);
    // A slow remote read or conflict retry must not use a revoked app session.
    await currentAuthority();
    const result = await github(config, path, 'PUT', fetchImpl, {
      message: changed.message, content: encoded(bytes), branch: config.branch, sha: current.sha,
    });
    if (result !== null) {
      if (!object(result) || !object(result.content) || result.content.path !== path || result.content.type !== 'file'
        || !SHA.test(result.content.sha ?? '') || !SHA.test(result.commit?.sha ?? '')) fail('board_invalid_document', 503);
      return { sha: result.content.sha, [kind === 'tasks' ? 'task_id' : 'id']: changed.task_id ?? changed.id,
        operation_id: body.operation_id, replayed: false, commit_sha: result.commit.sha };
    }
    if (attempt === 0) current = await readDocument(config, kind, fetchImpl);
  }
  fail('board_conflict', 409);
}
