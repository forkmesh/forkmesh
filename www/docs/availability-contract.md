# Repository availability contracts

A repository availability contract states how many verified copies, operators,
and regions hold a repository, which transports and refs they serve, how often
that is checked, how fast incidents are answered, and what is credited on a
miss. Each period yields a `forkmesh-availability-contract-report-v1` report
computed only from an evidence bundle, so anyone can recompute and verify it.
Managed checkout stays disabled until the [business model](business-model.md)
gates pass.

## Evidence bundle

Schema `forkmesh-availability-contract-evidence-v1`: `contract` (terms,
remedies, exclusions, fee), `observations` (`/api/repo/<owner>/<repo>/mirrors`
payloads, scored by the [availability proof](availability-proof.md)), `drills`
(restore drill records), `responses` (incident acknowledgements), and
`capacity` (`community` or `contracted` per node).

## Contract terms

| Term | Meaning |
| --- | --- |
| `copies` | Verified copies required in every slot. |
| `operators` | Distinct operators holding the same verified state (at least 2). |
| `regions` | Distinct regions among verified copies. |
| `uptimeBps` | Share of slots that must meet every check. |
| `verificationIntervalMs` | Slot length; at most 10,000 slots per period. |
| `transports` | `router`, `directHttps`, `smartHttp`, each needing a verified copy. |
| `refCoverage` | Copies must carry every branch. |
| `artifactCoverage` | Copies must carry every release artifact. |
| `recoveryTimeObjectiveMs` | Longest incident and slowest restore drill allowed. |
| `drillIntervalMs` | Oldest allowed drill at period end. |
| `incidentResponseMs` | Longest time to acknowledge an incident. |
| `failureTolerance` | Operators, and separately regions, that may fail at once (0 to 2). |
| `maxExcludedBps` | Share of slots exclusions may remove. |

Malformed terms stop evaluation with a closed error code; there is no partial
report.

## Verification slots

A slot misses on any failed check; a slot without evidence is a miss.

| Check | The slot misses when |
| --- | --- |
| `unverified` | No observation. |
| `quorum_below_contract` | Too few operators held the same verified state. |
| `copies_below_contract` | Too few verified copies. |
| `regions_below_contract` | Too few regions. |
| `transport_missing` | A contracted transport had no verified copy. |
| `ref_coverage_incomplete` | Too few copies carried every branch. |
| `artifact_coverage_incomplete` | Too few copies carried every artifact. |

Availability is `met slots * 10000 // counted slots`, rounding in the
customer's favor.

## Exclusions

Kinds `scheduled-maintenance`, `customer-requested`, and `legal-hold` remove
whole slots when announced before they start and within `maxExcludedBps`.
Rejections: `unknown_kind`, `announced_late`, `over_budget`.

## Incidents and drills

Consecutive missed slots form one incident, measured in whole slots. Response
states: `ok`, `late`, `missing`, `pending`. The latest restore drill per node
decides its state: `ok`, `drill_missing`, `drill_stale`,
`drill_beyond_objective`. See
[node backup and recovery](operations/node-backup-and-recovery.md).

## SLOs and remedies

SLOs: `availability`, `recovery`, `incident-response`, `restore-drills`. The
period is `met` only when all four are met. Every miss is listed, even when its
SLO holds:

| Miss | SLO |
| --- | --- |
| `availability_window` | `availability` |
| `recovery_objective_exceeded` | `recovery` |
| `response_late` | `incident-response` |
| `response_missing` | `incident-response` |
| `drill_missing` | `restore-drills` |
| `drill_stale` | `restore-drills` |
| `drill_beyond_objective` | `restore-drills` |

Credits come from `remedies`: one `availabilityTiers` credit for missed
availability, fixed credits per recovery, response, and drill miss, capped at
`maxCreditBps`. Money is integer micro-USD.

## Demonstrated capacity and admission

A node is demonstrated when it verified in at least `uptimeBps` of slots and
its latest drill is `ok`. Undeclared capacity counts as community. A contract
is admissible only when demonstrated capacity meets the terms; otherwise
`admission.reasons` lists `no_evidence`, `copies_not_demonstrated`,
`operators_not_demonstrated`, `regions_not_demonstrated`, or
`failure_simulation_failed`.

The failure simulation removes every combination of `failureTolerance`
operators, then regions, from the latest observation and recomputes the
quorum; a surviving node's drill must fit the recovery objective.

## Correlated-risk disclosures

Disclosures do not change the outcome.

### `community_capacity_relied_on`

Contracted capacity alone does not reach the operator quorum.

### `contracted_single_operator`

One operator runs every contracted copy.

### `operator_concentration`

One operator runs more than half of the demonstrated copies.

### `region_concentration`

More than half of the demonstrated copies share one region or none.

### `no_failure_headroom`

Losing any one operator breaks the quorum.

### `capacity_not_demonstrated`

A declared contracted node lacks continuous probes or a qualifying drill.

## Privacy

Reports show opaque operator numbers only, never accounts, endpoints, mirror
URLs, drill paths, or keys.

## Signed reports and tooling

A `forkmesh-availability-contract-signed-report-v1` document holds `report`,
`reportHash` (`sha256:` over canonical JSON), and an Ed25519 signature over
`forkmesh-availability-contract-report-v1\n<reportHash>`.

```sh
tools/forkmesh_availability_contract.py explain bundle.json
tools/forkmesh_availability_contract.py sign bundle.json --private-key seed.txt > signed.json
tools/forkmesh_availability_contract.py verify bundle.json signed.json --public-key <key>
tools/forkmesh_availability_contract.py fixture
```

`verify` recomputes the report and rejects edits to the report or evidence.
Exit codes: `explain` 0 met, 3 otherwise; `verify`/`fixture` 0 match, 1
mismatch; 2 for bad input. Model and fixture live in
`conformance/availability-contract/v1/`; tests in
`app/tests/test_availability_contract.py`. Reports are produced offline; drill
records are not signed.
