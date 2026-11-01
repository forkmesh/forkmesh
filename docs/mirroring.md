# Mirroring and software distribution

How ForkMesh keeps copies of a repository on independent hosts, how a cloner
knows a copy is authentic, and how releases reach users through those copies.

Part of the core documentation: [architecture](architecture.md) ·
[encryption](encryption.md) · [mirroring and distribution](mirroring.md) ·
[self-hosting](self-hosting.md) · [censorship resilience](censorship-resilience.md)
· [threat model](protocol/threat-model.md).

## Who holds copies

- **The source node**: the developer's desktop, or the cores for a repository
  created on the web ([hosted repositories](hosted-repositories.md)).
- **Mirrors**: headless servers running `forkmesh-mirror-node`, operated by the
  project, its friends, or anyone who chooses to carry it
  ([mirror node](operations/mirror-node.md)). The desktop can install and update
  a mirror on any Debian or Ubuntu server over SSH.
- **Every clone**: issues, pull requests, reviews and releases are signed files
  under `.forkmesh/`, so an ordinary `git clone` carries the collaboration
  history too.

A repository page shows how many copies are running and how healthy they are
([mirror goals](mirror-quota.md)).

## How a mirror stays in step

1. The source pushes, and the cores send each mirror carrying the repository a
   small event over its signed WebSocket. The event names the new signed refs
   digest.
2. The mirror fetches until its refs match that digest, retrying a stale peer
   with backoff; older generations are ignored. A safety pass reconciles every
   5 minutes in case an event was lost; a third-party upstream (GitHub, Gitea,
   Forgejo) is polled every 15 seconds by default.
3. The mirror publishes a signed catalog record for each repository and a lease
   on its HTTPS endpoint, renewed every 4 minutes. A mirror whose lease lapses
   stops receiving clones.
4. Web submissions (issues, pull requests, merges) are written into Git by the
   mirror and acknowledged with a signed `forkmesh-repostate-v1` statement of
   the resulting refs.

Code and traps: `docs/server/README.md`, `docs/server/intake-and-attestation.md`.

## How a cloner knows a copy is authentic

- **Refs pins.** For each repository the service keeps a pin set: the source's
  signed current state plus attested history. `git clone
  https://app.forkmesh.com/<owner>/<repo>` is routed only to a mirror whose live
  refs digest is in that set and whose signed health is fresh. A repository
  with no pin is not routed at all.
- **Fail closed.** A mirror whose gateway does not know the expected refs digest
  refuses to serve. When no healthy, matching copy exists, the clone fails; it
  never falls back to unverified bytes (TM-007 in the
  [threat model](protocol/threat-model.md)).
- **Verifiable without the service.** Git object IDs pin the content, and the
  signed `.forkmesh/` records can be checked with the authors' public keys
  ([signatures and authentication](protocol/signatures-and-authentication.md)).
- **Private repositories** are replicated only as ciphertext; a mirror cannot
  read them ([encryption](encryption.md#private-repositories)).

## Ways to reach a copy

| Route | Use |
| --- | --- |
| `https://app.forkmesh.com/<owner>/<repo>` | Normal clone; routed to a verified mirror or a core |
| `https://nN.next.forkmesh.com/...` | A specific core's Git gateway; works while the app is degraded |
| A mirror's own HTTPS or SSH address | Works with no ForkMesh server at all ([direct HTTPS gateway](operations/direct-https-mirror-gateway.md), [SSH gateway](operations/git-ssh-gateway.md)) |
| Signed offline bundle | USB stick or a slow, metered link, resumable ([offline sync](offline-sync.md)) |
| Tor, I2P, overlay or LAN | Device-side transport profiles ([transports](transports.md)) |

All of them are stock Git: no ForkMesh client is needed to read
([Git compatibility](git-compatibility.md),
[accountless reading](accountless-reading.md)).

## Software distribution

Releases are content-addressed: each file is stored and fetched by its SHA-256
(`/api/repo/<owner>/<repo>/releases/blob/sha256/<hash>`), and the release host
seeds every file into mirror release stores. A signed `release.json` lists every
file's hash; `install.sh` and the desktop updater verify the manifest against a
pinned Ed25519 key, then verify each file against its hash. Any copy can serve
the bytes, because no copy is trusted for them. Release downloads keep working
when the service is read-only. Linux packages are also published as a signed
apt repository. Operator detail: `docs/release/README.md`, `docs/release/apt.md`.

## When things go wrong

- A project that could disappear soon follows the
  [crisis preservation](crisis-preservation.md) plan, which orders the work
  (Git refs first, then identity proofs and trusted mirrors in an urgent
  takedown) to fit the time and access left.
- [Availability proof](availability-proof.md) and
  [availability contracts](availability-contract.md) measure whether a
  repository really survives losing its source.
- The [product promise](product-promise.md) walks through it end to end:
  collaborate, add a mirror, take the source offline, clone again.
- Withholding of collaboration events by a relay or mirror is not yet
  detectable (TM-008). A mirror can serve stale or wrong bytes, but the refs
  pin keeps clones away from it.

Related: [architecture](architecture.md), [self-hosting](self-hosting.md),
[censorship resilience](censorship-resilience.md), public index
[README.md](README.md).
