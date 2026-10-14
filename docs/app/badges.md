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
| Profile card | `data-profile-badges`, `renderProfileBadges`, `profileBadgeMedal` in `app/public/dashboard/js/04-account.js` |
| Medallion artwork | `app/public/assets/badges/<slug>.svg`, one per catalog slug |
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

## Medallions

The profile card shows each badge as an SVG medallion in place of the
catalog's emoji (the `icon` field stays in the API for other clients). Every
slug in `BADGE_DEFINITIONS` has a file `app/public/assets/badges/<slug>.svg`;
a new badge needs its medallion in the same change.

The art follows GitHub's achievement badges: a 128-unit square with a rim
(gradient ring, dark outer hairline so it holds an edge on white, light inner
hairline so it reads on `#0d1117`), a face disc and one bold glyph, using two
or three colours per badge. Strokes are at least 2.5 units so the glyph stays
legible when drawn at 32px. Distinguished Contributor uses a shield instead
of a circle to mark it as the rarer award.

| Slug | Medallion |
|---|---|
| `first_100_users` | gold rim, navy face, laurel wreath around a star and "100" |
| `world_first_hour` | blue rim, globe with continents and a gold orbit |
| `first_referral` | violet rim, two joined chain links |
| `mirror_operator` | cyan rim, a server and its mirrored reflection |
| `qa` | amber rim, magnifying glass over a bug |
| `coder` | green rim, dark face, `</>` |
| `discord` | indigo rim, headset with microphone (no Discord logo) |
| `poster` | orange rim, pinned note with text lines |
| `chat` | pink rim, speech bubble with three dots |
| `distinguished_contributor` | silver shield, purple face, gold star over two chevrons |
| `hackathon_builder` | copper rim, crossed hammer and wrench |
| `hackathon_connector` | teal rim, hub joined to six nodes in a ring |
| `hackathon_finalist` | silver rim, laurel wreath around a silver "2" coin |
| `hackathon_ambassador` | red rim, megaphone with sound waves |
| `hackathon_winner` | gold rim, crimson face, gold trophy |

The image is requested through `ForkMeshAPI.url()`, because the public
profile on forkmesh.com is proxied from the App while `/assets/` there is the
marketing site's own folder, which does not carry the medallions. If that
fails it retries the same-origin path, then falls back to a plain grey disc
with the badge's initial, so a relay without the files still renders the
card.

## Operations

Preview who the next sweep would award (read-only):

```sql
SELECT u.username, c.issues, c.pulls, c.discussions
  FROM contributor_activity c JOIN users u ON u.user_bi=c.author_bi
  LEFT JOIN badge_awards b
    ON b.account_bi=c.author_bi AND b.badge_slug='distinguished_contributor'
 WHERE b.account_bi IS NULL AND c.issues+c.pulls+c.discussions>=15;
```
