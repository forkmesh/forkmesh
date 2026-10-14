# Achievement badges

Badges are public recognition marks on a profile. The catalog is fixed; each
badge is awarded once per account, automatically at a platform event or by a
platform administrator. An award is permanent. Some badges have levels
(default, bronze, silver, gold); a level only ever goes up.

## Code map

| Piece | Where |
|---|---|
| Catalog, slug validation, thresholds, levels (`tiers`, `tier_for_value`, `tier_summary`) | `app/src/badges.py` |
| `_award_badge`, `_record_badge_progress`, `_send_badge_email`, `run_badge_sweep` and its sweeps | `app/src/entry.py` |
| Stargazer hook | `_award_stargazer_for_repo`, called by `repo_star_handler` after a new star |
| Genie's Gift hook | `_award_genies_gift`, called by `_account_tip_notice` after the tip row is written |
| `GET /api/badges`, `GET/POST/DELETE /api/badges/<name>` | `badges_catalog_handler`, `account_badges_handler` |
| Profile card | `data-profile-badges`, `renderProfileBadges`, `profileBadgeMedal` in `app/public/dashboard/js/04-account.js` |
| Medallion artwork | `app/public/assets/badges/<slug>.svg`, one per catalog slug, plus `<slug>-bronze.svg`, `-silver` and `-gold` for a badge with levels |
| Medallion generator | `app/tools/build_badge_medallions.py` (`--check` fails when a file is stale) |
| Hourly sweep | cron phase `badge-sweep` runs `run_badge_sweep` |
| Tables | `badge_awards` (`tier` 0-3, `tier_at`), `badge_progress` (best value seen per badge and account); migration `0264_badge_tiers` |
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

## Levels

A badge with `tiers` in the catalog has four thresholds, for default,
bronze, silver and gold, and a `unit`. `_record_badge_progress` keeps the
best value seen in `badge_progress` (`MAX`, so an unstar never lowers it)
and awards the highest level that value reaches. `_award_badge` inserts the
row at that level, or raises an existing row with `UPDATE ... WHERE tier<?`;
a new row or a raise sends the badge email, whose subject names the new
level ("Your Stargazer badge is now Gold on ForkMesh"). An award can skip
levels, for example when the sweep first sees a repository with 600 stars.

`GET /api/badges/<name>` adds `tier`, `tierLevel`, `tierAt`, `progress`,
`unit`, `tiers`, `nextTier` and `nextThreshold` to a badge with levels. The
profile loads `<slug>-<tier>.svg` (falling back to `<slug>.svg`), shows the
level as a chip after the name, and a line such as "640 stars · Gold at
4,096".

## Stargazer and Genie's Gift

- **Stargazer** (16 / 128 / 512 / 4096 stars, GitHub's Starstruck
  thresholds): the stars on one public repository a user account owns. The
  owner's own star does not count. Organisation-owned repositories earn
  nothing yet. A new star checks that repository at once; the hourly sweep
  backfills repositories that reached a threshold before the hook existed,
  at most `BADGE_SWEEP_LIMIT` (25) per run.
- **Genie's Gift** (one level): paid another account through a tip that the
  server confirmed on chain, on mainnet. A plain tip is only the sender's
  claim, so it does not count; a Ledger work payment (`context["task"]`) or a
  paid contributor reward does. The sweep backfills from `tip_notice` rows
  with a task, or whose signature is a `paid` `contributor_reward_estimates`
  row.

## Badges planned next

Proposed in October 2026. Medallions for all of them are already drawn by
the generator; none is in the catalog yet, so none can be awarded until its
earning code lands. "Ready" means the server already stores the data in a
table keyed to an account; "Sweep" needs the sweep over each repository's
`.forkmesh/` records; "Feature" needs a product feature first.

