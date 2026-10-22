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
  03524dc4dd77.
- **Database.** rqlite. `d1_rqlite.py` gives it the D1 shape; plain reads are
  in-process read-only `sqlite3` on the local file, reads after a write use
  `weak`, primary-first reads use `linearizable`. `tools/rqlite_migrate.py`
  applies migrations.
- **Durable Objects.** `durable.py` emulates them on the cores: rendezvous
  hash of `ns:name` over live members, storage in rqlite, alarms in
  `do_alarms` delivered exactly once, HMAC-authenticated forwarding over
  WireGuard. A node without quorum reports not-ready.
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
