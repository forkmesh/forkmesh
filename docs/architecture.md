# Architecture and availability

ForkMesh is built so that no single machine has to stay up for code to stay
reachable. Git data lives on hosts that developers and operators run; the
service at app.forkmesh.com runs on several equal core nodes that DNS hands out
in turn; independent mirrors serve clones on their own.

Part of the core documentation: [architecture](architecture.md) ·
[encryption](encryption.md) · [mirroring and distribution](mirroring.md) ·
[self-hosting](self-hosting.md) · [censorship resilience](censorship-resilience.md)
· [threat model](protocol/threat-model.md).

```text
        browsers · desktop nodes · phones · mirrors · git
                           │
     DNS round robin: app.forkmesh.com, world.forkmesh.com
        (one A/AAAA record per healthy core, TTL 60 s)
           ┌───────────────┼───────────────┐
        core n3         core n4         core n5      ◄─ WireGuard mesh
  Caddy · app · rqlite · Git gateway (each core is a full copy)
           ▲  outbound only: claim job, clone, run, push
     runner tier (untrusted, holds no core secrets)

  independent mirrors (anyone's server) ─► serve clones of the repositories they carry
  desktop nodes ─► hold and serve their own repositories
```

## The parts

| Part | What it does | Code |
| --- | --- | --- |
| Desktop node | Holds bare repositories, serves clones, signs every record with its Ed25519 key, runs coding agents in isolated worktrees | `desktop/` |
| Mirror | A headless server that carries copies of repositories and serves `git clone` and `git fetch` | `server/` ([mirror node](operations/mirror-node.md)) |
| Core node | Accounts, the web app and API, Git over HTTPS, chat, the World, the routing catalog of mirrors | `app/`, `app/host/`, `server/` |
| Runner | Runs agent jobs and Actions in a per-job sandbox with a short-lived, single-branch push grant | `server/` |
| Phone apps | Mobile nodes with their own key and Git engine: they clone, commit and publish, and drive desktop agents; they never host or serve repositories to others ([phone apps](mobile/README.md)) | `native/` |

Collaboration records (issues, pull requests, reviews, discussions, releases)
are signed files under `.forkmesh/` in the repository, so every clone carries
them ([protocol](protocol/README.md)).

## No single point of failure

```flow
title: How a request reaches a core
Browser, desktop or git -> DNS round robin -> Any healthy core -> Caddy -> Python app -> rqlite member
Health loop, /readyz every 30 s -> DNS round robin
rqlite member -> Raft majority across the cores
```

- **Equal cores, no leader in the request path.** Each core runs Caddy, the
  Python app, an rqlite (Raft) database member and the Go Git gateway. Any core
  answers any request. Membership, scheduled jobs and certificate renewal are
  leaderless: no core is special.
- **Direct DNS round robin.** `app.forkmesh.com` and `world.forkmesh.com` have
  one unproxied record per core with a 60-second TTL; clients connect straight
  to a core, with no proxy or CDN in between. A health loop probes every core's
  `/readyz` every 30 seconds and removes a core after three failures, so a dead
  core drops out of DNS within about two minutes. It never empties the set.
- **One core down:** the other two serve everything, reads and writes.
- **Two cores down:** the survivor has no database majority, so it switches to
  read-only mode on its own. Browsing, signed-in reads, `git clone`,
  `git fetch` and release downloads keep working from its full local copy;
  sign-in, pushes, edits and chat sends answer `503` with
  "ForkMesh is read-only while servers recover". It returns to normal when a
  second core comes back.
- **Per-core Git endpoints.** Every core also serves Git at its own name
  (`nN.next.forkmesh.com`) without touching the database, so a clone works even
  while the app is degraded.
- **Mirrors outside the fleet.** A public repository with a live mirror stays
  clonable when every ForkMesh server is unreachable: point Git at the mirror
  ([mirroring and distribution](mirroring.md)).
- **Untrusted work is separate.** Agent runs execute arbitrary code, so they run
  only on the runner tier, never on a core holding data or keys.

## What is still a dependency

We state these so nobody mistakes the design for more than it is:

- All three cores currently run with one hosting provider (Vultr) in one
  region. Losing that provider or region takes down the hosted service; mirrors
  and desktop nodes elsewhere keep serving their copies.
- DNS for forkmesh.com is one provider (Cloudflare DNS, unproxied for the app
  and World names). The website at `forkmesh.com` itself is still served from
  Cloudflare, and it forwards app paths such as `forkmesh.com/<owner>/<repo>`
  to the cores. `app.forkmesh.com` and the per-core names reach the cores
  directly.
- The DNS health loop runs on the operator's machine. If it stops, DNS keeps
  listing every core and clients still spread across them, but a failed core is
  not removed until it returns.
- Discovery of mirrors goes through the catalog on the cores; a mirror's own
  address works without it, but someone has to know that address.

## Where to read more

- Operators: [docs/operations/served-stack.md](operations/served-stack.md) (the served stack and read-only
  mode in detail), [docs/operations/README.md](operations/README.md) (runbook),
  [docs/codebase.md](codebase.md) (how the code fits together).
- Recovery from losing a domain, region, key or backup:
  [disaster recovery](disaster-recovery/README.md).
- What outside services see: [external dependency map](external-dependency-map.md).
- Developer index: [docs/README.md](README.md); public index:
  [README.md](README.md).
