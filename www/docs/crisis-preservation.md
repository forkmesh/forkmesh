# Preservation-first crisis onboarding

Use this guide when a project could disappear soon: a planned shutdown, a
takedown risk, a lost account, censorship, or a provider shutting down. It helps
you do three things:

1. Pick a plan that fits the time and access you still have.
2. Preserve the most important data first.
3. Get an honest account of what is actually safe and what is still at risk.

The plan is `forkmesh-crisis-preservation-plan-v1`. The assessment of what was
preserved is `forkmesh-crisis-preservation-assessment-v1`, and a timed drill is
`forkmesh-crisis-preservation-drill-v1`. All three come from one pure
reference implementation, so the API, the CLI, and the shared fixture always
give the same answer.

## Start here

1. Get a plan. Name your scenario, and add the time you have if you know it:

   ```sh
   curl -s 'https://app.forkmesh.com/api/preservation/crisis-plan?scenario=urgent_takedown&minutes=45'
   tools/forkmesh_crisis_preservation.py plan urgent_takedown --minutes 45
   ```

2. Work through `steps` in order. Each included step has an `action`: start
   the [resumable import](repository-imports.md), push a local clone, export
   your [portable identity](portable-identities.md), or ask another operator to
   mirror the repository.
3. Read `tradeoffs` before you start. In urgent mode, a plan that leaves
   anything out sets `acknowledgementRequired`. Every item there will be
   reported as skipped or unavailable, never as preserved.
4. When the work ends, download the migration report and the mirrors payload,
   then assess them:

   ```sh
   curl -s -H "Authorization: Bearer $SESSION" \
     https://app.forkmesh.com/api/repository-imports/migrations/$JOB/report > report.json
   curl -s https://app.forkmesh.com/api/repo/$OWNER/$REPO/mirrors > mirrors.json
   tools/forkmesh_crisis_preservation.py assess request.json \
     --report report.json --availability mirrors.json
   python3 tools/verify_migration_report.py report.json --objects objects.json
   ```

## Scenarios

Each scenario sets a default time window, default access, a priority order, and
a drill budget. You can override the window and access. A lost account caps
access at `public`, because without the account only public data can be read.

### `planned_shutdown`

You are closing the project or its hosting on a known date, and you still
control the account. Default window 30 days, default access `owner`.
Priority: `refs`, `releases`, `documentation`, `issues`, `artifacts`, `identity_proofs`, `trusted_mirrors`.
Drill budget: **120 minutes**.

There is time for everything. Import all of it and publish the artifacts. Then
have two independent operators prove the copy before the shutdown date.

### `urgent_takedown`

A takedown, suspension, or removal could happen at any moment. This scenario is
always in urgent mode. Default window 1 hour, default access `owner`.
Priority: `refs`, `identity_proofs`, `trusted_mirrors`, `releases`, `documentation`, `issues`, `artifacts`.
Drill budget: **30 minutes**.

Get the Git history and your identity proofs out first, then a copy held by a
second operator. Anything the window cannot hold is listed as skipped.

### `lost_account`

You no longer control the provider account that owns the repository. Default
window 1 day, access capped at `public`.
Priority: `refs`, `identity_proofs`, `trusted_mirrors`, `releases`, `documentation`, `issues`, `artifacts`.
Drill budget: **60 minutes**.

Preserve the public data. Prove you are the maintainer with the keys and proofs
you still hold, not with the lost account. A private repository can only be
preserved from a local clone (`localClone`). Its issues, releases, and artifact
files stay unavailable.

### `censorship`

The repository is blocked, or likely to be blocked, in a region or by the
provider. Default window 4 hours, default access `public`.
Priority: `refs`, `trusted_mirrors`, `identity_proofs`, `releases`, `documentation`, `artifacts`, `issues`.
Drill budget: **60 minutes**.

Independence matters more than completeness here. Copies held by different
operators in different regions keep the project readable when one path is
blocked. The [availability proof](availability-proof.md) counts operators, not
nodes.

### `provider_eol`

The hosting provider or product is being discontinued. Default window 7 days,
default access `owner`.
Priority: `refs`, `releases`, `documentation`, `issues`, `artifacts`, `identity_proofs`, `trusted_mirrors`.
Drill budget: **120 minutes**.

Everything the provider holds goes with it, including artifacts and
integrations. Import all of it, and replace every provider-only dependency
before the end date.

## How a plan is built

The request is `scenario`, and optionally `minutes` (1 to 525,600), `access`
(`owner`, `read`, `public`, or `none`), and the repository's `sizeMb`, `issues`,
`releases`, `assetsMb`, `private`, and `localClone`. Any size you leave out uses
a default (100 MB, 500 issues, 20 releases, 200 MB of artifacts). The plan lists
those defaults in `assumed`.

The plan walks the scenario's priority order. It includes each category whose
estimate fits the time that remains, and records why it left anything out.
Estimates are deliberately conservative:

