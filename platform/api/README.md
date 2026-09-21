# Read-only artifact API

Node 24 LTS target, small HTTP adapter and pinned `ws` transport. Implements `docs/API.md` run, instance, persisted validation and event routes. No authentication is supplied by this service: production must put it behind the Worker and an isolated Cloudflare Tunnel. No bot command or write endpoint exists.

```sh
npm ci --ignore-scripts
npm test
npm run dev
```

Development binds `127.0.0.1:8788` and explicitly reads `../fixtures/synthetic`. Every response declares `X-Data-Source: synthetic-fixtures`. For the real server set `RUNS_ROOT` to its approved read-only mounted artifact directory and use `npm start`. It refuses to guess a data root. Override `BIND_HOST` only for an isolated container network, never a published origin port. Configure exact `ALLOWED_HOSTS` and `ALLOWED_ORIGINS` for the approved proxy; defaults permit only local development.

JSON artifact bodies and raw NDJSON journal lines are preserved; legacy nanosecond numbers are never parse/stringified. CSV rows retain source column names and strings. Missing optional research artifacts return 404. If no index.csv exists, `/runs` returns the exact summary objects, without fabricating unavailable index columns. G3 review must reconcile these CSV/index details with the broad type wording in the draft contract.

Safety bounds: direct child identifiers only; symlink/junction and file-type checks; Linux `O_NOFOLLOW`; bounded 16 MiB reads; at most 500 directories, 10,000 rows/page, 32 WebSocket clients, 1 MiB backpressure. CSV and journal pagination uses `after` and `limit`, with `X-Next-After` and `X-Has-More`. Equity `every=N` preserves first/last source rows before pagination. Client requests cannot change any bound. Root and parent directories must be controlled by the trusted server operator; read-only container mounts are required.

The service polls directory/file metadata every two seconds, so bot temp-file-plus-rename writes are observed. Event contents follow the contract. The frontend also polls on reconnection and derives staleness from the real timestamp; frozen fixtures naturally become stale. It must not interpret a stale frozen sample as an online bot. Real container-stop and external-port tests remain pending until an authorized server is supplied.
