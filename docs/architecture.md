# Architecture overview

ForkMesh keeps Git data on hosts people run themselves and uses a relay only
for routing, accounts and collaboration metadata. The index of all developer
docs is [README.md](README.md).

```text
           browsers                 phones (iOS, Android)
              │                            │
 forkmesh.com ─► www Worker ─┐             │
                             ▼             ▼
        relay app on the core nodes (app.forkmesh.com, world.forkmesh.com):
        API, Git over HTTPS, accounts, rqlite, Durable Objects, cron
              ▲               ▲                    ▲
  signed HTTP │               │ node events        │ signed HTTP
  WebSockets  │               │ (WebSocket)        │
        desktop node     mirror daemon        runners (agent jobs)
        (Qt, desktop/)   (Go, server/)
              └── both hold bare Git repositories and serve clones ──┘
```

## Parts

| Part | Code | Runs on |
| --- | --- | --- |
| Desktop node | `desktop/` (Qt 6, C++) | Linux, Windows, macOS |
| Mirror daemon and gateway | `server/` (Go), `tools/mirror_gateway.py` | headless mirrors, core nodes, runners |
| Relay app | `app/` (Python, run on CPython by `app/host/`) | the core nodes ([PLAN.md](PLAN.md)) |
| Website | `www/` (Worker plus static assets) | forkmesh.com |
| World | `world/` (three.js client; backend in the app) | world.forkmesh.com |
| Native apps | `native/ios/`, `native/android/` | phones |
| Marketplace packages | `marketplace/` | inside the desktop, app, World and mobile hosts |
| Fleet tooling | `deploy/`, `tools/next_*.py` | operator box ([deploy/README.md](deploy/README.md)) |
| Releases | `tools/package/`, release tooling | build hosts ([release/README.md](release/README.md)) |
| Contracts | `conformance/` | CI and independent implementations ([conformance/README.md](conformance/README.md)) |

## How they talk

- **Identity.** Every node has an Ed25519 key and signs its requests; users
  have accounts with web sessions.
- **Desktop ↔ relay.** HTTP API plus an account event WebSocket that pushes
  topics instead of polling; chat rides WebSockets into room Durable Objects.
- **Mirror ↔ relay.** The daemon publishes a catalog record and endpoint lease
  per repository, holds a signed node-event WebSocket, materialises
  web-submitted issues and pulls into Git, and runs merges, agent jobs and
  Actions.
- **Desktop ↔ mirror.** The desktop installs and updates mirror hosts over SSH
  and pushes to them with Git.
- **Clones.** `git clone https://app.forkmesh.com/<owner>/<repo>` reaches the
  relay, which redirects or proxies to a mirror whose signed health is fresh
  and whose refs match an accepted revision. The relay stores no packfiles
  except for repositories hosted on the cores ([HOSTED_REPOS.md](HOSTED_REPOS.md)).
- **Collaboration records.** Issues, pulls, discussions, reviews and releases
  are signed files under `.forkmesh/` in the repository, so they travel with
  every clone.
- **Phones** are access clients over the same API and sockets; they do not
  mirror Git.
- **Agents.** Agent jobs run on desktops, mirrors or the untrusted runner tier,
  which holds only per-job grants ([RUNNERS.md](RUNNERS.md)).
- **Packages.** Hosts expose generic sockets; packages contribute pages, API
  handlers and World modules without core code naming them.

## Where data lives

- Desktop: `~/projects/forkmesh/{data,config,cache,state,logs,ssh}` with the
  checkout in `src/`.
- Mirror: the daemon's config, identity key and bare repositories.
- Relay: rqlite on the cores (payloads encrypted under `DATA_KEY`, HMAC blind
  indexes), node-local caches, and Durable Object storage in rqlite. Secrets
  are sealed in the vault ([VAULT.md](VAULT.md)).

Trust boundaries: [security/README.md](security/README.md). The normative
protocol for independent implementations is published under `www/docs/protocol/`.
