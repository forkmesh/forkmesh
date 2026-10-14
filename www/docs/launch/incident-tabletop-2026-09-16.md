# Launch incident tabletop — 2026-09-16

A desk walk-through of the incident plan in the
[launch readiness packet](y-combinator-news-readiness.md#incident-plan). Data
custody answers come from [relay data retention and deletion](../relay-data-retention.md).

## Record

| Field | Value |
| --- | --- |
| Date | 2026-09-16 |
| Kind | Desk tabletop; no fault injected, no production control used |
| Scenario | A measured clone outage replayed minute by minute, then every other row of the incident plan |
| Incident lead | ForkMesh account `forkmesh`, the fleet operator account that holds every mirror-side control |
| Thread voice | The human launch owner, in their own words |

## Relay rate limits

Each limit is a constant in `app/src/entry.py`; changing one needs a relay
release, and counters reset only when their window rolls over.

| Limit | Constant and value | Answer when exceeded |
| --- | --- | --- |
| Account creation per source IP | `SIGNUP_MAX_PER_IP = 5` per hour | 429 `rate_limited` with `Retry-After` |
| Failed sign-ins | `LOGIN_MAX_FAILS = 10` in 15 minutes, then a 15-minute lock | Sign-in refused |
| Password reset | `PASSWORD_RESET_ACCOUNT_WINDOW_LIMIT = 5` per account per day; `PASSWORD_RESET_REQUESTER_WINDOW_LIMIT = 10` per requester per hour | Same `{"ok": true}`, no email |
| Assistant prompts | `AI_ASK_MAX_PER_WINDOW = 60` per account per hour | 429 `rate_limited` |
| Browser error reports | `CLIENT_ERROR_RATE_PER_CLIENT = 8`, `CLIENT_ERROR_RATE_GLOBAL = 120` per minute | Not stored |
| Desktop error reports | `DESKTOP_ERROR_RATE_PER_OWNER = 6`, `DESKTOP_ERROR_RATE_GLOBAL = 60` per minute | Not stored |
| Chat frames per socket | `ROOM_MSG_MAX_PER_WINDOW = 20` per 10 seconds | Frame dropped |
| Node event channel | `NODE_EVENT_MSG_MAX_PER_WINDOW = 20` per 10 seconds | 429 `rate_limited` |
| Catalog writes per owner | `CATALOG_WRITE_COOLDOWN_MS = 500` | 429 `rate_limited` |
| Security reports | `MAX_SECURITY_REPORTS = 1000` stored | Oldest beyond the cap dropped |

Mirror limits: `LARGE_REQUEST_BODY_SLOTS` in `tools/mirror_gateway.py` (2
large push bodies in flight) and the SSH gateway's session, process and
upload-pack deadline limits.

## Reset and recovery controls

| Control | Use |
| --- | --- |
| Signed admin `POST /api/mirrors/admission-override` (`ttlSeconds` default 1800, max 21600, audited) | Readmits mirrors that are up but locked out by the refs pin window; useless when mirrors are down |
| `systemctl restart forkmesh-mirror-node`, then `/readyz` | One node at a time |
| Reinstall the prior binary ([rolling migration](../operations/mirror-node.md#rolling-migration)) | After a release that coincides with onset |
| `GET /v1/status` on a node | Read-only diagnosis |

Missing controls: no rate limit on anonymous Git or public API reads, no
read-only or maintenance mode, and no way to post a public incident.

## Row 1: clone fails on every mirror

| Time (UTC) | Inject | Decision and control |
| --- | --- | --- |
| 18:28:08 | `git ls-remote https://forkmesh.com/forkmesh/forkmesh` fails with 503 | Declare an incident, stop the demo, say clone is down; no retry loop |
| +1 min | Relay or mirrors? | Read the serving list from `/api/repo/forkmesh/forkmesh/mirrors` and probe each mirror's `/health` directly |
| +1 min | Status page shows every system operational | Quote timestamped probe results, not the status page |
| +2 min | Why did every mirror fail at once? | Check each node's journal and `/v1/status` for a fleet-wide gateway reload or roll |
| +2 min | Use the admission override? | No: the mirrors are down, not held out |
| 18:30:41 | Clone succeeds again | Resume only after `ls-remote` matches from two networks a minute apart, a depth-1 clone succeeds, and the availability proof verifies |
| After | Write-up | Record onset, first failed probe, recovery and cause-or-unknown; never edit history to look green |

The outage deleted or exposed no data.

## Rows 2–8

| Incident | First control |
| --- | --- |
| Status green while the Git path fails | Independent `ls-remote` and clone probes with timestamps |
| Install, checksum or signature failure | Stop recommending the installer; capture OS, manifest and asset hashes and the exact error |
| Platform questions (macOS, SmartScreen) | Link the signed manifest and the support matrix |
| Security report | Private channel only; preserve evidence; scope affected releases |
| Heavy traffic or abuse | Relay limits above; protect control endpoints first |
| Wrong claim found live | Correct the surface and link the evidence |
| What is stored and how to delete it | Link the retention table and its known gaps |

## Findings

| ID | Finding |
| --- | --- |
| T1 | One person holds every control; no second responder |
| T2 | No probe script or probe host outside the fleet |
| T3 | No public status history or incident record |
| T4 | No rate limit for anonymous Git or public API reads |
| T5 | No maintenance or read-only mode |
| T6 | No change freeze on `main` during a launch window, although each refs change reloads every mirror gateway |
| T7 | Changing a limit needs a release; counters cannot be reset |

The next exercise repeats row 1 on the current core nodes with two responders
and a prepared probe script against a disposable repository.
