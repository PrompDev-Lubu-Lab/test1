# Shared platform presentation

This is the T-006 / T-023 presentation foundation: the same plain HTML, CSS and
JavaScript interface for a browser and an isolated Electron host. It has the
eight requested tabs, an Account / Connect / Terms entry flow, and a clearly
labelled synthetic development environment.

## Run locally

Requires Node 22.16 or later. The browser application has no npm runtime
dependencies and can be run before installing Electron.

```powershell
cd platform/app
npm run dev
```

Open `http://127.0.0.1:8787`. The entry flow is explicit: Account → Connect →
Terms → scroll the terms pane to the end → Open synthetic preview. Entry is
in-memory for this tab. It creates no user, session or legal acceptance receipt.

The read-only Run API must already be listening on `http://127.0.0.1:8788`.
The presentation server forwards `GET /api/runs` to `GET /runs`, and similarly
for the allowlisted instance, run and board reads. It does not launch that API,
read a run directory, write run data or connect directly to an exchange.

If the API is down, the shell remains available with truthful unavailable
states and Retry. No generated data is fabricated by the interface.

`npm start` runs the same server with production access closed: every API read
returns `503 access_not_configured`. A query parameter cannot enable preview.
`PORT` may change the presentation port, but it always binds to `127.0.0.1`.

## Check and prepare

```powershell
npm run check
npm test
npm run build
```

The check validates JavaScript syntax and every imported asset hash. The four
server tests cover default access denial, bounded read-only proxying, decimal
string preservation, cross-origin denial, private-path rejection, validation
routing, bounded pagination, metadata headers and byte-exact legacy NDJSON. Two
presentation tests check summary versus index drawdown semantics and validated
fraction-string display without changing original source values. The build
copies the public interface into `dist/` and writes a closed runtime config;
it is not a deployment, installer or signed release.

The server supplies CSP, no-store, frame denial and no-referrer headers. A
production Worker serving the static build must supply equivalent headers and
the real account gate; merely uploading `dist` does not implement accounts.
Inline styles are allowed for the reused scene's geometry. Inline scripts are
not allowed.

## Electron source

After dependency installation:

```powershell
npm ci
npm run desktop:preview
```

The pinned host is Electron 44.2.0. `npm run desktop` leaves access closed;
`desktop:preview` enables synthetic preview only in an unpackaged development
process. Native execution, installer creation, signing, publication and actual
update installation have not been verified by this source task.

The host uses a separate application slug and in-memory browser partition,
sandbox, context isolation, no renderer Node integration, denied permissions,
blocked downloads and webviews, and a fixed HTTPS external-link allowlist.
The preload exposes presentation metadata only. There is no trading IPC,
credential handling, Codex runtime, case service or updater implementation.

## Data boundaries

- `public/config.js` owns the single `APP_NAME` display constant. The stable
  `APP_SLUG` is separate from the working display name.
- Overview, Runs, Live and Research read the exact endpoints in `docs/API.md`.
  Missing outputs remain unavailable. Money and quantity fields remain original
  decimal strings in text and tables. The SVG curve uses approximate numeric
  positions and is explicitly labelled; return fractions may be formatted as
  percentages, including validated fraction strings from the CSV index. A summary
  uses its explicit `max_drawdown_fraction`; its monetary `max_drawdown` is never
  formatted as a percentage. Missing round trips remain unavailable and are not
  inferred from fills. Summary labels use actual strategy labels where present.
- Synthetic instance records are labelled frozen snapshots. A `healthy` field
  in an old file is never promoted to a current healthy state.
- Journal reads request the first 100 lines as raw `application/x-ndjson` text.
  They never parse or re-serialize legacy nanosecond integer tokens. Fills are
  also bounded to 100 records. A visible notice identifies further pages;
  this source foundation has no interactive page navigation yet. The proxy
  preserves `X-Data-Source`, `X-Next-After` and `X-Has-More` and bounds
  `limit` to 1–10000, `after` to 0–1000000000 and `every` to 1–1000000.
- Research reads `validation.json` through `/runs/{id}/validation`, shows only
  the sections actually recorded, and labels the stored verdict as a recorded
  pass/fail. A passing synthetic validation is not a live-trading approval.
- Tasks and Notes read Worker endpoints if present. They expose no write
  controls in the local preview. Missing Worker routes are shown as pending.
- Account registration, verification, password reset, production consent,
  roles, avatars, server-side session revocation and Access integration require
  the platform Worker. This interface simulates none of those successes.
- The scene timezone changes this tab only. It is not saved. Live weather is
  not connected; there are no third-party weather requests.
- There is no trading start/stop action, live-mode switch, risk editor, exchange
  key storage or run-file write route.

## Case Forge presentation reuse

`source-manifest.json` records the source commit, exact allowlist and SHA256 of
each imported file. Source was copied from the separate clean `caseforge-source`
clone at `c8bea2a9fe3ad3a1702f7915f2712a604cc57cc9`, not the user's dirty checkout.
The imported source assets are unmodified. `public/styles.css` adapts their
layout; `public/app.js` provides new data and access behavior.

Reused: layered landscape, time-of-day palette, local Ubuntu/Quicksand fonts,
glass header styles, wizard progress/transitions, terms-reader styles and
scene-control styling. Preserve `licenses/CASE-FORGE-MIT.txt` and both font
licences in any distribution. No case documents, demos, profiles, output
directories, native case services or runtime credentials were imported.

## Remaining integration and QA

Browser verification is owned by the coordinating agent. Check all tabs,
ordinary desktop and 804×619 content, compact mobile, keyboard-only entry,
actual terms scrolling, day/night and reduced-motion layout, missing API
responses, exact source values and run switching. Then add real accounts and
authenticated Worker reads under the separate Worker task before enabling any
production access. Packaging and signed updates are separate release tasks.
