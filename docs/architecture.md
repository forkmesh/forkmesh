# Architecture overview

ForkMesh keeps Git data on hosts people run themselves and uses a hosted relay
only for routing, accounts and collaboration metadata. This page names the
parts, says how they talk, and links each part's home page. The index of all
developer docs is [`docs/README.md`](README.md).

```text
                 browsers                       phones
                    │                      (native iOS/Android)
                    ▼                                │
  forkmesh.com ─► www Worker ──┐                     │
  world.forkmesh.com ─► world Worker ─┐              │
                               ▼      ▼              ▼
                        app Worker (app.forkmesh.com): API, Git HTTP,
                        accounts, D1, Durable Objects, cron
                          ▲            ▲                 ▲
          signed HTTP +   │            │ node events     │ signed HTTP,
          WebSockets      │            │ (WebSocket)     │ WebSockets
                          │            │                 │
                  desktop node     mirror daemon      (other relays)
                  (Qt, desktop/)   (Go, server/)
                          │  SSH install/update  │
                          └──────────────────────┘
                     both hold bare Git repositories and
                     serve clones over HTTPS (and SSH)
```

## The parts

| Part | Code | Runs on | Home page |
| --- | --- | --- | --- |
| Desktop node | `desktop/` (Qt 6, C++) | Linux, Windows, macOS | [`docs/desktop/README.md`](desktop/README.md) |
| Mirror daemon | `server/` (Go), with `tools/mirror_gateway.py` | Headless mirror hosts | [`docs/server/README.md`](server/README.md) |
| App Worker (relay) | `app/` (Python on Cloudflare Workers), `world/src/` | `app.forkmesh.com` | [`docs/app/README.md`](app/README.md) |
| Website | `www/` (JavaScript Worker + static assets) | `forkmesh.com` | [`docs/www/README.md`](www/README.md) |
| World | `world/` (three.js client; backend staged into the App) | `world.forkmesh.com` | [`docs/world/README.md`](world/README.md) |
| Native apps | `native/ios/` (Swift), `native/android/` (Kotlin) | iOS, Android | [`docs/native/README.md`](native/README.md) |
| Marketplace packages | `marketplace/` | Inside the desktop, App, World and mobile hosts | [`docs/marketplace/README.md`](marketplace/README.md) |
| Agent runs | desktop, `server/agents.go`, App job queue | Desktop, mirrors, phone | [`docs/agents/README.md`](agents/README.md) |
| Releases | `tools/forkmesh-release-*`, `tools/package/`, `.forkmesh/releases/` | Actions sandbox, release host, Codemagic | [`docs/release/README.md`](release/README.md) |
| Tools and contracts | `tools/`, `conformance/` | Developer machines, CI | [`docs/tools/README.md`](tools/README.md), [`docs/conformance/README.md`](conformance/README.md) |

## How they talk

- **Identity.** Every node (desktop or mirror) has an Ed25519 key
  (`desktop/src/ForkMeshIdentity.cpp`, `server/identity.go`). Nodes sign
  their requests to the relay; the desktop sends no `Origin` header, so
  browser-session routes are not available to it. Users also have accounts
  with website sessions ([`docs/app/auth-sessions.md`](app/auth-sessions.md)).
- **Desktop ↔ relay.** HTTP API plus an account event WebSocket that pushes
  topics instead of polling (client polling was removed on 2026-09-19). Chat
  rides relay WebSockets into `ForkMeshRoom` Durable Objects
  ([`docs/desktop/networking.md`](desktop/networking.md)).
- **Mirror ↔ relay.** The daemon publishes a catalog record and an endpoint
  lease per repository, keeps a signed node event WebSocket to
  `/api/nodes/events`, leases web-submitted issues and pulls from the relay
  inbox and commits them into Git, and runs queued merges, agent jobs and
  Actions ([`docs/server/README.md`](server/README.md)).
- **Desktop ↔ mirror.** The desktop installs and updates mirror hosts over SSH
  (Hosts → Update also refreshes the mirror's agent CLIs) and pushes to them
  with Git ([`docs/server/fleet-operations.md`](server/fleet-operations.md)).
- **Clones.** `git clone https://forkmesh.com/<owner>/<repo>` reaches the
  App, which routes the clone to a mirror whose signed health is fresh and
  whose refs match an accepted revision. The relay never stores packfiles.
- **Collaboration records.** Issues, pulls, discussions, reviews and releases
  are signed files under `.forkmesh/` in the repository itself, so they travel
  with every clone.
- **Phones.** The mobile apps are access clients over the same HTTP API and
  WebSockets, receive APNs pushes on iOS, and can control a desktop on the
  local network ([`docs/desktop/networking.md`](desktop/networking.md)). They
  do not mirror Git.
- **Front doors.** `www` and `world` are thin Workers that serve static
  assets and proxy dynamic requests to the App through service bindings.
- **Packages.** Hosts expose generic sockets; packages contribute pages,
  Worker APIs and World modules without core code naming them
  ([`docs/marketplace/README.md`](marketplace/README.md)).

## Where data lives

- On a desktop: the projects layout, `~/projects/forkmesh/{data,config,cache,state,logs,ssh}`
  with the checkout in `src/` ([`docs/tools/projects-layout.md`](tools/projects-layout.md)).
- On a mirror: the daemon's config, identity and bare repositories
  ([`docs/server/fleet-operations.md`](server/fleet-operations.md)); release
  blobs under each repository's `forkmesh-releases/`.
- In the relay: D1 (encrypted payloads under `DATA_KEY`, HMAC blind indexes),
  KV and Durable Object storage ([`docs/app/d1.md`](app/d1.md)).

Trust boundaries and key locations: [`docs/security/README.md`](security/README.md).
The normative protocol for independent implementations:
[`www/docs/protocol/README.md`](../www/docs/protocol/README.md).

## ForkMesh Next

ForkMesh Next was built as an independent, Cloudflare-free stack seeded from
this repository on 2026-09-23. Since the 2026-09-24 cutover it serves
production, and its code and history are merged into this repository. Its old
folder is `~/projects/forkmesh-backup` (backup only); its operator files live
in `~/projects/forkmesh/{config,ssh,data,logs}/fleet` (docs/NEXT_ADOPTION.md).
