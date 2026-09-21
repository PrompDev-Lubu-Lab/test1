# Authenticated board adapter

The app writes only `board/tasks.json` and `board/notes.md` through the GitHub Contents API. The Worker owns the repository, branch and credential settings. Neither a form nor an agent can supply another path, branch, author or token. The current canonical board writer remains Fable; no staging change is merged into the planning branch.

## Configuration and rollout

Keep `BOARD_READY` absent until its scope is ready. The staging configuration requires `BOARD_READY=staging`, `BOARD_BRANCH=board/staging`, `BOARD_REPO=owner/repository` and `BOARD_GITHUB_TOKEN` as a Cloudflare Worker secret. The token must be a fine-grained credential restricted to the one approved repository with Contents read/write and required metadata access, an expiry, and an operator rotation record. The app does not accept a broad CLI token or expose any token to the browser. The prefix check is not proof of the credential's actual scopes; inspect those before activation. Branch permissions are enforced by this Worker, while the credential itself is repository-scoped.

Create `board/staging` from the agreed planning head after the adapter review. Use synthetic text only. Prove both directions: a signed-in app change appears in that branch, and a Fable git change appears after app refresh. Trigger a concurrent-file conflict deliberately and verify both entries survive. Do not merge the test branch. After the mainline planning PR is merged, changing the operator branch configuration to the canonical branch is a separate reviewed action. `BOARD_READY=verified` permits an explicitly configured canonical branch after that step.

## HTTP interface

All routes require a verified Access subject, a current app session and accepted terms. POST also requires the exact application Origin, session CSRF and the separate authenticated-write allowance. The actor is the current user's board handle (`deandre` or `ali`), never a request field. The app uses the existing account client and keeps drafts/operation IDs only in memory. Sign-out or an account boundary disposes the drafts.

| Route | Result / operation |
| --- | --- |
| GET `/api/board/tasks` | `{sha,tasks}` from the fixed tasks file |
| GET `/api/board/notes` | `{sha,notes}`; notes are original text |
| POST `/api/board/tasks` | Create, edit, status, assignment, due date, dependencies and appended log |
| POST `/api/board/notes` | Insert a new server-attributed note after the preamble and before the first history heading |

Each mutation supplies `expected_sha` and a random UUIDv4 `operation_id`. Task create supplies `action:create` and editable task fields; the Worker allocates the next task ID, initial status, dates and attribution. Update supplies `action:update`, `task_id`, `changes`, original `base` values for changed non-status fields and an optional appended `log`. Existing history and inherited fields are retained. Dependencies must exist, cannot form a cycle and must be done before a dependent task advances. Dropping a task requires a reason. Notes supply roster recipients `to` and plain `text`. New notes are inserted after the exact file preamble and before its first second-level heading, or appended when no such heading exists. The original bytes remain in order. Task instructions may contain up to 16 KiB; a board POST alone has a 64 KiB JSON envelope so both the old and new instruction text fit. Authentication bodies retain their 8 KiB limit. Operation hashing has its own bounded document digest and does not reuse the 2048-character credential-token helper.

## Concurrent writes and uncertain responses

The adapter uses the expected file SHA, one reread/merge retry and a second expected-SHA PUT. It reallocates colliding create IDs, preserves concurrent additions and merges status changes. A changed text field must still match its original base or the app receives 409 for explicit review. There is no blind overwrite. A still-current app session is checked before the initial read and immediately before each remote write; a revocation during a slow read returns 401 without a PUT.

The task log or note marker records the operation ID and a digest of its normalized actor/action/payload. Retrying an unchanged, unconfirmed operation returns its prior receipt; changing its actor or payload cannot reuse that ID. The UI keeps an uncertain draft and ID for retry, and requires refresh plus explicit review after 409. Returning a receipt never asserts a real staging test unless that actual branch was inspected.

GitHub JSON is streamed with a 1.5 MiB envelope bound, strict metadata/canonical base64/UTF-8 checks and a 1 MiB decoded file bound. Requests cannot grow a board beyond that bound. Remote redirects and error details are rejected or sanitized. Task/history text is escaped; note history uses textContent and does not render HTML. The board is public source: the UI tells authors to exclude passwords and private account information.

Local evidence: 25 adapter tests and signed-JWT/SQLite route tests exercise fixed scope, concurrent writers, idempotency, attribution, CSRF, terms and revocation. No real staging token was configured or GitHub round trip claimed by these tests.

References: [GitHub Contents API](https://docs.github.com/en/rest/repos/contents), [fine-grained token permissions](https://docs.github.com/en/rest/authentication/permissions-required-for-fine-grained-personal-access-tokens).
