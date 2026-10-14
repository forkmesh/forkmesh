# Monthly outcome review

Roadmap decisions use the KPIs in the versioned
[outcome metric dictionary](outcome-metrics.json), which defines each metric's
numerator, denominator, window, privacy class, consent, retention, minimum cell
size, freshness, coverage, baseline, target and source. This page is the monthly
record template; the [weekly scorecard](activation-revenue-scorecard.md) covers
`weekly` metrics. Baselines stay `unmeasured` and targets `to-be-set` until a
recorded review adopts them.

## Privacy rules

Measurement must never become required surveillance.

- Product analytics is opt-in. Other KPIs use operational records, closed
  ledgers, synthetic probes or voluntary operator registrations.
- A self-hosted node never has to report anything. Such KPIs are `lower-bound`.
- Observation files carry counts and segment labels only; the review tool
  rejects identifier fields and any email or IPv4 value.
- Segments below `minimumCellSize` are suppressed; if only one is suppressed,
  the next smallest is suppressed too.
- Exports built only for a monthly KPI are deleted once the review is recorded.
- A record never quotes a person, repository, pull, ticket, or customer.

## Prepare the review

```sh
python3 tools/outcome_metrics.py validate-dictionary
python3 tools/outcome_metrics.py review --input /path/to/month.json
```

Collect each source to exhaustion and record source and collected totals:

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

A metric without an observation is `UNKNOWN`, never zero. Data older than
`freshnessHours` is `stale`. Confidence:

| Confidence | Meaning |
| --- | --- |
| `high` | Reconciled, exhausted, fresh, coverage at least 0.95, full coverage class |
| `medium` | Reconciled and fresh, partial coverage or class |
| `low` | Reconciled but stale, or coverage below 0.5 (`PARTIAL`) |
| `unknown` | Missing, unreconciled, not exhausted, or cut early (`UNKNOWN`) |

Before circulating, reconcile billing metrics with
`python3 tools/unit_economics.py report`, operations metrics with the status
history, and attach the month's Git conformance report. A failed sample makes
the metric `UNKNOWN`.

## Copy for each month

Store records as `www/docs/strategy/outcome-reviews/YYYY-MM.md`, aggregates only.

```text
Month: YYYY-MM
Dictionary: forkmesh-outcome-metric-dictionary-v2 / version 2
Data cuts: hosted=<ts>; analytics=<ts>; finance=<ts>; synthetic=<ts>
Quality state: COMPLETE | PARTIAL | UNKNOWN
```

| Stage | Metric id | Value | Baseline | Target | Freshness | Confidence | Coverage |
| --- | --- | ---: | --- | --- | --- | --- | --- |
| Acquisition | `landing_activation` |  |  |  |  |  |  |
| Activation | `anonymous_clone` |  |  |  |  |  |  |
| Activation | `signup` |  |  |  |  |  |  |
| Activation | `first_mirror` |  |  |  |  |  |  |
| Activation | `contribution` |  |  |  |  |  |  |
| Revenue | `managed_service_qualification` |  |  |  |  |  |  |
| Revenue | `paid_conversion` |  |  |  |  |  |  |
| Revenue | `retention` |  |  |  |  |  |  |
| Product | `feature_adoption` |  |  |  |  |  |  |
| Sustainability | `support_load` |  |  |  |  |  |  |
| Sustainability | `infrastructure_cost` |  |  |  |  |  |  |
| Sustainability | `gross_margin` |  |  |  |  |  |  |
| Activation | `successful_migration` |  |  |  |  |  |  |
| Community | `first_contribution` |  |  |  |  |  |  |
| Community | `active_maintainers` |  |  |  |  |  |  |
| Product | `hosted_service_adoption` |  |  |  |  |  |  |
| Availability | `repository_availability` |  |  |  |  |  |  |
| Availability | `independent_mirror_nodes` |  |  |  |  |  |  |

```text
Decisions:
- <decision> — evidence: <metric ids> — revisit: YYYY-MM
Experiments:
- <hypothesis> — metric: <id> — guardrail: <id> — result: <pending|kept|reverted>
Roadmap changes:
- <milestone> — <added|reprioritized|dropped> — because: <metric ids>
```

Definitions change only between periods: update `outcome-metrics.json`,
increment `version`, and never rewrite earlier records. A missing or
unreconciled source makes the metric `UNKNOWN` for the period; never carry a
prior value forward or reconstruct history by hand.

Tests: `app/tests/test_outcome_metrics.py`.
