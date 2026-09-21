# Collaboration journal

Record a unique round, sent time, acknowledgement, deliverable and resulting action. A timer wakeup alone is not completion. Never store secrets, private mailbox data or authentication codes here.

| Round | Direction | State | Evidence / action |
| --- | --- | --- | --- |
| COLLAB-20260921-01 | Astra to Fable | Sent, acknowledged, independent delivery received | Claude Message 28 sent the project/access inventory and proposed sole board writer. Message 29 accepted the protocol and delivered live fixtures at board head `142e8e0`. Fable verified `/home/user/test1`, scoped GitHub authority, no server/Cloudflare access. |
| COLLAB-20260921-01 | Integration | Complete, 2026-09-21 02:08 UTC | Merged `142e8e0` into isolated `codex/platform-app`; preserved existing Case Forge checkout. |
| COLLAB-20260921-02 | Astra to Fable | Sent, 2026-09-21 02:09 UTC; awaiting reply | Claude Message 30: Node LTS API, pinned MIT import, clarified `platform/docs/` ownership, requested T-028 execution, lossless journal timestamp contract and a stacked draft PR base. |

The private handoff contains the existing Fable task link and local operational resource inventory. Public board updates are written by Fable during this cycle; Astra sends task evidence and proposed titles, never allocates competing IDs.

| COLLAB-20260921-02 | Fable to Astra | Delivered, integrated | Board heads `a2be84a` then `780dea0` merged. T-028 persisted validation and T-029 ISO journal source fix; legacy NDJSON passthrough accepted. Stacked draft PR approved, polling required for atomic file replacement. |
| COLLAB-20260921-03 | Astra to Fable | Sent, reviewed | Public engineering plan at `62cd7bf` reviewed after operational metadata was removed from the unpublished commit. Full inventory remains task-private. |
| COLLAB-20260921-03 | Fable to Astra | G1 approved with incorporated fixes | `780dea0`: T-018/T-021 done; T-004/T-015/T-027 accepted as API inputs. Seven auth fixes incorporated; RFC 7914 used for SHA-256 vectors and two-step password change preserves one KDF per request. |

### COLLAB-20260921-04

Sent PR #2 at 269f9cf for independent G3 review. Fable compared fixture JSON values, every CSV cell and raw NDJSON: zero fidelity failures. He found startup without status, large journal pagination and research roots above 500 directories; canonical contract now keeps all summaries at /runs and sweep index at /runs/index. CSV cells stay strings. Board/source head dbf20ec merged. T006 done; T023/T025 remain in review. Owner invitation address confirmed privately. Authentication proof expiry reduced to 120 seconds. Linux source-hash CI exposed checkout line-ending conversion; import now hashes original Git blob bytes with conversion disabled.

### COLLAB-20260921-05

Delivered 155cf5c. Fable independently passed 12 API tests and comparisons on fixtures, starting instance and indexed 501-run roots. T-025 done; G3 signed off at board 3b03f1c, merged. He approved journal WebSocket `{kind,instance,bytes}` invalidation; implemented and covered by the large-journal regression. Linux and Windows platform CI both pass. Authentication core now 60 local tests with real signatures/SQLite; remote Images compatibility verified on synthetic PNG/WebP, preview stopped. G2 delivery is next; browser account UI is in progress.
