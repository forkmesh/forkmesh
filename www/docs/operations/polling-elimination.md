# Polling inventory and elimination plan

Goal: no ForkMesh client polls the relay for data. Every "did anything change?"
question becomes a payload-free push over a WebSocket, answered by the one
signed HTTPS read the client would have made anyway. The transport is the
per-owner `ForkMeshNodes` object behind `GET /api/nodes/events` (desktops,
headless nodes and, via tickets, browsers) plus the chat, world, office and note
room sockets.

## Rules

- **Read on a reason, never on a display timer.** A client reads when a page
  opens, when the reader presses Refresh, when the app or window comes back, or
  when a socket frame says something changed.
- **No fallback polls.** A safety-net poll silently becomes the load. The socket
  owns liveness: reconnect is unconditional with bounded backoff, and every
  (re)connect fires one catch-up read.
- **Heartbeats are not polls.** Liveness proofs (presence, endpoint lease) keep
  their cadence but ride an already-open socket; HTTPS is only the recovery path.
- **The socket is transport, never authority.** A frame carries the same signed
  proof as the HTTPS route and is handed to the same handler.
- **Count requests per event, not timers per minute.** One request per row or
  several callers reading the same resource is a poll in disguise; coalesce and
  revalidate with ETags.
- **Server phases read one indexed row to learn nothing changed**, never the
  table itself (for example the `catalog_rev` counter before a catalog scan).

## Account event channel

`notify_account_event(env, owner, topic)` fans a payload-free frame to the
account's `ForkMeshNodes` object from the writes that move a count: pings
(`pings`), direct messages (`direct-messages`), private channels, issues,
catalog changes and cloud agent runs (`cloud-agents`). Browsers join with a
60-second ticket from `GET /api/accounts/event-ticket` (a WebSocket upgrade
carries no `Authorization` header) through the shared `public/account-events.js`.
The chat header badge advances locally from the room socket
(`noteSeenChatActivity`) and is re-read once on `visibilitychange`/`pagehide`.

## Eliminated

| Former poller | Replacement |
| --- | --- |
| Mirror collaboration intake `GET /api/repo/*/pending` | Node event socket; push leases the changed inbox, catch-up on reconnect |
| Desktop inbox sync and `/pending` badge fetch | Socket-driven sync; badge fetched once and invalidated by push, drain or open |
| Unread counts: `refreshWebAlerts`, `startNotificationPolling`, `refreshDirectMessages`, `markChatActivitySeen` | One read on open, then account event pushes |
| Mirror git sync loop | `commits` push carrying the signed refs digest; 30-minute reconciliation backstop |
| Account heartbeat and endpoint lease over HTTP | Frames on the node socket, HTTPS fallback |
| Session validity, dashboard and World (`validateActiveWorldSession`) | Event-ticket handshake refusal; focus/visibility check behind a 5-minute floor (`WORLD_SESSION_REVALIDATE_MIN_MS`) |
| Signed-out session restore on every page | 401 remembered per tab (`forkmesh.sessionRestoreMiss`) |
| Portable grant re-signing on each session read | Reused while younger than `PORTABLE_SESSION_REISSUE_MS` |
| Desktop status/page probes on a minute tick | Launch, recheck and window activation (`refreshFooterStatusOnActivation`) |
| Desktop user directory (`m_chatDirectoryTimer`), notes, pull requests, agent sessions, host probes | Read on open, Refresh, or a socket frame |
| Mobile agents, actions and inbox timers | Tab open, pull to refresh, push and resume catch-up |
| World admin error badge (`ADMIN_ERROR_POLL_MS`) and Office agent bots (`WORLD_AGENT_BOT_POLL_MS`) | Slowed to 60s and 30s, visible tabs only |
| Catalog republish from several publishers | Single publisher (the Go mirror daemon), fingerprinted records |

## Still polling

- Desktop: admin pending-user verification (5m, admins only), mirror
  registration renewal (5m), agent limits (60s).
- Dashboard: pending badge while a repository shows pending items (10m).
- World: community events, mirror catalogs, action runs, QA/build/status boards,
  imports and deploy status; all visible-tab only.
- Chat pages: the visible-tab `USERS_DIRECTORY_REFRESH_MS` directory tick (a new
  account has no frame to announce itself) and the room presence proof.
- Server: the App minute cron, which runs every due phase on one coordinator
  object and continues in a fresh alarm when its subrequest budget runs out.

## Remaining plan

1. Fold per-topic tallies into event frames so the desktop `/pending` read
   disappears, and move admin, notes and agent-limit reads onto socket topics.
2. Add a read-only per-repository event channel for the website and move the
   dashboard badge and World repository pollers onto it.
3. For each minute-cron phase, trigger the work from the write that made it
   necessary; keep cron only for time-based work such as expiries.

Acceptance for each step: the eliminated endpoint's request group in the API
traffic chart drops to about zero, not merely lower.
