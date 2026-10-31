# Scoped community trust lists

Status: protocol version 1, candidate. The dependency-free reference resolver
is `app/src/trust_lists.py`. The vectors in
`conformance/trust-lists/v1/vectors.json` bind four independent resolvers to
it byte for byte: the web client (`app/public/trust-lists.js`), the iOS client
(`native/ios/ForkMeshCore/Sources/FMAccount/TrustLists.swift`, tested by
`native/ios/ForkMeshCore/Tests/FMAccountTests/ConformanceTests.swift`), the
Android client
(`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/TrustLists.kt`,
tested by
`native/android/core/src/test/kotlin/com/forkmesh/nativeapp/security/TrustListsCryptoTest.kt`),
and the desktop client (`desktop/src/TrustLists.cpp`). The user guide is
[Trust lists](../trust-lists.md).

## Goals and non-goals

A trust list is a signed statement that says, for example, "in the `forkmesh`
organization, these keys are security researchers whose advisories I stand
behind". People follow the lists they find useful. ForkMesh does not run a
global list, and no list is followed by default.

- **Reputation is scoped.** Every entry names one role and one repository,
  organization, or community. There is no wildcard or global scope. A follow,
  an include, or a delegation can only narrow scope, never widen it.
- **Nothing essential depends on a list.** Cloning, reading, identity,
  sign-in, pushing, and opening issues or pull requests never consult trust
  lists. A client with no follows behaves exactly as it does today. Lists
  inform ranking, review routing, badges, and moderation queues.
- **Disagreement stays visible.** When followed lists disagree, the result is
  `conflict`. Only the user's local override picks a side.
- **Compromise is recoverable locally.** Equivocation, a forked history, or
  a revocation cuts a list back to its last good version. The client shows
  the list as `recovered` instead of silently following the newest document.

## Documents

Documents are canonical JSON. Every string is printable ASCII other than `"`
and `\`. Object keys are sorted by code point, there is no whitespace, and
integers are at most 2^53−1 in magnitude, with no fractions, exponents, or
`-0`. A receiver accepts a document only when re-encoding its parse
reproduces the received bytes exactly. The restricted alphabet lets every
language write this encoding with a plain sorted-key writer. A raw document
is at most 262,144 UTF-8 bytes.

A document's identifier is `sha256:<hex SHA-256 of the raw bytes>`. Because
the raw bytes include the signature, two documents with the same meaning but
different signatures have different identifiers.

The signed message is the document's `schema` value, a newline, and the
canonical JSON of the document without its `signature` key. Signatures are
Ed25519 over those bytes. The rules of FM-SIG-001 in the
[protocol suite](../protocol/signatures-and-authentication.md#ed25519) apply.
Every resolver also rejects a scalar `S ≥ L` explicitly before calling a
platform verifier. It also rejects non-canonical key encodings (including
x = 0 with the sign bit set), the identity, and small-order and mixed-order
keys. Platform verifiers such as WebCrypto and OpenSSL do not, so the iOS and
Android resolvers use their own strict verifier (`Ed25519.verifyStrict`).
The `forged-weak-keys` vector carries signatures that pass lax verification
for each of those keys. Keys are 43-character and signatures 86-character
unpadded base64url, and both must round-trip.

A list is identified by issuer key and name, not by any single version:

```text
listId = "urn:forkmesh:trust-list:sha256:" ||
         hex(sha256("forkmesh-trust-list-id-v1\n" || issuer || "\n" || name))
