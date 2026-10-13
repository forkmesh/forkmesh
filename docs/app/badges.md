# Achievement badges

Badges are public recognition marks on a profile. The catalog is fixed; each
badge is awarded once per account, automatically at a real platform event or
by a platform administrator. An award is permanent: losing eligibility later
does not take it back.

## Code map

| Piece | Where |
|---|---|
| Catalog, slug validation, Distinguished Contributor threshold | `app/src/badges.py` |
| `_award_badge` (the one write path), `_send_badge_email`, `award_distinguished_contributor_badges` | `app/src/entry.py`, "Achievement badges" section |
| `GET /api/badges`, `GET/POST/DELETE /api/badges/<name>` | `badges_catalog_handler`, `account_badges_handler` in `app/src/entry.py` |
| Profile sidebar card | `data-profile-badges` in `app/public/dashboard/partials/main.html`, `renderProfileBadges` in `app/public/dashboard/js/04-account.js` |
| Hourly sweep | cron phase `badge-sweep` (`_cron_due_phases`, `_run_cron_phase`, `cron_activity.CRON_PHASE_CATALOG`) |
| Table | `badge_awards` (`app/migrations/0084_badge_awards.sql`, `app/src/schema.py`) |
| Tests | `app/tests/test_badges.py`, `app/tests/test_cron_runner.py` |

## Architecture

Every award goes through `_award_badge`, an `INSERT OR IGNORE` into
`badge_awards`. It returns `True` only when the row is new (`meta.changes`), and
only then calls `_send_badge_email`. A retry, a second event or the hourly sweep
finding an existing row therefore never mails twice.

### Distinguished Contributor

Earned by an active user account whose own `contributor_activity` row (the row
keyed by the account's `user_bi`) has at least
`DISTINGUISHED_CONTRIBUTOR_MIN` (15) issues, pull requests and discussions
combined. Commits are not counted: that column is always 0 (see
`_contribution_commit_totals`).

Rows keyed by a node key or an external signer are not counted, even when their
display name matches an account. The display name is whatever the signer put
in the event, so crediting it would let anyone claim an account's badge.

The badge is awarded by the `badge-sweep` cron phase, once an hour at minute
57. It reads at most `DISTINGUISHED_CONTRIBUTOR_SWEEP_LIMIT` (25) qualifying
accounts that do not have it yet, re-reads each account record, and checks the
blind index still matches (a renamed account's old row is skipped). One sweep
rather than a hook in `_record_contributor` keeps the rule in one place and also
covers accounts that crossed the line before the badge existed.

An administrator can also grant it by hand:
`POST /api/badges/<name>` with `{"slug": "distinguished_contributor"}`.

### Award email

`_send_badge_email` mails kind `badge` ("Badge awarded" in Mailtrap and the
#email-log channel). It is sent only to an active user account with a verified
email that has not switched email notifications off. The button links the
public profile (`/@name`), or `/dashboard/profile` when the profile is private.
`badge` is in `mail_tracking.UNSUBSCRIBE_KINDS`, so the mail carries one-click
unsubscribe and honours an earlier unsubscribe. It counts against the bulk mail
budget, not the account reserve.

The email covers every badge, not only Distinguished Contributor: the first
chat message, first issue, Discord link, hackathon awards and the rest all mail
the person when they are first earned.

### Profile

The sidebar on `/dashboard/profile`, `/@name` and the network page shows a
Badges card filled from `GET /api/badges/<name>`. It stays hidden when the
account has no badges. Answers are kept in memory for a minute per name, and a
reply for a profile the visitor has left is ignored.

## Operations

To see who would get Distinguished Contributor before a sweep runs, query
production read-only (`tools/next_node_owner.py` `Db.rows`):

```sql
SELECT u.username, c.issues, c.pulls, c.discussions
  FROM contributor_activity c JOIN users u ON u.user_bi=c.author_bi
  LEFT JOIN badge_awards b
    ON b.account_bi=c.author_bi AND b.badge_slug='distinguished_contributor'
 WHERE b.account_bi IS NULL AND c.issues+c.pulls+c.discussions>=15;
```

## Recent changes

- 2026-10-07: Distinguished Contributor badge, the hourly `badge-sweep`, award
  emails for every badge, and the profile Badges card. On 2026-10-07 only
  `spidertest` qualified (17 issues).
