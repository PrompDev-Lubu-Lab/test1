# Read-only artifact API

Node 24 LTS target, small HTTP adapter and pinned `ws` transport. Implements `docs/API.md` run, instance, persisted validation and event routes. No authentication is supplied by this service: production must put it behind the Worker and an isolated Cloudflare Tunnel. No bot command or write endpoint exists.

```sh
npm ci --ignore-scripts
npm test
npm run dev
```

Development binds `127.0.0.1:8788` and explicitly reads `../fixtures/synthetic`. Every response declares `X-Data-Source: synthetic-fixtures`. For the real server set `RUNS_ROOT` to its approved read-only mounted artifact directory and use `npm start`. It refuses to guess a data root. Override `BIND_HOST` only for an isolated container network, never a published origin port. Configure exact `ALLOWED_HOSTS` and `ALLOWED_ORIGINS` for the approved proxy; defaults permit only local development.

JSON artifact bodies and raw NDJSON journal lines are preserved; legacy nanosecond numbers are never parse/stringified. CSV rows retain source column names and strings. Missing optional research artifacts return 404. `/runs` always returns exact summary objects from every run directory, with pagination; `/runs/index` separately returns the sweep index CSV rows. A starting instance may have `status`, `mode` and `label` set to null before the bot's first status flush. Fable approved source fidelity independently with zero fixture-comparison failures; his operational boundary findings are covered by regression tests.

Safety bounds: direct child identifiers only; symlink/junction and file-type checks; Linux `O_NOFOLLOW`; bounded 16 MiB artifact/response reads; at most 5000 directories, 500 items per listing page, 10,000 CSV/journal rows per page, 32 WebSocket clients and 1 MiB backpressure. Roots over 500 directories return `X-Result-Warning`. Listing/CSV pagination uses `after` and `limit`, with `X-Next-After` and `X-Has-More`. Equity `every=N` preserves first/last source rows before pagination. Client requests cannot change any bound. Root and parent directories must be controlled by the trusted server operator; read-only container mounts are required.

Journals may exceed 16 MiB in total. Pages use 64 KiB buffered reads, cap individual lines at 1 MiB and preserve complete NDJSON bytes. A page returns `X-Next-Offset`; use it as `offset` on the next request for efficient deep pagination. It must point just after a newline. `offset` and legacy line-count `after` are mutually exclusive; byte-cursor responses omit the unavailable global `X-Next-After`. Legacy `after` scans at most 256 MiB per request. A cursor beyond a rotated file returns 409; restart at zero. Incomplete trailing lines are withheld without advancing past them.

The service polls directory/file metadata every two seconds, so bot temp-file-plus-rename writes are observed. Event contents follow the contract. The frontend also polls on reconnection and derives staleness from the real timestamp; frozen fixtures naturally become stale. It must not interpret a stale frozen sample as an online bot. Real container-stop and external-port tests remain pending until an authorized server is supplied.
