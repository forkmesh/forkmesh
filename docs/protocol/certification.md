# Protocol certification program

Program version: `1.0.0` · Protocol suite: `1.0.0-rc.1` · Part of the
[ForkMesh protocol suite](README.md) · Current badges:
[certification registry](certification-registry.md)

Independent clients, relays, indexes and providers prove compatibility by
running the published suite through a small adapter and publishing the report.
A badge says exactly one thing: *this implementation, at this version, passed
every case of this profile, against these vectors, within the last 90 days, on
these platforms*. Badges are derived from retained reports only.

| File | Role |
| --- | --- |
| `conformance/certification/v1/program.json` | Profiles, detection classes, performance budgets, freshness window, closure gate |
| `conformance/certification/v1/implementations.json` | Registered implementations, adapter commands, profiles, platforms, independence review |
| `conformance/certification/v1/evidence/<run>.json` | Retained reports, named by content address |
| `conformance/certification/v1/results.json` | Index of retained reports |
| `tools/protocol_certification.py` | Harness, attestation, badge derivation, generated outputs |

Generated, never hand-edited: [certification-registry.md](certification-registry.md),
`/docs/certification/badges.json`, `/docs/certification/evidence/<run>.json`.

## Profiles

| Profile | For | Vector groups | Must detect |
| --- | --- | --- | --- |
| `core` | clients, relays, indexes, providers | canonical JSON; identifiers; Ed25519 strictness, canonical strings, git-push messages, freshness; signed events; portable sessions | malformed signatures, divergence, downgrade, replay, data loss, incompatible versions |
| `collaboration-fold` | clients and relays that fold collaboration logs | canonical JSON; identifiers; signed events; log folding | malformed signatures, divergence, replay, data loss, incompatible versions |

Every case is positive (exact value) or negative (rejected). Detection classes:

| Class | A failing implementation … |
| --- | --- |
| `malformed-signature` | accepts a non-canonical scalar, small-order key, tampered payload or alternate signature spelling |
| `divergence` | derives a different identifier or folded state from the same input |
| `downgrade` | verifies under an older version or another scheme |
| `replay` | accepts a stale request, expired or future session, or re-applies a redelivered event |
| `data-loss` | drops a ref, rounds an integer or discards a pending or rejected record |
| `incompatible-version` | parses an unknown or future version instead of rejecting it |

The clean-room adapter's `--fault` modes break one property at a time, and the
tests require each fault to fail its class. Each case must answer within
2,000 ms and a profile within 120 s. A rejection with a different stable error
code still passes and is recorded as a code deviation (FM-SUITE-003).

## Adapter protocol

`forkmesh-conformance-adapter-v1` is newline-delimited JSON over the adapter's
stdin and stdout, run from the repository root.

1. Harness: `{"type":"hello","protocol":"forkmesh-conformance-adapter-v1","suite":"forkmesh-protocol","suiteVersion":"1.0.0-rc.1"}`.
2. Adapter: `{"type":"hello","protocol":"forkmesh-conformance-adapter-v1","implementation":{"name":"…","version":"…"},"suiteVersions":["1.0.0-rc.1"]}`.
   A protocol or suite-version mismatch records the run as `incompatible`.
3. Per case, harness: `{"type":"case","id":"<set>:<case>","operation":"…","input":{…}}`;
   adapter: `{"id":"…","valid":true,"value":…}`,
   `{"id":"…","valid":false,"error":"<code>"}` or `{"id":"…","unsupported":true}`.

A missing, late or mismatched response fails the case; an adapter that exits
or hangs fails every remaining case, and a report is still produced.

| Operation | Input | Value |
| --- | --- | --- |
| `canonical.parse` | `rawHex` | ignored |
| `canonical.encode` | `value` | canonical text |
| `identifier.repository` | `verifiedRoot` | repository identifier |
| `identifier.policy` | `policy` | policy identifier |
| `identifier.object` | `repository`, `family`, `creator`, `objectKey`, `references` | object identifier |
| `identifier.refs-state` | `advertisementHex` | `{"canonicalText","stateHash"}` |
| `identifier.migration` | `operation` and fields | identifier or hash |
| `signature.ed25519` | `publicKeyHex`, `messageHex`, `signatureHex` | ignored |
| `signature.canonical-string` | `messageText`, `publicKey`, `signature`, optional `scheme`/`fields`, `catalogV2Record` | ignored |
| `signature.git-push-message` | `owner`, `repo`, `method`, `ts`, `bodySha256` | message text |
| `request.freshness` | `ts`, `nowMs` | ignored |
| `event.verify` | `rawHex`, `policy`, `context` | event identifier |
| `session.parse` | `token`, `nowMs`, `routerPublicKey` | claim object |
| `log.fold` | `ndjson`, `policy` | `{"stateDigest","telemetry","rejectionReasons"}` |

Working adapters in Python and JavaScript are in
`conformance/certification/v1/adapters/`.

## Running and publishing

```sh
python3 tools/protocol_certification.py run --profile core \
  --implementation acme-relay --command "cargo run --release --bin fm-adapter" \
  --output artifacts/acme-relay-core.json
python3 tools/protocol_certification.py attest artifacts/*.json
python3 tools/protocol_certification.py validate
python3 tools/protocol_certification.py render --check
python3 tools/protocol_certification.py gate
```

`attest` retains passing and failing reports, and rejects reports that do not
match the registered implementation, profile, platform or adapter command, that
disagree with their own cases, trim the case list, or contain local paths or
loopback addresses. The newest report per implementation, profile and platform
wins. `gate` runs in the quality gates: it validates everything, checks
generated outputs and re-runs every in-repository adapter.

## Badges

A badge covers one implementation, one profile, and the platforms registered
for it. Each report binds the program version, a profile digest, the suite
version and every vector file's digest.

| Status | Meaning |
| --- | --- |
| `revoked` | Withdrawn by a recorded governance decision |
| `unverified` | No retained run on some listed platform |
| `failing` | The newest run on some platform failed |
| `expired` | Vectors, suite, program or profile changed, or the oldest run is older than 90 days |
| `certified` | Every listed platform passed against the current suite within 90 days |

The hosted SVG at `/docs/certification/badges/<implementation>/<profile>.svg`
checks expiry on every request, and
`/docs/certification/badges/<implementation>/<profile>` redirects to the
evidence. A badge may be cited as "passes the ForkMesh `<profile>` profile,
suite `<version>`" only while it is `certified` and only with the hosted image.

## Independence and governance

The program's closure gate needs a non-ForkMesh implementation with a current
`core` badge: source public outside ForkMesh repositories, written from the
documents and vectors without copying ForkMesh source, and reviewed by someone
who does not maintain it (recorded in the registry's `independence` block).
In-repository implementations are `independent: false`.

Decisions follow the [governance charter](../governance/README.md) and are
recorded in the [decision register](../governance/decision-register.md).
Registration is a pull request adding a registry entry and an attested report,
rerun by a reviewer who does not work on that implementation. Program changes
follow the suite's [change process](README.md#change-process): narrowing a
profile is a major version, adding cases is a minor version that expires
existing badges. Disputed vectors are fixed through the change process with no
per-implementation exemptions; refusals and revocations can be appealed within
30 days. A badge is revoked when its evidence cannot be reproduced, its
independence statement is false, or it is used outside its scope.

New cases, especially adversarial and version-skew inputs, are added to
`conformance/protocol/v1/generate_vectors.py` and listed under their class in
`program.json`.
