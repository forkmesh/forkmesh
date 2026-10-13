# Payments, memberships and plans

## Rules that hold today

- **Patreon is the only payment rail.** The Polar integration (on-site card
  checkout and one-time package purchases) was removed on 2026-09-22
  (`dba3b4c7f`); migration `0189_drop_polar_tables.sql` dropped its three
  tables, which held no rows. No card data ever reaches the Worker.
- **Authorization comes from `role_grants`, never from a provider record.**
  Two scopes matter:
  - `platform`: memberships bought through Patreon (`supporting_member`,
    `pro_member`, `builder_member`, `studio_member`). Patreon sync writes and
    revokes these.
  - `promotion`: hackathon prizes and the one 14-day Supporter trial. They
    expire by their own `expires_at`; Patreon never touches them.
- Managed (hosted) services are not for sale:
  `launchGates.managedCheckoutEnabled` is `false` in
  `www/public/business-model.json`.
- Every package in `marketplace/packages/*/manifest.json` has `price.amount`
  0. The Voice Changer package was briefly $1 through Polar and was made free
  in `5c7e2e1a7`.

## Code

| Path | Role |
| --- | --- |
| `app/src/patreon.py`, `app/src/patreon_handler.py` | OAuth connect, webhook verification (`X-Patreon-Signature`, HMAC-MD5), tier mapping (`PRO_TIER_CENTS`, `BUILDER_TIER_CENTS`, `STUDIO_TIER_CENTS`), grace period |
| `app/src/plan_services.py` | `/api/plans/usage`, `/api/plans/trial`, `_active_membership`, `promotion_status` |
| `app/src/provider_runtimes.py` | `_AccountPlanRuntime`, the session plus D1 adapter `plan_services` uses (formerly the Polar runtime) |
| `app/src/ad_network.py` | ForkMesh Ads; slot allowance read from the same membership grants |
| `app/public/dashboard/js/04-account.js` | Membership card and Plan usage card (`data-membership-*`, `data-plan-usage-*`) |
| `app/public/chat.js` | Plan chip read from `/api/plans/usage` |

`GET /api/plans/usage` reports the viewer's membership (platform grants) and
promotion state alongside the usage meters; it is the one place clients ask
"what plan am I on".

## Operations

- Patreon setup, secrets and failure table: `www/docs/operations/patreon.md`.
- Trials and usage meters: `www/docs/operations/plan-trials.md`.
- Business model and gates: `www/docs/business-model.md`,
  `www/docs/operations/business-model-rollout.md`.
- Ads: `www/docs/operations/ad-network.md`.

## Traps

- `app/public/dashboard.js` and `app/public/dashboard/settings/index.html`
  are generated; rebuild with `app/tools/build_dashboard_assets.py` after
  editing membership UI.
- `app/tests/test_organization_discord.py` slices `provider_runtimes.py` from
  the Discord class to `_AccountPlanRuntime`; renaming that class breaks an
  unrelated test.

## Related

- [App Worker home](README.md)
