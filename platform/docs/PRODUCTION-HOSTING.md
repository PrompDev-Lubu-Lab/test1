# Single-origin production hosting

The web interface, `/api/*`, authenticated WebSocket and protected update feed share one approved HTTPS origin. `worker/site.mjs` validates the human Access assertion before any static asset is read. The account handler performs its existing ingress, identity, session, terms and role checks for API requests. Unprefixed account routes are not exposed by the production entry point.

## Configuration

Keep the actual host, account IDs, database IDs, audience and two invitation addresses in an ignored `wrangler.local.json`. Keep server credentials, Turnstile verification secret and rate-limit key in Worker secrets. Do not place secrets in configuration files, the static build, desktop ASAR, logs or the board.

Build the presentation with the exact private `APP_ORIGIN`. Bind its `dist` directory as `ASSETS`, set `run_worker_first` to `true` globally, and set both `html_handling` and `not_found_handling` to `none`. The wrapper maps only `/` to `/index.html`. Unknown paths stay 404 and API requests never fall back to HTML. Keep `workers_dev` and `preview_urls` false.

Create the dedicated Access application for the entire hostname before attaching the Worker custom domain. It may initially have no allow policy, which denies everyone. Preserve unrelated Access applications, DNS records, apex MX/SPF and mailbox routing. The Access authorization cookie must be HTTP Only for the reviewed desktop cookie bridge. Human sign-in and the account's own password/session checks remain independent gates.

Apply migration 0001 once to the new D1 database. Export before any later schema migration. Provision private avatar and release buckets with public access disabled. The initial configuration leaves avatars, events, board writes, downloads and `RUN_ORIGIN` disabled. Enable each capability only after its corresponding deployed acceptance check; an empty bucket is not a published release.

## Deployment and acceptance

1. Run the Worker tests, build the web assets, and dry-run the exact private configuration. Obtain peer review of the source commit and record it.
2. Deploy that commit with the approved custom domain and all optional features disabled. Record the Worker version ID privately; post only public-safe evidence to the board.
3. Verify unauthenticated requests to the root, scripts, API, WebSocket path and update feed are denied by Access. Verify the Worker independently rejects missing and forged assertions. Confirm alternate Worker hosts remain disabled.
4. After the intended human signs in, check protected HTML and CSP, `/api/config` with features false, `/api/me` without an app session, exact-origin mutation rejection, and an unknown asset 404. Record real user/browser evidence separately from local tests.
5. Verify the approved sender and a real test delivery before owner setup. The operator-only seed creates a one-use invitation digest, never a password or fabricated Access identity. The invited human chooses their password and completes verification and terms.
6. Complete the remaining acceptance rows before describing the app or native installer as ready. Signing, installed upgrade verification, board staging round-trip and the authorized read-only server connection remain independent checks.

Rollback uses the previously recorded Worker version with a compatible D1 schema. Do not roll back or delete account data to recover a code deployment. A rollback must preserve the hostname-wide Access gate and private storage.

References: [Worker-first routing](https://developers.cloudflare.com/workers/static-assets/routing/worker-script/), [asset binding settings](https://developers.cloudflare.com/workers/static-assets/binding/), [self-hosted Access applications](https://developers.cloudflare.com/cloudflare-one/access-controls/applications/http-apps/self-hosted-public-app/).
