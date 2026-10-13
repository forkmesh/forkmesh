# Conflict-safe signed collaboration objects

Status: normative protocol version 1, part of the
[ForkMesh protocol suite](../protocol/README.md). Suite version: `1.0.0-rc.1`. The machine-readable envelope schema is
[`collaboration-event-v1.schema.json`](../../public/docs/protocol/collaboration-event-v1.schema.json).
The dependency-free reference fold is `app/src/collaboration.py`; the independent
WebCrypto fold is `app/public/collaboration.js`.

## Invariants

ForkMesh collaboration state is a deterministic projection of an immutable set
of signed events. A relay, mirror, or database may transport and index those
events, but is never authoritative for their meaning. Given the same policy and
event set, implementations MUST produce the same state when events are
permuted, duplicated, delayed, or replayed.

Implementations MUST retain each canonical raw envelope. Materialized fields,
search indexes, counts, and notification views are disposable caches. Invalid
input is retained only in a bounded quarantine/audit channel and MUST NOT enter
the state fold.

## Repository, object, and event identity

A repository identity is:

```text
urn:forkmesh:repository:sha256:<sha256(
  "forkmesh-repository-identity-v1\n" || verified repository root
)>
```

The verified root is deployment-specific (for example a signed repository
manifest), but aliases and mutable URLs are not identities.

Every object's first event carries a random 16-byte `objectKey`, base64url
without padding. The object ID is:

```text
urn:forkmesh:collaboration:<family>:sha256:<sha256(
  "forkmesh-collaboration-object-v1\n" || canonical-json({
    repository, family, creator, objectKey, references
  })
)>
```

This binds the repository, family, creator, and immutable references before an
event is signed. For reactions, clients SHOULD derive `objectKey` from subject,
author, and normalized emoji to make one logical reaction idempotent. For
notifications, clients SHOULD derive it from recipient and causing event.

The event ID binds both the signed meaning and its deterministic Ed25519
signature:

```text
signingBytes = utf8("forkmesh-collaboration-event-v1\n") ||
               canonical-json(unsigned envelope)
eventId = "sha256:" || hex(sha256(signingBytes || 0x00 || rawSignature))
```

Ed25519 public keys are raw 32-byte keys and signatures are raw 64-byte values,
both base64url without padding. Schema/domain prefixes are frozen; changed
meaning requires a new version.

## Canonical encoding

The wire envelope is canonical JSON encoded as UTF-8 with no BOM or trailing
newline. Object keys are ASCII, sorted lexicographically; there is no
insignificant whitespace. Strings are NFC-normalized and lone surrogates are
forbidden. Values are objects, arrays, strings, booleans, null, or integers whose
magnitude is at most 2^53-1 (envelope counters are additionally
non-negative); floating point and duplicate object keys are forbidden. Arrays
whose semantics are sets (`parents`, `references`, observed IDs, and set
values) are sorted and duplicate-free. An envelope is at most 1 MiB and JSON
nesting is at most 32 levels, so malformed input cannot force unbounded parser
or canonicalizer work.

A receiver MUST parse with duplicate-key detection, reproduce the exact
canonical bytes, recompute `eventId`, and verify Ed25519 before considering
causality or authorization. Noncanonical encodings fail closed even if they
would parse to the same JSON value.

## Causality and replay defense

`parents` names the events observed when an author made an event. The Lamport
`clock` is zero without parents and otherwise exactly one plus the greatest
parent clock. `issuedAt` is display/audit time only and never decides a winner.

Each signing key allocates a repository-wide monotonically increasing
`sequence`. Gaps and concurrent slots from different keys are valid. Two
different events signed by one key in the same sequence slot are equivocation:
both are excluded and surfaced in audit telemetry. A sequence lower than or
equal to a same-author ancestor is a rollback and is rejected.