```

### List version (`forkmesh-trust-list-v1`)

| Field | Rule |
| --- | --- |
| `issuer` | Signing key of the list owner |
| `name` | Slug `[a-z0-9][a-z0-9-]{0,63}` |
| `version` | Integer ≥ 1 |
| `previous` | `null` for version 1; otherwise the document identifier of version − 1 |
| `issuedAt`, `expiresAt` | Milliseconds; `expiresAt > issuedAt`; lifetime at most 400 days |
| `roles` | Sorted, non-empty set of `maintainer`, `moderator`, `mirror-operator`, `organization`, `reviewer`, `security-researcher` |
| `communities` | Sorted, non-empty set of at most 64 scopes, none covered by another |
| `revocationKeys` | Sorted set of at most 4 keys, not including `issuer`, that may revoke later versions |
| `entries` | At most 2,000 `{subject, role, scope, stance, note}` entries, strictly sorted by subject, role, scope |
| `edges` | At most 16 `{kind, issuer, name, roles, communities, expiresAt}` edges, strictly sorted by issuer, name |
| `signature` | By `issuer` |

An entry's `stance` is `trust` or `distrust`. Its `note` is at most 280
characters. Its subject is never the issuer. An entry's role must be one of
the list's roles, and its scope must be covered by the list's communities.

An edge whose `kind` is `include` composes another list and has
`expiresAt: null`. An edge whose `kind` is `delegate` grants another key
authority to publish entries for this list and carries an integer
`expiresAt`. Either way, the edge's roles must be a subset of the list's
roles, its communities must be covered by the list's communities, and it
cannot name its own list.

### Scopes

| Scope | Covers |
| --- | --- |
| `repo:<owner>/<repository>` | That repository |
| `org:<owner>` | Itself and every `repo:<owner>/…` |
| `community:<slug>` | Itself |

Owners match `[a-z0-9][a-z0-9-]{0,38}`. Repositories match
`[A-Za-z0-9._-]{1,100}` other than `.` and `..`. The intersection of two
scope sets keeps each member of one set that the other set covers. It then
removes duplicates and any member covered by another member.

### Revocation (`forkmesh-trust-list-revocation-v1`)

`{list, issuer, name, fromVersion, revokedBy, revokedAt, reason, successor}`,
signed by `revokedBy`. `list` must equal the listId of `issuer` and `name`.
`reason` is `compromised`, `malicious`, `retired`, or `superseded`.
`successor` is `null` or `{issuer, name}` of a different list.

A revocation is authorized when `revokedBy` is the issuer, or when a verified
version of the list with `version < fromVersion` names `revokedBy` in
`revocationKeys`. A recovery key committed before a compromise can therefore
revoke everything the attacker publishes. An attacker who adds keys in a
malicious version cannot use them to revoke the versions before it.
Revocations have no expiry. Revocation is monotonic, so no document can undo
one.

### Appeal (`forkmesh-trust-list-appeal-v1`)

`{subject, list, version, role, scope, statement, issuedAt}`, signed by the
`subject` it concerns, with a statement of 1–1,000 characters. Nobody can
appeal on someone else's behalf.

## Verification order

A resolver rejects a document at the first failing step and records
`{documentId, code}`:

1. `too_large`; bracket nesting deeper than 8 outside strings, found by a
   scan before any parser runs, or unparseable JSON: `invalid_json`;
   `noncanonical`; a top level that is not an object, or a `schema` that is
   not a known string: `unsupported_schema`; wrong key set: `invalid_keys`.
2. List version: `invalid_issuer`, `invalid_name`, `invalid_version`,
   `invalid_previous`, `invalid_time`, `invalid_scope` (roles, then
   communities), `invalid_revocation_keys`. Then, entry by entry,
   `invalid_entry`, `entry_outside_scope`, `unsorted_entries`. Then, edge by
   edge, `invalid_edge`, `edge_outside_scope`, `unsorted_edges`. Last,
   `invalid_signature_encoding` and `invalid_signature`.
3. Revocation: `invalid_issuer`, `invalid_name`, `list_mismatch`,
   `invalid_version`, `invalid_revoker`, `invalid_time`, `invalid_reason`,
   `invalid_successor`, then the signature.
4. Appeal: `invalid_subject`, `invalid_list`, `invalid_version`,
   `invalid_scope`, `invalid_statement`, `invalid_time`, then the signature.
5. In context: a list version or appeal whose `issuedAt` is more than
   300,000 ms after the resolver's clock is rejected as `issued_in_future`,
   and an unauthorized revocation as `unauthorized_revoker`.

Duplicate raw documents are counted once.

## Version history

For each list, a resolver considers every verified version it holds:

- two different documents with the same `version` are an **equivocation** at
  that version;
- a document whose `previous` does not equal the identifier of the single
  held document for `version − 1` is a **history fork** at that version;
- an authorized revocation cuts at its `fromVersion`.

The cutoff is the lowest of these versions. The head is the highest held
version below the cutoff that has exactly one document. The list state is:

| State | Meaning |
| --- | --- |
| `active` | A head exists, it has not expired, and nothing was cut |
| `recovered` | A head exists below a cutoff; `cutoffReasons` says why |
| `expired` | The head's `expiresAt` has passed; older versions are never revived |
| `revoked` | An authorized revocation left no usable version |
| `conflicted` | Equivocation or a fork left no usable version |
| `missing` | The list is followed or referenced, but no verified version is held |

Only `active` and `recovered` lists contribute entries and edges. A
revocation's `successor` is shown to the user and never followed
automatically. A compromised issuer key might have been used to revoke too,
so the user decides.

## Composition and merge

Local inputs are **follows**, `{issuer, name, roles, communities}` where a
`null` narrowing means the list's own scope, and **overrides**,
`{subject, role, scope, stance}` where the stance is `trust`, `distrust`, or
`ignore`. Neither is published or signed. They stay on the device. The client
that stores them validates them before resolving: keys and names use the
document field rules, and each override tuple appears at most once.
Resolvers do not re-validate local input.

1. Each follow starts a walk at its list, scoped to the intersection of the
   list's roles and communities with the follow's narrowing.
2. A walk admits each head entry whose role is in the walk's roles and whose
   scope is covered by the walk's communities.
3. A walk follows each edge. An expired delegation is skipped and counted.
   An edge back to a list already on the path is skipped as a cycle. An edge
   from a path that already holds 4 lists is skipped as depth-limited.
   Otherwise the child walk's scope is the intersection of the current scope,
   the edge's scope, and the child's own scope. The walk stops if that scope
   is empty.
4. Each `(subject, role, scope)` tuple collects at most one source per list,
   the one reached by the shortest path, with ties broken by the
   newline-joined path. Its `stance` is the shared stance of its sources,
   `conflict` when they differ, or `none` when only an override names it.
   `effective` is the override when present, otherwise the stance.
5. A verified appeal attaches to the tuple's assertion when one of that
   assertion's sources is the appealed list. The appeal is `resolved` when the
   list's head is newer than the appealed version and no longer distrusts the
   tuple, and `open` otherwise. Appeals that match no assertion are counted as
   unattached.

The resolution is canonical JSON with `lists` sorted by `listId`,
`assertions` by subject, role, and scope, `sources` by `listId`, `appeals` by
identifier, `rejected` by identifier and code, and `conflicts` by their
canonical JSON. `conflicts` holds `stance` conflicts, noting whether an
override resolved them, plus `equivocation` and `history_fork` records. The
result does not depend on the order of documents, follows, or overrides.

## Telemetry and privacy

`telemetry` holds counts only (documents, rejections by code, lists by state,
assertions, conflicts, overrides, appeals, skipped edges), never keys, list
names or follows. Follows and overrides never leave the device. Clients SHOULD
fetch the whole public bundle, or fetch through a mirror they operate, so a
relay does not learn which lists interest them. Entries name keys, not account
names.

## Threats

| Threat | Mitigation |
| --- | --- |
| Stolen issuer key publishes a malicious version | Recovery key revokes `fromVersion`; followers fall back to the last good version as `recovered` |
| Issuer shows different versions to different followers | Equivocation cuts below the disputed version once both copies meet |
| Rewritten history | `previous` digest chain; a fork cuts the list |
| A list claims global authority | No global scope; entries and edges must stay inside the list's own scope |
| Delegate or included list overreaches | Scope intersection at every hop; delegations expire |
| Attacker revokes with a key added in a malicious version | Only keys named in a version *before* `fromVersion` may revoke |
| Stolen issuer key revokes the whole list | Accepted as a denial of service; the owner publishes a successor under a new key and followers choose to follow it |
| Signature malleability and canonicalization tricks | Byte-exact canonical re-encoding, strict base64url, and explicit `S < L` |
| Resource exhaustion | Size, entry, edge, community, and path-depth limits |
| Silent winner between lists | `conflict` until the user overrides |
| Forged appeals | Appeals must be signed by their subject |

## Rollout

Version 1 is additive: no existing route, table or authorization decision reads
it, and follows and overrides are local, so removing the resolvers is a full
rollback. Any change in meaning needs a new schema string; changing the vectors
means regenerating them with `generate_vectors.py` and passing all four
resolvers. Publishing lists over relays and the authoring and follow UI are not
implemented yet.
