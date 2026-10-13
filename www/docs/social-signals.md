# Portable social signals

Suite version: `1.0.0` · Vectors: `conformance/social-signals/v1/vectors.json`
· Implementations: `tools/social_signals.py` (Python) and
`app/public/social-signals.js` (JavaScript) · Tracking: issue #493

Stars, watches, bookmarks, follows, recommendations, usage endorsements,
security reviews, and mirroring commitments are **signed records you own**.
They are not rows in one service's database. You can export them from one
service, import them into another, merge copies from several places, and
revoke them. Any implementation can verify them without asking the service
that first stored them.

This page is normative for the v1 record, bundle, merge, and recommendation
rules (requirements `FM-SOC-*`), and it is also the user guide. It reuses the
protocol suite's canonical JSON and strict Ed25519 rules
([encoding](protocol/encoding-and-identifiers.md),
[signatures](protocol/signatures-and-authentication.md), FM-SIG-001).

## What ships today

| Capability | Status |
| --- | --- |
| Signed records, revocation, bundle export/import/merge, verification | Python and JavaScript implementations, shared vectors, cross-implementation round trip in CI |
| Explainable recommendations with an off switch and mutes | Both implementations; deterministic output pinned by vectors |
| Command-line tool (`tools/social_signals.py`) | Available |
| Web, desktop, and mobile screens that create and show these records | Not yet. Existing stars (`repo_stars`) and follows (`profile_follows`) are still unsigned per-account rows on the hosted service |
| Hosted import/export endpoint and production telemetry | Not yet |

Until the clients adopt these records, the hosted star and follow counts are
not portable. Use the command-line tool to create and carry signed records.

## Two tiers that cannot be confused

- **FM-SOC-001** MUST — Every record carries a `tier` that is fixed by its
  `kind`. A verifier rejects a mismatch (`tier_mismatch`), so a star can never
  claim to be an attestation and an endorsement can never be downgraded to look
  casual.

| Kind | Tier | Required scope | Evidence |
| --- | --- | --- | --- |
| `star`, `watch`, `bookmark`, `recommendation` | interest | none: `scope` must be `{}` | none: `evidence` must be `[]` |
| `follow` | interest | none; the subject is an identity, not a repository | none |
| `usage-endorsement` | attestation | `environment` (`production`, `staging`, `development`), `version`, `since` (≤ `issuedAt`) | 1–16 links |
| `security-review` | attestation | full 40- or 64-hex `revision`, `method` (`manual-audit`, `automated-scan`, `threat-model`), `findings` (`none`, `resolved`, `open`) | 1–16 links |
| `mirror-commitment` | attestation | `https` `endpoint`, sorted unique `protocols` from `forkmesh-relay`, `git-https`, `git-ssh`; the record must set `expiresAt` | 1–16 links |

- **FM-SOC-002** MUST — An interest record with any scope or evidence is
  rejected (`interest_claims_scope`). An attestation without evidence is
  rejected (`evidence_required`). Each evidence item is exactly
  `{label, uri, sha256}`: `label` is 1–120 UTF-8 bytes, `uri` is an `https://`
  URL of printable ASCII up to 2048 bytes, and `sha256` is `null` or
  `sha256:<64 hex>` for evidence whose bytes are pinned.
- **FM-SOC-003** MUST — Clients show attestations with their category,
  scope, and evidence count, never as a star icon or a count merged with
  stars. Both implementations expose the display line (`describe`), for
  example `Security review: manual-audit of 84a31cda4ed3, findings resolved
  (2 evidence)`. The vectors pin this text. Evidence is a claim the signer
  makes, not something ForkMesh has checked. Clients link to it rather than
  vouching for it.

## Record format

```json
{
  "schema": "forkmesh-social-signal-v1",
  "kind": "usage-endorsement",
  "tier": "attestation",
  "signer": "<base64url Ed25519 public key>",
  "subject": "urn:forkmesh:repository:sha256:<64 hex>",
  "issuedAt": 1767398400000,
  "expiresAt": null,
  "visibility": "public",
  "scope": {"environment": "production", "version": "2.4.1", "since": 1767225600000},
  "evidence": [{"label": "Deployment manifest", "uri": "https://example.org/deploy.yaml", "sha256": null}],
  "note": "",
  "signalId": "sha256:<64 hex>",
  "signature": "<base64url, 64 bytes>"
}
```

- **FM-SOC-004** MUST — A record has exactly these keys. Repository subjects
  are protocol repository identifiers (FM-ID). A `follow` subject is
  `urn:forkmesh:identity:ed25519:<base64url key>` and cannot name the signer
  (`self_follow`). Timestamps are non-negative safe integers in milliseconds.
  `expiresAt` is `null` or later than `issuedAt`. `note` is at most 1024 UTF-8
  bytes.
