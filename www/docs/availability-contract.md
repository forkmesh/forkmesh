# Repository availability contracts

A repository availability contract is a precise preservation promise. It names
how many verified copies a repository keeps, how many independent operators
and regions hold them, which transports and refs they serve, how often that is
checked, how fast incidents are answered and recovered, and what the customer
is credited when the promise is missed.

Each contract period produces a `forkmesh-availability-contract-report-v1`
report. The report is computed only from an evidence bundle, so anyone holding
the bundle can recompute it and check its signature. It shows every
verification slot that missed, every SLO miss, the credit each miss earned, the
capacity that was actually demonstrated, and what a simulated operator or
regional failure leaves behind.

This is the contract model and its verifier. Managed checkout stays disabled
until the gates in the [business model](business-model.md) pass; the model does
not open it.

## Evidence bundle

A bundle has the schema `forkmesh-availability-contract-evidence-v1`:

| Field | Contents | Source |
| --- | --- | --- |
| `contract` | `id`, `repository`, `periodStart`, `periodEnd`, `feeMicrousd`, `terms`, `remedies`, `exclusions` | The signed order |
| `observations` | `GET /api/repo/<owner>/<repo>/mirrors` payloads, each with its `generatedAt` | The mirrors API, carrying signed endpoint receipts and direct clone probes |
| `drills` | Restore drill records | `forkmesh-mirror-node --restore-drill` (`mirror-gateway/node-ops/last-restore-drill.json`) |
| `responses` | `incidentStart` and `acknowledgedAt` for each acknowledged incident | The incident log |
| `capacity` | `node` and `class` (`community` or `contracted`) | The order's capacity declaration |

Each observation is scored by the [availability proof](availability-proof.md)
exactly as the Mirrors tab scores it. Observations outside the period are
ignored and counted in `evidence.ignoredObservations`.

## Contract terms

| Term | Meaning |
| --- | --- |
| `copies` | Verified copies that must hold the counted state in every slot. |
| `operators` | Distinct operators that must hold the same verified state in every slot. At least 2, the availability proof's quorum target, and no more than `copies`. |
| `regions` | Distinct declared regions among verified copies in every slot. |
| `uptimeBps` | Share of counted slots, in basis points, that must meet every check. |
| `verificationIntervalMs` | Slot length, at least one minute. A period can have at most 10,000 slots. |
| `transports` | Transports that each need at least one verified copy: `router`, `directHttps`, `smartHttp`. |
| `refCoverage` | When true, at least `operators` verified copies must carry every branch. |
| `artifactCoverage` | When true, at least `operators` verified copies must carry every release artifact. |
| `recoveryTimeObjectiveMs` | Longest allowed incident, and the slowest allowed restore drill. |
| `drillIntervalMs` | Oldest allowed restore drill at period end. |
| `incidentResponseMs` | Longest allowed time from an incident's start to its acknowledgement. |
| `failureTolerance` | How many operators, and separately how many regions, may fail at once while the quorum holds (0 to 2). |
| `maxExcludedBps` | Share of slots that exclusions may remove. |

Malformed terms stop evaluation with a closed error code, such as
`invalid_operators` or `too_many_slots`. A bad contract never yields a
partial report.

## Verification slots

The period is split into slots of `verificationIntervalMs`. Every observation
in a slot must pass every check. A slot with no observation is `unverified`,
because missing evidence is a miss, not a pass.

| Check | The slot misses when |
| --- | --- |
| `unverified` | No observation was recorded in the slot. |
| `quorum_below_contract` | Fewer than `operators` distinct operators held the same verified state. Copies whose clone probes disagree on the refs digest do not count. |
| `copies_below_contract` | Fewer than `copies` verified copies held the counted state. |
| `regions_below_contract` | Verified copies spanned fewer than `regions` regions. |
| `transport_missing` | A contracted transport had no verified copy. |
| `ref_coverage_incomplete` | Fewer than `operators` copies carried every branch, or no branch count was known. |
| `artifact_coverage_incomplete` | Fewer than `operators` copies carried every release artifact, or no artifact count was known. |

Availability is `met slots * 10000 // counted slots`. The division rounds
down, so any rounding favors the customer.

## Exclusions

An exclusion removes whole slots from the count. Evidence in those slots is
still recorded, but it cannot miss or meet. It is honored only when:

- its kind is `scheduled-maintenance`, `customer-requested`, or `legal-hold`;
- it was announced (`announcedAt`) no later than its start; and
- the exclusion budget (`maxExcludedBps` of all slots) is not used up. Earlier
  exclusions use the budget first.

Every exclusion is listed with the slots it removed and, when it did not remove
everything it asked for, a reason:

