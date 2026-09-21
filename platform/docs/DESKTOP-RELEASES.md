# Desktop and protected releases

M5 source is prepared for Windows x64. Signing, deployment, real Access login and the installed 0.1.0 to 0.1.1 upgrade remain separate acceptance gates. This document contains public examples only; the private operational inventory contains the actual hosts and Cloudflare resource locations.

## Runtime and ownership

The native host loads the approved HTTPS app in a sandboxed BrowserWindow. It shares the web shell, wizard, terms, scene, footer and eight tabs. No C++ code, bot process, exchange key, risk setting or writable run mount is included. Fable owns the bot and canonical board; Astra owns this host, Worker, app CI and release preparation. The display name comes from `app/public/config.js`; stable package/cache identifiers do not change when the display name changes.

`desktop/settings.example.json` is a closed review example. Private settings select exact app and Access origins, a small external-link allowlist, channel, update enablement, publisher names and certificate thumbprints. Credentials never belong in this file. The packaging script validates it and embeds `settings.generated.json` inside the integrity-protected ASAR. Do not move this policy to a mutable external resource. Generated/private settings are ignored by Git.

Packaged Windows builds require OS protected storage. Packaging enables cookie encryption and ASAR integrity and disables RunAsNode, NODE_OPTIONS, inspector arguments and extra file-protocol privileges. Unpackaged development uses an in-memory session; it must not persist cookies through a stock Electron binary without the encryption fuse. Development has a separate app identity and user-data directory. The preload exposes presentation metadata only, with no credentials, filesystem calls, shell commands or general IPC.

Navigation permits the configured app, the configured Access team's `/cdn-cgi/access/` routes and GitHub login/session routes. New windows are denied; explicit allowed external links use the system browser. Permissions, webviews and arbitrary browser downloads are denied. Offline content tells the user to reconnect and never presents old live status as current. Actual Access/GitHub/Turnstile screens still need human browser QA; do not bypass browser security warnings or challenges to obtain it.

## Exact stable update contract

Pins: Electron 44.2.0, electron-builder 26.15.3 and electron-updater 6.8.9. Do not implement APIs from updater 7 development documentation against this lockfile.

The first feed is same-origin at `/api/updates/windows/x64/`. An optional separate updates hostname is a later decision; host-only cookies are not copied to another host. A private R2 binding named `RELEASES` contains only these delivery shapes:

| Object | Consumer |
| --- | --- |
| `releases/windows/x64/catalog.json` | Worker `/api/downloads`, authenticated web Downloads tab |
| `releases/windows/x64/latest.yml` | Native generic updater manifest |
| `releases/windows/x64/clawdie-platform-X.Y.Z-win-x64.exe` | Complete NSIS installer |

Set `DOWNLOADS_READY=verified` only after the protected staging round trip. The Worker verifies Access, the current app session and accepted terms. Its module requires an authorization callback and repeats it after R2 metadata reads. It uses an ETag conditional read, fixed filenames, bounded bodies and `private, no-store` responses. It never lists keys, generates public/signed URLs or redirects downloads. GET and HEAD are supported. Range/If-Range return 416; differential updates are disabled. Existing browser download streams are bounded but are not a continuous revocation monitor.

The updater's stable `netSession` is getter-only and does not automatically share the app session. Main owns its request hooks and, after a successful live `/api/me` preflight, obtains only the current `CF_Authorization` and `__Host-platform-session` cookies for each allowed request. It never retains cookie values in configuration, URLs, logs or renderer IPC. Foreign origins, paths, methods, duplicate cookies and every redirect are rejected. Sign-out/account changes cancel the operation; long downloads recheck the server session every ten seconds. Each authorization/network result belongs to its own operation, so an old completion cannot cancel a newer attempt. Access renewal takes place in the window, with no background retry loop.

There is no signed-manifest API in 6.8.9. Integrity relies on protected HTTPS delivery, the manifest SHA-512, and Windows Authenticode. The strict verifier binds expected size/hash, file identity and native signature to the same file snapshot. It requires Valid trust, a timestamp, an exact approved publisher and a certificate thumbprint from the configured allowlist; unavailable tools and malformed output deny installation. Include current and next approved certificates during rotation. Native cache hits do not skip the final verification. The app-session receipt must still be current after the final file check, no older than twenty seconds, and earlier than both the signed Access and app-session expiries returned by `/api/me`.

Downloads and installation each require explicit native confirmation. No automatic download/install or downgrade occurs. A newer stable numeric version and one relative complete installer filename are required. After verification, main spawns only that exact installer with fixed `--updated --force-run` arguments and quits only after a successful spawn event. It does not invoke the stock updater's elevation or shell-association fallbacks. NSIS is per-user with no elevation helper. Launching the installer is not proof that installation or relaunch succeeded.

## Build and verification

