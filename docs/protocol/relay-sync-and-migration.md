# Relay, synchronization, and migration

Suite version: `1.0.0-rc.1` · Vectors: `fold.json`, `identifiers.json` ·
Part of the [ForkMesh protocol suite](README.md)

## Roles

- A **source of truth** is a node holding the owner's working repository. It
  signs catalog records and `forkmesh-repostate-v1` attestations.
- A **mirror** is an independently operated HTTPS origin serving a copy. Its
  own attestations prove what it serves, not what the owner published.
- The **relay** (mainnode) routes reads to mirrors, verifies signed
  submissions, and indexes metadata. It never stores repository bytes and is
  never authoritative for collaboration meaning.

## Clone and fetch routing

- **FM-REL-001** MUST — A relay serves a public clone or fetch only from a
  mirror that passes three checks. The mirror has a fresh signed health
  result. Its live advertised refs hash (FM-ID-005) to a state that a source
  of truth in the repository's logical group attested with
  `forkmesh-repostate-v1`. Mirrors are grouped by root commit, falling back to
  name. A source is checked against its current pin plus bounded recent
  history. A mirror's self-attestation never activates a repository. When the
  group has no source-signed pin, routing fails closed.
- **FM-REL-002** MUST — The two requests of a smart-HTTP clone (`info/refs`,
  then `git-upload-pack`) reach the same accepted origin. A relay may retry
  another integrity-matching origin only before any response byte is sent. It
  never splices responses from different origins and never rewrites pack
  data.
- **FM-REL-003** MUST — Receive-pack is refused before any request body is
  read unless FM-AUTH-002 authorizes the request. Both upload-pack and
  receive-pack hide the `refs/forkmesh/` namespace, which holds merge
  anchors and staging refs, and reject wants for object ids that no
  advertised ref reaches.

Protocol negotiation inside Git (protocol v0 or v2) is left to stock Git
clients and servers. The refs digest is always computed from a protocol-v0
advertisement generated for attestation.

## Collaboration log exchange

- **FM-SYNC-001** MUST — A collaboration log is exchanged as one canonical
  envelope per line, separated by LF. A reader splits only on LF and retains
  a stray CR in the record, where it fails as `noncanonical_encoding` rather
  than being normalized into a valid envelope. An importer verifies every
  record and recomputes state; it never trusts an exported snapshot. Export
  sorts unique raw records by SHA-256 of their bytes.
- **FM-SYNC-002** MUST — Folding is a pure function of the policy and the set
  of unique raw records. Permutation, duplication, and delay never change the
  result. The state digest is `sha256:` + hex SHA-256 of the canonical JSON
  state document (`forkmesh-collaboration-state-v1`). Duplicate deliveries are
  counted, not re-applied.
- **FM-SYNC-003** MUST — Beyond FM-SIG-004, a fold enforces six rules:
  - An event whose parents are not all present stays **pending**, with its
    raw bytes retained.
  - A descendant of a rejected event is rejected.
  - The Lamport `clock` is 0 without parents and otherwise one more than the
    greatest parent clock.
  - Two different events signed by one key in the same `sequence` slot are
    **equivocation**; both are excluded and counted.
  - Role authorization follows the pinned policy. Unauthorized events are
    audited but never folded.
  - Concurrent differing scalar writes produce an explicit conflict rather
    than a winner chosen by time, author, or arrival order.

  The complete family rules are normative in
  [Conflict-safe collaboration](../design/conflict-safe-collaboration.md).

A fold over an incomplete set cannot detect omission. A relay that withholds
events is an open threat (TM-008), not a property this suite guarantees.

## Migration and rollback

- **FM-MIG-001** MUST — A repository migration state receipt (FM-ID-006)
  excludes run timestamps, attempt counters, and credentials. Re-running an
  import over unchanged provider data yields the same receipt, and source
  deletions are pruned, not retained as stale objects.
- **FM-MIG-002** MUST — Each legacy collaboration record migrates through an
  importer-signed `create` or `decide` event. That event binds the exact
  legacy record digest and records whether the legacy signature is `valid`,
  `invalid`, or `unavailable`. It never presents the importer's signature as
  the legacy author's. The original bytes are kept alongside the new log.
- **FM-MIG-003** MUST — Rollback proceeds in a fixed order: stop writers
  emitting the newer version first, then roll back readers. Readers keep
  accepting and preserving newer raw envelopes they already hold. No
  implementation down-converts or deletes signed events, because that loses
  causality. Recovery restores policy and NDJSON, re-verifies, and compares
  state digests; corrupt lines stay quarantined while valid objects keep
  folding.
