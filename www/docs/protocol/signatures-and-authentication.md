# Signatures and authentication

Suite version: `1.0.0-rc.1` · Vectors: `signatures.json`, `events.json`,
`sessions.json` · Part of the [ForkMesh protocol suite](README.md)

## Ed25519

- **FM-SIG-001** MUST — Signatures are Ed25519 (RFC 8032, PureEdDSA, no
  context) verified with the cofactorless equation `[S]B = R + [k]A`, where
  `k = SHA-512(R || A || M) mod L`. A verifier rejects:
  - a signature that is not exactly 64 bytes;
  - a scalar `S ≥ L`, so no `S + L` malleability;
  - a point encoding with `y ≥ p`, no square root, or `x = 0` with the sign
    bit set;
  - the identity as the public key;
  - any public key outside the prime-order subgroup (`[L]A ≠ identity`), which
    excludes all small-order and mixed-order keys.

Implementations that wrap a platform verifier (for example WebCrypto) MUST add
the checks their platform omits.

## Domain separation

- **FM-SIG-002** MUST — Every signed message begins with a frozen,
  scheme-specific prefix, and a verifier builds the message itself from the
  fields it will act on. A client-supplied message is never accepted. A
  signature valid under one prefix therefore never verifies under another
  scheme, another version, or a substituted field such as a different HTTP
  method.

### Canonical-string schemes

These follow FM-ENC-007. The fields below come after the prefix line.

| Prefix | Fields | Verified by |
| --- | --- | --- |
| `forkmesh-catalog-v1` | owner, name, updatedAt | relay, catalog publish (public records only) |
| `forkmesh-catalog-v2` | recordSha256 | relay, catalog publish |
| `forkmesh-repostate-v1` | owner, repo, stateHash (FM-ID-005), updatedAt | relay, catalog publish and mirror attestations |
| `forkmesh-private-manifest-v1` | owner, name, opaqueRepoId, keyEpoch, manifestHash, updatedAt | relay, private catalog publish |
| `forkmesh-view-v1` | owner, repo, ts | relay, private browse and clone |
| `forkmesh-rotate-v1` | oldPubkey, newPubkey, ts | relay, account key rotation (signed by the old key) |
| `forkmesh-git-push-v1` | owner, repo, method, ts, bodySha256 | relay and mirror gateway, HTTPS receive-pack |

For `forkmesh-catalog-v2`, `recordSha256` is the lowercase hex SHA-256 of the
normalized catalog record minus its descriptive `signature` field. The record
is serialized with sorted keys, no whitespace, and ASCII escaping. The
[published protocol reference](https://forkmesh.com/docs/protocol/#signature-schemes)
lists the remaining legacy schemes (issue, discussion, pull, release, room,
node link). Their field layout is informative until they receive vectors; see
TM-004 in the [threat model](threat-model.md).

## Collaboration envelopes

- **FM-SIG-003** MUST — A collaboration event's signing bytes are
  `"forkmesh-collaboration-event-v1\n"` followed by the canonical JSON of the
  unsigned envelope. The unsigned envelope is the event without `signature`
  and `eventId`, with exactly the keys `schema`, `repository`, `policy`,
  `family`, `object`, `action`, `author`, `sequence`, `clock`, `issuedAt`,
  `parents`, `references`, and `payload`. The author's key signs those bytes.
- **FM-SIG-004** MUST — A receiver rejects an event at the first failing step
  and never lets it influence causality, authorization, or state:
  1. size and canonical parse (FM-ENC-004, FM-ENC-005);
  2. exact key set (`invalid_envelope_keys`), then schema identifier
     (`unsupported_schema`);
  3. field shapes: repository URN, policy digest, known family and action,
     object URN of that family, author and signature encodings, non-negative
     safe integers, sorted duplicate-free `parents` (at most 64), references,
     and payload structure;
  4. event identity (`event_id_mismatch`, FM-ID-004);
  5. for `create` and `decide`, object identity (`object_id_mismatch`,
     FM-ID-003);
  6. repository binding (`wrong_repository`) and policy binding
     (`unknown_policy`) against the pinned policy;
  7. the Ed25519 signature (`invalid_signature`).

  Binding is checked before the signature. A replay into another repository
  is therefore classified as a replay even when its signature is also invalid.

Causality, sequence, equivocation, and authorization rules follow in
[Relay, synchronization, and migration](relay-sync-and-migration.md#collaboration-log-exchange).

## Request authentication

- **FM-AUTH-001** MUST — A time-bound request proof (`view`, `rotate`, and
  `git-push` schemes) is accepted only when its timestamp field is within
  300,000 ms of the verifier's clock in either direction, inclusive. Senders
  write the timestamp as ASCII decimal digits. A verifier rejects a timestamp
  that is not a base-10 integer, and it signs-checks the field text as sent
  (FM-ENC-007). The window limits replay but does not prevent it; see TM-012.
- **FM-AUTH-002** MUST — An HTTPS push is authorized by a signature over
  `forkmesh-git-push-v1`. Before signing and verifying, the owner is trimmed and
  lowercased, the repository trimmed, the method trimmed and uppercased, and
  the body digest trimmed and lowercased. Anything else is rejected: an owner
  that is not a valid account name, a repository that is not a single safe
  path segment, a method other than `GET` or `POST`, a timestamp that is not
  1–16 digits, or a body digest that is not 64 hex digits. The ref
  advertisement `GET` signs the digest of the empty body. When the relay
  streams a pack it has not buffered, it forwards the signed digest, and the
  gateway MUST check the received bytes against it before running
  receive-pack.
- **FM-AUTH-003** MUST — A portable session token is
  `v1.<claim>.<signature>`. `<claim>` is 80–1024 base64url characters of
  ASCII-escaped canonical JSON. `<signature>` is 86 characters and signs
  `"forkmesh-portable-session-v1\n" || <claim>` with the router key. The claim
  has exactly the keys:
  - `type` = `forkmesh.portable-session`;
  - `aud` = `forkmesh-mirror-dashboard`;
  - `v` = 1;
  - integer `iat` and `exp`;
  - `sub`: 32–128 lowercase hex;
  - `name`: an account name;
  - `sid` and `jti`: opaque 16–128 base64url-alphabet strings.

  A verifier rejects, in order: framing (`token_invalid`), encodings
  (`encoding_invalid`), claim shape (`claim_invalid`), time (`not_current`:
  `iat ≤ 0`, `exp ≤ iat`, lifetime over 12 hours, `iat` more than 60 s ahead,
  or `exp` more than 60 s past), a non-canonical claim
  (`claim_noncanonical`), and the signature (`signature_invalid`).
- **FM-AUTH-004** MUST — A catalog publication is signed by a key registered
  to the owner account.
  - When `catalogSigVersion` is 2, `catalogSig` MUST verify
    `forkmesh-catalog-v2`.
  - Otherwise it MUST verify `forkmesh-catalog-v1`, and only for a record
    whose visibility is `public`. Any other visibility is refused with HTTP 426
    `client_upgrade_required` and `requiredCatalogSigVersion: 2` before any
    confidential field is stored.
  - A present `stateSig` MUST verify `forkmesh-repostate-v1`, and a present but
    invalid attestation rejects the whole write.
  - A record whose `updatedAt` is older than the stored record is rejected as
    a rollback.

Inbox submissions (issues, pulls, discussions) prove only that a record was
not altered after signing. They carry no authority. The repository owner's
review and push is the authorization step.
