# Launch incident tabletop — 2026-09-16

Issue #709. Scenario source: the incident plan in section 8 of the
[Y Combinator News launch readiness packet](y-combinator-news-readiness.md#incident-plan).
Data custody answers come from [relay data retention and deletion](../relay-data-retention.md).

## Record

| Field | Value |
| --- | --- |
| Date | 2026-09-16 |
| Kind | Desk tabletop. No fault was injected and no production control was used |
| Scenario | The measured 2026-09-12 outage, walked minute by minute, then every other row of the incident plan |
| Incident lead | ForkMesh account `jett`, the operator of all four serving mirrors (`mirror35`–`mirror38`) and the only person who can run every mirror-side control below. Named here; the lead's acceptance is not yet recorded (finding T1) |
| Thread voice | The human launch owner, writing in their own words. This record supplies no reply text |
| Scribe | Y Combinator News Agent, from repository evidence and the 2026-09-12 measurements |
| People present | None besides the scribe. This record does not meet the people-present bar that the [disaster recovery register](../disaster-recovery/exercise-register.md) sets for tabletop closure |

Evidence labels match the readiness packet: **measured fact**, **repository
evidence**, **inference**, and **missing proof**.

## Controls the lead would use

These are the exact rate-limit and reset controls that exist on current main,
plus the ones the incident plan assumes but that do not exist.

### Relay rate limits

Every limit is a constant in `app/src/entry.py`. Changing one needs a Worker
deploy. Counters reset only when their window rolls over; no admin route
clears them.

| Limit | Constant and value | Counter | Answer when exceeded |
| --- | --- | --- | --- |
| Account creation per source IP | `SIGNUP_MAX_PER_IP = 5` per `SIGNUP_RATE_WINDOW_MS` (1 hour) | D1 `signup_rate` | 429 `rate_limited` with `Retry-After` |
| Failed sign-ins per identifier | `LOGIN_MAX_FAILS = 10` within `LOGIN_FAIL_WINDOW_MS` (15 minutes), then locked for `LOGIN_LOCKOUT_MS` (15 minutes) | D1 `login_attempts` | Sign-in refused until `locked_until` |
| Password reset | `PASSWORD_RESET_ACCOUNT_WINDOW_LIMIT = 5` per account per 24 hours; `PASSWORD_RESET_REQUESTER_WINDOW_LIMIT = 10` per requester per hour | D1 `password_reset_rate` | Same `{"ok": true}` answer, no email sent |
| ForkBot prompts | `AI_ASK_MAX_PER_WINDOW = 60` per account per `AI_ASK_RATE_WINDOW_MS` (1 hour) | D1 `ai_ask_rate` | 429 `rate_limited` with `Retry-After` |
| Browser error reports | `CLIENT_ERROR_RATE_PER_CLIENT = 8`, `CLIENT_ERROR_RATE_GLOBAL = 120` per minute | Counted from D1 `error_log` | 202 `stored: false` |
| Desktop error reports | `DESKTOP_ERROR_RATE_PER_OWNER = 6`, `DESKTOP_ERROR_RATE_GLOBAL = 60` per minute | Counted from D1 `error_log` | Report not stored |
| Chat frames per socket | `ROOM_MSG_MAX_PER_WINDOW = 20` per `ROOM_MSG_WINDOW_MS` (10 seconds) | Durable Object memory | Frame silently dropped |
| Node event channel | `NODE_EVENT_MSG_MAX_PER_WINDOW = 20` per 10 seconds | Durable Object memory | 429 `rate_limited` |
| Catalog writes per owner | `CATALOG_WRITE_COOLDOWN_MS = 500` | D1 `catalog_rate` | 429 `rate_limited` with `Retry-After` |

### Mirror limits

| Limit | Where | Value |
| --- | --- | --- |
| Large push bodies in flight | `LARGE_REQUEST_BODY_SLOTS` in `tools/mirror_gateway.py` | 2; the rest get 503 without the body being read |
| SSH gateway sessions and processes | `limits.maxConcurrentSessions`, `limits.maxConcurrentProcesses`, `limits.uploadPackDeadlineSeconds` in the SSH gateway config ([schema](../ssh-gateway-config.schema.json)) | Set per node; sessions 2–256 |

### Reset and recovery controls

| Control | Exact form | What it resets | When to use |
| --- | --- | --- | --- |
| Admission override | Signed admin `POST /api/mirrors/admission-override` with `node`, `owner`, `repo`, `ts`, `sig`, optional `ttlSeconds` (default 1800, max 21600). Recorded in `sensitive_audit_log` as `admin.admission_override` | Readmits a repository's mirrors at their advertised state when the refs pin window has locked all of them out. The KV key expires on its own | Only when mirrors are up but the relay answers `mirror_unavailable`. It cannot help a mirror that returns 502 |
| Mirror daemon restart | `systemctl restart forkmesh-mirror-node` on the node, then `/readyz` | Gateway and sync loop on one node | One mirror at a time, never the whole fleet at once |
| Mirror rollback | Reinstall the parked prior binary and unit ([rolling migration](../operations/mirror-node.md#rolling-migration)), `--check`, start, `/readyz` | A bad mirror release | After a roll that coincides with onset |
| Mirror operator status | `GET /v1/status` on the node-local operator API | Nothing; read-only restart and sync counts | Diagnosis |
| Rate-limit counter | Wait for the window. Deleting a D1 counter row needs the blind-indexed key | One limiter entry | Not during an incident |

### Controls the incident plan assumes but that do not exist

- **No rate limit on anonymous Git or public API reads.** The relay's
  `info/refs` and `upload-pack` routes and the HTTPS mirror gateway have no
  per-IP or concurrency limit. The Cloudflare zone's custom firewall rules are
  kept only in the Cloudflare dashboard, not in this repository. "Enable
  preplanned rate limits" has nothing preplanned to enable (finding T4).
- **No maintenance, read-only, or drill kill switch.** The failover drill
  fixture does not exist yet (#706), so there is no drill endpoint to disable
  (finding T5).
- **No manual status incident.** `/api/status/history` and
  `/api/status/incidents` return 404; nothing lets the lead mark an outage
  publicly (finding T3).

## Row 1: clone fails on every mirror

The walk replays the **measured** 2026-09-12 timeline. Every "decision" is what
the lead would do; nothing below was executed on 2026-09-16.

| Time (UTC, 2026-09-12) | Inject | Lead's decision and control | What was missing |
| --- | --- | --- | --- |
| 18:28:08 | `git ls-remote https://forkmesh.com/forkmesh/forkmesh` fails with 503 | Declare an incident. Stop the demo. The human owner says in the thread, in their own words, that clone is down. No retry loop | No written trigger for declaring an incident, and no second person to run probes while the owner answers the thread (T1) |
| +1 min | Is it the relay or the mirrors? | Read the serving list from `GET /api/repo/forkmesh/forkmesh/mirrors`, then request `/health` on each mirror directly. **Measured:** `/`, `/healthz`, and the Git path on every mirror returned 502 from Caddy, while the relay's release-manifest raw path returned `{"error":"mirror_unavailable"}`. The relay is up and the fault is on the mirrors | No prepared probe script and no probe host outside the fleet; the mirror list has to be looked up mid-incident (T2) |
| 18:30:00 | `/api/status` shows every system operational, including `flagship_repository` and `git_hosting` | Do not cite the status page. Quote timestamped probe results instead | No way to post an incident publicly; no status history (T3, #707) |
| +2 min | Why did every mirror fail at once? | Ask whether a fleet roll, a refs push, or a gateway reload is in progress: check each node's `forkmesh-mirror-node` journal for `reloading gateway after configuration change` and `restarting gateway after configuration change`, and its `/v1/status` restart count. **Inference:** the shape (every mirror, about 2.5 minutes, self-healing, Caddy 502) fits the fleet-wide gateway restart on every refs change that commit `33582b221` fixed later that day and that shipped in v0.8.7 | No journal from 18:28 was kept, so the cause is unproven. No change freeze for pushes to `main` during a launch window (T6) |
| +2 min | Should the admission override be used? | No. It readmits mirrors held out by the refs pin window; these mirrors were down. Using it would change nothing and add an audit row | The incident plan did not say which reset control applies to which failure (fixed by this record) |
| 18:30:41 | Clone succeeds again | Recovery needs `ls-remote` to return the expected `HEAD` from two networks one minute apart, a depth-1 clone, and `tools/forkmesh_availability_proof.py verify` to match. Only then may the demo resume | No agreed recovery bar existed (decided here) |
| After | Write-up | Record onset, first failed probe, recovery, and cause-or-unknown in the readiness packet, and reply in the thread in the owner's words. Never edit history to look green | No incident record template or public location (T3) |

**Data custody during row 1:** the outage deleted or exposed nothing. Failed
requests land in `api_metrics_minute` (7 days) and status samples (30 hours to
30 days). Mirror access logs are the operator's, not relay data.

## Rows 2–8

| Incident | First control | Reset and evidence | What was missing |
| --- | --- | --- | --- |
| Status green while Git path fails | Independent `ls-remote` and depth-1 clone probes with timestamps | Status fixes go to Reliability (#707); history is never edited | The same as T2 and T3 |
| Install, checksum, or signature failure | Stop recommending the installer. Capture OS, manifest `checksums_sha256`, asset `blob_sha256`, and the exact error. `install_diag` rows (newest 5,000) hold installer step reports | Release engineering corrects after human approval (#703) | No clean-machine log to compare against (#705) |
| macOS reader tries the installer | Say no native package is published | Link the signed manifest | None beyond the homepage wording (#704) |
| Windows SmartScreen warning | Say the certificate is self-issued | Link the support matrix row | No recorded dialog text (#705) |
| Security report | Send the reporter to `security@forkmesh.com` or the private report form. Reports are capped at `MAX_SECURITY_REPORTS = 1000` in D1 | Acknowledge privately, preserve evidence, scope affected commits and releases | No named security responder besides the incident lead (T1) |
| Heavy traffic or abuse | The relay limits above cover signup, sign-in, reset, ForkBot, chat, and error reports. Anonymous Git and API reads have no limit | Watch `api_metrics_minute` and D1 read usage; disable the drill before weakening auth or integrity checks | T4, T5, and T7 |
| Wrong or unsupported claim found live | Correct the factual surface and tell the thread's human owner | Link the manifest, test, or limitation | None new |
| Reader asks what is stored and how to delete it | Link [relay data retention and deletion](../relay-data-retention.md) and name its known gaps | Account delete removes most rows; the page lists what remains | Before this record, no table existed (fixed) |

## Findings

| ID | Finding | Owner | Status |
| --- | --- | --- | --- |
| T1 | The incident lead is named but has not accepted, and there is no second responder. The lead is also the only mirror operator, so one person holds every control | Human launch owner | Open |
| T2 | No probe script and no probe host outside the mirror fleet; the mirror list is looked up during the incident | Reliability (#707) | Open |
| T3 | No public status history, incident endpoint, or manual incident post | Reliability (#707) | Open |
| T4 | No preplanned rate limit for anonymous Git or public API reads; edge firewall rules live only in the Cloudflare dashboard | Reliability / operations (#709) | Open |
| T5 | No maintenance or read-only mode, and no drill kill switch, because the drill fixture does not exist | Mirror/product engineering (#706) | Open |
| T6 | No change freeze on pushes to `main` during a launch window, although each refs change reloads every mirror gateway | Human launch owner | Open |
| T7 | Changing a relay limit needs a Worker deploy, and counters cannot be reset by an admin | Reliability / operations (#709) | Open |
| T8 | The incident plan did not say which reset control fits which failure | Y Combinator News Agent | Fixed by this record |
| T9 | No public table of relay stores, backups, retention, and deletion | Y Combinator News Agent | Fixed: [relay data retention and deletion](../relay-data-retention.md) |

## Changes since the exercise

Checked against main on 2026-09-27. The record above stays as walked; these
changes decide how the next exercise runs.

- **The relay left Cloudflare on 2026-09-24.** The same code now runs on the
  core nodes `fmnext-n1` to `fmnext-n3`, and the Cloudflare database and the app
  and World Workers were deleted on 2026-09-27 ([cutover
  record](../../../docs/CUTOVER.md)). The relay limits above still hold the
  quoted values. Their counters are now in the replicated rqlite database, and
  the per-socket counters are in the memory of the core node that owns the
  object. Changing a limit needs a core node release (`tools/next_deploy.py`),
  not a Worker deploy (T7).
- **The mirrors in row 1 are retired.** `mirror35`–`mirror38` stopped serving on
  2026-09-24. Clones of `forkmesh/forkmesh` are now answered by the core nodes
  ([mirror retirement](../../../docs/MIRROR_RETIREMENT.md)). The mirror daemon
  restart and rollback controls apply to the core nodes' mirror daemon, and the
  lead is still the only operator of every core node (T1).
- **The Cloudflare edge no longer fronts `app.forkmesh.com` or
  `world.forkmesh.com`.** Their DNS records are not proxied, so dashboard
  firewall rules cover only `forkmesh.com` (T4).
- **`api_metrics_minute` is gone.** Migration 0190 dropped it on 2026-09-22.
  Failed flagship reads are now counted per minute in
  `flagship_read_failure_minute` (24 hours), and request counts go to the core
  node journal.

## Next exercise

Repeat row 1 on the core nodes with the incident lead and a second responder
present, using a prepared probe script against a disposable repository. Record
who ran each control and how long each step took.
