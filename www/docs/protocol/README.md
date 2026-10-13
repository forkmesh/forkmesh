# ForkMesh protocol suite

Suite version: `1.0.0-rc.1` · Status: **candidate** — not yet stable; see
[Stable release gate](#stable-release-gate).

This suite is the normative description of ForkMesh wire behavior. An
independent implementation should be able to derive every identifier, produce
and verify every signature, and fold collaboration state from these documents
and their vectors without reading ForkMesh source. The hosted
[protocol reference](https://forkmesh.com/docs/protocol/) remains the
narrative guide to HTTP routes and deployment; where the two disagree, this
suite wins and the disagreement is a bug.

| Document | Covers |
| --- | --- |
| [Encoding and identifiers](encoding-and-identifiers.md) | Canonical JSON, base64url, canonical strings, repository/policy/object/event/refs/migration identifiers |
| [Signatures and authentication](signatures-and-authentication.md) | Strict Ed25519, domain separation, envelope verification order, request tokens, push authorization, portable sessions, catalog publication |
| [Relay, synchronization, and migration](relay-sync-and-migration.md) | Integrity-gated clone routing, NDJSON log exchange, deterministic folding, migration receipts, rollback |
| [Version negotiation](version-negotiation.md) | Version identifiers, rejection of unknown versions, downgrade resistance, rollout order |
| [Conflict-safe collaboration](../design/conflict-safe-collaboration.md) | Collaboration event families, fold rules, moderation lineage |
| [Threat model](threat-model.md) | Trust boundaries, attackers, threat register, release blockers |
| [Certification program](certification.md) | Profiles, adapter protocol, detection classes, expiring badges, independence, governance, appeals, trademark use; current badges in the [registry](certification-registry.md) |

## Conformance language

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are to be interpreted
as described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear in
a numbered requirement. A requirement is written as a bold identifier followed
by its level, in the form `**FM-AREA-NNN** MUST`. Identifiers are never reused;
a withdrawn requirement keeps its number with the text "Withdrawn".

Everything outside a numbered requirement is informative.

## Suite requirements

- **FM-SUITE-001** MUST — The documents, schemas, vectors, and threat register
  of one suite version are released together. The manifest
  (`conformance/protocol/v1/manifest.json`) records the SHA-256 of each file
  and maps every requirement to its evidence. A verifier MUST reject the suite
  when any digest, version string, requirement identifier, requirement level,
  vector citation, or threat severity/status disagrees.
- **FM-SUITE-002** MUST — An implementation that claims a vector set passes
  every case in it: valid cases produce exactly the listed values, and invalid
  cases are rejected.
- **FM-SUITE-003** SHOULD — A rejection reports the listed error code.
  Validity is normative; codes are the stable vocabulary for audit telemetry
  and interop debugging. An implementation's known code deviations are
  recorded in the manifest (`codeDeviations`) and tested for exact equality,
  so a new deviation fails like any other inconsistency.

## Vectors

Vector files live in `conformance/protocol/v1/vectors/`:

| File | Content | Implementations passing it |
| --- | --- | --- |
| `canonical-json.json` | Parse and encode cases, including every rejection class | reference Python, WebCrypto JavaScript, clean-room Python |
| `identifiers.json` | Repository, policy, object, refs-state, and migration identifiers | reference Python, clean-room Python; JavaScript for repository/policy/object |
| `signatures.json` | RFC 8032 and strictness cases, canonical-string schemes, push-message normalization, freshness windows | reference Python, clean-room Python |
| `events.json` | Signed collaboration envelopes and each verification failure | reference Python, WebCrypto JavaScript, clean-room Python |
| `fold.json` | Folded NDJSON logs with state digests and telemetry | reference Python, WebCrypto JavaScript |
| `sessions.json` | Portable session tokens | reference Python, clean-room Python |

Byte-exact inputs are given as `rawHex`; `rawText` is informative. Keys and
signatures come from deterministic test identities and MUST NOT be used
outside tests.

Run the suite:

```bash
python3 conformance/protocol/v1/verify_suite.py          # clean-room verifier + consistency
python3 conformance/protocol/v1/generate_vectors.py --check
python3 -m pytest -q app/tests/test_protocol_suite.py
```

`verify_suite.py` uses only the Python standard library and was written from
these documents. It does not yet fold collaboration logs, so `fold.json` is
evidenced by the two reference implementations only.

## Compatibility policy

Wire version identifiers and the suite version are separate:

- A **wire version identifier** (for example `forkmesh-collaboration-event-v1`
  or the `v1.` session-token prefix) names frozen bytes. Its meaning never
  changes; see [Version negotiation](version-negotiation.md).
- The **suite version** uses semantic versioning. A patch release fixes
  wording or adds vectors for existing behavior. A minor release adds
  requirements, vectors, or wire identifiers without changing what an existing
  conforming implementation accepts. A major release removes acceptance of a
  wire identifier; it is announced one minor release in advance and ships
  with a migration path.

## Change process

Changes are decided under the [governance charter](../governance/README.md).
Steps 1–5 are the technical part of a proposal record (`FMP-NNNN`). Review
windows, quorum, dissent, and appeals follow the charter.

1. Open an issue that names the requirement identifiers and threats affected.
2. Change the normative text, the reference implementations, and the
   generator expectations in one change. The generator compares every vector
   with an expectation written in `generate_vectors.py`, so a behavior change
   that is not also a documented expectation fails generation.
3. Update `threats.json` and the [threat model](threat-model.md) table
   together when a control or residual risk changes.
4. Regenerate: `python3 conformance/protocol/v1/generate_vectors.py`. This
   rewrites vectors and manifest digests; `--check` fails CI on anything stale.
5. Review requires one maintainer for informative or vector-only changes and a
   security reviewer for any requirement, wire identifier, or threat status
   change.

## Stable release gate

The suite may be marked `stable` only when no `critical` or `high` threat in
the register is `open`. Each must be `mitigated` by a numbered requirement or
`accepted` with a reviewer, date, and rationale. The verifier enforces this and
reports the current blockers as `stableReleaseBlockers`.

Before the umbrella epic can close, more evidence is needed:

- a clean-room collaboration fold;
- a second-language clean-room verifier;
- stock-Git clone interoperability against an independently written relay;
- the platform matrix.

The [certification program](certification.md) packages these vectors into
profiles any implementation can run through a language-neutral adapter. Its
closure gate is an independent implementation holding a current `core`
badge.

The [Git conformance suite](../operations/git-conformance.md) already covers
stock clients.
