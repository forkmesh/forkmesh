# How the codebase fits together

The developer entry page. ForkMesh keeps Git data on hosts people run
themselves; the app on the core nodes handles accounts, routing and
collaboration metadata. The public overview, including what is still a single
dependency, is [docs/architecture.md](architecture.md); the
index of developer docs is [README.md](README.md).

```text
        browsers · phones (iOS, Android) · desktop nodes · mirrors
                              │
     DNS round robin (app.forkmesh.com, world.forkmesh.com), no proxy
                              ▼
   core nodes n3 · n4 · n5 (equal; WireGuard mesh between them):
   Caddy · app (Python on CPython) · rqlite · Go daemon (Git gateway)
         ▲                 ▲                        ▲
 signed HTTP,       node events (WebSocket)    outbound job leases
 WebSockets               │                         │
   desktop node      mirror daemon             runners (agent jobs)
   (Qt, desktop/)    (Go, server/)
         └── both hold bare Git repositories and serve clones ──┘
```

## Parts

| Part | Code | Runs on |
| --- | --- | --- |
| Desktop node | `desktop/` (Qt 6, C++) | Linux, Windows, macOS |
| Mirror daemon and gateway | `server/` (Go), `tools/mirror_gateway.py` | headless mirrors, core nodes, runners |
| App | `app/` (Python, run on CPython by `app/host/`) | the core nodes ([served stack](operations/served-stack.md)) |
| Website | `www/` (static site and docs) | forkmesh.com ([www/README.md](www/README.md)) |
| World | `world/` (three.js client; backend in the app) | world.forkmesh.com |
| Native apps | `native/ios/`, `native/android/` | phones |
| Marketplace packages | `marketplace/` | inside the desktop, app, World and mobile hosts |
| Fleet tooling | `deploy/`, `tools/next_*.py` | operator box ([fleet tooling](operations/fleet-tooling.md)) |
| Releases | `tools/package/`, release tooling | build hosts ([release/README.md](release/README.md)) |
| Contracts | `conformance/` | CI and independent implementations ([conformance/README.md](conformance/README.md)) |

## How they talk

- **Identity.** Every node has an Ed25519 key and signs its requests; users
  have accounts with web sessions.
- **Desktop ↔ app.** HTTP API plus an account event WebSocket that pushes
  topics instead of polling; chat rides WebSockets into room objects, which
  `app/host/` places on one core by rendezvous hash
  ([app/cron-and-durable-objects.md](app/cron-and-durable-objects.md)).
- **Mirror ↔ app.** The daemon publishes a catalog record and endpoint lease
  per repository, holds a signed node-event WebSocket, materialises
  web-submitted issues and pulls into Git, and runs merges, agent jobs and
  Actions.
- **Desktop ↔ mirror.** The desktop installs and updates mirror hosts over SSH
  and pushes to them with Git.
- **Clones.** `git clone https://app.forkmesh.com/<owner>/<repo>` reaches a
  core, which redirects or proxies to a mirror whose signed health is fresh
  and whose refs match the signed pin
  ([docs/mirroring.md](mirroring.md)). The app stores no
  packfiles except for repositories hosted on the cores
  ([core-hosted repositories](operations/core-hosted-repositories.md)).
- **Collaboration records.** Issues, pulls, discussions, reviews and releases
  are signed files under `.forkmesh/` in the repository, so they travel with
  every clone.
- **Phones** are access clients over the same API and sockets; they do not
  mirror Git.
- **Agents.** Agent jobs run on desktops, mirrors or the untrusted runner tier,
  which holds only per-job grants ([runners](operations/runners.md)).
- **Packages.** Hosts expose generic sockets; packages contribute pages, API
  handlers and World modules without core code naming them.

## Where data lives

- Desktop: `~/projects/forkmesh/{data,config,cache,state,logs,ssh}` with the
  checkout in `src/`.
- Mirror: the daemon's config, identity key and bare repositories.
- Cores: rqlite (payloads encrypted under `DATA_KEY`, HMAC blind indexes),
  node-local caches, and room-object storage in rqlite. Secrets are sealed in
  the vault ([vault](operations/vault.md)); every layer is summarised in
  [docs/encryption.md](encryption.md).

Trust boundaries: [security/README.md](security/README.md) and the
[threat model](protocol/threat-model.md). The normative protocol
for independent implementations is the
[protocol suite](protocol/README.md). Running your own:
[docs/self-hosting.md](self-hosting.md).
