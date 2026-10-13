# Activation-to-revenue weekly scorecard

This is the repeatable weekly operating view for the acquisition loop and its
economic boundary. The authoritative definitions, numerator/denominator,
source, privacy class, and availability of every row live in the [versioned
metric dictionary](outcome-metrics.json). This page is the reporting
template; it does not create a second definition for a metric.

The rows below are the dictionary's `weekly` metrics. Monthly community,
migration, availability, and hosted-adoption KPIs, together with the decisions
and roadmap changes they drive, are recorded in the [monthly outcome
review](outcome-review.md).

## Reporting contract

- Use the most recent complete Monday–Sunday week in UTC. Record the exact
  `periodStart`, `periodEnd`, `generatedAt`, and data-owner sign-off.
- Read acquisition sessions, events, clone snapshots, mirror counters, and
  consented feature events to exhaustion. Every continuation page is part of
  the report. A page size of 100 is a transport limit, never a population
  limit.
- A source total and collected-row total must reconcile before a value is
  published. If they do not, write `UNKNOWN — reconciliation failed`; never
  show the partial first page.
- Keep first-touch acquisition cohorts, anonymous clone totals, and closed
  accounting periods visibly separate. They answer different questions and
  must not be joined as if a clone or a dollar had a browser session.
- Record exclusions and coverage beside every value. Bot filtering, consent
  opt-out, missing snapshot baselines, and missing accounting evidence are
  part of the result, not footnotes.

## Copy for each week

```text
Week: YYYY-MM-DD through YYYY-MM-DD UTC
Generated at: YYYY-MM-DDThh:mm:ssZ
Dictionary: forkmesh-outcome-metric-dictionary-v2 / version 2
Owners: Growth=<name>; Product=<name>; Infrastructure=<name>; Finance=<name>; Support=<name>
Data cut: acquisition=<timestamp>; product analytics=<timestamp>; finance=<timestamp>
Pagination audit: pages collected=<n>; provider/source rows=<n>; all continuations exhausted=true|false
Quality state: COMPLETE | PARTIAL | UNKNOWN
Decision: <one sentence>
```

| Stage | Metric id | This week | Prior week | WoW | Target/guardrail | Coverage/confidence | Source check / decision |
| --- | --- | ---: | ---: | ---: | --- | --- | --- |
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

Use `n/a` for a mathematically undefined rate and `UNKNOWN` for a missing or
unreconciled source. `WoW` is `(this week - prior week) / prior week` only when
the denominator is non-zero and coverage is comparable. Keep the raw unit next
to the displayed number: counts, hours, micro-USD, or basis points are not
interchangeable.

## Funnel readout

The first-touch activation view may show these directional rates when the same
cohort and exclusion policy is used:

```text
signup rate          = signup / landing_activation
first-mirror rate    = first_mirror / signup
contribution rate    = contribution / first_mirror
qualification rate  = managed_service_qualification / managed_waitlist_submitted
paid conversion      = paid_conversion numerator / paid_conversion denominator
```

The first three are attributed session rates. `anonymous_clone` is deliberately
not in those divisions because Git clients do not carry the browser cookie.
`paid_conversion`, `retention`, and `gross_margin` are accounting measures and
must use their own closed-period denominators. Feature adoption is an opt-in
coverage signal, not a total-population adoption rate.

## Reconciliation checklist

Run this checklist before circulating the scorecard:

1. Load [the metric dictionary](outcome-metrics.json) and confirm
   its schema/version are unchanged from the scorecard header:

   ```sh
   python3 tools/outcome_metrics.py validate-dictionary
   python3 tools/outcome_metrics.py review --input /path/to/week.json
   ```

   The review output carries each row's freshness and confidence; copy them
   into the `Coverage/confidence` column.
2. For acquisition data, call the existing `channel_report`; verify that
   `sessions_collected` and `events_collected` equal their source
   `total_count` values after `collect_all` follows every `next_cursor`.
3. Run the pagination regression with more than one 100-row page:

   ```sh
   cd app
   pytest -q tests/test_attribution.py \
     -k 'report_returns_all_250_rows_not_the_page_size or page_past_100_rows or count_mismatch_raises'
   pytest -q tests/test_activation_revenue_scorecard.py
   ```

   The tests must collect 250 sessions, 130 events, and 150 clone snapshots
   through continuation cursors; reject a reader that returns only the first
   200 rows while claiming 250 exist; and prove every dictionary source,
   locator, and report reader still exists.
4. Reconcile anonymous clones to adjacent daily snapshot rows and retain the
   number of snapshot rows collected. Never add `clone_url_copied` to that
   total.
5. Reconcile feature adoption to the complete consented provider export,
   including its continuation-page count, and state opt-in coverage.
6. Validate the closed accounting period and margin calculation:

   ```sh
   python3 tools/unit_economics.py validate-contract
   python3 tools/unit_economics.py report --input /path/to/period.json
   ```

   Attach billing reconciliation, provider invoice, support review,
   portability, recovery, and security/privacy evidence. A missing input is a
   blocker, not a zero.

7. Write exclusions, privacy boundary, freshness, and confidence next to the
   scorecard. Do not include raw email, account, repository, path, IP,
   provider-customer, or support-case identifiers.

## Decisions and interpretation

Use the scorecard to choose one action for the next week: a landing or proof
experiment, a contribution/onboarding fix, a qualified-service interview, a
support-load reduction, or a cost/margin intervention. A falling acquisition
number with stable clones may be attribution coverage; a falling margin with
stable revenue is a cost or allocation issue. Record that hypothesis and the
follow-up owner rather than changing a metric definition mid-week.

The current managed-services offer remains a research preview. A qualification
or paid-conversion row can measure demand and validated billing evidence without
turning the free protocol, public reading, public cloning, self-hosting, or
portable Git data into paid entitlements.
