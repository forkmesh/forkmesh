# The served stack

forkmesh.com, app.forkmesh.com and world.forkmesh.com run on a small
self-hosted fleet: three core nodes that hold data and secrets, and a runner
tier that runs untrusted work. `deploy/fleet.toml` describes the fleet;
[OPERATIONS.md](OPERATIONS.md) is the runbook, [CUTOVER.md](CUTOVER.md)
records the move from the old Cloudflare Worker stack, and
[NEXT_ADOPTION.md](NEXT_ADOPTION.md) covers operator files.

## Who serves what

| Host / page family | Served by |
|---|---|
| `forkmesh.com`, `www.forkmesh.com` (homepage, blog, docs, static pages) | the `forkmesh-www` Cloudflare Worker (`www/worker.js`, `www/wrangler.production.toml`); it proxies app paths to `app.forkmesh.com` with a signed edge header. `www/public/_redirects` is read only by that Worker. |
| `app.forkmesh.com` (dashboard, API, WebSockets, Git over HTTPS) | Caddy on each core: precompressed static assets from the release's `app/dist`, dynamic routes to the Python app on `127.0.0.1:8080` |
| `world.forkmesh.com` | Caddy on each core: `world/public` plus the app for the API and sockets |
| `nN.next.forkmesh.com` | one core's Git gateway (the Go daemon) and health |
| DNS | Cloudflare DNS only; unproxied round-robin A records (TTL 60) for the app and world names |

## Architecture

```
          browsers, desktops, phones, mirrors
                       │
       forkmesh.com ── www Worker ──┐
                                    ▼
  ┌──────── core cell: 3 small VMs (trusted: data + secrets) ────────┐
  │ Caddy · Python app on CPython · rqlite (Raft) · Go daemon/gateway │
  │ WireGuard mesh between cores. No agents, no untrusted code.       │
  └──────────────▲────────────────────────────────────────────────────┘
                 │ outbound only: claim lease → clone → run → push → report
  ┌──────── runner tier (untrusted, stateless, no core secrets) ──────┐
  │ an always-on runner, burst runners from a snapshot, user runners  │
  └───────────────────────────────────────────────────────────────────┘
```

- **Trust split.** Agent runs execute arbitrary code, so they never run on a
  node holding `DATA_KEY`, password hashes or the database. A runner gets a
  short-lived, router-signed push capability for exactly
  `refs/heads/agent/<session>` of one repository ([RUNNERS.md](RUNNERS.md)).
- **Host shim.** The app is the same Python code that ran as a Worker, on
  CPython behind `app/host/forkmesh_host/`: `js.py`, `ffi.py`, `workers.py`,
  `env.py`, `asgi.py` (uvicorn). `tools/js_surface_inventory.py` fails when
  the app uses an attribute the shim does not implement. The test that uses
  it requires a binding only for `DB`, `ASSETS`, `REPOSITORY_METADATA` and
  names the app calls through (`env.X.idFromName()`). An `env.FORKMESH_*`
  name read as a plain value, such as `FORKMESH_CALLS_NODES` or
  `FORKMESH_CALLS_SECRET`, is an optional var. Before 2026-10-22 every
  `FORKMESH_*` name counted as a binding, and that failed the node deploy of
  03524dc4dd77. Any name `ctx` in app, World or marketplace worker code is
  read as the Workers execution context, so a local of that name fails the
  test: the Poker package's per-round `ctx` (`ctx.big`, `ctx.credits`)
  blocked every node deploy from ab5642740e on 2026-10-28 until it was
  renamed `context`.
- **Database.** rqlite. `d1_rqlite.py` gives it the D1 shape; plain reads are
  served by the node's own copy (`level=none` over HTTP, the fleet's
  `read_mode = "http"`, or read-only `sqlite3` on the local file with
  `read_mode = "local"`), reads after a write use `weak`, primary-first reads
  use `linearizable`. `tools/rqlite_migrate.py` applies migrations.
- **Durable Objects.** `durable.py` emulates them on the cores: rendezvous
  hash of `ns:name` over live members, storage in rqlite, alarms in
  `do_alarms` delivered exactly once, HMAC-authenticated forwarding over
  WireGuard. A node without quorum reports not-ready and serves read-only
  (below).
