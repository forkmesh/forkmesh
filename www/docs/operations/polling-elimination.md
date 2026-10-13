# Polling inventory and elimination plan

Goal: no ForkMesh client polls the relay for data. Every "did anything
change?" question becomes a payload-free push over a WebSocket to a Durable
Object, answered by the one signed HTTPS request the client would have made
anyway. The push transport already exists — the per-owner `ForkMeshNodes` DO
behind `GET /api/nodes/events` (desktop/headless nodes) plus the chat, world,
office and note room DOs on the web side — so the work is moving each poller
onto it, not building new infrastructure.

Two rules from the mirror-node conversion (2026-08-06), which is the template
for the rest:

- **No fallback polls.** A "safety net" poll quietly becomes the load. The
  event socket owns liveness instead: reconnect is unconditional with bounded
  backoff, and every successful (re)connect fires one catch-up event that
  drains anything queued while the channel was down.
- **Heartbeats are not polls.** A message whose purpose is to *prove this
  client is alive* (presence heartbeat, endpoint lease renewal) cannot be
  replaced by a server push. Its signed proof and cadence stay in place, but
  it can share an already-open client-to-relay socket instead of creating an
  HTTP request of its own; HTTPS remains the recovery path.

## Status: eliminated

| Poller | Was | Now |
| --- | --- | --- |
| Managed mirror collaboration intake (`server/intake.go`) | `GET /api/repo/*/pending` every 5s per node — the busiest endpoint on the relay, later wrapped in a short-lived desktop process | Native push-only Go: one `EventSocket` (`server/nodeevents.go`) to `wss://…/api/nodes/events`; an issue/pull/discussion frame leases only the changed inbox, materializes it in Git, and acknowledges exact row IDs with a signed refs attestation. Reconnect performs one all-kind catch-up. No desktop process and no fallback poll. |
| Qt desktop fallback inbox sync (`m_inboxPollTimer`) | `GET /api/sync` every 5m (15m while the event socket was up) | Timer deleted. The `NodeEventSocket` is the only sync trigger: push → debounced sync, plus one catch-up sync per (re)connect and one 20s post-launch pass. |
| Qt desktop account-heartbeat transport | `POST /api/accounts/heartbeat` every 60s even though the desktop already held `NodeEventSocket`; the unchanged success body dominated detailed network logs | The 60s signed presence proof remains, but travels as an advertised `heartbeat` frame on `GET /api/nodes/events`. `ForkMeshNodes` hands the body to the exact `_account_heartbeat` handler and returns a correlated `heartbeat-result`, so payout presence, public-wallet checks, admin/claim prompts and mirror-request acknowledgements keep their existing semantics without a steady-state HTTP request. A relay that does not advertise the frame uses the POST immediately; a disconnect or missing result retries the same signed body over HTTPS after a bounded acknowledgement timeout. The rare credits-refilled beat stays on HTTPS because processing it fans a new ping event back into the same account object. |
| Qt desktop `/pending` badge fetch (`fetchMirrorPendingCounts`) | refetched on a 10m TTL through every roster/refresh tick | Fetch-once per repo; the cache entry is invalidated only by a node event push, a local drain, or an explicit repo open (user action). Steady-state ticks reuse the cache with no HTTP. A *failed* fetch stamps nothing, so it stays eligible — held off by an exponential `m_pollBackoff` channel (Retry-After honoured as a floor) instead of re-firing on every roster blip. Folding the tallies into the event frame itself (dropping the GET) remains open. |
| Qt desktop office chat backlog (`OfficeChannelMirror`) | `GET /api/chat/channels` + a `/history?since=` per mirrored room every 30s — the client asking "any unread messages?" on a beat | Timer deleted. One channel-list + backlog pass per run seeds each room (and its key); the room's ticketed WebSocket is then held open, so later messages — the ones that light the chat unread badge — arrive as pushes. Reconnect is unconditional with bounded backoff and fires one catch-up history read; the channel list re-lists only on a user action (opening Chat, 10s floor) or an account switch. Live sockets are budgeted to the first 16 rooms; the rest still load on open/send, and the cap is logged. |
| Unread counts, all three surfaces (`refreshWebAlerts`, `startNotificationPolling`, `refreshDirectMessages`, `markChatActivitySeen`) | desktop ping inbox every 5m off the heartbeat; /world ping digest every 60s; /chat conversation list every 30s and the chat-activity counters every 60s | One read when the surface opens, then push-only. `notify_account_event(env, owner, topic)` fans a payload-free frame to the account's `ForkMeshNodes` DO from the only two writes that can move a count: `enqueue_notification` (`pings`) and `_chat_direct_message_retained` (`direct-messages`, to the participant who did not send it). Browsers join that same per-account socket with a 60s ticket from `GET /api/accounts/event-ticket` (a WebSocket upgrade carries no `Authorization` header) through the shared `public/account-events.js`; the desktop already held it. No fallback poll — one catch-up read per (re)connect. The header chat badge's baseline now advances locally off the room socket (`noteSeenChatActivity`) while chat is open, and is re-read absolutely once more on `visibilitychange`/`pagehide` — two open tabs share one localStorage baseline and would each bump it for the same message, so that read on the way out is the drift correction the 60s timer used to provide. That 30s re-read also discovered lists that changed for somebody else's reason, so a channel invite and a newly-opened conversation push `private-channels` / `direct-messages` too (via `runtime.notify_account`) rather than quietly becoming reload-only. |
| Go mirror node git sync (`daemon.go` `syncLoop`) | upstream fetch + catalog publish every 30s | Push-first: the relay fans a `commits` event to every known group member when a source publishes changed public heads or tags (catalog publish path, `source == "local-node"` only, so mirror republishes cannot loop); the event includes the source-signed refs digest, and the daemon retries a successful stale-peer no-op on 2s→15s backoff until that generation lands. Native managed-host receives are change-driven too: the desktop pushes changed heads/tags to every installed saved host, and a service-owned `post-receive` hook rings the daemon's loopback sync endpoint immediately; an explicit authenticated wake covers older installations, while an all-up-to-date pass sends no wake. Native collaboration intake runs inside that same bounded cycle before the signed refs state is published. Since fan-out is best-effort, relay-only nodes retain one 30-minute reconciliation pass so a completely lost live event still heals without recreating the dominant fleet poll; third-party upstreams still use `syncInterval` because the relay cannot push for them. The 4m endpoint liveness proof now rides the existing node WebSocket as an `endpoint` frame and runs through the same signed registration handler; HTTPS is only the mixed-version/disconnect fallback. One more timer survives: while any repository has no readable local clone, `syncLoop` retries the clone on 15s→5m backoff and disarms the moment one lands. That is error recovery for a failed operation, not a change poll — a first clone that drops mid-pack cannot be rescued by a push, because the relay only pushes when a source changes its refs. |
| Dashboard mirror roster | `GET /api/repo/*/mirrors` every 5m while the Mirrors tab was visible, in addition to room-socket mirror signals | Timer removed. The existing account socket now forwards payload-free `catalog` envelopes (including the repository key) into the same coalesced mirror-signal handler as the room socket. A reconnect performs one catch-up refresh; opening the tab remains an explicit read. |

