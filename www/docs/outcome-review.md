# Monthly outcome review

ForkMesh makes roadmap and investment decisions from the KPIs in the
[versioned outcome metric dictionary](outcome-metrics.json). The dictionary
names every KPI's numerator, denominator, window, owner, privacy class,
collection method, consent basis, opt-out, retention, minimum cell size,
freshness limit, coverage, baseline, target, and source. This page is the
monthly record template. It never creates a second definition for a metric.

The [weekly activation-to-revenue scorecard](activation-revenue-scorecard.md)
covers the `weekly` metrics. This review covers every metric, including the
`monthly` community, migration, availability, and hosted-adoption KPIs.

## Decision cadence

| Cadence | Forum | Output |
| --- | --- | --- |
| Weekly | Activation-to-revenue scorecard | One action for the next week. Definitions do not change mid-period. |
| Monthly | This review, after the UTC month is complete | Every KPI with freshness and confidence, plus decisions, experiments, and roadmap changes. |
| Quarterly | The monthly review that closes a quarter | Adopt, keep, or retire targets. Publish any dictionary version change. |

A baseline is `unmeasured` until a recorded review reports a `COMPLETE` value
for it. A target stays `to-be-set` until a review adopts it and the dictionary
names that review as its `source`. The dictionary validator rejects an invented
baseline or target.

## Privacy rules

Measurement must never become required surveillance.

- **Consent.** Product analytics is opt-in and off by default. Attribution sets
  no cookie for DNT/GPC visitors or configured skip countries. Every other KPI
  uses records the service already keeps to operate, closed accounting
  ledgers, synthetic probes, or voluntary operator registrations.
- **Self-hosting stays invisible.** A self-hosted node never has to report
  anything. Operator-reported and hosted-only KPIs have `lower-bound`
  coverage; they are never backfilled or inferred for nodes that stay silent.
- **Minimization.** Observation files carry counts, totals, coverage, and
  segment labels only. `tools/outcome_metrics.py review` rejects email, IP,
  account, user, owner, author, actor, session, customer, subscription,
  repository, path, URL, referrer, and name fields, and any email or IPv4
  value.
- **Aggregation.** When a segment is smaller than the metric's
  `minimumCellSize`, it is suppressed. If only one segment is suppressed, the
  next smallest is suppressed too, so the other segments cannot be subtracted
  from the total to reveal it. A value computed over a population smaller than
  the minimum is `publishable: false` and stays in the internal record.
- **Retention.** Raw sources keep the retention named on each metric. Daily
  and boundary exports that exist only to build a monthly KPI are deleted once
  the month's review is recorded. The review record holds aggregates only.
- **Opt-out.** Each metric names its opt-out. Removing an attribution session,
  withdrawing analytics consent, self-hosting a repository, or merging on your
  own node never degrades the product.
- **No deanonymization.** A record never quotes a person, repository, pull,
  ticket, or customer. It also never cross-tabulates small segments to
  explain a change. Interview and support evidence is linked by sanitized
  reference only.

## Prepare the review

1. Validate the dictionary the review will cite:

   ```sh
   python3 tools/outcome_metrics.py validate-dictionary
   ```

2. Build one aggregate observation file for the month. Collect every source to
   exhaustion. Record the source total beside the collected total, and set
   `continuationExhausted` only after the last page:

   ```json
   {
     "schema": "forkmesh-outcome-observations-v1",
     "dictionarySchema": "forkmesh-outcome-metric-dictionary-v2",
     "dictionaryVersion": 2,
     "cadence": "monthly",
     "periodStart": "2026-09-01T00:00:00Z",
     "periodEnd": "2026-10-01T00:00:00Z",
     "generatedAt": "2026-10-03T09:00:00Z",
     "observations": [
       {
         "metric": "repository_availability",
         "numerator": 1293120,
         "denominator": 1296000,
         "sourceTotal": 30,
         "collectedTotal": 30,
         "continuationExhausted": true,
         "coverage": 1.0,
         "dataCutAt": "2026-10-01T01:00:00Z",
         "segments": [{ "label": "clone", "value": 646560 }]
       }
     ]
   }
   ```

3. Produce the report:

   ```sh
   python3 tools/outcome_metrics.py review --input /path/to/month.json
   ```

   Every metric in scope appears in the report. A metric without an
   observation is `UNKNOWN`, never zero. Freshness is `stale` when the data
   cut is older than the metric's `freshnessHours`. Confidence follows the
   dictionary's `confidencePolicy`:

   | Confidence | Meaning |
   | --- | --- |
   | `high` | Reconciled, exhausted, fresh, measured coverage at least 0.95, and full coverage class |
   | `medium` | Reconciled and fresh, but partial coverage or a partial/lower-bound class |
   | `low` | Reconciled but stale, or measured coverage below 0.5 (`PARTIAL`) |
   | `unknown` | Missing, unreconciled, not exhausted, or cut before the window ended (`UNKNOWN`) |

## Reconcile with billing and operations samples

Before the review is circulated, sample at least one source row family per
collection method and record the result:

- **Billing.** Reconcile `paid_conversion`, `retention`, and `gross_margin` to
  the provider ledger and invoices with `python3 tools/unit_economics.py report
  --input /path/to/period.json`. A missing invoice is a blocker, not a zero.
