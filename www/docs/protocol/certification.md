# Protocol certification program

Program version: `1.0.0` · Protocol suite: `1.0.0-rc.1` · Part of the
[ForkMesh protocol suite](README.md) · Current badges:
[certification registry](certification-registry.md)

Independent clients, relays, indexes, and providers prove compatibility by
running the published suite through a small adapter and publishing the
report. A badge says exactly one thing: *this implementation, at this version,
passed every case of this profile, against these vectors, within the last 90
days, on these platforms*. Nothing is certified by assertion; badges are
derived from retained reports only.

Sources, all versioned together:

| File | Role |
| --- | --- |
| `conformance/certification/v1/program.json` | Profiles, detection classes, performance budgets, freshness window, closure gate |
| `conformance/certification/v1/implementations.json` | Registered implementations, adapter commands, claimed profiles and platforms, independence review |
| `conformance/certification/v1/evidence/<run>.json` | Retained reports, named by content address |
| `conformance/certification/v1/results.json` | Index of retained reports, recomputed from evidence on `validate` |
| `tools/protocol_certification.py` | Harness, attestation, badge derivation, and generated outputs |

Generated outputs, never edited by hand: [certification-registry.md](certification-registry.md),
`/docs/certification/badges.json`, and `/docs/certification/evidence/<run>.json`.

## Profiles

A profile is a list of vector groups plus the defect classes the profile must
detect. The program refuses a profile that names a class with no case in its
groups.

| Profile | For | Vector groups | Must detect |
| --- | --- | --- | --- |
| `core` | clients, relays, indexes, providers | canonical JSON parse and encode; repository, policy, object, refs-state, and migration identifiers; Ed25519 strictness, canonical strings, git-push messages, freshness; signed events; portable sessions | malformed signatures, divergence, downgrade, replay, data loss, incompatible versions |
| `collaboration-fold` | clients and relays that fold collaboration logs | canonical JSON; repository, policy, and object identifiers; signed events; log folding | malformed signatures, divergence, replay, data loss, incompatible versions |

Every case is **positive** (valid input, exact value) or **negative**
(rejected input). Detection classes group the adversarial and version-skew
cases that matter most:

| Class | A failing implementation … | Example cases |
| --- | --- | --- |
| `malformed-signature` | accepts a non-canonical scalar, a small-order key, a tampered payload, or an alternate signature spelling | `signatures:ed25519-order-2-public-key`, `events:event-tampered-payload` |
| `divergence` | derives a different identifier or folded state from the same input | `identifiers:repository-decomposed-root-normalizes`, `fold:concurrent-scalar-conflict` |
| `downgrade` | verifies a signature under an older version or another scheme | `signatures:cross-version-view-signature` |
| `replay` | accepts a stale request, an expired or future session, or re-applies a redelivered event | `signatures:stale-past`, `sessions:session-expired` |
| `data-loss` | drops a ref, rounds an integer, or silently discards a pending or rejected record | `identifiers:refs-bytewise-refname-order`, `fold:missing-parent-pending` |
| `incompatible-version` | parses an unknown or future version identifier instead of rejecting it | `events:event-future-schema`, `sessions:session-future-token-prefix` |

The suite's own tests prove each class is detected. The clean-room adapter
accepts `--fault` modes that break one property at a time (lenient Ed25519,
skipped normalization, version downgrade on failure, ignored expiry, dropped
refs, accepted future versions), and `app/tests/test_protocol_certification.py`
requires every fault to fail its class. A faulted run records a different
adapter command, so it can never be attested.

**Performance.** Each case must answer within 2,000 ms and a profile within
120 s on the runner. A slower run fails; the report records the slowest case.

**Error codes.** Validity and values are normative. When a rejection uses a
different stable code than the vector lists, the case still passes and the
report records `expectedError`/`actualError` as a code deviation (FM-SUITE-003).

## Adapter protocol

`forkmesh-conformance-adapter-v1` is newline-delimited JSON over the
adapter's standard input and output, one request and one response per line,
so any language can take part without linking ForkMesh code. The harness runs
the adapter with the repository root as its working directory; the vectors
arrive inside the requests.

1. The harness sends
   `{"type":"hello","protocol":"forkmesh-conformance-adapter-v1","suite":"forkmesh-protocol","suiteVersion":"1.0.0-rc.1"}`.
2. The adapter answers
   `{"type":"hello","protocol":"forkmesh-conformance-adapter-v1","implementation":{"name":"…","version":"…"},"suiteVersions":["1.0.0-rc.1"]}`.
   If the protocol differs or `suiteVersions` lacks the harness's suite
   version, the run is recorded as `incompatible` and every case fails. That
   is how version skew between an implementation and the suite shows up.