| Web session validity, every signed-in page (#11) | `GET /api/accounts/sessions` every 20s per tab, from *both* `dashboard/js/02-helpers.js` and `site-header.js` — so every dashboard tab, and every docs/blog/marketing page, forever, in background tabs too. The whole body was discarded; only a 401 acted. At 370 hits in one 21.5h network-log window it was the most-requested endpoint on the relay and 46% of all browser traffic | Both timers deleted. The dashboard joins the same per-account `ForkMeshNodes` socket the desktop and /world already hold (`public/account-events.js`, new `onSignedOut` callback): every (re)connect trades the stored session for a fresh event ticket, so a revoked session surfaces as an authoritative 401/403 on that handshake for no extra request. Marketing pages carry no socket — opening one per blog page would cost more than the poll — so they keep only the plan's other prescription, `focus`/`visibilitychange`, now behind a 5-minute floor so an alt-tab burst cannot become the interval again. A failed check leaves the floor unarmed so an offline stretch stays eligible. The dashboard also now refreshes pings from the `pings`/`issues` topics, so no notification timer is ever needed there. |
| Membership plan badge, marketing pages | `GET /api/accounts/sessions` on *every* navigation — the in-page `membershipProjectionPromise` memo dies with the document, so browsing docs → blog → pricing re-read it each time | 5-minute `sessionStorage` cache keyed by account, shared by `site-header.js` and `home-header-auth.js`. The projection is a badge, not an authorization decision (the Worker re-checks membership on every request that depends on one), and `sessionStorage` dies with the tab so a stale plan cannot outlive the visit. Both logout paths already sweep `forkmesh.`-prefixed `sessionStorage` keys, so the cache clears itself. |
| Desktop self-reachability probe | `GET /api/repo/*/mirrors/<self>/reachability` every 2 minutes — for the machine that is the repository's *source*, so the relay has no mirror row and answered **404 every single time** (38/38 in the same window) | Not sent. `loadMirrorNodesPanel` no longer enqueues the self node, and `makeReachabilityCell` renders that row from local knowledge ("Source · this machine"). |
| Desktop network-repos mirror tally | `GET /api/repo/*/mirrors` **per table row**, issued from the row builder with no cache — so every re-render re-asked the relay about every repository on the page (~60s per repo across five repos). More requests than any timer on the client, and no timer involved: see "startup bursts" below | 5-minute per-repo body memo (`m_networkRepoMirrorsCache`). A hit repaints from the stored body so the column still fills in on every render; only the HTTP disappears. A failed fetch stamps nothing, so it stays eligible. |
| Desktop full-page edge probes | `/` (which redirects to `/dashboard`) and `/status` fetched every 5 minutes — four real page loads for two footer dots | Healthy cadence widened 5 → 15 minutes (`kDesktopWebsiteProbeHealthyEveryNth`). The moment any row grades non-operational it drops back to the 60-second cadence, so an outage and its recovery are still caught within a minute. |
| World session validity | `GET /api/accounts/sessions` every 20s per visible World tab (`validateActiveWorldSession` on `sessionWatchTimer`) — the poll the dashboard and the site header had already dropped, still running on the third surface: 180 full authenticated listings an hour, read for their status code | Timer deleted. World already holds the account event channel; its `onSignedOut` (the relay refusing a ticket for the stored session) forces one confirming read, and the return to a hidden tab revalidates behind a five-minute floor (`WORLD_SESSION_REVALIDATE_MIN_MS`). |
| Signed-out session restore, every www page and World | `restoreSession` in `api-client.js` asked `GET /api/accounts/sessions` on every navigation of every anonymous reader; its in-page promise died with the document and nothing remembered the 401 | An authoritative 401 is remembered per tab for five minutes (`forkmesh.sessionRestoreMiss` in sessionStorage; outages and network errors are never remembered). Arriving from another origin — the login page lives on App — skips the memo, so signing in and coming back still shows on the first paint. |
| Signed-in header session read, www | `site-header.js` and `home-header-auth.js` called `fetchMembershipProjection(true)` on load, bypassing the five-minute cache written for exactly that read; a cookie restore then re-read the route twice more (its `.then` and the `forkmesh:session-restored` listener) | Load and restore reads are unforced, and `restoreSession` files the relay's projection in the shared cache, so a restore is one request instead of three. The focus/visibility watch is the only forced caller left. |
| Portable mirror grant re-signing (server) | every `GET /api/accounts/sessions` and `GET /api/accounts/devices` paid a primary D1 read, a blind index and an Ed25519 signature to replace a twelve-hour grant minted minutes earlier | A presented grant for the same session and account younger than `PORTABLE_SESSION_REISSUE_MS` (4h, the dashboard's own refresh beat) is left alone and the response says `X-ForkMesh-Portable-Session: current`. Login always issues. |
| Desktop footer status read | `GET /api/status?view=world` every 60s from every running desktop, against a snapshot the relay publishes every three minutes | Follows the publish cadence while every row is operational (`kFooterWebsiteStatusHealthyEveryNth`); any other grade, or none yet, keeps the minute tick. |
| Desktop synced-note version check (#6) | full `GET /api/notes/<id>` every 3s while a synced note was open — 1,200 requests an hour, minimized or not | 15s, and not at all while the window is minimized. Still a poll; the web editor's note socket is the eventual replacement. |
| World admin error badge (#17) and Office agent droids | `ADMIN_ERROR_POLL_MS` 15s for an administrator's open World; `WORLD_AGENT_BOT_POLL_MS` 8s × two authenticated reads (`/api/tasks` and the agent-bots list) while standing in the Office | 60s (the badge's notice was already limited to one a minute; the badge and that notice were removed later, and the World shows no toasts since 2026-10-09) and 30s. The element deposit watch also skips hidden tabs. |
| Mobile inbox safety-net poll | `NotificationService` re-read the inbox every minute under a live account event channel, and kept going on Android while the app was backgrounded | Five minutes, cancelled on `paused`/`hidden` and re-armed with one catch-up read on `resumed`. |
| Desktop registered-user directory (#4) | `GET /api/accounts/users` every 60s with a 45s floor | Still a poll — a brand-new account has no socket frame to announce itself, and a global "someone signed up" fan-out to every account's DO would cost more than it saves — but a slow one: 10-minute timer, 5-minute floor. Every surface that shows the directory (Chat, Settings, the issue assignee picker) already calls it on open, and those calls ride the floor. |

| Catalog republish storm, all publishers (`POST /api/repositories`) | the Go mirror and legacy Qt desktop both re-POSTed records; the desktop's volatile served/resource counters made nearly every sync pass look changed, causing duplicate signing, D1 writes, endpoint challenges, and cache purges | Single publisher. The Qt catalog publisher, contribution-snapshot pipeline, retries, local serve counters, and legacy `RepoHost` were removed. Only the managed Go mirror daemon publishes; it fingerprints records minus volatile telemetry and renews a bounded lease. The relay still independently suppresses unchanged writes and reserves cache purge + `catalog_rev` changes for real state transitions. |
| Qt desktop 5m catalog re-attest read (`reattestStalePins`) | One Worker invocation every mirror-sync pass, even when its ETag produced a bodiless 304; the desktop could only log drift because it no longer owns publication | Removed. Managed mirror daemons own pin publication and reconcile it during their bounded sync cycle. The desktop performs no periodic catalog read. |

### Desktop and mobile: no display polling (2026-09-19)

Measured first, from the desktop's own `actions.jsonl`: an idle desktop sent
`/api/version`, `/api/status`, `/dashboard` and `/status` **every minute** (the
"healthy" throttles disengage the moment any row is not operational, so it
polled hardest exactly while the site was struggling), and every roster frame
turned a rebuild of the *hidden* Mirror nodes panel into `/mirrors` plus
`/health` and `/branches` per mirror. On mobile the Agents tab alone cost two or
more requests per repository every ten seconds, backgrounded or not.

The rule now: **a client reads when a page opens, when the reader presses
Refresh, when the app or window comes back, or when a socket frame says
something changed — never on a display timer.**

| Poller | Was | Now |
| --- | --- | --- |
| Desktop minute tick (`m_relayLatencyTimer`) | `/api/version` probe + `/api/status?view=world` + `/dashboard` and `/status` page loads, every 60s when anything was not operational | The tick repaints local node state only. Status, page probes and latency are read at launch, on a dot's recheck, and on window activation behind `refreshFooterStatusOnActivation` (15 min floor when operational, 2 min otherwise). The websocket pong supplies latency between reads; a failed probe still re-arms itself until the relay answers, which is how automatic offline mode ends |
| Desktop Mirror nodes panel | `/mirrors` + per-mirror `/health` and `/branches` on any rebuild older than 20s–5m, visible or not, plus a 5m repair timer | Network reads only while the table is on screen; the timer is gone. Opening the tab reads, Refresh forces |
| Desktop registered-user directory (`m_chatDirectoryTimer`) | 10m timer behind a 5m floor | Timer deleted; Chat, Settings, the assignee picker and Users read on open behind the floor, Users has Refresh |
| Desktop synced-note version check (`m_noteRefreshTimer`) | `GET /api/notes/<id>` every 15s while a synced note was open | Timer deleted; Notes reads on open and has a Refresh button (refused while there are unsaved edits) |
| Desktop cloud agent sessions | `agent-sessions` every 20s tick (60s TTL) while the Agents tab was open, and only for the open repository | One account-wide `GET /api/account/agent-sessions`, read on the `cloud-agents` account event frame (the relay rings it when a mirror claims, starts or finishes a run; the desktop's own `agents` push frame stays a no-op, or every local agent step would cost a relay read), when the tab opens, when the list reloads, on window activation and on the Cloud filter button. Floors: 5 min idle, 2 min with a run in flight |
| Desktop agent-session snapshot push | per-repo POST safety net every 30s | One push after launch; changes already schedule a debounced push |
| Desktop Pull requests auto-refresh | ledger reconcile + reload every 60s while the tab was open | Removed; tab open, relay event frames and Refresh |
| Desktop Logs → Cron report | signed read every 60s while the tab was open | Removed; tab open and Refresh |
| Desktop saved-host SSH probes | every 10m while the Hosts page was hidden | Hidden page does not probe; shown page and Refresh do |
| Desktop ad banner | `/api/ads/serve` every 30m | One request after launch, then on window activation once 30m old |
| Mobile Agents tab | `agents/list` + `agents/sessions` per repository every 10s while any session ran — and Android kept it firing in the background | Timer deleted; tab open, header Refresh, pull to refresh |
| Mobile repo Actions / Agents tabs, launched-session screen | one to two requests every 10s each while a run was active | Timers deleted; each has a Refresh button and shows "Running · refresh to update" |
| Mobile inbox net (`NotificationService`) | every 5m, plus a duplicate read at launch and on every resume | No timer. `start()` reads once, the event channel reads on push and (re)connect, resume catches up once, and overlapping reads share one request |
| Mobile relay room key | a signed-out phone sent `GET /api/chat/room-key` (401) on every reconnect attempt, every 30s forever | Not requested without a session; reconnect backoff now doubles to 5m |
| Mobile rebuild-driven reads | `mirrors`, `fedi-comments` and `commit` were requested from `build()`, so every tab swipe re-asked | Behind the `ApiService` repo cache like their neighbours |
| Mobile user status on resume | `POST /api/accounts/user-status` on every app switch | `refreshIfStale`, 10m floor |
| Native Android inbox poll | every 60s for the life of the process | Loop deleted; `start()` reads once, the account event loop reads on push and (re)connect |

Kept on purpose: websocket keepalives and the 60s presence proof (they ride
the socket, not HTTP), reconnect backoff, the mirror safety sweep (the source
node's collaboration intake over git, not display data), release finalization
and error-report retries (self-stopping), the mirror fleet reconciler (opt-in)
and the hourly update check (opt-in).

## Related: startup bursts (not polls, same symptom)

A client that makes one request per *row* instead of one per *topic* produces
the same relay load a poll does, just all at once. The desktop's organization
task board is push/action-driven and refreshes `GET /api/tasks` once per launch
(following bounded continuation pages only when the catalog exceeds one page).
Until adhoc #1618 it then published each agent session's live run state as its
own signed `POST /api/tasks/<id>/agent-status`, so a node with a fleet of
sessions opened seventeen writes in the same millisecond and the relay
rate-limited (HTTP 429) most of them, its own account lookup included. The
states now coalesce into a single write carrying every changed run. When
auditing a client path, count requests per *event*, not just timers per minute.

The same "one request per row" shape is still live on the desktop's catalog
reads, and it is the largest remaining client-side item. `GET /api/repositories`
has **four independent callers**, each issuing its own full-body read of the
same resource: `fetchCatalogRepos` (`MainWindowSetup.cpp`, synchronous, pumps
the event loop), the repo pin check and `reattestStalePins` (both
`MainWindowChat.cpp`), and the network-repositories page. In the 21.5h log they
fire within about three seconds of each other around every publish cycle — four
signed full-catalog GETs where one would do (491 GETs against 58 local
publishes).

Only `reattestStalePins` sends `If-None-Match`, which is why the relay's weak
`catalog_rev` ETag — implemented correctly on both sides (`catalog_routes.py`
answers 304 for an authed viewer whose `if-none-match` matches) — produced
**zero 304s across 6,519 `200 Ok` responses** in that window. The fix is one
short-TTL, in-flight-coalescing memo that all four callers share, revalidating
with the ETag on expiry; the memo collapses the burst and the ETag makes the
refill bodiless. Note the synchronous caller pumps the Qt event loop, so
converting it needs the by-value discipline in the git-pump UAF family.

That write then stops being a request at all. The desktop is already holding
the node event socket so the relay can push to it, so the batch rides back up
that socket as one `{"type":"agent-status"}` frame and the relay answers with
its per-task verdicts on the same connection. The signed HTTPS
`POST /api/tasks/agent-status` remains the fallback for a node whose socket is
down, and for a relay too old to advertise the frame in its hello — the
desktop only diverts a write off HTTPS once the relay has named it in
`NODE_FRAME_ACCEPTS`.

The socket is transport, never authority. The frame carries the same
`forkmesh-org-task-agent-status-batch-v1` node/ts/sig proof the HTTPS route
demands, and the relay hands it to the same task handler, which re-runs the
proof check and then the per-task ownership and agent-row gates on every entry.
Being connected proves nothing on its own — which is what keeps this channel
safe to widen. Frames a node sends share the socket's existing per-window rate
budget, so a flooding write path is throttled like a flooding keepalive.

## Inventory: still polling (relay-facing)

### Qt desktop client

| # | Poller | Interval | Endpoint(s) |
| --- | --- | --- | --- |
| 3 | `m_adminPollTimer` pending-user verification (`MainWindowSetup.cpp:1894`) | 5m (admins only) | pending-users API |
| 4 | `m_chatDirectoryTimer` | removed 2026-09-19 | see "no display polling" above |
| 5 | `m_directMirrorRegistrationTimer` (`MainWindowControlNode.cpp:743`) | 5m | mirror registration renewal |
| 6 | `m_noteRefreshTimer` | removed 2026-09-19 | see "no display polling" above |
| 7 | `m_agentLimitsTimer` (`MainWindowSettings.cpp:1661`) | 60s | agent limits API |
| 8 | `m_homeStatsTimer` / `m_controlNodeRefreshTimer` | 60s / 10s | mostly local git/process reads; audit for hidden HTTP |

Heartbeats (keep): `m_heartbeatTimer` still emits the 60s reward-eligibility
proof, but it now rides `NodeEventSocket` rather than polling an HTTP endpoint
in steady state. `m_relayLatencyTimer` is now a local repaint tick with no network calls.

### Website — dashboard

| # | Poller | Interval | Endpoint(s) |
| --- | --- | --- | --- |
| 9 | pending-badge re-poll while a repo shows pending items (`06-repo-content.js`, `pendingInboxRefreshes`) | 10m | `GET /api/repo/*/pending` |

(#11, session validity, is eliminated — see the table above.)

### Website — /world

All in `world/public/world/world.js` (constants near the top);
each is document-visible-only but still a steady per-tab drumbeat:

| # | Poller | Interval |
| --- | --- | --- |
| 13 | `WORLD_EVENT_POLL_MS` community events | 3m |
| 14 | `MIRROR_STATUS_POLL_MS` mirror catalogs | 5m |
| 15 | `MIRROR_ACTIONS_POLL_MS` action runs | 2m (was 20s) |
| 16 | `WORLD_QA_POLL_MS` / `WORLD_BUILD_BOARD_POLL_MS` / `WORLD_STATUS_POLL_MS` | 15s / 60s / 60s |
| 17 | `ADMIN_ERROR_POLL_MS` admin error feed | 60s (was 15s) |
| 18 | `REPOSITORY_IMPORT_POLL_MS`, `WORLD_DEPLOY_STATUS_POLL_MS`, `WORLD_ELEMENT_DEPOSIT_POLL_MS`, media/social timers | 2m / 2.5s / 4s / var |

(#12, personal notifications, is eliminated — see the table above.)

Chat pages (`chat.js`, `dashboard-chat.js`) are WebSocket-driven, including
their unread counts. What is left on a timer there is not a count: the 30s
`renderPeople` tick is a local re-render, the 60s `PRESENCE_INTERVAL_MS` beat is
a liveness proof the room DO requires, and the visible-tab-only 5m
`USERS_DIRECTORY_REFRESH_MS` tick is the same never-spoken-account problem as
desktop #4.

### Server side

| # | Poller | Notes |
| --- | --- | --- |
| 19 | App cron `* * * * *` (`app/wrangler.toml`) | The App rings one coordinator object whose alarm runs every due phase itself, continuing in a fresh alarm invocation on the same object when a subrequest budget runs out, so no second object is ever billed while a first one waits. The deep repository probe runs every ten minutes; the full immutable installer-artifact sweep runs hourly because signed mirror leases and artifact-count parity are still checked each minute. Minute-sensitive status and alert phases retain their cadence. |

Already event-driven: the SSH post-receive refresh path
(`server/packaging/systemd/forkmesh-mirror-refresh.path` is an inotify path unit;
`ssh-refresh-notify` pushes).

## D1 rows read: change-driven instead of timer-driven (2026-09-11)

The same beat problem shows up inside the Worker: cron phases that fire every
minute re-read tables whose contents move a few times a day. The D1 dashboard
put the daily total at ~1.4M rows read (a 5M Free allowance) with fifteen
queries accounting for most of it. Each now reads only when something changed:

| Query (rows/day before) | Trigger | Now |
| --- | --- | --- |
| `SELECT … FROM repositories WHERE is_private = 0` (~165k, the largest) | the 30s per-isolate catalog memo lapsed on every `status-sample` and `mirror-health` tick, so both phases rescanned and re-decrypted the table each minute | a lapsed TTL first reads the one-row `catalog_rev` counter; the table is rescanned only when the counter moved or the last scan is older than 10 minutes. Every catalog mutation bumps the counter — `schedule_public_catalog_republish`, which every write path already rings, does it — so cross-isolate propagation stays inside the old 30s. |
| `repository_monitor_state … LIKE 'status:%'` + the `json_each` upsert (~80k) | `status-alert-pings` every minute, `status-alert-email` every 5 | the read is an exact `IN (…)` list of the roster (the prefix scan also walked retired mirrors and the non-status monitors), memoized per isolate; a minute whose computed rows equal the memo neither reads nor writes. Any transition or deliverable alert re-reads D1 first, and a delivery forgets the memo. |
| `system_status_minute_batch` window (~35k) | alert phases re-read the minute the sampler just wrote; `/api/status` rebuilt 60 minutes every 3 | the sampler's write feeds a per-isolate memo the alert phases answer from; completed minutes are cached, so the status rebuild reads only minutes it has not seen. |
| `online_hourly_nodes` ×2 (~60k) | `network_leaderboards` (48h) and `online_history` (24h) each scanned it inside one `snapshot-network` rebuild, and edge TTLs of 20s/30s let every colo recompute on the 60s desktop poll | one shared per-isolate read of the wider window; leaderboards/history edge TTL 300s, stats 60s (matching the 60s desktop and 5m web cadences). |
| `SELECT COUNT(*) FROM repositories` (~24k) | network stats per edge miss, `/api/status` every 3 min | memoized behind `catalog_rev` (10-minute max age). |
| `SELECT … FROM repositories WHERE is_private = 0` again (~140k by mid-day 2026-09-13, ~3 scans a minute) and `COUNT(*) FROM repositories` (~33k) | isolate churn: the fingerprint fast path ran ~570 times in 2.5 hours, and every cold isolate's first catalog read (any info/refs, the cron, the catalog GET) rescanned and re-decrypted the table before the `catalog_rev` memo could help | whichever isolate scans publishes the decrypted rows plus the repository count as one encrypted `public_catalog_snapshot` row keyed by the `catalog_rev` it read first; a cold isolate hydrates from that single row (count-only readers select the leading columns, so the payload's overflow pages are not read) and scans only when the rev moved or the snapshot is older than the same 10-minute max age. One table scan per catalog change fleet-wide, not one per isolate. `admin_stats` now goes through the count memo too. |
| `repositories` scan inside `network_leaderboards` (~18k) | every rebuild | reuses the decrypted catalog memo. |
| `repo_state_history … LIMIT 100` (~20k) | `mirror-health` every minute | a one-row `MAX(ts)` probe fingerprints the history; the bounded list is re-read only when it moved. |
| admin signup/referral evidence, four whole-table aggregates (~85k) | every `GET /api/accounts/users` that qualified as admin — including an administrator's browser chat tab every 5 minutes via the session cookie | only an explicit bearer/signed credential opens the admin projection; it is memoized per isolate for 60s; the desktop asks for it only while the Users page is showing. |
| `users LEFT JOIN feedback_email_sends` (~13k) | hourly `feedback-email` | every six hours (signup time is inside the encrypted record, so the anti-join must walk every user). |

Rule of thumb carried forward: a per-minute phase may read one indexed row to
learn nothing changed; it may not re-read the thing itself.

### Second pass (2026-09-13): request-driven readers

Two days later the daily total was back near 1.5M, but the bill had moved from
the cron to request paths — queries that are cheap once and were simply run far
too often. Same rule, applied to requests:

| Query (rows/day before) | Trigger | Now |
| --- | --- | --- |
| `DELETE FROM notifications … NOT IN (SELECT … LIMIT 500)` (~150k in one hour, the largest) | every ping insert trimmed the recipient's inbox to the cap with an anti-join over the whole inbox — two full scans per insert for a 500-row inbox | the trim runs at most once per recipient per 15 minutes per isolate, and deletes by an indexed `ts <` range below the 500th-newest row (one index walk, then only the rows it removes). The 90-day retention delete is already an indexed range and still runs every time. |
| `SELECT … FROM repositories WHERE is_private = 0` (~237k, back again) | `_https_mirror_public_context_cache_clear` wiped the decrypted catalog memo on every state-pin admission and lease renewal, not only on catalog mutations — ~4.5k full rescans a day | a context clear keeps the rows and only forgets the `catalog_rev` probe; the next reader re-reads the counter and rescans solely when it moved. Real mutations (`purge_catalog_related_caches`, `_bump_catalog_rev`) mark the memo stale explicitly. |
| admin users directory `users LEFT JOIN … ×4` (~90k) + `profile_contribution_days` commit totals (~140k) + `referral_attributions … decision='credited'` (~15k) | every desktop Users-page refresh rebuilt the projection from D1 (33 builds in one hour) | the joined rows are memoized per isolate for 60s, commit totals and credited referrers for 5 minutes; every roster invalidation (`edge_cache_delete(USERS_DIRECTORY_CACHE_KEY)`) drops all three. Public roster edge TTL 30s → 120s. |
| `organization_tasks … ORDER BY completed_at>0,priority,updated_at DESC,task_id DESC LIMIT 25 OFFSET n` (~64k) | the desktop task list page; no index matched the sort, so every page sorted the organization's whole task set | migration 0178 adds an expression index in exactly that order; SQLite walks it and stops at the page boundary. |
| `chat_history` replay window (~66k) and per-store bounds prune (~33k) | joining a room ran window functions over every retained row; every stored message re-ran the count/byte prune over the whole room | the replay's innermost SELECT stops at the 64-message ceiling through the `(room_key, ts)` index; the bounds prune runs on the first store per room per isolate and then every 16th. |
| `repo_state_history … LIMIT 100` (~34k, back again) | the pin memo had one slot, so the fleet alternating between two repositories evicted it on every switch | one slot per repository (64 keys). |
| `SELECT COUNT(*) FROM repositories` on publish (~29k) | every catalog publish counted the table for the cap check | uses the `catalog_rev`-keyed count memo. |
| `online_hourly_nodes` (~70k) | per-isolate memo of 2 minutes behind boards cached 5 minutes at the edge | memo 5 minutes. |

## Durable Object duration: one object per tick (2026-09-13)

The account used 98.6% of the 13,000 GB-s Free duration allowance on
2026-09-12 (12,818 GB-s), and was on course for 107% on 2026-09-13. Billed by
namespace: `ForkMeshCronRunner` 11,338 GB-s (88,578 object-seconds),
`ForkMeshNodes` 916, `ForkMeshRoom` 346, `ForkMeshDiscordGate` 203. Inside
the cron namespace one object (`scheduled-runner-v1`, the coordinator) was
active 50,799 s with only 37 s of CPU: it spent the whole tick awaiting the
batch objects it had fanned the phases out to, so every second of phase work
was billed twice, and each batch object paid its own wake on top. Cloudflare
bills duration for wall-clock time while an object has work in flight
(objects idle and eligible for hibernation are free), so the fix is
structural, not a longer TTL anywhere:

- The coordinator's alarm now runs the phases itself
  (`ForkMeshCronRunner._coordinator_alarm` / `_run_tick_slice`). When the
  next phase's measured reserve no longer fits the invocation's fifty
  subrequests, it persists `tick_state_v5` and re-arms its alarm for "now";
  the next invocation resumes on the same object with a fresh budget. The
  batch, per-phase and disposable status objects and the dispatcher that
  waited on them are deleted. Per-isolate memos (`_STATUS_MINUTE_BATCH_MEMO`,
  the catalog snapshot memo, mirror proof memos) now also survive from one
  minute to the next, because the same object runs every minute.
- `ForkMeshNodes` answers the `{"type":"ping"}` keepalive with
  `setWebSocketAutoResponse`, which the runtime serves without waking the
  hibernated object and without billing duration; the stale sweep reads
  `getAutoResponseTimestamp()` beside the attachment clock. The desktop sends
  that frame every four minutes and the Go daemon every five, per node.

Expected: cron duration falls from ~61 object-seconds per minute to the tick's
own wall time (10–25 s of sequential probes and D1 round trips) plus nothing
for waiting; the node hubs drop to the frames that carry work. Measure with
`durableObjectsPeriodicGroups` (see the D1 section for the GraphQL recipe).
What is left after that is the tick's wall time itself: the per-minute
sampler, alert scan and mirror challenge are sequential network waits, and
parallelising them (JS promises started ahead of the awaits, never Python
tasks) or moving the per-minute sampler out of Python and Durable Objects
entirely is the next lever toward a 10% day.

## Elimination plan

**Phase 1 — desktop client goes push-only (mirrors of the intake pattern).**
Done for the big ones (see the eliminated table): the `m_inboxPollTimer`
fallback is deleted, `fetchMirrorPendingCounts` is fetch-once with
push/drain/open invalidation, and `refreshWebAlerts` reads the ping inbox once
per run behind the new `pings` topic. Remaining: include the per-topic tally in the
event frame itself — the relay already knows the counts at write time
(`notify_repo_host` fires on every submission) and the frame stays tiny and
content-free — so the desktop's `/pending` GET disappears entirely. #3–#7
each become a topic on the same socket (`admin-pending`, `directory`,
`notes`, `agent-limits`); the relay's write paths already call
`notify_repo_host` hooks where most of these change.

**Phase 2 — mirror node git sync.** Done (see the eliminated table): the
catalog publish path fans a `commits` event to mirrors whenever a source's
heads-or-tags digest moves, and the daemon's `syncRequests` channel is
subscribed to the shared `EventSocket`. Each commit frame carries the signed
target refs digest, so a stale round-robin response is retried on a short
backoff instead of counting as converged, and relay-only nodes keep one
30-minute reconciliation pass solely as a dropped-event backstop. Third-party
upstreams use their configured fetch cadence because the relay cannot push for
them. The 4m endpoint lease proof stays, but shares the event WebSocket instead
of opening an HTTP request; older/disconnected relays use HTTPS recovery.

**Phase 3 — website.** The dashboard and world already hold sockets on several
pages (chat, world, office, notes). Add a read-only, unauthenticated
"repo events" channel — one DO per repo, fed by the same write-path notify
calls — and move #9 and #13–#16 onto it. Personal notifications (#12) are
done, and they turned out not to need a new socket at all: the browser joins
the desktop's per-account `ForkMeshNodes` channel with a short-lived ticket
(`public/account-events.js`), which is now the obvious home for any other
account-scoped web poller. Session validity (#11) needs no timer at all: check on
`focus`/`visibilitychange` (both hooks already exist) and on any 401 from a
real request. Fast operational views (#15, #17, deploy status) move to the
socket last, since they are visible-tab-only and cheap.

**Phase 4 — relay cron audit.** For each minute-cron job, ask "which write
made this necessary?" and trigger it there (or via a DO alarm set by the write
path). Cron stays only for true time-based work (reward intervals, expiries).

Order matters: phase 1 removes the largest authenticated-poll population,
phase 2 the steadiest background load, phase 3 the long tail of anonymous
per-tab traffic. After each phase, the API traffic chart — the same one that
caught `/pending` — is the acceptance test: the eliminated endpoint's request
group should drop to ~zero, not merely shrink.
