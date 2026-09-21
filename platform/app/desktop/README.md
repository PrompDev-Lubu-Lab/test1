# Desktop settings

Copy `settings.example.json` to an ignored private settings file. The release script accepts its absolute path through `PLATFORM_DESKTOP_CONFIG`. Never add secrets to settings or commit real operational origins.

| Setting | Meaning |
| --- | --- |
| `appOrigin` | Exact HTTPS application origin with no port, path or trailing slash. |
| `accessOrigin` | Exact dedicated `https://TEAM.cloudflareaccess.com` origin. |
| `externalOrigins` | Small explicit list of HTTPS origins allowed in the system browser. |
| `channel` | `review` for a separate unsigned development identity; `production` for the approved deployment. |
| `updatesEnabled` | Production only, after protected delivery and signing validation. |
| `publisherNames` | Exact signer certificate simple/common name, obtained with `GetNameInfo(SimpleName, false)`, such as `Example Publisher`. Do not use the full `CN=..., O=..., C=...` subject. |
| `certificateThumbprints` | Uppercase 40-character SHA-1 thumbprints of the approved current and next signer certificates. Installer contents are separately bound by SHA-512. |

The normalized policy is embedded in the integrity-protected application archive. Feed and cache locations are derived from it. See the [release runbook](../../docs/DESKTOP-RELEASES.md) for packaging, certificate rotation, publication and the remaining real acceptance checks.
