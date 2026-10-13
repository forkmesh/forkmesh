# Availability proof and repository risk

The Mirrors tab on every repository answers three questions: is this repository
actually preserved, how independently, and what is still at risk. The answer is
the `forkmesh-availability-proof-v1` report. The Worker computes it from the
same mirrors payload it publishes, so anyone can recompute it and get the same
result.

## Evidence

The report reads only timestamped evidence that already appears in
`GET /api/repo/<owner>/<repo>/mirrors`:

| Evidence | Fields | Source |
| --- | --- | --- |
| Signed endpoint receipt | `endpointFresh`, `endpointHealthy`, `endpointIntegrity`, `checkedAt` | The router's challenge of the node's signed endpoint registration |
| Direct clone probe | `directVerified`, `directCheckedAt`, `directRefsSha256` | A Smart HTTP probe of the registered mirror URL against its signed refs |
| Integrity gate verdict | `integrity`, `behind`, `status`, `lastSeen` | The clone integrity gate and presence |
| Operator and region | `ownerUser` or `owner`, `region` | The signed catalog record and endpoint registration |
| Coverage | `branchCount`, `artifactCount`, `operations` | The node's signed catalog record |

`generatedAt` is the observation time. Nothing else feeds the report.

**Freshness objective.** A signed endpoint receipt counts for 10 minutes, the
same window the router trusts it for. So dashboard values are never older than
10 minutes, and the API's edge cache adds at most 3 seconds. Direct clone probes
run when someone asks for one, so a passing probe counts for 24 hours.

## Copy classes

Each copy gets one class. When more than one could apply, the earlier row wins:

| Class | Meaning |
| --- | --- |
| `failing` | The integrity gate rejected the copy, the node is blocked for abuse, or its latest clone probe (within 24 hours) failed. |
| `verified` | The copy is online, current, and has integrity `ok`. It also has a signed endpoint receipt within the freshness objective, or a clone probe that passed within 24 hours. |
| `declared` | Only a catalog record exists; nothing has ever observed the copy. |
| `stale` | The copy was observed before, but is now offline, behind, or has no evidence within the freshness objective. |
| `observed` | The copy is online and was seen within the freshness objective, but has no signed receipt or passing probe yet. A copy that declares a `replicaScope` other than `full` (a filtered, metadata-only, or deferred-object replica; see [Offline and low-bandwidth sync](offline-sync.md)) is `observed` with `limited_replica` even when its evidence is fresh. |

Each copy also lists closed reason codes: `integrity_rejected`, `abuse_blocked`,
`probe_failed`, `offline`, `behind`, `evidence_expired`, `awaiting_receipt`,
`never_observed`, `limited_replica`.

## Trusted quorum

A repository has a quorum when at least **2 distinct operators** hold the same
verified state. The count is of operators, not nodes, so one operator running
several machines can never make a quorum alone.

If clone probes on verified copies return different refs digests, only copies
matching the digest backed by the most operators count. Copies with no digest
are left out, and `state_disagreement` is reported. A copy that is failing,
stale, observed, or only declared never counts, so a limited replica can never
make or inflate a quorum.

| Proof | Meaning |
| --- | --- |
| `quorum` | Independent operators hold the same verified copy. |
| `partial` | At least one copy is verified, but not by enough independent operators. |
| `none` | No copy has fresh signed evidence. |

A source-host outage raises risk (`source_offline`) but does not remove a quorum
that mirrors already prove. A mirror outage removes that mirror from the quorum
as soon as its receipt expires.

## Risk score

The score is the sum of the weights of every risk code that applies, capped at
100. Levels: `critical` from 60, `high` from 30, `moderate` from 10, otherwise
`low`. Risks are listed heaviest first.

## Risk codes

### `no_verified_copy`

Weight 50, critical. No copy has a fresh signed endpoint receipt or a passing
clone probe. **Remediation:** ask a node to mirror the repository. Repository
owners can do this from the Mirrors tab ("Ask a node to mirror this repo").
Anyone can also [run a mirror node](operations/) and mirror it themselves.

### `quorum_not_met`

Weight 25, high. Fewer than 2 independent operators hold the same verified
state. **Remediation:** ask a node run by a different operator to mirror the
repository.

### `state_disagreement`

Weight 20, high. Clone probes on verified copies returned different refs
digests. **Remediation:** run the direct URL check again on each mirror
(**check** in the Endpoint column). A mirror that stays on a different digest
should re-sync from the source.