| Reason | Meaning |
| --- | --- |
| `unknown_kind` | The kind is not one of the three contract kinds. |
| `announced_late` | It was announced after it started, so it cannot be applied retroactively. |
| `over_budget` | It was honored, but some of its slots were beyond the exclusion budget and still count. |

## Incidents, recovery, and response

Consecutive missed slots form one incident. An excluded slot neither ends nor
extends an incident, but its time still counts toward recovery. An incident
recovers at the start of the next counted slot that meets every check. Recovery
is measured in whole slots, so it is never shorter than what the evidence
shows. An incident still open at period end is measured to period end.

Response is measured from the incident's start to the earliest
acknowledgement recorded for that start:

| Response | Meaning |
| --- | --- |
| `ok` | Acknowledged within `incidentResponseMs`. |
| `late` | Acknowledged after `incidentResponseMs`. |
| `missing` | Not acknowledged, and the response window has closed. |
| `pending` | Not acknowledged yet, but still inside the response window at period end. |

## Restore drills

A drill record counts when it has `drill: true`, a node name, a `completedAt`
time (Go RFC 3339 with nanoseconds, or epoch milliseconds), a `durationMs`, and
at least one restored repository. The **latest** drill for a node at period end
decides its state, so an older passing drill cannot hide a newer slow one:

| State | Meaning |
| --- | --- |
| `ok` | The latest drill is within `drillIntervalMs` and took no longer than `recoveryTimeObjectiveMs`. |
| `drill_missing` | The node has no drill record. |
| `drill_stale` | The latest drill is older than `drillIntervalMs`. |
| `drill_beyond_objective` | The latest drill took longer than `recoveryTimeObjectiveMs`. |

See [node backup and recovery](operations/node-backup-and-recovery.md) for how
operators run drills.

## SLOs

| SLO | Target | Met when |
| --- | --- | --- |
| `availability` | `uptimeBps` | Attainment is at least the target over at least one counted slot. |
| `recovery` | `recoveryTimeObjectiveMs` | No incident outlasted the objective. |
| `incident-response` | `incidentResponseMs` | No incident was acknowledged late or left unacknowledged. |
| `restore-drills` | Every contracted node | Every node declared `contracted` has an `ok` drill. |

The period `outcome` is `met` only when all four SLOs are met.

## Misses and remedies

Every miss is listed, even when its SLO is still met. A short incident inside
the uptime target is visible, but it earns no credit.

| Miss | SLO | Listed for |
| --- | --- | --- |
| `availability_window` | `availability` | Every incident. |
| `recovery_objective_exceeded` | `recovery` | Every incident that outlasted the recovery objective. |
| `response_late` | `incident-response` | Every incident acknowledged late. |
| `response_missing` | `incident-response` | Every incident never acknowledged. |
| `drill_missing` | `restore-drills` | Every contracted node without a drill. |
| `drill_stale` | `restore-drills` | Every contracted node whose latest drill is too old. |
| `drill_beyond_objective` | `restore-drills` | Every contracted node whose latest drill was too slow. |

Credits come from the contract's `remedies`:

- When availability is missed, the largest `availabilityTiers` credit whose
  `belowBps` is above the attained value applies once. It covers every
  availability window. No tier may start above `uptimeBps`, so a met SLO can
  never earn a tier credit.
- Each recovery, response, and drill miss earns its fixed
  `recoveryMissCreditBps`, `responseMissCreditBps`, or `drillMissCreditBps`.
- The total is capped at `maxCreditBps`, and `capped` says when the cap applied.

Money is integer micro-USD: `fee * creditBps // 10000`. Each miss names the
remedy line that covers it, and each remedy line lists its misses.

## Demonstrated capacity and admission

A contract may only be backed by capacity that was demonstrated. A node is
demonstrated when both of these hold:

- **Continuous probes.** Every observation in at least `uptimeBps` of the
  counted slots verified it, so it must have a fresh signed endpoint receipt or
  a passing clone probe each time.
- **A restore drill.** Its latest drill is `ok`.

`capacity.nodes` lists every observed or declared node with its class, opaque
operator, region, verified slots, and drill state. Totals are reported
separately for `community` capacity (independent mirrors the customer does not
pay for) and `contracted` capacity (paid, controlled nodes). A declaration
other than `contracted` is treated as community, so a typo can never claim paid
capacity.

The contract is admissible only when demonstrated capacity alone meets the
terms. Otherwise `admission.reasons` lists why:

| Reason | Meaning |
| --- | --- |
| `no_evidence` | The period has no observation. |
| `copies_not_demonstrated` | Fewer demonstrated copies than `copies`. |
| `operators_not_demonstrated` | Fewer demonstrated operators than `operators`. |
| `regions_not_demonstrated` | Fewer demonstrated regions than `regions`. |
| `failure_simulation_failed` | A simulated failure broke the quorum or its recovery, or there was nothing to simulate. |

