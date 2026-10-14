# Activation-to-revenue weekly scorecard

Weekly reporting template. Definitions live only in the [metric
dictionary](outcome-metrics.json); monthly KPIs go in the [monthly outcome
review](outcome-review.md).

## Reporting contract

- Report the last complete Monday–Sunday week in UTC.
- Read every source to exhaustion. A page size of 100 is a transport limit, never a population limit.
- If source and collected totals do not reconcile, write `UNKNOWN`; never show the partial first page.
- Keep acquisition cohorts, anonymous clone totals and accounting periods separate.
- A missing input is a blocker, not a zero.

```text
Week: YYYY-MM-DD through YYYY-MM-DD UTC
Dictionary: forkmesh-outcome-metric-dictionary-v2 / version 2
Pagination audit: pages collected=<n>; provider/source rows=<n>; all continuations exhausted=true|false
Quality state: COMPLETE | PARTIAL | UNKNOWN
Decision: <one sentence>
```

| Stage | Metric id | This week | Prior week | WoW | Coverage/confidence |
| --- | --- | ---: | ---: | ---: | --- |
| Acquisition | `landing_activation` |  |  |  |  |
| Activation | `anonymous_clone` |  |  |  |  |
| Activation | `signup` |  |  |  |  |
| Activation | `first_mirror` |  |  |  |  |
| Activation | `contribution` |  |  |  |  |
| Revenue | `managed_service_qualification` |  |  |  |  |
| Revenue | `paid_conversion` |  |  |  |  |
| Revenue | `retention` |  |  |  |  |
| Product | `feature_adoption` |  |  |  |  |
| Sustainability | `support_load` |  |  |  |  |
| Sustainability | `infrastructure_cost` |  |  |  |  |
| Sustainability | `gross_margin` |  |  |  |  |

## Reconciliation checklist

1. `python3 tools/outcome_metrics.py validate-dictionary`, then `review --input week.json`.
2. Run `cd app && pytest -q tests/test_attribution.py -k 'report_returns_all_250_rows_not_the_page_size or page_past_100_rows or count_mismatch_raises'`.
3. Reconcile anonymous clones to daily snapshots. Never add `clone_url_copied` to that total.
4. `python3 tools/unit_economics.py validate-contract` and `python3 tools/unit_economics.py report --input period.json`.
5. Do not include raw email, account, repository, path, IP, customer or support identifiers.
