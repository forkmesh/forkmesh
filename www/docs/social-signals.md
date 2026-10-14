# Portable social signals

Suite `1.0.0` · Vectors: `conformance/social-signals/v1/vectors.json` ·
Implementations: `tools/social_signals.py` (Python), `app/public/social-signals.js`
(JavaScript).

Stars, watches, bookmarks, follows, recommendations, usage endorsements,
security reviews and mirroring commitments are signed records the user owns.
They can be exported, imported, merged and revoked, and any implementation can
verify them. This page is normative for requirements `FM-SOC-*`; it reuses the
protocol's canonical JSON and strict Ed25519 rules
([encoding](protocol/encoding-and-identifiers.md),
[signatures](protocol/signatures-and-authentication.md)).

Status: records, bundles, merge, verification, recommendations and the CLI ship.
Client screens, a hosted import/export endpoint and telemetry: Not yet. Hosted
stars and follows are still unsigned per-account rows.

## Tiers

| Kind | Tier | Scope | Evidence |
| --- | --- | --- | --- |
| `star`, `watch`, `bookmark`, `recommendation`, `follow` | interest | `{}` | `[]` |
| `usage-endorsement` | attestation | `environment`, `version`, `since` | 1–16 links |
| `security-review` | attestation | `revision`, `method`, `findings` | 1–16 links |
| `mirror-commitment` | attestation | `https` `endpoint`, `protocols`; `expiresAt` required | 1–16 links |

- **FM-SOC-001** `tier` is fixed by `kind` (`tier_mismatch`).
- **FM-SOC-002** Interest records carry no scope or evidence
  (`interest_claims_scope`); attestations need evidence (`evidence_required`),
  each item exactly `{label, uri, sha256}`.
- **FM-SOC-003** Attestations are displayed with category, scope and evidence
  count (`describe`), never merged with stars. Evidence is the signer's claim.

## Record format

Schema `forkmesh-social-signal-v1` with keys `schema, kind, tier, signer,
subject, issuedAt, expiresAt, visibility, scope, evidence, note, signalId,
signature`.

- **FM-SOC-004** Exactly these keys; subjects are protocol repository
  identifiers or, for `follow`, `urn:forkmesh:identity:ed25519:<key>` other than
  the signer (`self_follow`); millisecond timestamps; `note` ≤ 1024 bytes.
- **FM-SOC-005** Signing bytes are `"forkmesh-social-signal-v1\n"` + canonical
  JSON without `signalId`/`signature`; `signalId` is `sha256:` of those bytes;
  strings are NFC-normalized.
- **FM-SOC-006** Verification order ends with `signal_id_mismatch`,
  `invalid_signature` and `issued_in_future` (more than five minutes ahead); the
  first failure code is reported.
- **FM-SOC-007** Signed `visibility`: `public`, `followers` or `private`.
  Exports enforce it; records are signed, not encrypted.

## Revocation

- **FM-SOC-008** Schema `forkmesh-social-revocation-v1`, prefix
  `"forkmesh-social-revocation-v1\n"`, reason `withdrawn`, `superseded`,
  `compromised` or `mistaken`. Only the record's signer can revoke; otherwise
  status `signer_mismatch` and no effect. A revocation arriving first is
  `pending` until its record arrives.
- **FM-SOC-009** Revocation is final; sign a new record instead.

## Export, import, and merge

Bundle: `{"schema": "forkmesh-social-signal-bundle-v1", "exportedAt", "signals", "revocations"}`.

- **FM-SOC-010** Each record is validated independently; invalid ones are
  reported in `{accepted, duplicate, rejected}` and skipped. Only a bad envelope
  or more than 10,000 records rejects the bundle.
- **FM-SOC-011** The store is the union keyed by ID, so merge order does not
  matter. State digest: `sha256` over `"forkmesh-social-signal-state-v1\n"` +
  canonical sorted ID lists.
- **FM-SOC-012** Status precedence: `revoked`, `superseded` (newer record, same
  signer, kind and subject), `expired`, `active`.
- **FM-SOC-013** Export audience `public`, `followers` or `self`; revocations
  travel with their targets.

## Recommendations

- **FM-SOC-014** Local controls `{viewer, enabled, asOf, limit, mutedSigners,
  mutedSubjects}`, never exported; `enabled: false` computes nothing.
- **FM-SOC-015** Only signers the viewer actively follows count (no
  transitivity); mutes apply at once.
- **FM-SOC-016** Weights: `star` 1, `watch` 1, `recommendation` 2,
  `mirror-commitment` 2, `usage-endorsement` 3, `security-review` 3. One signer
  adds at most their highest weight per subject.
- **FM-SOC-017** Each result lists score, supporters and every contributing
  record, plus counts of what was excluded and why.

## Command line

```sh
python3 tools/social_signals.py keygen --out ~/.forkmesh/social.seed
python3 tools/social_signals.py sign --seed-file ~/.forkmesh/social.seed star.json > signed.json
python3 tools/social_signals.py revoke --seed-file ~/.forkmesh/social.seed sha256:<signalId> --reason withdrawn
python3 tools/social_signals.py merge a.json b.json --audience public --output public.json
python3 tools/social_signals.py verify public.json
python3 tools/social_signals.py recommend public.json --viewer <key> [--disable]
python3 tools/social_signals.py gate
```

`gate` checks the committed vectors, runs both implementations against them and
a cross-implementation round trip (quality gate `social-signal-portability`).
Any rule change regenerates vectors (`vectors --write`), updates both
implementations and bumps the suite version.

## Failure and recovery

| Situation | Result |
| --- | --- |
| Tampered or malformed record in a bundle | Rejected with its code; the rest imports |
| Revocation before its record | `pending` until the record arrives |
| Revocation by another signer | `signer_mismatch`, no effect |
| Compromised key | Revoke with `compromised`, rotate identity ([portable identities](portable-identities.md)), re-sign |
| Lapsed mirror commitment | `expired`; sign a new one |
| Two services disagree | Compare state digests and merge both bundles |
