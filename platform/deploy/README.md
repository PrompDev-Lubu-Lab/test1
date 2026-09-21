# Server deployment preparation

These are templates, not a deployed server. The operator must confirm the existing server/SSH authority, run directory, tunnel application, service identity and approved origin first. They add only the read API and outbound connector; the bot deployment is unchanged.

`compose.example.yml` has no host ports, a read-only artifact mount and read-only API filesystem. The API is attached only to an internal Docker network; the connector additionally has outbound access. The protected tunnel's exact Host header must be allowlisted. Access must authenticate the Worker-to-origin service as well as the human frontend perimeter. Do not expose an unauthenticated tunnel hostname.

Select and record tested image digests before production, including the Node base image currently represented by the major-version template. Provision the Cloudflare-issued tunnel secret outside git and restrict it to the connector. Do not copy exchange keys into these services. Use the existing Cloudflare/GitHub secret mechanisms and the approved server provisioning path; no secret belongs in a peer message.

Before declaring test 11 passed, verify `docker compose config`, read-only mount behavior and the actual absence of any published API listener, then test an outside connection against the real host. A successful local API request or a syntactically valid Compose file cannot prove the remote firewall. Preserve the prior deployment and roll back only these app services when needed.