| Slug | Name | Earned by | Levels | Data | Medallion |
|---|---|---|---|---|---|
| `web_weaver` | Web Weaver | pull requests merged into repositories the author does not own (GitHub: Pull Shark) | 2 / 16 / 128 / 1024 | Sweep | the Genie in the middle of a glowing web |
| `many_webs` | Many Webs | distinct repositories, not the author's own, with a merged pull request (Open Sourcerer) | 2 / 8 / 32 / 128 | Sweep | a globe wrapped in web lines, the Genie dangling on top |
| `six_eyes` | Six Eyes | pull request reviews (`profile_contribution_days.reviews`) | 1 / 16 / 64 / 256 | Ready | the Genie peering through a gold magnifying glass |
| `quick_silk` | Quick Silk | closed an issue or pull request within 5 minutes of opening it (Quickdraw) | one | Sweep, plus `closedAt` in `pull.md` | the Genie dropping on a thread, stopwatch beside it |
| `carpet_ride` | Carpet Ride | merged a pull request without a review (YOLO) | one | Sweep, or the web merge `overrideReview` flag | the Genie in pink sunglasses on the magic carpet |
| `eight_hands` | Eight Hands | co-authored commits on merged pull requests (Pair Extraordinaire) | 1 / 10 / 24 / 48 | Sweep, plus `Co-authored-by` parsing | two spider claws meeting in a high five |
| `oracle` | Oracle | accepted answers in discussions (Galaxy Brain) | 2 / 8 / 16 / 32 | Feature: mark an answer | a crystal ball holding a galaxy, on a gold stand |
| `brood` | Brood | forks of your repositories by other accounts (`repo_forks`) | 2 / 16 / 64 / 256 | Ready | a silk egg sac with baby Genies hatching |
| `fan_club` | Fan Club | followers (`profile_follows`) | 10 / 50 / 250 / 1000 | Ready | the Genie with heart eyes and blush, hearts around it |
| `night_watch` | Night Watch | hours your nodes were online (`online_hourly_nodes`) | 24 / 168 / 720 / 8760 | Ready, joined through node ownership | a crescent moon with a glowing lantern on a thread |
| `daily_spinner` | Daily Spinner | longest run of days with a contribution (`profile_contribution_days`) | 7 / 30 / 100 / 365 | Ready | a flame ringed by a silk orbit |
| `bug_catcher` | Bug Catcher | issue reports accepted and paid (`microjob_claims`, `contributor_reward_estimates`) | 1 / 10 / 50 / 200 | Ready | a ladybird caught in a web, silk across it |
| `lift_off` | Lift Off | downloads of your releases (`release_downloads`) | 10 / 100 / 1000 / 10000 | Ready | a rocket with a violet nose, the Genie's eyes in the porthole |
| `shopkeeper` | Shopkeeper | accounts using your marketplace package (`marketplace_package_uses`) | 10 / 100 / 1000 / 10000 | Ready | an open treasure chest spilling gems |
| `critic` | Critic | marketplace reviews that another account voted helpful (`marketplace_review_votes`) | 1 / 10 / 50 / 200 | Ready | a parchment of stars and the Genie's feather as a quill |
| `bounty_hunter` | Bounty Hunter | paid microjobs and bounties (`microjob_claims` status `paid`) | 1 / 5 / 25 / 100 | Ready | the Genie in a leather eye patch with a gold coin |

The first pull request, issue and discussion stay the existing Coder, QA and
Poster badges. Eight of the eighteen generated medallions show the Genie in
a costume or expression; the rest are objects, so a profile full of badges
is not a wall of the same face.

## Medallions

The profile card shows each badge as an SVG medallion in place of the
catalog's emoji (the `icon` field stays in the API for other clients). Every
slug in `BADGE_DEFINITIONS` has a file `app/public/assets/badges/<slug>.svg`;
a new badge needs its medallion in the same change.

The first fifteen medallions are hand-drawn and follow GitHub's achievement
badges: a 128-unit square with a rim
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

### Generated medallions

Medallions from Stargazer on are drawn by `app/tools/build_badge_medallions.py`
and must not be edited by hand; change the script and run it. The look is
ForkMesh's own rather than GitHub's:

- **Frame**: a rim etched with a spider web (16 spokes, two sagging rings),
  a highlight arc and bevel hairlines. The default level uses the badge's own
  colour; bronze, silver and gold swap only the frame metal, so the picture
  stays in full colour at every level.
- **Level gems**: one, two or three teal gems at the bottom of the rim, so a
  level reads without relying on colour; gold also gets four rim glints.
- **Subject**: one bold picture on a bright gradient. Not every badge shows
  the Genie spider. When it does, it is the pet's own painter
  (`www/public/genie-spider.js`) ported to SVG, wearing an expression or
  costume for that badge (`spider(svg, pose)`: `bust`, `eyes`, `mouth`,
  `blush`, `raised`). Other badges are an object.
- The fine detail of the spider is thinner than 2.5 units; the subject's
  silhouette is what has to read at 32px.

| Slug | Medallion |
|---|---|
| `stargazer` | the Genie up close and starstruck: six gold star eyes, open grin, pink blush, on a violet sunburst |
| `genies_gift` | no spider: the golden lamp, its smoke curling up into a pink heart, on teal |

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
