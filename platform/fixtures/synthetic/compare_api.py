#!/usr/bin/env python3
"""G3 fidelity check: compares a running read-only run API against the
fixture files it serves. Every JSON file must come back byte-identical
(after JSON re-parse, key set and values equal, money still strings);
every CSV route must return the file's rows with every cell as the
source string; the journal must come back byte-identical; instances must
carry the heartbeat's raw status text.

    node platform/api/server.mjs --fixtures &   # serves platform/fixtures/synthetic
    python3 platform/fixtures/synthetic/compare_api.py http://127.0.0.1:8788

Exit code 0 when every check passes, 1 otherwise. Pure standard library.
"""
import csv, json, os, sys, urllib.request

BASE = sys.argv[1].rstrip('/') if len(sys.argv) > 1 else 'http://127.0.0.1:8788'
ROOT = os.path.dirname(os.path.abspath(__file__))
failures = []

def get(path):
    req = urllib.request.Request(BASE + path, headers={'Host': BASE.split('//')[1]})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, r.headers, r.read()

def check(name, ok, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ': ' + detail))
    if not ok: failures.append(name)

def file(run, name):
    with open(os.path.join(ROOT, run, name), 'rb') as f: return f.read()

def csv_rows(raw):
    return list(csv.DictReader(raw.decode().splitlines()))

def has_only_string_money(obj, path='$'):
    # Every key that the bot writes as money must still be a string.
    money = {'equity','cash','realized_pnl_net','unrealized_pnl','fees','position','mark','initial_cash',
             'final_equity','max_drawdown','turnover','volume','peak_equity','drawdown','price','quantity',
             'fee','filled','remaining','gross_profit','gross_loss','net_pnl','expectancy','average_win',
             'average_loss','largest_win','largest_loss','total_fees','open_quantity'}
    bad = []
    def walk(o, p):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in money and not isinstance(v, str) and not (k == 'max_drawdown' and p.endswith(('returns','benchmark'))):
                    bad.append(f'{p}.{k}={v!r}')
                walk(v, f'{p}.{k}')
        elif isinstance(o, list):
            for i, v in enumerate(o): walk(v, f'{p}[{i}]')
    walk(obj, path)
    return bad

runs = sorted(d for d in os.listdir(ROOT) if os.path.isdir(os.path.join(ROOT, d)) and os.path.exists(os.path.join(ROOT, d, 'summary.json')))
instances = sorted(d for d in os.listdir(ROOT) if os.path.isdir(os.path.join(ROOT, d)) and os.path.exists(os.path.join(ROOT, d, 'heartbeat')))

# /runs
status, _, body = get('/runs')
listing = json.loads(body)
check('/runs status 200', status == 200)
check('/runs lists every run directory', sorted(r['run_id'] for r in listing) == runs, f'{sorted(r["run_id"] for r in listing)} vs {runs}')
for r in listing:
    is_index = 'recorded_at' in r
    check(f'/runs row {r["run_id"]} shape', is_index != ('max_drawdown_fraction' in r), 'row is neither an index row nor a summary object')
    if not is_index:
        check(f'/runs row {r["run_id"]} equals summary.json', r == json.loads(file(r['run_id'], 'summary.json')))

for run in runs:
    status, headers, body = get(f'/runs/{run}')
    obj = json.loads(body)
    check(f'/runs/{run} summary equals file', obj['summary'] == json.loads(file(run, 'summary.json')))
    metrics_path = os.path.join(ROOT, run, 'metrics.json')
    if os.path.exists(metrics_path):
        check(f'/runs/{run} metrics equals file', obj['metrics'] == json.loads(file(run, 'metrics.json')))
    check(f'/runs/{run} config equals file', obj['config'] == file(run, 'config.txt').decode())
    check(f'/runs/{run} money stays string', not has_only_string_money(obj), ', '.join(has_only_string_money(obj))[:300])
    check(f'/runs/{run} X-Data-Source header', headers.get('X-Data-Source') in ('synthetic-fixtures', 'bot-artifacts'))
    for name in ['equity', 'fills', 'orders', 'metrics', 'round_trips']:
        fname = name + '.csv'
        if not os.path.exists(os.path.join(ROOT, run, fname)): continue
        status, _, body = get(f'/runs/{run}/{name}')
        rows = json.loads(body); expected = csv_rows(file(run, fname))
        check(f'/runs/{run}/{name} rows equal CSV', rows == expected, f'{len(rows)} rows vs {len(expected)}; first diff: ' + next((f'{a} != {b}' for a, b in zip(rows, expected) if a != b), 'count'))
        check(f'/runs/{run}/{name} every cell is a string', all(isinstance(v, str) for row in rows for v in row.values()))
    for name, fname in [('validation', 'validation.json'), ('drift', 'drift.json')]:
        if os.path.exists(os.path.join(ROOT, run, fname)):
            status, _, body = get(f'/runs/{run}/{name}')
            check(f'/runs/{run}/{name} byte-identical', body == file(run, fname))
    for name, fname in [('report', 'report.txt')]:
        if os.path.exists(os.path.join(ROOT, run, fname)):
            status, _, body = get(f'/runs/{run}/{name}')
            check(f'/runs/{run}/{name} byte-identical', body == file(run, fname))
    # Downsampling keeps endpoints and only drops rows.
    status, _, body = get(f'/runs/{run}/equity?every=100')
    sampled = json.loads(body); full = csv_rows(file(run, 'equity.csv'))
    check(f'/runs/{run}/equity?every=100 keeps first and last', sampled[0] == full[0] and sampled[-1] == full[-1] and all(s in full for s in sampled))

# /instances
status, _, body = get('/instances')
listing = json.loads(body)
check('/instances lists every heartbeat directory', sorted(i['id'] for i in listing) == instances, f'{sorted(i["id"] for i in listing)} vs {instances}')
for inst in listing:
    i = inst['id']
    hb = file(i, 'heartbeat').decode().strip()
    check(f'/instances {i} heartbeat time and status raw', hb == f"{inst['heartbeat']['time']} {inst['heartbeat']['status']}", f'{hb!r} vs {inst["heartbeat"]!r}')
    check(f'/instances {i} status equals status.json', inst['status'] == json.loads(file(i, 'status.json')))
    check(f'/instances {i} mode/label from status', inst['mode'] == inst['status']['mode'] and inst['label'] == inst['status']['label'])
    for name, fname in [('status', 'status.json'), ('state', 'state.json'), ('metrics.prom', 'metrics.prom')]:
        status, _, body = get(f'/instances/{i}/{name}')
        check(f'/instances/{i}/{name} byte-identical', body == file(i, fname))
    status, headers, body = get(f'/instances/{i}/journal')
    check(f'/instances/{i}/journal byte-identical NDJSON', body == file(i, 'journal.jsonl') and headers.get('Content-Type', '').startswith('application/x-ndjson'))
    n = len(file(i, 'journal.jsonl').decode().splitlines())
    check(f'/instances/{i}/journal X-Next-After', headers.get('X-Next-After') == str(n) and headers.get('X-Has-More') == 'false', f'{headers.get("X-Next-After")} {headers.get("X-Has-More")}')

print(f'\n{len(failures)} failure(s)')
sys.exit(1 if failures else 0)
