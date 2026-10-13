# Plan trials and self-serve usage meters

Issue #488's commercial gate requires that a user can start a trial, see
usage, pay, change plan, and cancel without support intervention. This page
documents the pieces that exist, how they reconcile, and their boundaries.

## Surfaces

- `GET /api/plans/usage` — session-authenticated per-account usage meters
  (published repositories, owned organizations, SSH keys, stored
  notifications) against the operational caps their owning routes already
  enforce. The limits are plan-independent: no membership or trial raises
  them, and no numeric *plan allowance* may be added here until it is
  published in `business-model.json` with metering tests in the same change.
- `GET /api/plans/trial` — trial state: `eligible`, `used`, `active`,
  `endsAt`. Eligibility is one trial per account, and never while a paid
  membership is active.
- `POST /api/plans/trial` — starts the one 14-day Supporter trial. One D1
  batch writes the `plan_trials` register row (plain INSERT; the primary key
  arbitrates races) and a `role_grants` upsert under
  `scope_type='promotion'`, `scope_bi=blind('plan-trial')`.
- Dashboard: the **Plan usage** card on `/dashboard/settings/account`
  (`data-plan-usage-*`, `data-plan-trial-start` in
  `app/public/dashboard/partials/views/settings.html` + `04-account.js`)
  renders the meters and the trial button beside the membership card.
  Paying for a membership is Patreon
  (`www/docs/operations/patreon.md`); the card reads the entitlement back
  from `/api/plans/usage`.

## Why the promotion scope

Patreon revokes platform-scope membership grants on every sync; nothing
touches `scope_type='promotion'` (the hackathon-prize lane). A trial granted there expires by its own `expires_at`, so:

- A billing-provider outage can never revoke, extend, or resurrect a trial.
- Trial expiry silently withholds member perks only — portable identity,
  public read/clone, and protocol access are never involved (issue #488's
  invariant).
- The upsert only ever lengthens an unrevoked expiry (`MAX(...)`), so a trial
  can never shorten a hackathon prize and replays are safe.

## Register and reconciliation

`plan_trials` (migration `0184_plan_trials.sql`) is the approved trial
register the activation/revenue scorecard's `trialsCompleted` reconciles to
(`www/docs/outcome-metrics.json`, `paid_conversion`):

- Trials completed in a window:
  `SELECT COUNT(*) FROM plan_trials WHERE ends_at > :start AND ends_at <= :end`
- Trial-to-paid conversion joins the register to paid membership state:
  `SELECT COUNT(*) FROM plan_trials t WHERE EXISTS (SELECT 1 FROM
  role_grants g WHERE g.account_bi = t.account_bi AND g.scope_type =
  'platform' AND g.role IN ('supporting_member','pro_member') AND
  g.revoked_at = 0 AND g.expires_at > :end)`
- Account deletion purges the account's register row (privacy); deleted
  accounts therefore leave the denominator, matching the metric's exclusion
  of accounts that no longer exist. Rename moves the row with the account.

## Failure paths and rollback

- Rollback is `migrations/down/0184_plan_trials.sql`: it drops the register
  only. Live promotion grants are untouched and lapse on their own, so no
  in-flight access changes on rollback.
- A failed trial batch writes nothing (register and grant land atomically);
  the dashboard reports the specific `trial_already_used` /
  `already_subscribed` errors and re-enables the button otherwise.
- The usage endpoint reads only local D1 state; provider outages cannot
  affect it.

## Boundaries

Managed services remain `research-preview-not-for-sale`
(`launchGates.managedCheckoutEnabled: false` in `business-model.json`); this
machinery validates the self-serve trial→usage→pay→cancel loop on the
membership tiers and is the register managed plans must reuse before any
managed checkout can open. Evidence: `app/tests/test_plan_services.py`,
`app/tests/test_local_migrations.py`.
