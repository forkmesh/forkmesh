# Patreon membership operations

Patreon is the membership payment rail for the Supporter, Pro, Builder, and
Studio tiers. The server never sees card data. Member OAuth tokens are
discarded right after identity lookup and are never stored or logged.

## Application setup

1. Create a Patreon creator application for the campaign with OAuth scopes
   `identity`, `identity[email]`, and `identity.memberships`.
2. Register the callback `https://<your-origin>/api/integrations/patreon/callback`.
3. Point a webhook at `https://<your-origin>/api/integrations/patreon/webhook`
   for `members:pledge:create`, `members:pledge:update`, and
   `members:pledge:delete`.
4. Record the numeric campaign ID and a creator-scoped access token that can
   read `GET /api/oauth2/v2/members/{id}`.

## Configuration

Non-secret variable: `PATREON_OAUTH_REDIRECT_URI`. Secrets, published together
by `./deploy.sh secrets` (checked by `./deploy.sh validate-secrets`):

```text
PATREON_CLIENT_ID
PATREON_CLIENT_SECRET
PATREON_WEBHOOK_SECRET
PATREON_CAMPAIGN_ID
PATREON_CREATOR_ACCESS_TOKEN
```

## Runtime behavior

- OAuth state is random, account-bound, single-use, and valid ten minutes.
- Callbacks redirect to `/dashboard/settings/account?patreon=<outcome>`.
- Webhooks verify `X-Patreon-Signature` (hex HMAC-MD5 of the raw body).
- Members are matched by linked Patreon user, then by email blind index.
- Entitled cents map to roles: 1–999 `supporting_member`; 1000–4999 adds
  `pro_member`; 5000–9999 adds `builder_member`; 10000+ adds `studio_member`.
  Higher tiers inherit every lower role.
- Deletion or zero entitlement revokes the membership roles after grace.
- Membership roles also fund [ForkMesh Ads](ad-network.md) slots, read from the
  same `role_grants` at serve time.

## Secret rotation

Generate the new secret in Patreon, update the deployment secrets, run
`./deploy.sh secrets`, confirm OAuth start and a test webhook succeed, then
revoke the old secret. Until a rotated creator token is published, webhook
reconciliation cannot fetch member snapshots.

## Failure recovery

| Symptom | Likely cause | Recovery |
| --- | --- | --- |
| OAuth start returns 503 | Missing client, campaign, or redirect config | Publish the secrets and variable |
| Callback `expired` / `replayed` | State TTL or retry | Start Connect again |
| Callback `wrong_campaign` | Membership on another campaign | Check `PATREON_CAMPAIGN_ID` |
| Callback `duplicate_identity` | Patreon user linked to another account | Unlink the prior account |
| Webhook 503 | Missing webhook secret | Publish `PATREON_WEBHOOK_SECRET` |
| Webhook accepted, roles unchanged | Creator token or campaign mismatch | Rotate the token; check the campaign |

Git, collaboration, mobile, and self-hosted paths never depend on Patreon;
only Connect, webhook sync, and membership-gated hosted features do.
Authorization always comes from active `role_grants`; the account `membership`
projection is display metadata.