3. For each case the harness sends
   `{"type":"case","id":"<set>:<case>","operation":"…","input":{…}}` and the
   adapter answers `{"id":"<same>","valid":true,"value":…}`,
   `{"id":"<same>","valid":false,"error":"<code>"}`, or
   `{"id":"<same>","unsupported":true}`.

A missing, late, or mismatched response fails that case. An adapter that exits
or hangs fails the case in flight and every later case, and the run still
produces a report.

| Operation | Input | Valid response value |
| --- | --- | --- |
| `canonical.parse` | `rawHex` | ignored |
| `canonical.encode` | `value` | canonical text |
| `identifier.repository` | `verifiedRoot` | repository identifier |
| `identifier.policy` | `policy` | policy identifier |
| `identifier.object` | `repository`, `family`, `creator`, `objectKey`, `references` | object identifier |
| `identifier.refs-state` | `advertisementHex` | `{"canonicalText","stateHash"}` |
| `identifier.migration` | `operation` plus its fields | identifier or hash string |
| `signature.ed25519` | `publicKeyHex`, `messageHex`, `signatureHex` | ignored (validity is the verdict) |
| `signature.canonical-string` | `messageText`, `publicKey`, `signature`, optional `scheme`/`fields`, `catalogV2Record` | ignored |
| `signature.git-push-message` | `owner`, `repo`, `method`, `ts`, `bodySha256` | message text |
| `request.freshness` | `ts`, `nowMs` | ignored (valid means fresh) |
| `event.verify` | `rawHex`, `policy`, `context` (ancestor envelopes as hex) | event identifier |
| `session.parse` | `token`, `nowMs`, `routerPublicKey` | claim object |
| `log.fold` | `ndjson`, `policy` | `{"stateDigest","telemetry","rejectionReasons"}` |

Each vector file's `procedure` and the protocol documents define what an
operation must do. The adapters in `conformance/certification/v1/adapters/`
are working examples in Python and JavaScript.

## Running and publishing

```sh
# Any implementation, registered or not
python3 tools/protocol_certification.py run --profile core \
  --implementation acme-relay --command "cargo run --release --bin fm-adapter" \
  --output artifacts/acme-relay-core.json

# A registered implementation uses its registered adapter command
python3 tools/protocol_certification.py run --profile core \
  --implementation forkmesh-clean-room-python

python3 tools/protocol_certification.py attest artifacts/*.json   # retain + regenerate
python3 tools/protocol_certification.py validate
python3 tools/protocol_certification.py render --check
python3 tools/protocol_certification.py status --as-of 2026-12-31
python3 tools/protocol_certification.py gate
```

`attest` accepts passing and failing reports. It rejects a report that does
not match its registered implementation, profile, platform, or adapter
command. It also rejects a summary or detection table that disagrees with its
cases, a case list trimmed from the current profile, and any local path, file
URL, or loopback address. The newest report for each implementation, profile,
and platform replaces older ones, and an older report never replaces a newer
one. Editing evidence or the results index fails `validate`.

`gate` is part of the `core` and `release` quality-gate profiles. It validates
everything, checks the generated outputs, and re-runs every in-repository
adapter on the current machine, so it never fails just because the date
changed. It also prints the closure gate status.

## Badges

**Scope.** One implementation, one profile. A badge covers only the vector
groups of that profile and the platforms the registry lists for the
implementation. It says nothing about features outside the profile, hosted
availability, security audits, or product quality.

**Version.** Each report binds the program version, a digest of the profile
definition (groups, detection cases, and budgets), the suite version, and a
digest of every vector file. The implementation's own name and version come
from the adapter handshake.

**Statuses**, derived in this order:

| Status | Meaning |
| --- | --- |
| `revoked` | Withdrawn by a governance decision recorded in the registry |
| `unverified` | No retained run on at least one listed platform |
| `failing` | The newest run on some platform failed |
| `expired` | The suite vectors, suite version, program version, or profile changed after testing, or the oldest platform run is older than 90 days |
| `certified` | Every listed platform passed, bound to the current suite and profile, within 90 days; `expiresAt` is the last valid day |

**Expiry and retesting.** Expiry is automatic in two places. First, a vector,
profile, or suite-version change changes the badge data, so
`render --check` fails in CI until the registry is regenerated with those
badges marked `expired`. Second, the hosted SVG at
`/docs/certification/badges/<implementation>/<profile>.svg` compares
`expiresAt` with the time of each request, so an embedded badge shows
`expired` the day after the window closes, with no redeploy. To renew,
re-run the profile and attest the report. The same rules apply to a retest:
there is no grace period, and a failing retest replaces a passing one.

