# Censorship resilience

ForkMesh exists so that the source code and releases of a project stay
reachable, and provably authentic, when a hosting provider, a domain, an
account or a country's network turns against it. This page says what happens
under each kind of block, what still works, and what is not solved.

Part of the core documentation: [architecture](architecture.md) ·
[encryption](encryption.md) · [mirroring and distribution](mirroring.md) ·
[self-hosting](self-hosting.md) · [censorship resilience](censorship-resilience.md)
· [threat model](protocol/threat-model.md).

## Who it is for

Maintainers of tools people rely on under censorship or surveillance:
circumvention, privacy and security projects whose repositories and downloads
are blocked or taken down, and whose users need to know the copy they get is
the real one. The goal is continuity of access and integrity of the code, not
another general-purpose forge.

## What happens when something is blocked

| What is blocked or lost | What still works | How |
| --- | --- | --- |
| The original host (GitHub account suspended, repository removed) | Clone, issues, pull requests, releases | Import or sync the project first ([repository imports](repository-imports.md), [GitHub sync](github-sync.md)); the signed history and every mirror carry on without the original host. For a project at risk now: [crisis preservation](crisis-preservation.md). |
| The `forkmesh.com` website | The app, API and Git | Use `app.forkmesh.com`, a core's own name (`nN.next.forkmesh.com`) or a mirror's address. |
| The ForkMesh service entirely (all cores unreachable) | Clone and fetch of public repositories with a live mirror; releases from any copy | Point Git at a mirror's own HTTPS or SSH address ([mirroring and distribution](mirroring.md#ways-to-reach-a-copy)). Desktop nodes keep serving what they hold. |
| One or two cores | Everything with one down; reads, clone and downloads with two down | DNS round robin and read-only mode ([architecture](architecture.md#no-single-point-of-failure)). |
| The IP addresses of the cores | Mirrors on other networks | Any operator can run a mirror on a different provider and address ([self-hosting](self-hosting.md#mirror)). |
| Direct internet access to every known address | Clone and fetch through an anonymity or overlay network | Device transport profiles for Tor, I2P, WireGuard-style overlays and LAN, with leak tracing and no silent fallback ([transports](transports.md)). |
| The network itself (shutdown, metered or broken link) | Moving a repository by hand | Signed, resumable bundles on USB sticks or slow links ([offline sync](offline-sync.md)). |
| A user's account | Their identity and signed work | Keys can be exported and recovered; records are signed by keys, not by a server account ([portable identities](portable-identities.md)). |
| A tampering mirror or network | Detection, not service | Clones go only to copies whose refs match the signed pin; releases are checked against a pinned key ([encryption](encryption.md)). |

## Distributing tools, not only code

Many circumvention tools reach their users as downloads from a repository's
releases. ForkMesh releases are content-addressed and signed: any mirror can
serve the bytes, and the installer or user checks them against the signed
manifest, so a blocked origin does not stop distribution and an untrusted copy
cannot alter it ([software distribution](mirroring.md#software-distribution)).

## What is not solved

We avoid claiming more than the design delivers:

- **Discovery.** Finding a working mirror normally goes through the catalog on
  the cores. If those are blocked, a user needs a mirror address from somewhere
  else; out-of-band discovery is open work.
- **Visible traffic.** Connections to ForkMesh names and mirrors are ordinary
  HTTPS; the server name is visible to the network and can be filtered. There is
  no built-in traffic obfuscation; users in heavily filtered networks route
  through Tor or another tool they trust ([transports](transports.md)).
- **Metadata.** The service learns public repository names, sizes, node
  versions, presence and timing (TM-016 in the
  [threat model](protocol/threat-model.md)). Private repositories use opaque
  handles and ciphertext ([private repository confidentiality](security/private-repository-confidentiality.md)).
- **Operator concentration.** The hosted cores currently run with one provider
  in one region, and forkmesh.com uses one DNS provider. Real resilience needs
  mirrors run by different people on different networks and in different
  countries ([architecture](architecture.md#what-is-still-a-dependency)).
- **Withheld events.** A relay or mirror that silently drops collaboration
  events is not yet detected (TM-008).

## How we test it

What is tested today:

- The [product promise](product-promise.md) drill: collaborate, add a mirror,
  take the source offline, clone again.
- [Availability proofs](availability-proof.md) and
  [availability contracts](availability-contract.md) that measure whether a
  repository survives losing its source.
- Read-only survival of the cores with two of three stopped
  (`app/host/tests_rqlite/test_read_only_survivor.py`).
- Transport profiles refuse forbidden routes and trace leaks
  (`conformance/transports/v1`).
- [Disaster recovery](disaster-recovery/README.md) drills for losing a domain,
  region, key or backup.

Not yet done: testing inside real national filtering systems (DNS tampering, IP
and SNI blocking, throttling, shutdowns) with developers in the regions where
they happen. The results, negative ones included, will be published here.

Related: [mirroring and distribution](mirroring.md),
[self-hosting](self-hosting.md), public index [README.md](README.md).