- **Operations.** Compare one day of `repository_availability` with the
  `/status` history and the status alert log. Compare one day of
  `independent_mirror_nodes` with the `/status` mirror roster. Compare
  `successful_migration` with a `finished_at`-bounded row count. The
  authenticated migration-metrics view only shows the latest 1,000 runs and is
  not a monthly population.
- **Synthetic.** Attach the latest stock Git conformance report for the same
  month.

A failed sample turns the affected metric `UNKNOWN` for the month. Record the
reason; do not adjust the value by hand.

## Copy for each month

Store completed records as `www/docs/strategy/outcome-reviews/YYYY-MM.md`.
Record only aggregate values.

```text
Month: YYYY-MM (YYYY-MM-01T00:00:00Z through YYYY-MM-01T00:00:00Z)
Generated at: YYYY-MM-DDThh:mm:ssZ
Dictionary: forkmesh-outcome-metric-dictionary-v2 / version 2
Owner sign-off: Growth=<name>; Product=<name>; Community=<name>; Infrastructure=<name>; Finance=<name>; Support=<name>; Privacy=<name>
Data cuts: hosted=<timestamp>; analytics=<timestamp>; finance=<timestamp>; synthetic=<timestamp>
Reconciliation samples: billing=<pass|fail + reference>; operations=<pass|fail + reference>; synthetic=<report reference>
Quality state: COMPLETE | PARTIAL | UNKNOWN
```

| Stage | Metric id | Value | Baseline | Target | Freshness | Confidence | Coverage | Note |
| --- | --- | ---: | --- | --- | --- | --- | --- | --- |
| Acquisition | `landing_activation` |  |  |  |  |  |  |  |
| Activation | `anonymous_clone` |  |  |  |  |  |  |  |
| Activation | `signup` |  |  |  |  |  |  |  |
| Activation | `first_mirror` |  |  |  |  |  |  |  |
| Activation | `contribution` |  |  |  |  |  |  |  |
| Revenue | `managed_service_qualification` |  |  |  |  |  |  |  |
| Revenue | `paid_conversion` |  |  |  |  |  |  |  |
| Revenue | `retention` |  |  |  |  |  |  |  |
| Product | `feature_adoption` |  |  |  |  |  |  |  |
| Sustainability | `support_load` |  |  |  |  |  |  |  |
| Sustainability | `infrastructure_cost` |  |  |  |  |  |  |  |
| Sustainability | `gross_margin` |  |  |  |  |  |  |  |
| Activation | `successful_migration` |  |  |  |  |  |  |  |
| Community | `first_contribution` |  |  |  |  |  |  |  |
| Community | `active_maintainers` |  |  |  |  |  |  |  |
| Product | `hosted_service_adoption` |  |  |  |  |  |  |  |
| Availability | `repository_availability` |  |  |  |  |  |  |  |
| Availability | `independent_mirror_nodes` |  |  |  |  |  |  |  |

```text
Decisions:
- <decision> — evidence: <metric ids> — owner: <role> — revisit: YYYY-MM
Experiments:
- <hypothesis> — metric: <id> — guardrail: <id> — start/stop: YYYY-MM-DD/YYYY-MM-DD — result: <pending|kept|reverted>
Roadmap changes:
- <issue or milestone> — change: <added|reprioritized|dropped> — because: <metric ids>
Definition changes proposed for the next dictionary version:
- <metric id> — change — reason
Exports deleted after this review: <list of export references>
```

## Changing a definition

Definitions change only between periods. A change updates
`www/docs/outcome-metrics.json`, increments `version`, appends a `changelog`
entry, and updates the dictionary line in both templates. It does not rewrite
earlier records. An earlier record keeps the version it cites, and a
comparison across versions is labelled as not comparable.

## Failure and recovery

- **A source is unavailable or unreconciled.** The metric is `UNKNOWN` for the
  period. Record the reason, and do not carry the prior value forward.
- **A data cut is stale.** Re-cut the source and regenerate the report. If a
  fresh cut cannot be produced, publish at `low` confidence as `PARTIAL`.
- **An export or record contains an identifier.** Delete it, regenerate it
  from the aggregate observation file, and log the incident for privacy
  review. The review tool refuses such input, so a record with an identifier
  was assembled by hand.
- **A dictionary version is wrong.** Revert the JSON change, re-run
  `validate-dictionary`, and regenerate any record that cited the bad version.
- **A daily or boundary export was missed.** The affected monthly KPI
  (`first_contribution`, `active_maintainers`, `hosted_service_adoption`,
  `independent_mirror_nodes`, or availability days beyond the 30-day status
  retention) is `UNKNOWN` for the month. Resume exports the next day; do not
  reconstruct history from cumulative tables.

## Linked automated evidence

- Dictionary validation, consent/coverage rules, baseline/target honesty,
  freshness, confidence, suppression, identifier rejection, and CLI failure
  paths: `app/tests/test_outcome_metrics.py`
- Weekly scorecard rows, source locators, and pagination to exhaustion:
  `app/tests/test_activation_revenue_scorecard.py` and
  `app/tests/test_attribution.py`
- Closed-period revenue, cost, and margin: `app/tests/test_unit_economics.py`
- Consent-gated product analytics: `app/tests/test_first_party_analytics.py`
- Migration completion and setup percentiles:
  `app/tests/test_repository_migration.py`
- Status samples and history: `app/tests/test_status_page.py`

Automated checks prove the contract and the report arithmetic. They do not
replace a recorded month with real data cuts, billing and operations samples,
owner sign-off, or privacy review.
