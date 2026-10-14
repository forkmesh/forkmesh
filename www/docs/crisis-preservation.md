# Preservation-first crisis onboarding

For a project that could disappear soon. A plan
(`forkmesh-crisis-preservation-plan-v1`) orders the work to fit the time and
access left; an assessment (`forkmesh-crisis-preservation-assessment-v1`) and a
drill (`forkmesh-crisis-preservation-drill-v1`) report what is actually safe.
One pure reference implementation backs the API, CLI, and fixture.

## Start here

```sh
curl -s 'https://app.forkmesh.com/api/preservation/crisis-plan?scenario=urgent_takedown&minutes=45'
tools/forkmesh_crisis_preservation.py plan urgent_takedown --minutes 45
```

Work through `steps` in order: a [resumable import](repository-imports.md), a
pushed local clone, a [portable identity](portable-identities.md) export, or a
mirror request. Read `tradeoffs` first; an urgent plan that leaves anything out
sets `acknowledgementRequired`. Afterwards, assess the migration report and the
mirrors payload:

```sh
tools/forkmesh_crisis_preservation.py assess request.json \
  --report report.json --availability mirrors.json
```

## Scenarios

Request fields: `scenario`, optional `minutes` (1 to 525,600), `access`
(`owner`, `read`, `public`, `none`), `sizeMb`, `issues`, `releases`,
`assetsMb`, `private`, `localClone`. Missing sizes use defaults listed in
`assumed`.

### `planned_shutdown`

Known end date, account still controlled. Window 30 days, access `owner`.
Priority: `refs`, `releases`, `documentation`, `issues`, `artifacts`, `identity_proofs`, `trusted_mirrors`.
Drill budget: **120 minutes**.

### `urgent_takedown`

Removal could happen any moment; always urgent. Window 1 hour, access `owner`.
Priority: `refs`, `identity_proofs`, `trusted_mirrors`, `releases`, `documentation`, `issues`, `artifacts`.
Drill budget: **30 minutes**.

### `lost_account`

Provider account lost; access capped at `public`. Window 1 day. Private data is
only preservable from a `localClone`.
Priority: `refs`, `identity_proofs`, `trusted_mirrors`, `releases`, `documentation`, `issues`, `artifacts`.
Drill budget: **60 minutes**.

### `censorship`

Blocked in a region or by the provider. Window 4 hours, access `public`.
Independent operators in different regions matter most; see the
[availability proof](availability-proof.md).
Priority: `refs`, `trusted_mirrors`, `identity_proofs`, `releases`, `documentation`, `artifacts`, `issues`.
Drill budget: **60 minutes**.

### `provider_eol`

Provider or product discontinued. Window 7 days, access `owner`.
Priority: `refs`, `releases`, `documentation`, `issues`, `artifacts`, `identity_proofs`, `trusted_mirrors`.
Drill budget: **120 minutes**.

## How a plan is built

The plan walks the priority order and includes each category whose
conservative estimate fits the remaining time. The import copies issues,
releases, and documentation in a fixed phase order, so a deferred import
category defers every later one. A plan is urgent for `urgent_takedown` or a
window of 240 minutes or less. `refs` is always included, flagged when it
exceeds the window.

## Reasons

### `time_budget`

Higher-priority data uses the available time.

### `requires_token`

The source is private and the current access cannot read it.

### `source_unreachable`

The source is gone (`access: none`) and no local clone holds the data.

### `import_order`

An earlier import phase did not fit, so this one will not be reached.

### `needs_refs`

Mirrors can only be requested once `refs` is preserved.

### `over_budget`

`refs` is required but its estimate exceeds the window.

## Coverage states

Only evidence can make a category `preserved` or `empty`; without evidence the
plan decision maps to `missing`, `skipped`, or `unavailable`.

| State | Meaning |
| --- | --- |
| `preserved` | Evidence proves a complete copy. |
| `empty` | Evidence proves the source has none. |
| `partial` | Evidence proves only some of it. |
| `failed` | The attempt produced nothing usable. |
| `missing` | Planned, no evidence yet. |
| `skipped` | Deferred by the plan. |
| `unavailable` | Out of reach with the access available. |

`refs` needs an availability proof with a verified copy carrying every branch;
a migration report alone is `partial`. `trusted_mirrors` needs a `quorum`
proof. Report rows are recomputed, not trusted. `status` is `preserved`,
`partial` (a verified usable mirror exists), or `at_risk`.

## Risks to understand

### `ownership`

A copy does not transfer ownership; keep license and attribution.

### `legal`

Each node operator answers legal requests for their own node. See the
[node operator policy](operations/node-operator-policy.md).

### `secrets`

A mirror copies every committed secret; rotate them first. See
[secret scanning](security/secret-scanning.md).

### `verification`

Only a reconciled, signed migration report and an availability proof count.

### `external_dependencies`

Artifact bytes, wikis, CI logs, and registries are not imported; the report
lists them under `externalDependencies`.

### `emergency_contacts`

Report compromised accounts or keys to **security@forkmesh.com**.

## Getting help without lock-in

| Route | Who | Payment | Available |
| --- | --- | --- | --- |
| `self_serve` | You, with an import and your own mirror node | No | Yes |
| `community_mirror` | Independent operators via the Mirrors tab | No | Yes |
| `managed_recovery` | Paid verified recovery | Yes | No |

Every route exits through `git clone --mirror`, the signed report, and the
identity kit. See the [business model](business-model.md).

## Drills

A drill records `startedAt` and `verifiedAt` (ms) plus evidence, and passes
within its scenario's drill budget with no failures:

```sh
tools/forkmesh_crisis_preservation.py drill drill.json
```

| Failure | Meaning |
| --- | --- |
| `invalid_timing` | Timestamps missing or reversed. |
| `over_time_budget` | Took longer than the budget. |
| `no_migration_report` | No readable report. |
| `migration_report_unreconciled` | Report incomplete or rows do not add up. |
| `no_verified_mirror` | No verified copy carries every branch. |

## API and conformance

`GET /api/preservation/crisis-plan` returns the
`forkmesh-crisis-preservation-catalog-v1` catalog, or a plan when `scenario` is
given. It is public, cached five minutes, stores nothing, and returns `400` with
a closed code on bad input. Assessments and drills run locally only. CLI exit
codes: 0 success, 3 at risk or failed, 2 unreadable input. Migration metrics
are under [migration reports](repository-imports.md#migration-reports).

Model and fixture: `conformance/crisis-preservation/v1/` (`model.json`,
`fixture-input.json`, `generate_fixture.py --check`). Tests:
`app/tests/test_crisis_preservation.py`.