Run from `platform/app` using Node 24 and the committed lockfile:

```sh
npm ci --ignore-scripts --no-fund
npm run check
npm test
npm run package:review
node scripts/verify-package.mjs --review
```

Review packages use the example origin, a Development title and `UNSIGNED-DEVELOPMENT.exe` name; updates are disabled. They are packaging evidence, not a usable production inbox or trading platform. Do not install one over an existing user application as part of a source check.

`.github/workflows/platform-desktop.yml` exposes explicit review and signed preparation jobs. Signed preparation runs only on `main` and the `platform-production-signing` environment. Before adding secrets, configure required reviewers and main-only environment deployment rules. Store approved `DESKTOP_SETTINGS_JSON`, `WINDOWS_CSC_LINK` and `WINDOWS_CSC_KEY_PASSWORD` in that GitHub environment. The private policy must name the approved current/next certificate pins. No certificate purchase is authorized by this source. The build refuses absent signing credentials and `forceCodeSigning` is required.

`verify-package.mjs` reads the real executable fuse wire and ASAR, checks version/entrypoint/policy and excludes private/development source. For a signed release it verifies both application and installer signatures, the packaged publisher/feed policy, and the native manifest's relative filename, checksum and size. It produces a bounded catalogue and verification record. GitHub retains reviewable artifacts; the workflow does not silently publish them.

## Controlled publication

The release operator is the sole writer of the private R2 release prefix. Confirm the approved account and bucket from the private inventory; preserve the previous feed/catalogue and all versioned installers. Check that the new version exceeds the published one and that its versioned object does not already exist. Never overwrite a versioned installer. The runbook deliberately keeps publication separate until actual account access and signed upgrade evidence exist.

Use the pinned Wrangler 4.135.0 toolchain with a scoped Cloudflare credential stored in the approved GitHub environment or Cloudflare secret facility. Use structured argument arrays or direct native shell arguments; do not construct shell strings from release notes. The publication sequence is:

1. Verify the signed artifact bundle and the exact source commit reviewed by Fable.
2. Upload the new versioned installer with `wrangler r2 object put BUCKET/releases/windows/x64/FILE.exe --file LOCAL_FILE --remote`. Read it back and compare size/SHA-512 before updating any pointer.
3. Upload the verified native `latest.yml`, then `catalog.json`; preserve prior copies under the release's audit record. A catalogue failure cannot introduce an unsigned installer, but must be reported as a partial publication.
4. Check authenticated GET/HEAD through the app origin, signed-out denial, expired Access, revoked app session, terms denial, strict cross-origin/redirect denial and exact download checksum.
5. Install the earlier signed version on the agreed test machine, accept the offered newer update, then verify the relaunched executable's installed version and normal account/data flow. Record actual evidence on the board before marking acceptance test 10 complete.

R2's public development URL and custom public domain must remain disabled. Do not publish through GitHub Releases or a public bucket as an authentication workaround. The app/API Worker, protected Pages host and `/api/*` route precedence need their own deployment check before serving real accounts. The repository currently has no approved `main` branch or signing secrets, so the signed workflow is intentionally gated.

## Failure, rollback and revocation

Withdraw a bad feed/catalogue first. Since installed clients reject equal/lower versions, recovery for an already upgraded client is forward-only: rebuild the corrected earlier code under a higher version, sign it and publish it. Keep old versioned installers for audit/recovery; do not restore a lower feed and call that an installed rollback. Never disable signature checking to recover a release.

A client version/header is not an identity proof. A future minimum-version/426 response can enforce compatibility only. Account revocation remains in Worker Access/session policy. Certificate loss or expiry without a previously distributed next pin may require a separately verified manual installer; do not add a network-supplied trust pin.

macOS Developer ID/notarization and Linux packaging/checksums remain separate native lanes. Their installed update tests are pending; no Windows result certifies them. No release is published or installed merely because source, tests or an unsigned package pass.

Sources: [stable updater HTTP/session implementation](https://github.com/electron-userland/electron-builder/blob/electron-updater%406.8.9/packages/electron-updater/src/electronHttpExecutor.ts), [stable NSIS verifier and installer](https://github.com/electron-userland/electron-builder/blob/electron-updater%406.8.9/packages/electron-updater/src/NsisUpdater.ts), [Electron request hooks](https://www.electronjs.org/docs/latest/api/web-request), [R2 Workers API](https://developers.cloudflare.com/r2/api/workers/workers-api-reference/), [R2 CLI](https://developers.cloudflare.com/r2/reference/wrangler-commands/).

Local evidence: `desktop-review-results.json` records the built unsigned111566442-byte NSIS package and actual fuse/archive checks. `downloads-runtime-results.json` records18 real local workerd/R2 checks. Installer GET is correctly streamed without Content-Length in that runtime; HEAD returns the size. These are not production or installed-upgrade results.
