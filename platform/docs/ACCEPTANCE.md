# Acceptance evidence

Cycle `COLLAB-20260921`. Status must be Pass, Fail or Pending with the environment and evidence. Local fixtures, local signed-token tests, deployed endpoints and real human workflows are separate evidence. No real-market profitability claim is made.

| # | Required outcome | Current state | Evidence needed / owner |
| --- | --- | --- | --- |
| 1 | Invite Ali; unverified login rejected; verification single-use and expiring | Pending | Worker integration tests and real invite/link delivery/sign-in; Astra + Ali |
| 2 | Ten wrong passwords lock login and record audit events | Pending | Atomic per-IP/account rate test including concurrent attempts; Astra |
| 3 | Member cannot call owner-only APIs directly | Pending | Correctly signed Access/session member denied on each privileged route; Astra/Fable review |
| 4 | Password reset revokes old sessions | Pending | One-use reset, session-version revocation and real reset delivery; Astra |
| 5 | Avatar rejects non-images/over 2 MB; output 128 px square | Pending | Valid decode/size boundary/private access tests and rendered avatar; Astra |
| 6 | Terms block access and save version/time | Pending | Server gate and D1 receipt; local wizard alone is insufficient; Astra |
| 7 | Runs UI exactly matches summary/metrics fixtures | Pending | API key/value comparisons, exact decimal strings and visual UI comparison; Astra/Fable |
| 8 | Stopped heartbeat becomes stale within 45 seconds | Pending | Controlled local test plus later real paper-container stop; Astra + human server operator |
| 9 | Board app commit visible in git, agent git update visible in app | Pending | Real authorized round trip and expected-SHA conflict test; Astra/Fable |
| 10 | Actual installed desktop updates from v0.1.0 to v0.1.1 | Pending | Signed feed/artifacts, installation, update, relaunch and version evidence per supported platform; Astra + user |
| 11 | Direct outside connection to origin API port fails | Pending | Real server firewall/container/Tunnel evidence and external denial; authorized server operator |

Additional foundation checks and their exact commands will be appended after implementation. No milestone is complete until its applicable rows pass or the user explicitly changes scope.