- **No leader.** Membership, cron, alarms, DNS health votes and certificates
  are leaderless: each node writes only its own membership row, the cron
  coordinator rotates across live nodes every minute, and
  `deploy/core/certsync.py` runs the same on every core.
- **Secrets.** Nodes start sealed and hold `DATA_KEY` only in memory; the
  operator unseals them ([VAULT.md](VAULT.md)). Other secrets come from
  systemd credentials, never from rqlite or backups.
- **Integrations start off.** A service credential is added to
  `optional_secrets` only on purpose. ForkMesh Next starts with Solana rewards
  off mainnet; production enables them only through `[production.vars]`.
- **Backups.** The operator box pulls an rqlite backup nightly over SSH,
  verifies it and keeps 7 daily and 4 weekly copies (`tools/next_backup.py`).

## Read-only mode

Three cores are three rqlite voters, and we chose not to add servers
(2026-10-28). With two cores down the survivor has no rqlite leader, so
nothing can be written. It keeps serving everything that only reads, from
its own full copy of the database, and refuses writes with one clear answer.
When a leader returns it goes back to normal by itself.

**When.** `cluster.Membership.read_only`: the node has gone 10 s
(`QUORUM_LOSS_SECONDS`) without seeing an rqlite leader, or 15 s without a
successful heartbeat write, and is not sealed, quarantined or leaving. The
first 10 s after the process starts never count, so a restart during a
normal deploy does not refuse writes while the first heartbeat is in
flight. `HostRuntime.read_only` exposes it; the same 1 s loop that checks it
logs `read-only mode ON` / `read-only mode OFF`.

**Database (`d1_rqlite.py`, `read_only.py`).** While read-only:

- Reads stay local. Every read uses `level=none` (or the local `sqlite3`
  file) without the `freshness` bound, including primary-first sessions and
  reads after a write. An empty result is not re-asked at `weak`, because
  that leader read would fail the whole request.
- A write is never sent to rqlite, so it can neither hang on a missing leader
  nor wait out the 15 s timeout. What happens depends on the request:
  - `GET`, `HEAD`, `OPTIONS` and `POST .../git-upload-pack` only read, so
    their side writes (session stamps, counters, `clone_sticky`, download
    counts, rate-limit rows) are skipped: the statement returns no rows and
    `changes = 0`, and the page renders.
  - Every other request, plus the GET links whose point is a write
    (`WRITING_GET_PATHS`: e-mail verification, marketing unsubscribe, the
    Discord and Patreon OAuth callbacks), gets `ReadOnlyError`. Its text is a
    D1 overload error, so the app's existing `database_unavailable` path
    handles it with no app change and no Sentry event.
  - Writes outside a request (cron, queue consumers, background loops) also
    raise.
- A statement that starts like a read but contains a write keyword (a
  `SELECT` matching `'delete'`, a `WITH ... INSERT`) is tried as a read: the
  local `sqlite3` connection is `query_only`, and over HTTP rqlite answers a
  real write with "leader not found". Either way a write is refused and a read
  is served.
- `d1_sqlite.py` follows the same rules (it switches `PRAGMA query_only` on),
  which is how the host tests drive the real app in read-only mode.

**Request layer (`asgi.py`).** While read-only, a response is replaced with
the read-only answer when:

- it is a 5xx; or
- a write was refused; or
- the request is a write request and the app answered 4xx (the signup form,
  for example, answers 403 `signup_page_required` because its GET could not
  store the signup intent).

The answer is HTTP 503 with `Retry-After: 30`, `x-forkmesh-read-only: 1` and
`x-forkmesh-expected-degraded: 1`. The body is
`{"error": MESSAGE, "message": MESSAGE, "code": "read_only", "readOnly": true}`,
or a short HTML page when the client accepts `text/html`. The desktop, the
phones and the dashboard all show `error` or `message`, so they show "ForkMesh
is read-only while servers recover…". Every response the app itself answers
on a read-only node carries `x-forkmesh-read-only: 1`. A WebSocket upgrade
that fails with a 5xx gets the same answer as an HTTP denial.

**Off while read-only.** Durable Objects, which need quorum for storage and
placement, are off; the fetch-only ingress objects run on every node and keep
working. Off with them: alarms, the cron loop (skipped like a sealed node),
queue consumers (paused; messages wait in memory and resume when writes come
back), DNS reconciler votes and peer unseal (VAULT.md).

