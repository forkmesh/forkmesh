# Payments, memberships and plans

## Rules

- **Patreon is the only payment rail.** No card data reaches the App.
- **Authorization comes from `role_grants`, never from a provider record.**
  - `platform` scope: memberships bought through Patreon
    (`supporting_member`, `pro_member`, `builder_member`, `studio_member`).
    Patreon sync writes and revokes these.
  - `promotion` scope: prizes and the one 14-day Supporter trial; they expire
    by their own `expires_at`.
- Managed (hosted) services are not for sale:
  `launchGates.managedCheckoutEnabled` is `false` in
  `www/public/business-model.json`.
- Every package in `marketplace/packages/*/manifest.json` has `price.amount` 0.

## Code

| Path | Role |
| --- | --- |
| `app/src/patreon.py`, `app/src/patreon_handler.py` | OAuth connect, webhook verification (`X-Patreon-Signature`, HMAC-MD5), tier mapping (`PRO_TIER_CENTS`, `BUILDER_TIER_CENTS`, `STUDIO_TIER_CENTS`), grace period |
| `app/src/plan_services.py` | `/api/plans/usage`, `/api/plans/trial`, `_active_membership`, `promotion_status` |
| `app/src/provider_runtimes.py` | `_AccountPlanRuntime`, the session and database adapter `plan_services` uses |
| `app/src/ad_network.py` | Ad slot allowance from the same membership grants |
| `app/public/dashboard/js/04-account.js` | Membership and Plan usage cards |

`GET /api/plans/usage` is the one place clients ask "what plan am I on": it
reports membership, promotion state and usage meters.

## Operations

- Patreon setup and secrets: `www/docs/operations/patreon.md`.
- Trials and usage meters: `www/docs/operations/plan-trials.md`.
- Business model: `www/docs/business-model.md`.

## Traps

- `app/public/dashboard.js` is generated; rebuild with
  `app/tools/build_dashboard_assets.py` after editing membership UI.
- `app/tests/test_organization_discord.py` slices `provider_runtimes.py` up to
  `_AccountPlanRuntime`; renaming that class breaks an unrelated test.
