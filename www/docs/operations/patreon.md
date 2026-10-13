# Patreon membership operations

ForkMesh uses Patreon as the membership payment rail for Supporter ($5/mo),
Pro ($10/mo), Builder ($50/mo), and Studio ($100/mo). The Worker never sees
card data. Member OAuth access and refresh tokens are discarded immediately
after identity lookup and must never be stored in D1, secrets, logs, or backups.

## Application setup

1. Create a Patreon creator application in the Patreon developer portal for the
   ForkMesh campaign.
2. Enable OAuth scopes:
   - `identity`
   - `identity[email]`
   - `identity.memberships`
3. Register the exact callback URI:
   - Production: `https://forkmesh.com/api/integrations/patreon/callback`
   - Forks: `https://<your-origin>/api/integrations/patreon/callback`
4. Create a webhook pointing at:
   - Production: `https://forkmesh.com/api/integrations/patreon/webhook`
5. Subscribe the webhook to:
   - `members:pledge:create`
   - `members:pledge:update`
   - `members:pledge:delete`
6. Copy the campaign numeric ID (not the vanity slug) and a creator-scoped
   access token that can `GET /api/oauth2/v2/members/{id}`.

## Required configuration

Non-secret production var in `wrangler.toml`:

```text
PATREON_OAUTH_REDIRECT_URI=https://forkmesh.com/api/integrations/patreon/callback
```

Secrets in `.env.production` (published atomically by `./deploy.sh secrets`):

```text
PATREON_CLIENT_ID
PATREON_CLIENT_SECRET
PATREON_WEBHOOK_SECRET
PATREON_CAMPAIGN_ID
PATREON_CREATOR_ACCESS_TOKEN
```

`app/deploy.sh` publishes only allowlisted secret names, and these five are on
its list; `./deploy.sh validate-secrets` checks the file before a deploy.
Patreon is the only payment rail: the Polar integration was removed on
2026-09-22 (`dba3b4c7f`). ForkMesh
deploy workflows materialize the same names from secure node variables into
`app/.env.production`.

## Runtime behavior

- Authenticated users start Connect from Account settings or `/pricing`.
- OAuth state is random, account-bound, ten-minute, and single-use.
- Callbacks redirect to `/dashboard/settings/account?patreon=<outcome>`.
- Webhooks verify `X-Patreon-Signature` as hex HMAC-MD5 of the raw body.
- Matching order: linked Patreon user blind index, then email blind index.
- 1–999 entitled cents grants `supporting_member`.
- 1000–4999 cents grants `pro_member` + `supporting_member`.
- 5000–9999 cents additionally grants `builder_member`.
- 10000 cents and above additionally grants `studio_member`; higher tiers
  inherit every lower-tier role and therefore every lower-tier entitlement.
- Delete / zero entitlement revokes all membership roles after grace handling.
- The active membership roles also fund [ForkMesh Ads](ad-network.md) slots
  (Supporter 1, Pro 2, Builder 5, Studio 10 running ads). Nothing is written
  for this: the ad network reads the same `role_grants` rows at serve time, so
  a revoked or expired grant pauses that member's ads on its own.

## Secret rotation

1. Generate a new client secret and/or webhook secret in Patreon.
2. Update the matching ForkMesh variables / `.env.production` values.
3. Run `./deploy.sh secrets` (or a full deploy) so the atomic bulk update
   publishes every secret together.
4. Confirm `/api/accounts/patreon/oauth/start` returns an authorization URL and
   a test webhook is accepted (`ok: true`).
5. Revoke the old Patreon secret only after the new secret is live.

If `PATREON_CREATOR_ACCESS_TOKEN` is rotated, webhook reconciliation cannot
fetch member snapshots until the new creator token is published.

## Failure recovery

| Symptom | Likely cause | Recovery |
| --- | --- | --- |
| OAuth start returns 503 | Missing client ID/secret/campaign/redirect | Publish required secrets + vars |
| Callback `expired` / `replayed` | State TTL or retry | Start Connect again once |
| Callback `wrong_campaign` | Membership not on configured campaign | Confirm `PATREON_CAMPAIGN_ID` |
| Callback `duplicate_identity` | Patreon user already linked elsewhere | Unlink/support the prior account |
| Webhook 503 | Missing webhook secret | Publish `PATREON_WEBHOOK_SECRET` |
| Webhook accepted but roles unchanged | Creator token / campaign mismatch | Rotate creator token; confirm campaign |

Core Git, collaboration, mobile, and self-hosted paths stay available when
Patreon is misconfigured. Only Connect, webhook sync, and Pro-gated hosted
agent create/resume depend on these credentials.

## Privacy checklist

- Do not persist Patreon OAuth access or refresh tokens.
- Do not log emails, raw Patreon user IDs, or request bodies.
- Keep creator tokens and webhook secrets in the encrypted secret store only.
- Treat account `membership` projections as display metadata; authorization
  always comes from active `role_grants`.
