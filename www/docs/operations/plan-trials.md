# Plan trials and self-serve usage meters

Users can start a trial, see usage, pay, change plan, and cancel without
support intervention.

## Endpoints

- `GET /api/plans/usage`: per-account usage meters (published repositories,
  owned organizations, SSH keys, stored notifications) against the operational
  caps their routes already enforce. Limits are plan-independent; a numeric plan
  allowance may be added only once it is published in `business-model.json`
  with metering tests.
- `GET /api/plans/trial`: `eligible`, `used`, `active`, `endsAt`. One trial per
  account, never while a paid membership is active.
- `POST /api/plans/trial`: starts the single 14-day Supporter trial. One batch
  writes the `plan_trials` register row (the primary key arbitrates races) and a
  `role_grants` upsert under `scope_type='promotion'`.

The dashboard's **Plan usage** card on `/dashboard/settings/account` renders
the meters and the trial button. Membership payment is described in
[Patreon](patreon.md).

## Grant scope

Membership sync revokes only platform-scope grants, so a trial in the
`promotion` scope expires by its own `expires_at`: a billing-provider outage
cannot revoke, extend, or resurrect it. The upsert only lengthens an unrevoked
expiry, so replays are safe. Trial expiry withholds member perks only; portable
identity, public read/clone, and protocol access are never affected.

## Register and metrics

`plan_trials` is the trial register that the `trialsCompleted` and
trial-to-paid metrics in `www/docs/outcome-metrics.json` reconcile to:

```sql
SELECT COUNT(*) FROM plan_trials WHERE ends_at > :start AND ends_at <= :end;
```

Account deletion purges the row; rename moves it with the account.

## Failure and rollback

- A failed trial batch writes nothing; the API returns `trial_already_used` or
  `already_subscribed`.
- The usage endpoint reads only local state; provider outages cannot affect it.
- The down migration drops only the register; live grants lapse on their own.

Managed services stay unavailable for purchase
(`launchGates.managedCheckoutEnabled: false`); managed plans must reuse this
register before checkout can open.
