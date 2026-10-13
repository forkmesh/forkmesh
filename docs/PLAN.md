# ForkMesh Next: an independent, low-budget, Cloudflare-free stack

## Status (2026-09-24)

Phases 0 to 7 are built and the fleet runs: three core nodes (`n1` to `n3`,
Vultr labels `fmnext-core-1` to `-3`) and one runner (`r1`, `fmnext-run-1`), described in `deploy/fleet.toml`. The plan
below is kept as written; where the running system went another way, this
list says so and names the doc that is now authoritative.

- **Data.** The plan called for a fresh, empty database. Next instead runs on
  an imported copy of production's D1, re-keyed on 2026-09-24 onto Next's own
  `DATA_KEY` ([REKEY.md](REKEY.md)).
- **Secrets.** Nodes start sealed and hold `DATA_KEY` only in memory; the
  desktop unseals them ([VAULT.md](VAULT.md)).
- **No leader.** Cron, Durable Object alarms, DNS health votes and
  certificates are leaderless (ac7c6d3, migration 0196): the cron coordinator
  rotates across live nodes every minute, each node writes only its own
  membership row, and `deploy/core/certsync.py` runs the same on every core
  ([OPERATIONS.md](OPERATIONS.md) section 4).
- **Integrations.** Instance identity stays Next's own, but some service
  credentials are reused from production on purpose (Mailtrap, APNs, Discord
  OAuth; production's Sentry DSN was dropped again in 0b3ee3b; `optional_secrets` in `deploy/fleet.toml`, 11fb6f1).
  ActivityPub and webhooks stay off production (d1a577f).
- **Signup** is protected by Cloudflare Turnstile (`app/src/turnstile.py`,
  c7804de; [OPERATIONS.md](OPERATIONS.md) "Signup challenge").
- **Added after the plan:** host intrusion detection ([IDS.md](IDS.md),
  migration 0197), runner job sandboxing and web sign-in
  ([RUNNERS.md](RUNNERS.md)), Market ratings
  ([MARKETPLACE_RATINGS.md](MARKETPLACE_RATINGS.md), 0198), repositories
  hosted on the core ([HOSTED_REPOS.md](HOSTED_REPOS.md), 0199), page
  performance work ([PERF_PAGES.md](PERF_PAGES.md)), native iOS/Android
  development (`docs/native/`), and a private Git remote on the cores for
  Codemagic builds ([OPERATIONS.md](OPERATIONS.md) "Native app builds").
- **Later, now started:** moving production onto this stack is planned in
  [CUTOVER.md](CUTOVER.md).

The index of all Next docs is [README.md](README.md).

## Context
Production ForkMesh runs on one Cloudflare Python Worker (`forkmesh-app`: D1, KV, Queue, cron, 8 Durable Objects, Workers AI), and a Vultr mirror fleet serves Git. We want to leave Cloudflare, but without risking production. So we build a **completely separate** deployment: a new subdomain, a new repo, a fresh database and **new Vultr servers**, sharing nothing with today's Worker, D1, mirror fleet or desktop autoscaler. Only the homepage (a Cloudflare Worker) and DNS records stay on Cloudflare.

Requirements from the user: keep costs low (the cheapest ~1 GB Vultr plan), keep it performant, run agents on the nodes, and "come up with something better than what we're planning or what we have".

## The rethink: split trusted state from untrusted compute
The old plan put the app, database, Git and agents on every node. That fails in two ways:
1. **Memory.** An agent run (the Claude or Codex CLI, a clone, and builds) takes 300–600 MB. The app and rqlite already need 550–800 MB. They can't share a 1 GB box.
2. **Security. This is the real problem.** An agent runs arbitrary code chosen by a prompt. On a node that holds `DATA_KEY`, `ADMIN_PASS`, password hashes and the full database, one prompt injection could leak everything. The production mirrors have the same exposure today (the daemon runs agents next to its identity key).

**New architecture: two tiers that talk only through the existing signed lease APIs.**

```
Cloudflare:  next.forkmesh.com (homepage Worker, static) + DNS records only
                    │ fetch
┌─────────── CORE cell: 3 × 1 GB (trusted, holds data + secrets) ───────────┐
│ app.next / world.next round-robin A records                                │
│ each node: Caddy · app (Python on CPython) · rqlite (Raft) · Go daemon +   │
│            Git gateway.  NO agents, NO untrusted code.                     │
│ WireGuard mesh between the 3 nodes                                         │
└──────────────▲─────────────────────────────────────────────────────────────┘
               │ outbound only: claim job lease → clone → run → push branch → report
┌──────────── RUNNER tier (untrusted, stateless, holds no core secrets) ─────┐
│ a) 1 always-on runner, 1 GB, one agent at a time (cheap baseline)          │
│ b) burst runners: hourly Vultr instances, started from a snapshot when     │
│    the queue backs up, destroyed when idle; hard cap on count and spend    │
│ c) bring-your-own: user desktops and self-hosted boxes run the same daemon │
│    as runners (the desktop already claims agent jobs today)                │
└────────────────────────────────────────────────────────────────────────────┘
```

**Why this is better than both the plan and what we have now:**
- **Cost.** The core is 3 of the cheapest plan (about the price of today's fleet). The always-on runner is one more cheap box. Burst runners are billed by the hour and exist only while jobs run; a 20-minute job on a 2–4 GB instance costs cents. BYO runners cost us nothing.
- **Blast radius.** A runaway build or a malicious prompt can only damage a disposable runner. That runner holds a per-job, per-branch push capability, not the database or secrets. The core's memory budget stays predictable, so rqlite never gets OOM-killed by an agent.
- **Scaling.** Agent capacity scales with the job queue, not with the size of the database cluster. The core stays at exactly 3 nodes for Raft.
- **Reuse.** The Go daemon already claims signed agent leases, runs the CLIs headlessly on a scratch clone, and pushes a branch (`server/agents.go`, `agentMaxConcurrentRuns = 2`). Runners are the same binary with a new `role: "runner"` (no gateway, no catalog, no repos kept on disk).

## Decisions so far
- **Hostnames:**
  - `next.forkmesh.com`: homepage, a new Cloudflare Worker named `forkmesh-next-www`.
  - `app.next.forkmesh.com` and `world.next.forkmesh.com`: round-robin across the 3 core nodes.
  - `n1/n2/n3.next.forkmesh.com`: per-node mirror and health hostnames.
  - Runners have no public hostname; they only make outbound connections.
- **New repo** `~/projects/forkmesh-next/src`, seeded with `git archive` of this repo's `main` at a recorded commit: `app/`, `world/`, `www/`, `server/`, `tools/`, `marketplace/`, `conformance/`, `desktop/` for reference only. Tracked files only, so no `.env.production`. Its own history from there.
- **Fresh, empty rqlite database.** The 199 migrations are applied on day one, and no production user data ever lands on the new servers.
- **Core runtime:** the Python app, unchanged, on CPython behind a thin host shim for `js`, `pyodide.ffi`, `workers` and `env` (the Go rewrite was considered and rejected: about 150k LOC). The database is rqlite as a sidecar process.
- **Independence from production:**
  - New secrets: `DATA_KEY`, `ADMIN_PASS`, router seed and node identity keys.
  - A new Vultr SSH key at `~/projects/forkmesh-next/ssh/`.
  - Instances are labelled `fmnext-core-N` / `fmnext-run-N` and tagged `forkmesh-next`.
  - They are **never** added to the desktop's `hosts/list`. The desktop only destroys instances it matches to saved hosts by name or IP (`MainWindowHosts.cpp:1284`, `MainWindowNodes.cpp:1197`), so distinct labels keep them safe.
  - Integrations start **off**: Discord, Patreon, APNs, Sentry, ActivityPub, Solana. Mailtrap uses its own sandbox token. Nothing can double-process production webhooks or federate as forkmesh.com.

## Phase 0: New repo, measurements, accounts (nothing touches production)
1. Create `~/projects/forkmesh-next/{src,data,config,ssh,logs}` and `git init` in `src/`, then `git archive main | tar -x`. Record the source commit in `UPSTREAM.md`.
2. **Rehost the code.** `tools/rehost.py` turns hardcoded production hosts into config values:
   - `app.forkmesh.com`: 343 hits in app, 168 in www, 42 in world, and hits in server/.
   - Mainly `wrangler.toml [vars]` (`PUBLIC_BASE_URL`, `APP_ORIGIN`, `WORLD_ORIGIN`, `WWW_ORIGIN`, `CORS_ALLOWED_ORIGINS`), `server/daemon.go:591`'s relay host switch, and `www/worker.js` `DIRECT_MIRRORS`.
   - Add a test that fails on any production hostname outside an allowlist.
3. **Measure on one 1 GB Vultr box:**
   - RSS of the app (every lazy module loaded, 300 idle WebSockets, a full cache) and of rqlite (empty DB, then a synthetic 1 GB DB).
   - Peak memory of a git clone.
   - PBKDF2 time.
   - k6 baselines. There is no production traffic to compare against yet, so compare with the production Worker on the same routes.
   - **Gate:** app ≤ 260 MB and rqlite ≤ 150 MB at steady state, and ≥ 150 MB left for page cache. Otherwise, fall back to 2 GB for the core only.
4. **Run the rqlite checks:**
   - RETURNING through `/db/request`, `?transaction` rollback, associative rows, `rows_affected`/`last_insert_id`, JSON1 functions, window functions, int64.
   - Rewrite of non-deterministic SQL: the trigger `trg_chat_channel_member_remove_rotate` (`app/src/schema.py:639-646`) uses `strftime('%s','now')`, so move that timestamp into app code.
   - Safe read-only in-process `sqlite3` reads of rqlite's on-disk file.
5. **Region:** all core nodes in one Vultr region (ewr). Runners can go anywhere.
6. **No object storage.** Backups are pulled to the operator box (Phase 2), and certificates are shared over WireGuard (Phase 4).

## Phase 1: CPython host shim (`app/host/forkmesh_host/`)
- **`js.py`** covers everything used today:
  - `Date`, `JSON` (backed by orjson), `Object.fromEntries`, `URL`, `AbortController`/`AbortSignal.timeout`, `setTimeout`, `scheduler.wait`, `Uint8Array` (over a memoryview), `Request.new`/`Response.new` (including the `{"status":101,"webSocket":…}` form), `WebSocketPair`, `TransformStream`.
  - `fetch` on a shared streaming `httpx.AsyncClient`.
  - `caches.default` as an in-process LRU of about 8 MB, capped by bytes.
  - `crypto.subtle` on `cryptography`: SHA-1/256, HMAC, Ed25519, AES-GCM, PBKDF2, ECDSA P-256, RSASSA. CPU-heavy operations run through `asyncio.to_thread`.
- **`pyodide/ffi.py`:** `to_js` returns the value unchanged. `jsnull` is a singleton whose type is named `JsNull`.
- **`workers.py`:** `Response`, `WorkerEntrypoint`, `DurableObject`.
- **`asgi.py`:** uvicorn (uvloop, httptools, websockets) in one process on one event loop, calling `Default().fetch()`. On a 101 response it bridges the WebSocket; otherwise it sends a denial.
- **`env.py`:** `[vars]` plus secrets from systemd credentials.
  - `NOTIFICATION_QUEUE`: a per-node outbox drained by that node through the existing `process_notification_queue` (no leader).
  - `REPOSITORY_METADATA`: a local disk cache.
  - `AI`: an optional OpenAI-compatible HTTP shim. Unset means the existing "AI binding is missing" path runs.
- **Cron:** every node's host timer calls `scheduled()` every minute and kicks `FORKMESH_CRON_RUNNER`; the kick lands on this minute's owner of the coordinator, which rotates round-robin across the live nodes (Phase 3 "Placement"), so the status probes run on a different node each minute.
- **Small guarded edits to `entry.py`** behind `FORKMESH_PLATFORM=vps`:
  - D1 query logging off.
  - The GraphQL usage budget (~5416/5593) returns "unavailable".
  - DoH (17727, 40987) uses a configurable URL.
  - PBKDF2 (~13568) runs in a thread.
- **CI gate:** `tools/js_surface_inventory.py` fails when app code uses a `js`/`workers`/`env` attribute the shim doesn't implement.

## Phase 2: rqlite adapter and migrations
- **`forkmesh_host/d1_rqlite.py`:** the D1 shape (`prepare().bind().first/all/run`, `batch`, `withSession`).
  - Writes go to `POST 127.0.0.1:4001/db/request?associative`. `batch()` uses `?transaction`, so it is atomic.
  - `.meta.changes` and `.meta.last_row_id` are filled, and errors map to D1-style strings (no leader → "overload", which gives the existing 503 backpressure).
- **Reads:**
  - Plain reads are in-process read-only `sqlite3` on the rqlite file (microseconds). If the Phase 0 check fails, fall back to HTTP with `level=none`.
  - Reads after a write in the same request use `weak`.
  - `d1_first_primary` / `withSession("first-primary")` use `linearizable`.
- **`tools/rqlite_migrate.py`:** records applied migrations in the `d1_migrations` table, splits statements with `sqlite3.complete_statement`, runs against any node of a cluster that has a Raft leader, and stamps `schema_meta` so `_apply_schema` takes its fast path. `d1_read_guard` is set to `off`.
- **Backups:** no object storage. The operator box pulls `GET /db/backup?compress` from the leader every night over an SSH tunnel (`tools/next_backup.py`, `deploy/operator/fmnext-backup.timer`), verifies it (`PRAGMA integrity_check` plus table counts) and keeps 7 daily + 4 weekly copies in `~/projects/forkmesh/data/fleet/backups/`.

## Phase 3: Durable Objects on the core (`forkmesh_host/durable.py`)
- **Owner routing:** rendezvous hash of `ns:name` over the **live** members. Membership is leaderless: each node writes only its own `cluster_members` row (heartbeat every 5 s, status `active` or `sealed`); every node reads the table with `level=weak` and derives liveness itself (heartbeat ≤ 15 s old, not sealed). Nobody writes another node's row.
  - When the owner is this node, the call is direct.
  - Otherwise it goes to `wg-ip:8792/_do/…` with an HMAC (`FORKMESH_MESH_SECRET`), and WebSocket frames are bridged.
- **Hibernation API emulation:** `acceptWebSocket`, `getWebSockets(tag)`, socket attachments, and one `asyncio.Lock` per object.
  - `ctx.storage` uses the rqlite table `do_storage`.
  - `setAlarm` uses `do_alarms` plus an asyncio timer; a new owner re-arms them.
  - Idle objects are evicted after 10 minutes.
- **Placement:** ForkMeshRoom, Nodes, NoteRoom, World, OfficeRoom, DiscordGate and the CronRunner doorbells (snapshot-republish, mirror-wake) are hashed. The cron coordinator (`scheduled-runner-v1`) and CronWatchdog **rotate**: `live_sorted[(h(ns:name) + minute) mod N]`, with the hand-off 45 s into each minute (after the tick's 35 s bound). At each hand-off the new owner re-arms the alarm at once; an alarm is delivered only by the node whose `DELETE FROM do_alarms ... AND at_ms=?` removed the row (exactly once), and a dropped rotating object's storage refuses writes (a fence). `host_cron_runs` records which node ran which minute. rqlite's own Raft leader is not used by any host-level role.
- **Split-brain guard:** a node without quorum reports not-ready and closes its object sockets with code 1012.

## Phase 4: Core node packaging (3 × 1 GB)
- **systemd units:** `rqlite` (`OOMScoreAdjust=-800`, `GOMEMLIMIT=110MiB`), `forkmesh-app` (`MemoryHigh=280M`, `MemoryMax=340M`), `caddy`, and `forkmesh-mirror-node` with `role: "core"`. Agent jobs and Actions are disabled on the core.
- **Swap and OOM:** zram (lz4, 50%) plus a 1 GB swapfile, `earlyoom`, and journald capped at 64 MB.
- **Caddy:** the stock Debian package (2.6.x). No storage module and **no DNS token on the core**:
  - No fixed ACME issuer. For `app.next`/`world.next` the node that issues or renews is the rendezvous-first (on the hostname) **live** core node, and only when the freshest certificate held anywhere is missing or within 30 days of expiry; if it dies, the next node in rendezvous order takes over. Every node's Caddy sends `/.well-known/acme-challenge/*` for those names to a small forwarder (`fmnext-certsync serve`, loopback) that asks the other cores' Caddy over WireGuard in rendezvous order, so HTTP-01 succeeds whichever A record Let's Encrypt picks.
  - `deploy/core/certsync.py` runs the same way on every node: each exports the freshest valid pair it holds and serves it on its WireGuard address to HMAC-authenticated requests (`FORKMESH_MESH_SECRET`); every 5 minutes each node pulls from all live peers, keeps the freshest pair that validates (key matches, covers the name, not expired), writes it 0600 to `/etc/forkmesh-next/tls/`, picks `files`/`acme`/`internal` per name in generated Caddy includes and reloads Caddy only on change.
  - Each node gets its own `nN.next` certificate normally.
  - `app.next` and `world.next`:
    - Static files (`app/dist`, `world/public`) are served directly and precompressed, so Python never sees them.
    - Dynamic routes go to uvicorn on 127.0.0.1:8080.
    - Incoming `Cf-*` headers are stripped and `Cf-Connecting-Ip {remote_host}` is set.
    - Country comes from a local DB-IP Lite file.
  - `nN.next`: the Git gateway on 8790.
- **Local app calls:** `/etc/hosts` maps `app.next.forkmesh.com` to 127.0.0.1, so the daemon's catalog and lease calls stay on the node and TLS still validates.
- **Secrets:** they live on the operator box in `~/projects/forkmesh/config/fleet/`, are pushed over SSH into `systemd-creds encrypt`, and loaded with `LoadCredentialEncrypted=`. They never go into rqlite or backups. The firewall allows 22, 80, 443 and 51820 (WireGuard).

## Phase 5: Runner tier (agents)
- **`role: "runner"` in the Go daemon (`server/config.go`, `server/agents.go`):**
  - Runs no gateway, no catalog publishing, and keeps no repos on disk.
  - Claims agent and Actions leases from `app.next`, clones the one repo over HTTPS, runs `claude`/`codex` in a per-job systemd scope (`MemoryMax`, `CPUQuota`, a private tmp dir, no access to other jobs), pushes the result branch, reports, and wipes the workdir.
- **Per-job capability.** The lease includes a short-lived, router-signed push capability for exactly `refs/heads/agent/<session>` of that repo. The gateway's existing receive-pack capability check (`--verify-mirror-capability`) enforces it. Runners never hold a node identity that can write anything else.
- **Provider credentials (Claude/Codex login):**
  - The always-on runner keeps its own login through the existing relayed login terminal.
  - Burst runners get the owner's provider token sealed to the runner's one-time key inside the lease. It exists only for the job and is deleted afterwards.
  - Or they only accept jobs from owners who allow cloud runs.
- **Burst autoscaler** (`server/runnerpool.go`, enabled in one core node's daemon config via `runnerPool`; off by default):
  - Queue depth ≥ 1 for 2 minutes and the always-on runner busy → create a `fmnext-run-N` instance from a prebuilt Vultr snapshot (Debian + daemon + CLIs).
  - Destroy it after 10 minutes idle.
  - Limits: `maxBurstRunners` (default 2) and a monthly spend ceiling. The Vultr API key is the only core-held cloud credential; use a Vultr sub-user limited to the instances permission.
- **Bring-your-own:** the desktop and `install.sh` can register a machine as a runner for `app.next` with a link code (same flow as today). Personal and org jobs can be pinned to your own runner.

## Phase 6: Homepage, DNS, health
- **`www/worker.js` → `forkmesh-next-www`**, custom domain `next.forkmesh.com`:
  - Replace the `env.APP` service binding with `fetch(APP_ORIGIN + path)`, with a timeout and a fallback to the `nN.next` node URLs.
  - The client IP and country travel in an HMAC-signed `x-forkmesh-edge-client` header (`WWW_EDGE_SECRET`).
- **DNS records:** 3 unproxied A records each for `app.next` and `world.next` (TTL 60), plus `n1–n3.next`.
- **Failover (leaderless, on the nodes):** every unsealed core node probes every core node's `https://app.next/readyz` (connected to that node's IP) and `https://nN.next/readyz` every 30 s and publishes its vote in `dns_health_votes` (down after 3 failures, up after 2 minutes healthy). The node chosen for each 30 s interval (round-robin over the live members) changes the `app.next`/`world.next` A records through the Cloudflare API only when a strict majority of live voters agree. Guards (shared with `tools/next_dns.py` in `forkmesh_host/dns_rules.py`): only those two names (never `nN.next`), never empty a set, at most one change per 5 minutes cluster-wide (compare-and-set on `last_change_ms` in rqlite), TTL 60, DNS only. Acting needs `NEXT_DNS_API_TOKEN` in the vault; since the 2026-09-24 audit it is not there (the only token edits the whole forkmesh.com zone), so the fleet runs `dry`: nodes vote and log, and record changes stay with `tools/next_dns.py` on the desktop until a next-only-zone token exists. A sealed node neither votes nor acts. Browsers also try the next A record on their own.
- **world:** served by Caddy on the core, with the api-origin meta injected at build time. There is no world Worker.

## Phase 7: Fleet tooling (in the new repo; the desktop autoscaler is not involved)
- `tools/next_fleet.py`, using the Vultr API with the new SSH key:
  - `create-core`: cheapest plan with at least 1 GB, region ewr, label `fmnext-core-N`, tag `forkmesh-next`.
  - `install`: WireGuard, rqlite join, venv from bundled wheels, Caddy, the daemon, secrets.
  - `create-runner-snapshot`.
  - `status`.
  - `replace-core N`: join the new voter and wait for catch-up, then `rqlite remove` the old node, then destroy it. It refuses to act if fewer than 3 voters are healthy.
- `tools/next_deploy.py`:
  1. Build (the existing `[build]` command), then a tarball with `BUILD_REV`.
  2. Upload to `/opt/forkmesh-next/app/releases/<rev>`.
  3. Run `rqlite_migrate` through any node of a healthy cluster (rqlite forwards the writes).
  4. Rolling restart one node at a time in fleet order, each gated on `/api/version` and `/readyz`.
  5. On failure, move the symlink back to the previous release.
- `forkmesh-next-www` is deployed with plain `wrangler deploy` under its own config.
- Installer and fleet tests follow AGENTS.md: sandbox `HOME` and the `XDG_*` dirs, drop `FORKMESH_HOME`/`FORKMESH_PROJECTS_DIR`, and never delete `~/projects/forkmesh/*`.

## Performance (release gates)
- Python never serves static bytes; Caddy serves precompressed, immutable assets.
- Local reads are in-process read-only SQLite. Only writes and fresh reads use rqlite HTTP.
- Nothing CPU-heavy runs on the event loop: PBKDF2, big JSON, zlib, OG/PNG rendering and RSA use `to_thread`.
- CPython 3.14 via `uv` (matching Pyodide's version), uvloop, `-OO`, precompiled `.pyc`, `gc.freeze()` after warm-up, `MALLOC_ARENA_MAX=2`, orjson.
- Warm-up before `/readyz`: import the modules the top routes need and prime the snapshot caches.
- Public snapshot, catalog and status JSON are cached in memory, invalidated by the republish doorbell.
- Cron yields to the event loop between steps and rotates between nodes every minute. A node whose `/readyz` fails for a majority of voters drops out of the DNS set.
- Metrics: event-loop lag, logs for requests slower than 250 ms, and per-route p50/p95 at `/api/metrics/summary`. `py-spy` is available on every node.
- **Targets on 1 GB:**
  - API reads p50 ≤ 15 ms and p95 ≤ 80 ms at the node.
  - Writes p95 ≤ 120 ms.
  - Login p95 ≤ 300 ms.
  - 300 WebSockets plus 30 rps with event-loop lag p99 ≤ 50 ms.
  - No slower than the production Worker on its top 20 routes.
- **Escape hatch:** a route that still misses its target moves into the Go daemon, which already runs on every node.

## Critical files (in the new repo)
- `app/src/entry.py`: imports L1-22, fetch/cache 2064-2200, D1 helpers 12458-12782, WebSocket handoff ~46674, `Default` 47016, DOs 47178-49308.
- `app/src/app_runtime.py`, `app/src/schema.py:639`, `app/wrangler.toml`.
- `world/src/world_durable_objects.py`, `www/worker.js`.
- `server/config.go` (roles core/runner), `server/agents.go` (runner mode, per-job capability), `server/daemon.go` (relay hosts), new `server/runnerpool.go` and `server/fleet.go`.
- `tools/mirror_gateway.py` (receive-pack capability for runner pushes), `tools/direct_https_bootstrap.py` (`caddy_config`).
- New: `app/host/forkmesh_host/*`, `tools/{rehost,rqlite_migrate,next_fleet,next_deploy,js_surface_inventory}.py`.

## Verification
- The ~499 existing app tests pass unchanged in the new repo.
- New `app/tests/host/` covers:
  - WebCrypto vectors against Node;
  - the D1-adapter suites parametrized over sqlite3 and a local rqlite;
  - DO ownership moves and alarm takeover;
  - the surface-inventory gate.
- `server/` Go tests for runner mode:
  - capability scope (a push outside `agent/<session>` is rejected);
  - the workdir is wiped;
  - the spend cap stops the autoscaler.
- Local cluster: 3 rqlite + 3 uvicorn processes, with `app/browser_tests` and `world/browser_tests` run through a CPython runner in place of `run-browser-worker.sh`.
- **Live on the new fleet:**
  - Sign up on `app.next`, then log in and revoke a session.
  - Chat, world and notes WebSockets across 2 nodes.
  - Push a repo from a desktop pointed at `app.next`, then clone it via `n1.next`.
  - Queue an agent job: the always-on runner runs it and the branch shows up. Queue 3 at once: a burst runner starts, runs, and is destroyed.
  - Kill the leader mid-write.
  - Stop a core node: DNS drops it and service continues.
  - k6 against the performance targets.
- **Production isolation check:** `app.forkmesh.com` traffic, D1 row counts, the mirror fleet and the desktop `hosts/list` are unchanged throughout.

## Later (not in this plan)
- Moving production users onto this stack, with a D1 import and a hostname flip. That gets its own plan once Next has run cleanly.
- Moving DNS off Cloudflare (deSEC or Vultr DNS).