| Category | Estimate |
| --- | --- |
| `refs` | 2 minutes + 1 minute per 50 MB of repository |
| `identity_proofs` | 5 minutes |
| `trusted_mirrors` | 10 minutes for the first signed receipt + 1 minute per 50 MB |
| `artifacts` | 2 minutes + 1 minute per 50 MB of artifact files |
| `releases`, `documentation`, `issues` | Provider pages at 30 pages a minute with a token, 1 a minute without one |

**Import order.** One import job copies provider data in a fixed phase order:
issues, pull requests, and reviews first; then releases and documentation; then
labels, users, and discussion history. So it finishes `releases`, then
`documentation`, then `issues`, and it cannot skip ahead. Each estimate is the
extra time until the import finishes that category. It can be 0 when two
categories finish in the same minute. If one import category does not fit, every
later one is deferred with `import_order`.

**Urgent mode.** A plan is urgent when the scenario is `urgent_takedown` or the
window is 240 minutes or less. In urgent mode, a plan with any tradeoff sets
`acknowledgementRequired: true`. The summary names every category that is not
planned.

**Git history always starts.** `refs` is required. If it cannot fit, it is still
included, flagged `over_budget`, and the summary says so.

## Reasons

Every step that is not included, and every over-budget step, carries one reason
and a tradeoff with its consequence and follow-up.

### `time_budget`

Higher-priority data uses up the time available. Run a follow-up when time
allows. Until evidence arrives, the category stays skipped.

### `requires_token`

The source is private, and the access you have cannot read it. Resume the
import with a read-scoped token if you get access back.

### `source_unreachable`

The source can no longer be reached (`access: none`), and no local clone holds
this data. Check the repository's Mirrors tab for copies other operators already
hold.

### `import_order`

The import copies provider data in a fixed order, and an earlier part did not
fit, so this part will not be reached in the window.

### `needs_refs`

Mirrors can only be requested once the Git history is being preserved. Preserve
`refs` first.

### `over_budget`

`refs` is required, but its estimate is longer than the time available. Start it
now. The plan cannot promise it finishes.

## Coverage states

An assessment gives each category exactly one state. **Only evidence can make a
category `preserved` or `empty`.** Without evidence, the plan's decision picks
the label: included becomes `missing`, deferred becomes `skipped`, and
unavailable stays `unavailable`.

| State | Meaning |
| --- | --- |
| `preserved` | Evidence proves a complete copy. |
| `empty` | Evidence proves the source has nothing of this kind. |
| `partial` | Evidence proves some of it, but records failed, stayed external, went unread, or are unaccounted for. |
| `failed` | Evidence shows the attempt produced nothing usable. |
| `missing` | Planned, but no evidence has arrived. |
| `skipped` | Deferred by the plan, with no evidence. |
| `unavailable` | Out of reach with the access available, with no evidence. |

The evidence for each category:

| Category | Evidence |
| --- | --- |
| `refs` | An availability proof with a verified copy that carries every branch. A migration report alone is only `partial`, because it captures ref names, not the Git history. |
| `identity_proofs` | `identity.recoveryKitVerified` (the recovery-kit restore drill passed) and `identity.externalProofVerified` (an external-account proof verified). One of the two is `partial`. |
| `trusted_mirrors` | An availability proof with `quorum` is `preserved`, and `partial` is `partial`. |
| `issues` | Report rows `issues`, `pull_requests`, `reviews`, `labels`, `users`, and `collaboration_history`. |
| `releases` | Report row `releases`. |
| `documentation` | Report row `documentation`. The wiki is a separate repository. |
| `artifacts` | Availability artifact coverage on a verified copy. The report's `release_assets` row can only prove `empty`, because the import does not copy artifact bytes. |

The assessment recomputes each report row. It does not trust the report's
`reconciled` flag. A category is `preserved` only when the report completed,
every one of its rows adds up, and nothing failed, stayed external, or went
unread. The assessment does not check the report's signature. Check it with
`tools/verify_migration_report.py`.

`verifiedUsableMirror` is true when an availability proof is `quorum` or
`partial` and `refs` is `preserved`. `status` is `preserved` when every category
is preserved or empty, `partial` when a verified usable mirror exists, and
`at_risk` otherwise.

## Risks to understand

Every plan lists these six topics, in the order that matters most for its
scenario.

### `ownership`

A copy does not transfer ownership. Keep the license, copyright notices, and
contributor attribution with every copy. Record who may act for the project.

### `legal`

Each node operator answers legal requests for their own node. A lawful takedown
of one copy does not remove the others, and ForkMesh cannot make an operator
keep or remove a copy. See the [node operator policy](operations/node-operator-policy.md).
Get legal advice before you republish content that is subject to a notice.

### `secrets`

A mirror copies every secret ever committed. Rotate exposed credentials before
mirroring, and see [secret scanning](security/secret-scanning.md). Never put
provider tokens into a copy. ForkMesh never stores them.

### `verification`

Only evidence counts: a reconciled, signed migration report and an availability
proof with verified copies. Check the report signature yourself.