### `single_operator`

Weight 10, medium. Two or more copies are verified, but one operator runs all of
them. **Remediation:** ask a different operator to mirror the repository.

### `failing_copy`

Weight 10, medium. At least one copy was rejected by the integrity gate, blocked
for abuse, or failed its latest clone probe. **Remediation:** the node's
operator re-syncs the mirror from the source and runs the direct URL check again.

### `source_offline`

Weight 10, medium. The working-copy source is offline, so mirrors that fall
behind cannot re-sync. **Remediation:** start the ForkMesh desktop or headless
node that holds the working copy.

### `no_direct_clone_url`

Weight 10, medium. No verified copy has passed a direct clone probe in the last
24 hours, so clones depend on the ForkMesh router. **Remediation:** the mirror
operator registers a direct mirror URL, and anyone runs its direct URL check.

### `partial_ref_coverage`

Weight 10, medium. Fewer verified copies carry every branch than the quorum
target requires. The reference is the highest branch count among the source and
the verified copies. **Remediation:** re-sync the mirrors with every branch.

### `stale_copy`

Weight 5, low. At least one copy is offline, behind, or has expired evidence.
**Remediation:** bring the mirror back online or let it catch up; it becomes
verified again at its next signed receipt.

### `partial_artifact_coverage`

Weight 5, low. Fewer verified copies carry every release artifact than the
quorum target requires. **Remediation:** re-sync the release artifacts to the
mirrors.

### `single_region`

Weight 5, low. Two or more copies are verified, every one declared a region, and
all declared the same region. **Remediation:** ask a node in another region to
mirror the repository.

### `declared_only_copy`

Weight 3, low. A node published a catalog record, but nothing has ever observed
it serving. **Remediation:** check that the node's endpoint is registered and
reachable.

## Privacy

Operator accounts are never shown. Copies run by the same operator share an
opaque operator number (1, 2, …), so you can see how independent the copies are
without learning who runs them. Endpoint addresses, mirror URLs, and free-form
probe reasons from the evidence are never shown either. Node names and
operator-declared regions already appear on the public mirrors payload. The
shared fixture plants private strings in its evidence, and the test suite fails
if any of them appears in a report.

## Where it appears

| Client | Where | Implementation |
| --- | --- | --- |
| API | `availability` on `GET /api/repo/<owner>/<repo>/mirrors` | `app/src/availability_proof.py` |
| Web | Availability proof panel on the repository Mirrors tab, with risk remediation and per-copy evidence | `app/public/dashboard/js/07-repo-compose-branch.js` |
| CLI | `tools/forkmesh_availability_proof.py` | reuses the API implementation |

## Reproduce a report

```sh
curl -s https://app.forkmesh.com/api/repo/forkmesh/forkmesh/mirrors > mirrors.json
tools/forkmesh_availability_proof.py verify mirrors.json         # recompute and compare
tools/forkmesh_availability_proof.py explain mirrors.json        # plain-language summary
tools/forkmesh_availability_proof.py explain --json mirrors.json # machine-readable report
tools/forkmesh_availability_proof.py fixture                     # verify the shared fixture
```

`explain` exits 0 when the quorum is proven and 3 when it is not. `verify` exits
0 when the published `availability` matches the recomputed report and 1 when it
does not. Both exit 2 when the input cannot be read.

## Conformance

- `conformance/availability-proof/v1/model.json` sets out the classes, reasons, proof states, levels, risk weights, and policy.
- `conformance/availability-proof/v1/fixture.json` holds a case for every risk code and class. It also holds sequences for a mirror outage, a source-host outage, and an integrity failure; each one ends recovered.
- `conformance/availability-proof/v1/generate_fixture.py` regenerates the expected output. Every entry also records a hand-written proof, risk list, and class list, so a reference change that moves any of them fails loudly. Run it with `--check`.

Run `python -m pytest app/tests/test_availability_proof.py` to check the
reference, CLI, API wiring, and web rendering against the fixture.

## Rollout and rollback

The `availability` field is additive, and older clients ignore it. Rolling back
the Worker removes the field, and the Mirrors tab hides the panel; the mirror
table and the `syncStatus` line are unaffected. To change the model, edit
`model.json` and the reference implementation, then run `generate_fixture.py`
and update this document until the suite passes.