Run the report over a trailing qualification window before accepting an order.
Run it again each period, so capacity that stops demonstrating is flagged.

## Simulated failures

The simulation takes the period's latest observation and keeps only
demonstrated nodes. It then removes every combination of `failureTolerance`
operators, and separately every combination of `failureTolerance` regions, and
recomputes the availability proof on what is left. Nodes that declared no
region share one `undeclared` failure domain. With `failureTolerance` 0, the
simulation checks the demonstrated capacity once, with nothing removed.

A simulation passes when the remaining operators still meet `operators` and a
surviving node's restore drill finished within `recoveryTimeObjectiveMs`. That
survivor can rebuild the lost copies within the objective.

## Correlated-risk disclosures

These do not change the outcome. Each one names a way the promise could fail
together, and it links back to this section.

### `community_capacity_relied_on`

Contracted capacity alone does not reach the promised operator quorum. The
promise then depends on independent community mirrors that are not under
contract and can leave at any time.

### `contracted_single_operator`

Two or more contracted copies are demonstrated, and one operator runs all of
them, so contracted capacity fails together.

### `operator_concentration`

A single operator runs more than half of the demonstrated copies.

### `region_concentration`

More than half of the demonstrated copies share one region, or declared no
region.

### `no_failure_headroom`

Demonstrated operators do not exceed the promised quorum, so losing any one
operator breaks it.

### `capacity_not_demonstrated`

A node declared as contracted capacity lacks continuous verified probes or a
qualifying restore drill, so it does not count toward the contract.

## Privacy

Operator accounts are never shown. Copies run by the same operator share an
opaque operator number for the whole bundle. Endpoint addresses, mirror URLs,
drill archive paths, and node keys are never shown either. The shared fixture
plants private strings in its evidence, and the tests fail if any of them
appears in a report.

## Signed reports

A signed report is a `forkmesh-availability-contract-signed-report-v1`
document holding `report`, `reportHash`, and an Ed25519 `signature`. The hash
is `sha256:` over the report's canonical JSON: sorted keys, no whitespace, and
UTF-8. The signature covers
`forkmesh-availability-contract-report-v1\n<reportHash>`. The report's
`evidence` section records digests of the contract, capacity, observations,
drills, and responses, so a report names exactly what it was computed from.

```sh
tools/forkmesh_availability_contract.py explain bundle.json         # plain-language summary
tools/forkmesh_availability_contract.py explain --json bundle.json  # full report
tools/forkmesh_availability_contract.py sign bundle.json --private-key seed.txt > signed.json
tools/forkmesh_availability_contract.py verify bundle.json signed.json --public-key <base64url key>
tools/forkmesh_availability_contract.py fixture                     # verify the shared fixture
```

`verify` recomputes the report from the bundle, checks the hash, and checks the
signature. It rejects a report that was edited after signing, even when the
hash is recomputed, and it rejects evidence edited after signing. Without
`--public-key` it still checks the signature, but only against the key embedded
in the document, and says the key is not pinned.

`explain` exits 0 when the contract was met with admissible capacity, and 3
otherwise. `verify` and `fixture` exit 0 on a match and 1 on a mismatch.
Unreadable input or invalid terms exit 2.

## Conformance

- `conformance/availability-contract/v1/model.json` sets out the terms,
  checks, SLOs, misses, states, reasons, disclosures, and policy.
- `conformance/availability-contract/v1/fixture.json` holds one case for every
  check, miss, drill and response state, admission reason, exclusion reason,
  disclosure, and simulation scope. The cases include a regional outage that
  keeps its quorum and an operator failure beyond tolerance.
- `conformance/availability-contract/v1/generate_fixture.py` builds the
  fixture. Each case carries a hand-written outcome, admission reason list,
  miss list, and disclosure list, so a reference change that moves any of them
  fails loudly. Run it with `--check`.

Run `python -m pytest app/tests/test_availability_contract.py` to check the
reference, CLI, signatures, and documentation against the fixture.

## Limitations

- Restore drill records are node-local operation records, not signed. The
  report records their digest, but it cannot prove a node ran the drill it
  reports.
- A drill record counts restored repositories, not which ones, so a drill
  proves the node can restore its backups in general, not this repository
  specifically.
- No Worker API, dashboard panel, or billing flow publishes these reports yet.
  The report is produced and signed offline from exported evidence.

## Rollout and rollback

The model is additive. No Worker route imports it, and it is listed in
`LAZY_MODULES` so it never counts toward Worker startup. To roll back, delete
the module, CLI, suite, and this page; no stored data changes. To change the
model, edit `model.json` and the reference implementation, run
`generate_fixture.py`, and update this page until the suite passes.