### `external_dependencies`

A metadata import does not copy release artifact files, wikis, CI logs, package
registries, or links to the old provider. The migration report lists each one
under `externalDependencies`.

### `emergency_contacts`

Report a compromised account or leaked key to **security@forkmesh.com**. Name
recovery contacts for your [portable identity](portable-identities.md) before
you need them.

## Getting help without lock-in

Every plan lists the same routes. Every route has the same exit: `git clone
--mirror`, the signed migration report export, and the portable identity kit all
work without ForkMesh.

| Route | Who | Payment | Available |
| --- | --- | --- | --- |
| `self_serve` | You: a resumable import and your own mirror node, free and MIT-licensed | No | Yes |
| `community_mirror` | Independent operators asked from the Mirrors tab; they choose what they carry | No | Yes |
| `managed_recovery` | Paid verified recovery | Yes | No: a research preview, not for sale |

Paid recovery is never offered as available before it is sold. Under the
[business model](business-model.md), it would run a replaceable role with no
authority over your repository.

## Drills

A drill proves a scenario works before you need it. Start the plan, then record
`startedAt` and the time a verified usable mirror and a migration report both
existed (`verifiedAt`, in milliseconds):

```json
{
  "id": "2026-09-urgent",
  "request": {"scenario": "urgent_takedown"},
  "startedAt": 1800000000000,
  "verifiedAt": 1800001500000,
  "evidence": {"migrationReport": {}, "availability": {}, "identity": {}}
}
```

```sh
tools/forkmesh_crisis_preservation.py drill drill.json
```

A drill passes when it meets its scenario's budget and has no failures:

| Scenario | Drill budget |
| --- | --- |
| `urgent_takedown` | 30 minutes |
| `lost_account` | 60 minutes |
| `censorship` | 60 minutes |
| `planned_shutdown` | 120 minutes |
| `provider_eol` | 120 minutes |

| Failure | Meaning |
| --- | --- |
| `invalid_timing` | `startedAt` or `verifiedAt` is missing, or the drill ends before it starts. |
| `over_time_budget` | The verified copy and report took longer than the budget. |
| `no_migration_report` | No readable migration report was supplied. |
| `migration_report_unreconciled` | The report has not completed, or its rows do not add up. |
| `no_verified_mirror` | No verified copy carries every branch. |

A passing drill can still have skipped categories. `residualRisks` lists each one.

## API and CLI

```http
GET /api/preservation/crisis-plan
GET /api/preservation/crisis-plan?scenario=lost_account&private=1&localClone=1
```

Without `scenario`, the endpoint returns the `forkmesh-crisis-preservation-catalog-v1`
catalog: scenarios, access levels, categories, states, and routes. With one, it
returns the plan. Both are public and cached for five minutes. A bad value
returns `400` with a closed code: `unknown_scenario`, `invalid_minutes`,
`unknown_access`, or `invalid_<field>`.

Assessments and drills read owner-only migration reports, so they run locally in
`tools/forkmesh_crisis_preservation.py` and never send a report to the Worker.
The CLI exits 0 for a built plan, a fully preserved assessment, or a passed
drill. It exits 3 for at risk, partial, or failed, and 2 for unreadable input.

## Conformance

- `conformance/crisis-preservation/v1/model.json` sets out the scenarios, policy constants, reasons, states, routes, and drill failures.
- `conformance/crisis-preservation/v1/fixture-input.json` holds shared evidence blocks and hand-written expectations. It covers 9 plans, 11 assessments, 12 drills, and 3 sequences (retry, mirror outage, and import rollback), and each sequence ends recovered.
- `conformance/crisis-preservation/v1/generate_fixture.py` checks the reference output against those expectations, then writes `fixture.json`. Run it with `--check`.

Run `python -m pytest app/tests/test_crisis_preservation.py` to check:

- The reference, the fixture, and the CLI.
- The Worker route, using the real handler source.
- A drill built from a real migration report and a real availability proof.
- Sweeps over every scenario, window, access level, and repository shape. They
  prove that no plan exceeds its window unless `refs` is over budget, that every
  tradeoff is explained, and that data without evidence is never marked
  preserved.

## Telemetry

`GET /api/preservation/crisis-plan` is listed in the endpoint catalog, so its
traffic appears in the API metrics groups. The plan endpoint stores nothing
about the request. Crisis migrations themselves show up in the
[migration metrics](repository-imports.md#migration-reports).

## Rollout and rollback

The route and module are additive. Rolling back the Worker removes the endpoint.
Imports, reports, and availability proofs are unaffected. To change the model,
edit `model.json`, the reference implementation, and `fixture-input.json`, run
`generate_fixture.py`, and update this document until the suite passes.

## Not covered yet

- A guided web journey. Today the plan is served by the API and the CLI.
- A directory of recovery partners beyond community mirror operators.
- Crisis-specific outcome telemetry: time to verified copy and recovery success
  per scenario, measured from production drills.
