# Achievement badges

Badges are public recognition marks on a profile. The catalog is fixed; each
badge is awarded once per account, automatically at a platform event or by a
platform administrator. An award is permanent.

## Code map

| Piece | Where |
|---|---|
| Catalog, slug validation, thresholds | `app/src/badges.py` |
| `_award_badge`, `_send_badge_email`, `award_distinguished_contributor_badges` | `app/src/entry.py` |
| `GET /api/badges`, `GET/POST/DELETE /api/badges/<name>` | `badges_catalog_handler`, `account_badges_handler` |
| Profile card | `data-profile-badges`, `renderProfileBadges` in `app/public/dashboard/js/04-account.js` |
| Hourly sweep | cron phase `badge-sweep` |
| Table | `badge_awards` |
| Tests | `app/tests/test_badges.py`, `app/tests/test_cron_runner.py` |

## Rules

- Every award goes through `_award_badge`, an `INSERT OR IGNORE`; the email
  is sent only when the row is new, so retries never mail twice.
- **Distinguished Contributor**: an active user account whose own
  `contributor_activity` row (keyed by its `user_bi`) has at least
  `DISTINGUISHED_CONTRIBUTOR_MIN` (15) issues, pull requests and discussions.
  Rows keyed by a node key or external signer never count, since their
  display name is unverified. The `badge-sweep` phase awards at most
  `DISTINGUISHED_CONTRIBUTOR_SWEEP_LIMIT` (25) per run and re-checks each
  account's blind index. Admins can grant it with
  `POST /api/badges/<name>` `{"slug": "distinguished_contributor"}`.
- **Email**: kind `badge`, only to active accounts with a verified address
  and notifications on; it is in `mail_tracking.UNSUBSCRIBE_KINDS` and counts
  against the bulk mail budget.
- **Profile**: the Badges card is hidden when there are none; answers are
  cached in memory for a minute per name.

## Operations

Preview who the next sweep would award (read-only):

```sql
SELECT u.username, c.issues, c.pulls, c.discussions
  FROM contributor_activity c JOIN users u ON u.user_bi=c.author_bi
  LEFT JOIN badge_awards b
    ON b.account_bi=c.author_bi AND b.badge_slug='distinguished_contributor'
 WHERE b.account_bi IS NULL AND c.issues+c.pulls+c.discussions>=15;
```
