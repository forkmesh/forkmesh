# Availability proof and repository risk

The `forkmesh-availability-proof-v1` report says whether a repository is
preserved, how independently, and what is at risk. It is computed from the
public mirrors payload, so anyone can recompute it.

## Evidence

Inputs come only from `GET /api/repo/<owner>/<repo>/mirrors`: signed endpoint
receipts (`endpointFresh`, `endpointHealthy`, `endpointIntegrity`,
`checkedAt`), direct clone probes (`directVerified`, `directCheckedAt`,
`directRefsSha256`), integrity gate verdicts (`integrity`, `behind`, `status`,
`lastSeen`), operator and region, and coverage (`branchCount`,
`artifactCount`). A receipt counts for 10 minutes; a passing probe for 24 hours.

## Copy classes and quorum

Each copy gets the first matching class: `failing`, `verified`, `declared`,
`stale`, `observed`. Reason codes: `integrity_rejected`, `abuse_blocked`,
`probe_failed`, `offline`, `behind`, `evidence_expired`, `awaiting_receipt`,
`never_observed`, `limited_replica`. A copy whose `replicaScope` is not `full`
is `observed` with `limited_replica` (see [Offline and low-bandwidth
sync](offline-sync.md)) and never counts toward quorum.

Proof is `quorum` when at least 2 distinct operators hold the same verified
state, `partial` with any verified copy, otherwise `none`. When probe digests
disagree, only the digest backed by the most operators counts. The risk score
is the sum of applicable weights, capped at 100: `critical` from 60, `high`
from 30, `moderate` from 10, else `low`.

## Risk codes

### `no_verified_copy`
Weight 50, critical. No copy has fresh signed evidence. **Remediation:** ask a node to mirror the repository, or [run a mirror node](operations/).

### `quorum_not_met`
Weight 25, high. Fewer than 2 operators hold the same verified state. **Remediation:** ask another operator to mirror it.

### `state_disagreement`
Weight 20, high. Verified copies returned different refs digests. **Remediation:** rerun the direct URL check; a divergent mirror re-syncs.

### `single_operator`
Weight 10, medium. All verified copies share one operator. **Remediation:** ask a different operator to mirror it.

### `failing_copy`
Weight 10, medium. A copy failed integrity, abuse checks or its clone probe. **Remediation:** re-sync the mirror and recheck it.

### `source_offline`
Weight 10, medium. The working-copy source is offline. **Remediation:** start the node that holds the working copy.

### `no_direct_clone_url`
Weight 10, medium. No verified copy passed a direct clone probe in 24 hours. **Remediation:** register a direct mirror URL and check it.

### `partial_ref_coverage`
Weight 10, medium. Too few verified copies carry every branch. **Remediation:** re-sync every branch.

### `stale_copy`
Weight 5, low. A copy is offline, behind or expired. **Remediation:** bring it back online.

### `partial_artifact_coverage`
Weight 5, low. Too few verified copies carry every release artifact. **Remediation:** re-sync release artifacts.

### `single_region`
Weight 5, low. All verified copies declare the same region. **Remediation:** ask a node in another region to mirror it.

### `declared_only_copy`
Weight 3, low. A catalog record exists but nothing has observed it. **Remediation:** check the endpoint is registered and reachable.

## Privacy and tooling

Operators appear only as opaque numbers; endpoint addresses and probe reasons
are never shown. The report is the `availability` field of the mirrors API
(`app/src/availability_proof.py`). `tools/forkmesh_availability_proof.py
verify|explain|fixture` recomputes it. The model and fixture live in
`conformance/availability-proof/v1/`.