**Readiness and DNS.**

- `/readyz` still answers 503 on a read-only node, so deploy gates,
  `next_fleet.roll_healthy`, the release host and add-core see it as degraded.
  Its body adds `"readOnly": true` and the reason `read_only`. A node whose
  local rqlite is unreachable, or that is sealed or quarantined, is not
  flagged: it cannot serve reads.
- `dns_rules.probe_state` reads that flag and reports `ready`, `read_only` or
  `down`. `tools/next_dns.py health-loop` counts a read-only core as healthy
  only while no core is ready (`dns_rules.serving`):
  - In a partition, the isolated read-only core leaves DNS and the two ready
    ones keep the writes.
  - With two cores down, the survivor stays in, or comes back, and the dead
    ones leave.
  - Undrain still needs a fully ready core.
- Caddy on each core proxies only to its own app, so it needs no change. The
  in-node DNS reconciler needs quorum to vote and does nothing while
  read-only.

**What works read-only.** These need no leader:

- the website, static pages and assets;
- dashboard and API reads with an existing session (the session-stamp write
  is skipped);
- public profiles, organizations, notes and chat history reads;
- repository browsing through the fast path and the app;
- release downloads;
- `git clone` and `git fetch`:
  - over `nN.<domain>`, the gateway touches no rqlite at all;
  - over the app name, the app skips its `clone_sticky` and counter writes.

**What does not.** These need a leader and answer the read-only message:

- sign-up, login and logout (a session is a row), password reset, and e-mail
  verification links;
- every create, edit and delete;
- pushes;
- chat sends;
- anything behind a Durable Object: World rooms, live sockets, the status
  WebSocket, cron jobs and alarms.

**Risks.**

- Data is as fresh as the survivor's copy when the leader was lost.
- A GET handler whose write matters later fails on that later step. The
  signup intent is one; the 4xx rule above turns its POST into the read-only
  answer.
- A new GET route that must write has to be added to `WRITING_GET_PATHS`,
  or its write is silently skipped while read-only.

**Tests.**

- `app/tests/host/test_host_read_only.py`: policy, bindings, membership
  transitions, readiness flag, DNS rules.
- `app/tests/host/test_host_read_only_e2e.py`: the real app with the
  `FORKMESH_HOST_DEBUG` toggle `/_host/read-only?on=1|0|auto`.
- `app/host/tests_rqlite/test_read_only_survivor.py`: a real 3-node rqlite
  cluster with two nodes stopped, then restarted.
- `deploy/tests/test_next_dns.py`: the health loop.

## Core node

- systemd units for rqlite, the app, Caddy and the Go daemon (role `core`),
  with memory limits; zram plus swap and `earlyoom`.
- Caddy issues certificates leaderlessly; ACME HTTP-01 challenges are
  forwarded across cores so any A record works.
- The firewall allows 22, 80, 443 and the WireGuard port.

## Runner tier

The Go daemon with role `runner` runs no gateway and keeps no repositories.
Each job runs in its own systemd scope with memory and CPU limits and a
private tmp dir, and its workdir is wiped afterwards. Burst runners
(`server/runnerpool.go`) are created from a snapshot when the queue backs up
and destroyed when idle, under a count and monthly spend cap
([fleet/AUTOSCALING.md](fleet/AUTOSCALING.md)).

## Performance rules

- Python never serves static bytes.
- CPU-heavy work (PBKDF2, large JSON, compression, image rendering, RSA) runs
  in a thread, never on the event loop.
- Warm up before `/readyz`; cache public snapshots in memory and invalidate on
  republish.
- A route that cannot meet its budget in Python moves into the Go daemon.

## Fleet tooling

`tools/next_fleet.py` (create, install, replace or add a core, runner
snapshot, status), `tools/next_deploy.py` (build, upload, migrate, rolling
restart gated on `/api/version` and `/readyz`, automatic rollback),
`tools/next_dns.py`, `tools/next_vault.py`, `tools/next_ids.py`,
`tools/next_backup.py`. Instances are labelled `fmnext-*`, tagged
`forkmesh-next`, and never added to the desktop's saved hosts.

Installer and fleet tests sandbox `HOME` and the `XDG_*` folders and drop
`FORKMESH_HOME` / `FORKMESH_PROJECTS_DIR` (AGENTS.md).