Missing-parent events remain pending, with their raw bytes retained. A child of
a rejected event is rejected; it cannot bypass validation by arriving first.
Duplicate event IDs are one logical event and only increment the replay metric.
Repository, object, policy, and schema binding prevents a valid signature from
being replayed into another context.

## References and policy

References are sorted `{relation, family, object}` triples. A comment has one
issue/PR/comment subject; a reaction has one issue/PR/comment/review subject; a
review has one PR subject; a moderation object has one non-notification,
non-moderation subject. References on every event for one object must exactly
match its first event, preventing later retargeting.

Every event binds the SHA-256 ID of a canonical repository policy document:

```json
{
  "schema": "forkmesh-collaboration-policy-v1",
  "repository": "urn:forkmesh:repository:sha256:…",
  "maintainers": ["<Ed25519 key>"],
  "moderators": ["<Ed25519 key>"],
  "notifiers": ["<Ed25519 key>"]
}
```

The repository trust root pins this document. Version 1 deliberately treats a
policy change as a new collaboration epoch; clients must retain the old epoch
and policy to verify history rather than re-authorizing old events under a new
role list.

| Family | Create | Later mutation / tombstone |
| --- | --- | --- |
| Issue, pull request | Any signed identity | Creator or maintainer |
| Comment, reaction | Any signed identity | Creator; moderation is a separate decision |
| Review | Any signed identity | Creator; maintainer may dismiss via tombstone |
| Label, milestone, board | Maintainer | Maintainer |
| Notification | Recipient or policy notifier | Recipient |
| Moderation decision/resolution | Moderator | Moderator; subject creator may appeal |

Unauthorized events remain auditable but never influence state. Renderers MUST
NOT infer moderator power from an author name, server origin, arrival channel,
or possession of a database row.

Authority comes only from the policy document's keys. Implementations MUST NOT
infer any role from asset ownership or transaction history, and MUST NOT
require a wallet, token, blockchain account, payment, reward, tip, or bounty to
create, fold, authorize, moderate, or govern a collaboration object. Incentive
adapters may read folded state, but folded state never reads them; see
[incentive independence](../incentive-independence.md). Changing this rule is a
breaking protocol change.

## Fold and conflict rules

All families use the same primitives, with family-specific fields and role
rules:

| Family | Scalar registers | Observed-remove sets | Required reference |
| --- | --- | --- | --- |
| Issue | title, body, status, priority, dueAt | labels, assignees | — |
| Pull request | title, body, status, base, head, mergeCommit | labels, reviewers | — |
| Comment | body | — | subject |
| Label | name, color, description | subjects | — |
| Reaction | emoji (immutable) | — | subject |
| Review | state, body, revision (immutable) | — | pull request |
| Milestone | title, description, status, dueAt | subjects | — |
| Board | name, description, status | items | — |
| Notification | kind/recipient (immutable), status, message | — | optional subject |
| Moderation | decision, reasonCode, note | — | subject |

For a scalar field, discard a candidate only when another write causally
descends it. One remaining value is current. Concurrent candidates with equal
values coalesce; concurrent differing values produce an explicit `conflicts`
entry and no current value. A resolving event names every conflicting head as a
parent and therefore causally dominates them. Timestamp, lexical author order,
or arrival order never silently resolves ambiguity.

`add` gives each set value an `(eventId, value)` tag. `remove` names the exact
ancestor add events it observed. A concurrent add survives. Tombstones use the
same rule: `restore` removes only the ancestor tombstone IDs it names, so an
unobserved concurrent tombstone continues to hide the object. History is never
erased.

## Moderation lineage

The first `decide` event is moderator-signed and binds its subject. An `appeal`
must be signed by the subject creator, name an ancestor `decide`/`resolve` as
`predecessor`, and descend from it. A moderator `resolve` names both that
predecessor and appeal as ancestors. Skipped, cross-object, or forged lineage is
rejected.