- **FM-SOC-005** MUST — The signing bytes are
  `"forkmesh-social-signal-v1\n"` followed by the canonical JSON of the record
  without `signalId` and `signature`. `signalId` is `sha256:` plus the
  lowercase hex SHA-256 of those same bytes. Strings are NFC-normalized before
  hashing, so a decomposed copy of a note verifies as the signed form.
- **FM-SOC-006** MUST — A verifier checks in the order the implementations
  use: canonical JSON (`invalid_number`, `invalid_unicode`, `invalid_key`), keys,
  schema, kind, tier, signer, subject, times, visibility, note, scope and
  evidence, signature encoding, `signal_id_mismatch`, the strict Ed25519
  signature (`invalid_signature`, including small-order keys and `S ≥ L`), and
  finally `issued_in_future` when `issuedAt` is more than five minutes past
  the verifier's clock. The first failure's code is reported.

### Privacy choices

- **FM-SOC-007** MUST — `visibility` is signed, so a relay cannot widen it.
  `public` records go to anyone. `followers` records go only to exports for
  followers. `private` records, such as bookmarks, go only to your own `self`
  backup. Exports enforce this; see [bundles](#export-import-and-merge).
  Visibility controls who a conforming service hands a record to. Records are
  signed, not encrypted, so encrypt a `self` backup you store elsewhere.

## Revocation

```json
{
  "schema": "forkmesh-social-revocation-v1",
  "signer": "<same key as the revoked record>",
  "revokes": "sha256:<signalId>",
  "revokedAt": 1767830400000,
  "reason": "withdrawn",
  "revocationId": "sha256:<64 hex>",
  "signature": "<base64url>"
}
```

- **FM-SOC-008** MUST — A revocation is signed like a record under the
  prefix `"forkmesh-social-revocation-v1\n"`. `reason` is `withdrawn`,
  `superseded`, `compromised`, or `mistaken`. A revocation takes effect only
  when its signer is the revoked record's signer. Otherwise it stays in the
  store with status `signer_mismatch` and changes nothing. A revocation whose
  target has not arrived yet is kept as `pending` and applies as soon as the
  record is imported, whatever the order.
- **FM-SOC-009** MUST — Revocation is final. There is no un-revoke. To
  express interest again, sign a new record.

## Export, import, and merge

A bundle is
`{"schema": "forkmesh-social-signal-bundle-v1", "exportedAt", "signals": [...], "revocations": [...]}`.

- **FM-SOC-010** MUST — Importing validates each record independently. An
  invalid record is reported with its code and skipped. It never aborts the
  import or hides the valid records next to it. The import report is
  `{accepted, duplicate, rejected: [{type, id, error}]}`. The whole bundle is
  rejected only when its envelope is wrong (`invalid_bundle`,
  `unsupported_bundle_schema`) or it holds more than 10,000 records
  (`bundle_too_large`).
- **FM-SOC-011** MUST — The store is the union of verified records keyed by
  `signalId` and `revocationId`. Merging bundles in any order, with duplicates,
  yields the same state. The state digest is `sha256` over
  `"forkmesh-social-signal-state-v1\n"` plus the canonical JSON of the sorted
  ID lists. The vectors check all 24 import orders of a four-service scenario.
- **FM-SOC-012** MUST — The folded status of a record is, in precedence
  order:
  1. `revoked` (by a matching revocation).
  2. `superseded`: a newer record exists from the same signer with the same
     kind and subject. The newest is the largest `issuedAt`, with ties going
     to the larger `signalId`. The newest counts even when it is itself
     revoked, so revoking your latest star doesn't bring back an older one.
  3. `expired`: `expiresAt ≤ asOf`.
  4. `active`.
- **FM-SOC-013** MUST — Export takes an audience: `public` (public records
  only), `followers` (public and followers), or `self` (everything). It
  optionally takes one signer. Revoked and superseded records are exported
  with their revocations so that removal propagates. A revocation is exported
  when its target is exported. A `self` export also carries pending
  revocations.

## Recommendations you can inspect and turn off

Recommendations are computed locally from the records you hold. There is no
required global graph and no hidden ranking service.

- **FM-SOC-014** MUST — Recommendation controls are
  `{viewer, enabled, asOf, limit (1–100), mutedSigners, mutedSubjects}`.
  They are your settings. They are not signed records and are never exported.
  With `enabled: false` the output has no recommendations, and nothing is
  ranked or counted.
- **FM-SOC-015** MUST — Only a signer the viewer follows with an **active**
  `follow` record can influence the viewer's recommendations. Follows are not
  transitive, so a crowd of fresh keys that nobody you follow vouches for
  adds nothing. That's the v1 sybil resistance. Muting a signer or a subject
  removes it at once (moderation you control).
- **FM-SOC-016** MUST — Weights are `star` 1, `watch` 1, `recommendation` 2,
  `mirror-commitment` 2, `usage-endorsement` 3, and `security-review` 3.
  Bookmarks and follows never count. One signer adds at most their single
  highest weight to a subject, never a sum, so signing many records gains
  nothing. Subjects the viewer already signed for, private records, and the
  viewer's own records are left out.
- **FM-SOC-017** MUST — Every recommendation lists its `score`, number of
  `supporters`, and every contributing record with `signer`, `kind`, `tier`,
  `weight`, the display line, `scope`, and `evidence`. That's the answer to
  "why was this recommended?" The output also counts what was left out and
  why: `inactive`, `mutedSigner`, `untrusted`, `mutedSubject`, `private`,
  `alreadyKnown`. Results sort by score, then subject.

## Using the command-line tool

```sh
python3 tools/social_signals.py keygen --out ~/.forkmesh/social.seed    # prints your public key
cat > star.json <<'EOF'
{"kind": "star", "subject": "urn:forkmesh:repository:sha256:<hex>", "issuedAt": 1767398400000}
EOF
python3 tools/social_signals.py sign --seed-file ~/.forkmesh/social.seed star.json > signed.json
python3 tools/social_signals.py revoke --seed-file ~/.forkmesh/social.seed sha256:<signalId> --reason withdrawn
python3 tools/social_signals.py merge laptop.json server.json --audience public --output public.json
python3 tools/social_signals.py verify public.json          # exit 1 if any record was rejected
python3 tools/social_signals.py recommend public.json --viewer <your key>
python3 tools/social_signals.py recommend public.json --viewer <your key> --disable
```

A signed record must be wrapped in a bundle before `merge`, `verify`, or
`recommend`. `merge` prints the import reports and the state digest on stderr.
The seed file is created with mode `0600`. Never share it, and don't send it
to support.

## Failure and recovery

| Situation | What happens | Recovery |
| --- | --- | --- |
| A bundle contains a tampered, forged, or malformed record | That record is reported in `rejected` with its code; everything valid still imports | Fix or drop the source; re-import. Nothing partial is written for a rejected record |
| A revocation arrives before its record | Kept `pending`; applies on arrival | None needed |
| Someone else "revokes" your record | `signer_mismatch`, no effect | None needed |
| You signed a record by mistake | Sign a revocation with reason `mistaken` and export again | Revocation is final; sign a fresh record if you change your mind |
| Your signing key is compromised | Revoke what you can with reason `compromised`, then rotate your identity ([portable identities](portable-identities.md)) | Re-sign the records you still stand behind with the new key and ask followers to follow the new identity |
| A mirror commitment lapses | Status becomes `expired` and it stops counting | The operator signs a new commitment with a new `expiresAt` |
| A state disagrees between two services | Compare the state digests; import both bundles into one store | Merging is order-independent, so the union is the reconciliation |

## Conformance and evidence

```sh
python3 tools/social_signals.py vectors --check   # committed vectors match the reference generator
python3 tools/social_signals.py vectors           # Python implementation against the vectors
node app/tests/run_social_signal_vectors_js.js    # JavaScript implementation against the vectors
python3 tools/social_signals.py gate              # all of the above plus a cross-implementation round trip
python3 -m pytest app/tests/test_social_signals.py
```

The `social-signal-portability` quality gate and the collaboration
conformance workflow run `gate` on Linux, macOS, and Windows runners. The
round trip works like this:

1. Python signs a record and exports a `self` bundle.
2. JavaScript imports and merges it with other services' bundles, signs a new
   record and a revocation of the Python record with WebCrypto, and exports.
3. Python verifies every JavaScript signature, sees the revocation applied,
   and must reach the identical folded state and digest.

`gate` prints per-group case counts for each implementation. That's the
operational report for this layer. Hosted telemetry for signal portability,
retained follows, recommendation engagement, abuse rate, and fulfilled mirror
commitments needs the hosted endpoint, which isn't built yet. Issue #493
stays open until clients, the hosted service, and that telemetry exist.

## Changing this format

Any change to a rule, weight, or display line changes the vectors. Regenerate
them with `python3 tools/social_signals.py vectors --write`, update both
implementations in the same change, and bump the suite version. The two
implementations are maintained separately. Fix a disagreement in the
implementation that departs from this page, never by copying the other's
behavior.