**Links.** `/docs/certification/badges/<implementation>/<profile>` redirects
to the raw evidence report of the badge's current run. The registry page
lists the reproduction command for every badge. Embed a badge as:

```markdown
[![ForkMesh core certification](https://forkmesh.com/docs/certification/badges/<implementation>/core.svg)](https://forkmesh.com/docs/certification/badges/<implementation>/core)
```

## Independence

The issue this program serves closes only when a **non-ForkMesh**
implementation holds a current `core` badge (`closureGate` in the program). An
implementation counts as independent when all of the following hold:

- its source is published at a public `https://` URL outside ForkMesh-controlled repositories;
- it was written by people who are not ForkMesh maintainers, from the protocol
  documents and vectors, without copying ForkMesh source;
- a reviewer who does not maintain it records the review in the registry's
  `independence` block (`reviewer`, `date`, `statement`).

The two registered in-repository implementations are marked `independent:
false` and do not count, even though the Python verifier is clean-room.

## Governance

Certification decisions belong to the protocol council under the
[governance charter](../governance/README.md). Appeals go to its appeals panel
on the timeline below. Until independent seats are filled, the stewards below
act under the charter, and their decisions are recorded in the
[decision register](../governance/decision-register.md).

- **Stewards.** Two ForkMesh maintainers act as certification stewards for each
  program minor version. At least one of them must not work on the
  implementation under review. Stewards review registry entries, independence
  statements, and appeals.
- **Registration.** An implementer opens a pull request that adds a registry
  entry and one attested report. The entry includes a runnable adapter command,
  so a reviewer can reproduce the run with the command on the registry page.
  A steward reruns the report on at least one listed platform before merging.
- **Program changes.** They follow the protocol suite's
  [change process](README.md#change-process). A change that removes or narrows
  what a profile accepts is a major program version. Adding cases or classes to
  a profile is a minor version and expires existing badges, which is intended:
  they must be retested against the new cases. Wording changes are patch
  versions and leave badges valid.
- **Conflicts of interest.** A steward who maintains, or is paid by the
  maintainer of, an implementation does not review its registration, appeal,
  or revocation.
- **Transparency.** Every decision lands as a commit to the registry. A
  revocation carries its date and reason, and the registry page shows both.

## Appeals and revocation

- **Disputing a vector.** An implementer who believes a vector or expectation
  is wrong opens an issue naming the case identifiers and the requirement
  text. If the stewards agree, the vector is fixed through the change process,
  and every badge for the affected profile expires and is retested. No
  implementation receives an exemption.
- **Appealing a decision.** A registration refusal or revocation can be
  appealed within 30 days. Two stewards who took no part in the original
  decision hear it and publish their written decision within 30 days.
- **Revocation.** A badge is revoked when its evidence cannot be reproduced
  from the registered adapter, the independence statement proves false, or the
  badge is used outside its scope. Revocation adds an entry to the
  implementation's `revocations` list. It is lifted by a steward decision and
  a fresh attested run.

## Trademark use

"ForkMesh" and the badge artwork identify the project. A certified
implementation may:

- embed the hosted badge, linked as shown above;
- say "passes the ForkMesh `<profile>` profile, suite `<version>`" while the
  badge is `certified`.

It may not:

- say "ForkMesh certified" without naming the profile;
- use a copied or self-hosted badge image, since only the hosted badge expires;
- use the ForkMesh name as part of the product name;
- imply endorsement, hosting, or security review by ForkMesh.

After a badge expires or is revoked, the claim must be removed or qualified
with the date it last held.

## Interoperability events

The stewards announce public interoperability events on the project chat and
the protocol issue tracker at least four weeks ahead. Each event pins a program
and suite version.

- Participants bring an adapter. During the event every participant runs every
  profile, and the stewards attest the reports the same day, passing or failing.
- Cross-implementation pairs also exchange live NDJSON logs and compare folded
  state digests. Any mismatch is filed as an issue against the vector or the
  implementation.
- Event notes list the participants, reports, failures found, and each
  failure's time to retest. They are published as a note linked from the
  registry page.

## Contributing tests

Independent implementers are encouraged to contribute cases, especially
adversarial and version-skew inputs that caught a real bug.

1. Open an issue naming the requirement identifiers the case exercises and the
   detection class it belongs to.
2. Add the expectation to `conformance/protocol/v1/generate_vectors.py`,
   regenerate the vectors, and list the new case under its class in
   `program.json`.
3. Include one run of the reference implementation that passes and, where
   possible, a fault or an implementation that the case catches.

A contributed case is reviewed like any vector change. Once merged, it expires
the badges of every profile that includes it, so all implementations are
retested against it.