Concurrent differing active decisions never pick a quiet winner: the subject
is visibly `quarantined` with `moderationConflict`. Equal decisions coalesce.
Resolved `hide`, `quarantine`, and `allow` project to hidden, quarantined, and
visible respectively; the signed decision and appeal history remains readable
to authorized audit tools.

## Storage, migration, and recovery

The portable log format is one canonical envelope per NDJSON line with LF
record separators. Export sorts unique raw records by their raw SHA-256 digest.
Import verifies every line and recomputes state; it does not trust an exported
snapshot or silently normalize CRLF records.

Legacy `forkmesh-issue-v1`, `forkmesh-pull-v1`, comment, label, reaction,
review, milestone, project/board, notification, and moderation records migrate
through an importer-signed `create`/`decide` event. That event binds the exact
legacy record digest and records `valid`, `invalid`, or `unavailable` legacy
signature status. It never claims the legacy author made a new signature. The
original bytes stay beside the new log until retention policy permits archival.

Roll back readers before writers: stop emitting v1, keep accepting and
preserving raw envelopes, and project the prior snapshot format read-only. Do
not down-convert or delete v1 events because that loses causality. Recovery is
to restore policy plus NDJSON, run the independent verifier, and compare the
state digest. Corrupt lines remain quarantined while valid independent objects
continue folding.

Use the verifier locally:

```bash
python3 tools/verify_collaboration.py \
  --policy repository-collaboration-policy.json \
  --require-complete collaboration.ndjson
```

`tools/export_collaboration_log.py export --repository IDENTITY --signing-key
SEED` performs this import for a checkout's `forkmesh-issue-v1` records. It
writes `.forkmesh/collaboration/policy.json` and `issues.ndjson`, which can be
committed so a visitor verifies them from a plain clone. The digest binds the
exact record file bytes, and each record's legacy status is `valid` only when
every legacy event signature in it verifies.

The exporter carries its own copy of `issue_event_content` from
`app/src/events.py`, because that module needs the Worker's js bridge and a
stdlib tool cannot import it; `app/tests/test_export_collaboration_log.py` pins
the two byte-for-byte. A legacy `open` event is signed before the issue has a
number, so its signature may bind either 0 or the assigned number; every other
event binds the number. Signing uses `cryptography` when installed and falls
back to pure-Python RFC 8032. A record that v1 cannot represent, such as a
title over the 512-byte limit, is skipped and named in the export summary
rather than truncated, so the log never misstates an issue.

## Threat, abuse, privacy, and telemetry

The main threats are forged authorship, replay across repositories, malicious
clock/timestamp choice, key equivocation, reference retargeting, unauthorized
moderation, deletion races, and a relay omitting events. Signatures and domain
binding address forgery/replay; DAG validation and explicit conflicts address
ordering; role policy and lineage address moderation; observed tombstones
address deletion races. Omission requires peer exchange/availability receipts
and cannot be solved by a fold over an incomplete set.

Raw events can contain private issue text, notification recipients, moderation
notes, and relationship metadata. Private logs stay inside the repository's
authorization/encryption boundary. Telemetry exports counts only:
`received`, `uniqueRaw`, `signatureValid`, `accepted`, `pending`, `rejected`,
`duplicateDeliveries`, `equivocations`, `conflicts`, rejection reason, and
accepted family. It MUST NOT export bodies, object IDs, public keys, refs, or
raw signatures.

## Automated evidence and supported runtimes

`app/tests/test_collaboration_protocol.py` supplies the signed convergence
corpus, randomized permutations/replays, byte mutation fuzzing, adversarial
authorization/signature/sequence cases, all-family legacy migration,
rollback/recovery, telemetry, and Python↔WebCrypto parity. The dedicated
`collaboration-conformance.yml` workflow runs it with Python 3.12 and Node 24 on
Linux, macOS, and Windows and retains JUnit evidence. The same focused suite is
part of the core quality gate.

No network service, wall clock, or database is required. This is intentional:
an implementation passes only by deriving state from retained signed bytes.
