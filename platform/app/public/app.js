import { APP_NAME, APP_VERSION, TABS } from './config.js';
import { formatFraction as fraction, runDrawdownFraction, runLabel } from './format.js';

const $ = id => document.getElementById(id);
const escape = value => String(value ?? '—').replace(/[&<>"']/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
const glyphs = {
  overview:'M3 3h7v7H3zM14 3h7v7h-7zM3 14h7v7H3zM14 14h7v7h-7z',
  live:'M2 12h4l3-8 5 16 3-8h5', runs:'M4 4h16v16H4zM8 8h8M8 12h8M8 16h5',
  research:'M10 3v6l-6 10a1.3 1.3 0 0 0 1.2 2h13.6a1.3 1.3 0 0 0 1.2-2L14 9V3M8 3h8M7 15h10',
  tasks:'m3 6 2 2 3-4M11 6h10m-18 7 2 2 3-4M11 13h10M4 20h3m4 0h10',
  notes:'M5 3h14v18H5zM9 7h6M9 11h6M9 15h4', downloads:'M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5',
  settings:'M3 7h4m4 0h10M3 17h10m4 0h4M11 7a2 2 0 1 1-4 0 2 2 0 1 1 4 0m6 10a2 2 0 1 1-4 0 2 2 0 1 1 4 0',
  refresh:'M20 7v5h-5M4 17v-5h5M6 7a7 7 0 0 1 12-1l2 6M4 12l2 6a7 7 0 0 0 12-1',
  shield:'m12 2 8 3v6c0 5-4 8-8 11-4-3-8-6-8-11V5zM8 11l3 3 5-6',
  lock:'M5 10h14v11H5zM8 10V6a4 4 0 0 1 8 0v4M12 14v3',
  sun:'M12 2v2M12 20v2M2 12h2m16 0h2M5 5l1.5 1.5M17.5 17.5 1.5 1.5M5 19l1.5-1.5M17.5 6.5 1.5-1.5M17 12a5 5 0 1 1-10 0 5 5 0 1 1 10 0',
  arrow:'M4 12h16m-6-6 6 6-6 6', clock:'M12 8v5l3 2M22 12a10 10 0 1 1-20 0 10 10 0 1 1 20 0',
  chart:'M4 3v17h17M7 15l4-5 4 2 5-7', folder:'M3 6V4h7l3 3h8v14H3z',
};
const icon = name => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="${glyphs[name] || glyphs.overview}"/></svg>`;
document.querySelectorAll('[data-icon]').forEach(el => { el.innerHTML = icon(el.dataset.icon); });
document.querySelectorAll('[data-app-name]').forEach(el => { el.textContent = APP_NAME; });
document.title = `${APP_NAME} · Private workspace`;
$('footer-version').textContent = `${APP_VERSION} Beta`;
$('navigation').innerHTML = TABS.map(name => `<button class="nav" type="button" data-tab="${name.toLowerCase()}">${icon(name.toLowerCase())}<span>${name}</span></button>`).join('');

let runtime = { syntheticPreview: false, access: 'closed' }, entered = false, loading = false;
let currentView = 'overview', selectedRun = '', selectedInstance = '', viewRevision = 0, runRevision = 0;
const data = { runs: [], instances: [], details: null, equity: [], errors: [], runsAvailable: false, instancesAvailable: false };
const descriptions = {
  overview:'Saved runs and instance snapshots, together.', live:'Observe the reported state of an instance.',
  runs:'Read the outputs behind each result.', research:'Inspect completed analysis and its limitations.',
  tasks:'A shared board for the project and its agents.', notes:'Decisions, handoffs and the next useful action.',
  downloads:'Desktop builds and their verification state.', settings:'Your account, access and workspace preferences.',
};
const titles = Object.fromEntries(TABS.map(name => [name.toLowerCase(), name]));
const time = value => {
  if (!value) return 'Not recorded';
  const date = new Date(value);
  return Number.isFinite(date.getTime()) ? new Intl.DateTimeFormat('en-AU', { dateStyle:'medium', timeStyle:'short', timeZone:'UTC' }).format(date) + ' UTC' : String(value);
};
// Decimal money and quantity fields are never parsed for text display.
const exact = value => value === null || value === undefined ? '—' : String(value);
async function api(path, options = {}) {
  const response = await fetch(`/api${path}`, { cache:'no-store', credentials:'same-origin', signal:AbortSignal.timeout(12000) });
  if (!response.ok) {
    const result = await response.json().catch(() => ({}));
    throw new Error(result.detail || (response.status === 404 ? 'This output is not available.' : `Data is unavailable (HTTP ${response.status}).`));
  }
  const body = await (options.text ? response.text() : response.json());
  if (options.withMeta) return {body,dataSource:response.headers.get('X-Data-Source'),nextAfter:response.headers.get('X-Next-After'),hasMore:response.headers.get('X-Has-More') === 'true'};
  return body;
}
const tag = text => `<span class="tag">${escape(text)}</span>`;
const button = (text, tab, style = 'text-button') => `<button class="${style}" data-tab="${tab}">${escape(text)}${icon('arrow')}</button>`;
function empty(title, detail, glyph = 'folder', label = 'NOT AVAILABLE') {
  return `<section class="panel empty-state"><span class="empty-icon">${icon(glyph)}</span>${tag(label)}<h2>${escape(title)}</h2><p>${escape(detail)}</p></section>`;
}
function stat(label, value, note, glyph = 'chart', tone = '') {
  return `<div class="panel stat-card"><div class="stat-label"><span>${escape(label)}</span>${icon(glyph)}</div><div class="stat-value number ${tone}">${escape(value)}</div><p class="stat-note">${escape(note)}</p></div>`;
}
function kv(items) {
  return `<dl class="key-values">${items.map(([label,value]) => `<div><dt>${escape(label)}</dt><dd class="number">${escape(value)}</dd></div>`).join('')}</dl>`;
}
function table(headers, rows) {
  return `<div class="table-wrap"><table><thead><tr>${headers.map(h => `<th scope="col">${escape(h)}</th>`).join('')}</tr></thead><tbody>${rows.map(row => `<tr>${row.map(cell => `<td>${cell}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`;
}
function runPicker() {
  return `<label class="select-field run-selector">SELECTED RUN<select id="run-select">${data.runs.map(run => `<option value="${escape(run.run_id)}"${run.run_id === selectedRun ? ' selected' : ''}>${escape(runLabel(run))}</option>`).join('')}</select></label>`;
}
function chart(rows) {
  const samples = rows.map(row => ({ date:new Date(row.time).getTime(), value:Number(row.equity) })).filter(point => Number.isFinite(point.date) && Number.isFinite(point.value));
  if (samples.length < 2) return `<div class="empty-state"><p>No equity curve is available for this run.</p></div>`;
  const low = Math.min(...samples.map(point => point.value)), high = Math.max(...samples.map(point => point.value));
  const delta = Math.max(high - low, Math.abs(low) * 0.002, 1), min = low - delta * 0.13, max = high + delta * 0.13;
  const start = samples[0].date, end = samples.at(-1).date;
  const x = point => 50 + (point.date - start) / Math.max(1,end - start) * 585;
  const y = point => 178 - (point.value - min) / (max - min) * 158;
  const line = samples.map(point => `${x(point).toFixed(2)},${y(point).toFixed(2)}`).join(' ');
  const grid = [0,.5,1].map(ratio => { const value = min + ratio * (max - min), yy = 178 - ratio * 158; return `<line class="chart-gridline" x1="50" x2="635" y1="${yy}" y2="${yy}"/><text x="40" y="${yy + 3}" text-anchor="end">${escape(value.toFixed(0))}</text>`; }).join('');
  const dateLabel = point => new Intl.DateTimeFormat('en-AU',{ day:'2-digit',month:'short',timeZone:'UTC' }).format(point.date);
  return `<figure class="chart-wrap"><svg class="equity-chart" viewBox="0 0 650 208" role="img" aria-label="Equity across the selected synthetic run. Chart positions are approximate; exact amounts are shown beside it.">${grid}<polygon class="chart-area" points="50,178 ${line} 635,178"/><polyline class="chart-line" points="${line}"/><text x="50" y="201">${escape(dateLabel(samples[0]))}</text><text x="635" y="201" text-anchor="end">${escape(dateLabel(samples.at(-1)))}</text></svg><figcaption class="chart-caption">Equity · approximate plot positions · exact decimal amounts retained in the records. UTC.</figcaption></figure>`;
}
function curvePanel() {
  const s = data.details?.summary;
  if (!s) return '';
  return `<section class="panel"><div class="panel-head"><div><h2>Equity through the run</h2><p>${escape(s.symbol)} · ${escape(time(s.from))} to ${escape(time(s.to))}</p></div>${tag('SYNTHETIC')}</div><div class="chart-grid">${chart(data.equity)}${kv([['Initial cash',exact(s.initial_cash)],['Final equity',exact(s.final_equity)],['Realized P&L, net',exact(s.realized_pnl_net)],['Fees',exact(s.fees)],['Maximum drawdown',exact(s.max_drawdown)]])}</div></section>`;
}
function instancesPanel() {
  return `<section class="panel"><div class="panel-head"><div><h2>Instance snapshots</h2><p>Frozen fixture state</p></div>${button('View all','live')}</div>${data.instances.length ? `<div class="instance-list">${data.instances.slice(0,3).map(instance => `<div class="instance-item"><span class="instance-icon">${icon('live')}</span><div class="instance-copy"><strong>${escape(instance.label || instance.id)}</strong><p>${escape(instance.mode || 'Unknown mode')} · synthetic snapshot</p></div><div class="instance-value"><strong class="number">${escape(exact(instance.status?.equity))}</strong><p>Reported equity</p></div></div>`).join('')}</div>` : `<div class="panel-body"><p class="muted">${data.instancesAvailable ? 'No instance snapshots were returned.' : 'The instance API is unavailable.'}</p></div>`}<p class="subtle-note">These records do not represent a running bot. The displayed mode and feed state are fields from the fixture.</p></section>`;
}
function overview() {
  const s = data.details?.summary;
  return `<div class="overview-grid"><section class="panel hero-panel"><div><span class="overline">RESEARCH WORKSPACE</span><h2>A clear view of every run.</h2></div><p>Follow instance state, inspect saved results, and keep the next decision connected to its evidence.</p><div class="hero-meta"><div>${icon('folder')}<span>${data.runsAvailable ? data.runs.length : '—'} saved runs</span></div><div>${icon('live')}<span>${data.instancesAvailable ? data.instances.length : '—'} instance snapshots</span></div></div>${button('Explore the runs','runs','primary')}</section>${instancesPanel()}</div><div class="stat-grid">${stat('Selected run return',fraction(s?.total_return),'Synthetic sample · not live performance','chart',typeof s?.total_return === 'number' && s.total_return < 0 ? 'negative' : 'positive')}${stat('Final equity',exact(s?.final_equity),s?.symbol || 'No run selected','folder')}${stat('Maximum drawdown',fraction(s?.max_drawdown_fraction),'Fraction from summary.json','chart')}${stat('Fills',exact(s?.fills),'Completed fills in selected run','tasks')}</div>${curvePanel()}${runList()}`;
}
function runList() {
  if (!data.runs.length) return empty('No saved runs to show',data.runsAvailable ? 'The Run API returned no summary files.' : 'Start the read-only Run API and refresh to inspect the sample runs.','runs');
  return `<section class="panel"><div class="panel-head"><div><h2>Saved runs</h2><p>Source results from the Run API</p></div>${tag(`${data.runs.length} RUNS`)}</div>${table(['Run','Period (UTC)','Return','Max drawdown','Round trips'],data.runs.map(run => [`<button class="row-link" data-run="${escape(run.run_id)}">${escape(runLabel(run))}<small>${escape(run.params || 'Saved run')}</small></button>`,`${escape(String(run.from || '').slice(0,10))} — ${escape(String(run.to || '').slice(0,10))}`,`<span class="number ${Number(run.total_return) < 0 ? 'negative' : 'positive'}">${escape(fraction(run.total_return))}</span>`,escape(fraction(runDrawdownFraction(run))),escape(exact(run.round_trips))]))}</section>`;
}
async function runs() {
  if (!selectedRun) return runList();
  const s = data.details?.summary;
  let fills = [], missing = '', fillsHasMore = false;
  try { const page = await api(`/runs/${encodeURIComponent(selectedRun)}/fills?after=0&limit=100`,{withMeta:true}); fills = page.body; fillsHasMore = page.hasMore; } catch (error) { missing = error.message; }
  return `<section class="panel"><div class="panel-head"><div><h2>Run detail</h2><p>${escape(selectedRun)}</p></div>${runPicker()}</div><div class="panel-body">${kv([['Symbol',s?.symbol],['Recorded start',time(s?.from)],['Recorded finish',time(s?.to)],['Seed',s?.seed],['Strategies',s?.strategies?.map(item => item.label).join(', ') || '—']])}</div></section>${curvePanel()}<section class="panel"><div class="panel-head"><div><h2>Fills</h2><p>Original decimal strings, as recorded</p></div>${tag(Array.isArray(fills) ? `${fills.length} FILLS` : 'UNAVAILABLE')}</div>${Array.isArray(fills) && fills.length ? table(['Time (UTC)','Side','Price','Quantity','Fee','Liquidity'],fills.slice(0,100).map(fill => [escape(time(fill.time)),escape(fill.side),escape(exact(fill.price)),escape(exact(fill.quantity)),escape(exact(fill.fee)),escape(fill.liquidity)])) : `<p class="subtle-note">${escape(missing || 'No fills were recorded for this run.')}</p>`}${fillsHasMore ? '<p class="subtle-note">Showing the first 100 fills. More records are available in the paginated API.</p>' : ''}</section>${runList()}`;
}
async function live() {
  if (!data.instances.length) return empty('No instance is connected',data.instancesAvailable ? 'No heartbeat directories were returned by the Run API.' : 'The instance API is unavailable. Connection details remain read only.','live');
  const instance = data.instances.find(item => item.id === selectedInstance) || data.instances[0];
  selectedInstance = instance.id;
  const s = instance.status || {};
  const [state,journal] = await Promise.allSettled([api(`/instances/${encodeURIComponent(instance.id)}/state`),api(`/instances/${encodeURIComponent(instance.id)}/journal?after=0&limit=100`,{text:true,withMeta:true})]);
  const ledger = state.status === 'fulfilled' ? state.value : null;
  return `<section class="panel"><div class="panel-head"><div><h2>${escape(instance.label || instance.id)}</h2><p>Frozen synthetic snapshot · ${escape(time(s.time || instance.heartbeat?.time))}</p></div><label class="select-field">INSTANCE<select id="instance-select">${data.instances.map(item => `<option value="${escape(item.id)}"${item.id === instance.id ? ' selected' : ''}>${escape(item.label || item.id)}</option>`).join('')}</select></label></div><div class="panel-body">${kv([['Mode field',s.mode || instance.mode],['Feed state field',s.feed?.state || 'Not recorded'],['Heartbeat text',instance.heartbeat?.status || 'Not recorded'],['Heartbeat age (API)',instance.heartbeat?.age_seconds === undefined ? 'Not available' : `${instance.heartbeat.age_seconds} seconds`]])}</div><p class="subtle-note">A healthy field in an old snapshot is not a current health check. There are no start, stop, live-mode or risk-limit controls.</p></section><div class="stat-grid">${stat('Equity',exact(s.equity),'Reported snapshot value','chart')}${stat('Cash',exact(s.cash),'Exact decimal string','folder')}${stat('Position',exact(s.position),'Reported quantity','live')}${stat('Open orders',exact(s.open_orders),'Snapshot count','tasks')}</div><div class="two-column"><section class="panel"><div class="panel-head"><h2>Runtime observations</h2>${tag('FROZEN')}</div><div class="panel-body">${kv([['Uptime',s.uptime],['Fees',exact(s.fees)],['Realized P&L, net',exact(s.realized_pnl_net)],['Orders',s.orders],['Rejected',s.rejected],['Kill-switch trips',s.kill_switch_trips],['Feed silence',s.feed?.silence]])}</div></section><section class="panel"><div class="panel-head"><h2>Account state</h2>${tag(ledger ? 'SOURCE RECORD' : 'UNAVAILABLE')}</div><div class="panel-body">${ledger ? `<pre class="raw-record">${escape(JSON.stringify(ledger,null,2))}</pre>` : '<p class="muted">The state output is not available.</p>'}</div></section></div><section class="panel"><div class="panel-head"><h2>Journal</h2><span class="muted">Original event fields</span></div><div class="panel-body">${journal.status === 'fulfilled' ? `<pre class="raw-record">${escape(journal.value.body)}</pre>` : '<p class="muted">The journal is not available.</p>'}</div>${journal.status === 'fulfilled' && journal.value.hasMore ? '<p class="subtle-note">Showing the first 100 journal lines. More records are available in the paginated API.</p>' : ''}<p class="subtle-note">Raw NDJSON is displayed unchanged. Legacy nanosecond integers are never parsed or reformatted.</p></section>`;
}
async function research() {
  const results = selectedRun ? await Promise.allSettled([api(`/runs/${encodeURIComponent(selectedRun)}/drift`),api(`/runs/${encodeURIComponent(selectedRun)}/revalidation`,{text:true}),api(`/runs/${encodeURIComponent(selectedRun)}/validation`)]) : [];
  const validation = results[2]?.status === 'fulfilled' ? results[2].value : null;
  const metrics = data.details?.metrics || validation?.report;
  return `<section class="panel"><div class="panel-head"><div><h2>Analysis for the selected run</h2><p>Availability follows the files the bot has written.</p></div>${data.runs.length ? runPicker() : ''}</div><div class="research-list">${[['Performance analysis',metrics ? 'Available in metrics.json' : 'No metrics.json was returned'],['Drift check',results[0]?.status === 'fulfilled' ? 'Available in drift.json' : 'No drift output for this run'],['Revalidation',results[1]?.status === 'fulfilled' ? 'Available in revalidation.txt' : 'No revalidation output for this run'],['Validation',validation ? 'Available in validation.json' : 'No validation output for this run'],['Sweep / walk-forward / Monte Carlo',validation ? 'Only completed sections are included in the validation record below' : 'Run validation to persist the selected analysis sections']].map(([name,status]) => `<div class="research-item"><div><strong>${escape(name)}</strong><p>${escape(status)}</p></div>${tag(status.startsWith('Available') ? 'AVAILABLE' : 'NOT AVAILABLE')}</div>`).join('')}</div></section>${metrics ? `<div class="stat-grid">${stat('Sharpe',exact(metrics.returns?.sharpe),'Value from metrics.json','chart')}${stat('Round trips',exact(metrics.trades?.round_trips),'Completed trade analysis','tasks')}${stat('Win rate',fraction(metrics.trades?.win_rate),'Fraction from metrics.json','chart')}${stat('Net P&L',exact(metrics.trades?.net_pnl),'Exact decimal string','folder')}</div>` : empty('Analysis has not been supplied','This interface does not invent research metrics when an output is missing.','research')}${validation ? `<section class="panel"><div class="panel-head"><div><h2>Validation record</h2><p>Only sections written by the bot are shown. A passing sample is not a live-trading decision.</p></div>${tag(validation.verdict?.pass === true ? 'RECORDED PASS' : validation.verdict?.pass === false ? 'RECORDED FAIL' : 'SOURCE RECORD')}</div><div class="panel-body"><pre class="raw-record">${escape(JSON.stringify(validation,null,2))}</pre></div></section>` : ''}<div class="two-column">${results[0]?.status === 'fulfilled' ? `<section class="panel"><div class="panel-head"><h2>Drift record</h2></div><div class="panel-body"><pre class="raw-record">${escape(JSON.stringify(results[0].value,null,2))}</pre></div></section>` : ''}${results[1]?.status === 'fulfilled' ? `<section class="panel"><div class="panel-head"><h2>Revalidation report</h2></div><div class="panel-body"><pre class="raw-record">${escape(results[1].value)}</pre></div></section>` : ''}</div>`;
}
async function board(kind) {
  let rows;
  try { rows = await api(`/board/${kind}`); } catch { return empty(kind === 'tasks' ? 'The board connection is pending' : 'Shared notes will appear here','Tasks and notes are served by the authenticated Cloudflare Worker. Local synthetic preview does not pretend to be an agent or write to GitHub.',kind,'AUTHENTICATED WORKER REQUIRED'); }
  if (!Array.isArray(rows) || !rows.length) return empty(kind === 'tasks' ? 'No tasks were returned' : 'No notes were returned','The connected board contains no entries for this view.',kind,'READ ONLY');
  if (kind === 'tasks') return `<section class="panel"><div class="panel-head"><h2>Project tasks</h2>${tag('READ ONLY')}</div>${table(['Task','Status','Owner'],rows.map(task => [escape(`${task.id || ''} ${task.title || ''}`),escape(task.status),escape(task.owner)]))}</section>`;
  return rows.map(note => `<section class="panel"><div class="panel-head"><div><h2>${escape(note.from)}</h2><p>${escape(time(note.time))} · To ${escape(Array.isArray(note.to) ? note.to.join(', ') : note.to)}</p></div>${tag('READ ONLY')}</div><div class="panel-body"><p class="raw-record">${escape(note.text)}</p></div></section>`).join('');
}
function downloads() {
  return `<section class="panel empty-state"><span class="empty-icon">${icon('downloads')}</span>${tag('PREPARATION ONLY')}<h2>A desktop home for the same workspace.</h2><p>The Electron source shares this interface. A distributable installer, publisher signature and update feed have not been produced or verified.</p><button class="secondary" disabled>${icon('downloads')}Installer not published</button></section><section class="panel"><div class="panel-head"><h2>Release readiness</h2></div><div class="panel-body">${kv([['Shared interface','Prepared in source'],['Native host','Prepared; native execution not yet verified'],['Installer','Not built'],['Publisher signature','Not configured'],['Update feed','Not connected'],['Installed upgrade','Not tested']])}</div></section>`;
}
function settings() {
  return `<div class="two-column"><section class="panel"><div class="panel-head"><div><h2>Account & access</h2><p>Production account controls are unavailable.</p></div>${icon('shield')}</div><div class="setting-list">${[['Current access','Local synthetic preview; no authenticated user'],['Invitations','Owner and member accounts must be seeded by the Worker'],['Verification & password reset','Requires the account service and a verified email sender'],['Avatar & sessions','Private R2 storage and server-side session revocation are pending']].map(([label,detail]) => `<div class="setting-row"><div><strong>${label}</strong><p>${detail}</p></div>${icon('lock')}</div>`).join('')}</div></section><section class="panel"><div class="panel-head"><h2>Workspace</h2></div><div class="setting-list"><div class="setting-row"><div><strong>Weather & scene</strong><p>The footer changes the scene timezone for this tab. Live weather and saved preferences are not connected.</p></div><button class="secondary" id="open-scene-settings">Open</button></div><div class="setting-row"><div><strong>Terms</strong><p>Preview conditions are not a production acceptance receipt.</p></div><button class="secondary" id="review-terms">Review</button></div><div class="setting-row"><div><strong>Application version</strong><p>${escape(APP_VERSION)} · shared presentation foundation</p></div>${tag('BETA')}</div><div class="setting-row"><div><strong>Leave preview</strong><p>Return to the access screen. No account session is revoked because none was created.</p></div><button class="secondary" id="leave-preview">Leave</button></div></div></section></div>`;
}
async function renderView(focus = false) {
  if (!entered) return;
  const revision = ++viewRevision;
  const requested = location.hash.slice(1).toLowerCase();
  currentView = Object.hasOwn(titles,requested) ? requested : 'overview';
  $('view-title').textContent = titles[currentView];
  $('view-description').textContent = descriptions[currentView];
  $('view-eyebrow').textContent = currentView === 'live' ? 'SYNTHETIC INSTANCE SNAPSHOTS' : 'YOUR WORKSPACE';
  document.querySelectorAll('[data-tab]').forEach(button => { button.classList.toggle('on',button.dataset.tab === currentView); if (button.closest('nav')) button.setAttribute('aria-current',button.dataset.tab === currentView ? 'page' : 'false'); });
  $('view').innerHTML = '<div class="loading"><span class="pulse"></span>Reading available outputs…</div>';
  try {
    const body = await ({overview,live,runs,research,tasks:() => board('tasks'),notes:() => board('notes'),downloads,settings})[currentView]();
    if (revision !== viewRevision || !entered) return;
    const errors = data.errors.length && ['overview','runs','live','research'].includes(currentView) ? `<div class="error-strip">${icon('folder')}<p>${escape(data.errors.join(' '))}</p><button class="text-button" id="retry-data">Retry</button></div>` : '';
    $('view').innerHTML = errors + body;
    $('view').scrollTop = 0;
    if (focus) $('view-title').focus({preventScroll:true});
  } catch (error) { if (revision === viewRevision) $('view').innerHTML = empty('This view could not load',error.message,'folder'); }
}
async function loadRun() {
  const revision = ++runRevision, id = selectedRun;
  data.details = null; data.equity = [];
  if (!id) return;
  const [detail,equity] = await Promise.allSettled([api(`/runs/${encodeURIComponent(id)}`),api(`/runs/${encodeURIComponent(id)}/equity?every=4`)]);
  if (revision !== runRevision || id !== selectedRun) return;
  if (detail.status === 'fulfilled') data.details = detail.value;
  else data.errors.push('Selected run detail is unavailable.');
  if (equity.status === 'fulfilled' && Array.isArray(equity.value)) data.equity = equity.value;
}
async function refresh() {
  if (!entered || loading) return;
  loading = true; $('refresh-data').disabled = true;
  data.errors = [];
  const [runsResult,instancesResult] = await Promise.allSettled([api('/runs'),api('/instances')]);
  data.runsAvailable = runsResult.status === 'fulfilled' && Array.isArray(runsResult.value);
  data.instancesAvailable = instancesResult.status === 'fulfilled' && Array.isArray(instancesResult.value);
  data.runs = data.runsAvailable ? runsResult.value : [];
  data.instances = data.instancesAvailable ? instancesResult.value : [];
  if (!data.runsAvailable) data.errors.push('Saved runs are unavailable.');
  if (!data.instancesAvailable) data.errors.push('Instance snapshots are unavailable.');
  if (!data.runs.some(run => run.run_id === selectedRun)) selectedRun = data.runs.find(run => String(runLabel(run)).startsWith('ma_'))?.run_id || data.runs[0]?.run_id || '';
  await loadRun();
  $('checked-at').textContent = `Checked ${new Intl.DateTimeFormat('en-AU',{hour:'2-digit',minute:'2-digit'}).format(new Date())}`;
  loading = false; $('refresh-data').disabled = false;
  await renderView();
}
function showGate(stage = 'configure') {
  entered = false; viewRevision++; $('workspace').hidden = true; $('gate').hidden = false;
  window.CaseForgeEntry.show(stage,true);
}
function setScenePanel(open) {
  $('scene-panel').hidden = !open; $('scene-toggle').setAttribute('aria-expanded',String(open));
  if (open) $('scene-close').focus();
}
document.addEventListener('click',async event => {
  const tab = event.target.closest('[data-tab]');
  if (tab) { const next = tab.dataset.tab; if (location.hash === `#${next}`) await renderView(true); else location.hash = next; }
  const run = event.target.closest('[data-run]');
  if (run) { selectedRun = run.dataset.run; await loadRun(); if (location.hash === '#runs') await renderView(true); else location.hash = 'runs'; }
  const stage = event.target.closest('[data-stage]');
  if (stage) window.CaseForgeEntry.show(stage.dataset.stage,true);
  if (event.target.closest('#retry-data')) void refresh();
  if (event.target.closest('#leave-preview, #open-account, #review-access')) showGate();
  if (event.target.closest('#review-terms')) showGate('ready');
  if (event.target.closest('#open-scene-settings')) setScenePanel(true);
  if (!event.target.closest('#scene-controls') && !event.target.closest('#open-scene-settings')) setScenePanel(false);
});
document.addEventListener('change',async event => {
  if (event.target.id === 'run-select') { selectedRun = event.target.value; await loadRun(); await renderView(); }
  if (event.target.id === 'instance-select') { selectedInstance = event.target.value; await renderView(); }
});
$('refresh-data').addEventListener('click',() => void refresh());
window.addEventListener('hashchange',() => void renderView(true));
$('account-continue').addEventListener('click',async () => {
  if (!runtime.syntheticPreview) return;
  window.CaseForgeEntry.show('preferences',true);
  $('connection-status').textContent = 'Checking the read-only Run API…';
  try { const records = await api('/runs'); $('connection-status').textContent = Array.isArray(records) ? `${records.length} saved runs available. All preview data is synthetic.` : 'The API returned an unexpected response.'; }
  catch { $('connection-status').textContent = 'Run API not yet connected. You can enter the shell and retry when it is ready.'; }
});
$('connection-continue').addEventListener('click',() => window.CaseForgeEntry.show('ready',true));
let userScrolled = false;
$('terms-reader').addEventListener('scroll',() => {
  if ($('ready-stage').hidden) return;
  userScrolled = true;
  if ($('terms-reader').scrollTop + $('terms-reader').clientHeight >= $('terms-reader').scrollHeight - 6 && userScrolled && runtime.syntheticPreview) {
    $('enter-preview').disabled = false;
    $('terms-scroll-status').textContent = 'Preview conditions read. Entry creates no account.';
  }
});
$('enter-preview').addEventListener('click',async () => {
  if (!runtime.syntheticPreview || $('enter-preview').disabled) return;
  entered = true; $('gate').hidden = true; $('workspace').hidden = false;
  $('environment-label').textContent = 'Synthetic environment';
  await refresh(); $('view-title').focus({preventScroll:true});
});
$('scene-toggle').addEventListener('click',() => setScenePanel($('scene-panel').hidden));
$('scene-close').addEventListener('click',() => { setScenePanel(false); $('scene-toggle').focus(); });
document.addEventListener('keydown',event => { if (event.key === 'Escape' && !$('scene-panel').hidden) { setScenePanel(false); $('scene-toggle').focus(); } });
let sceneZone = 'Australia/Brisbane';
function updateClock() {
  $('footer-clock').textContent = new Intl.DateTimeFormat('en-AU',{timeZone:sceneZone,weekday:'short',hour:'2-digit',minute:'2-digit',hourCycle:'h23'}).format(new Date());
}
$('scene-zone').addEventListener('change',() => { sceneZone = $('scene-zone').value; window.CaseForgeScene?.setTimezone(sceneZone); updateClock(); });
window.CaseForgeScene?.setTimezone(sceneZone); updateClock();
const clockTimer = setInterval(updateClock,30000);
window.addEventListener('pagehide',() => clearInterval(clockTimer));
try {
  const response = await fetch('/runtime-config.json',{cache:'no-store'});
  if (response.ok) runtime = await response.json();
} catch { /* Production fails closed until its account service exists. */ }
$('account-continue').disabled = runtime.syntheticPreview !== true;
$('gate-status').textContent = runtime.syntheticPreview === true ? 'Local development preview is available. No sign-in or account creation takes place.' : 'Production access is closed. The account Worker and identity checks are not configured.';
if (runtime.syntheticPreview !== true) $('account-continue').textContent = 'Account service not yet available';
