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
| Achievements page | `/dashboard/achievements`: `partials/views/achievements.html`, `js/09-achievements.js` (lazy chunk); tests `app/tests/test_dashboard_achievements.py` |
| Badge pings | `badge_ping`, `_send_badge_ping` in `app/src/entry.py`; kind `badge` in `NOTIFICATION_KINDS` and `ping_settings.PING_KIND_CATALOG` |
| Locked progress | `refresh_badge_progress`, `_account_badge_progress` |
| Medallion artwork | `app/public/assets/badges/<slug>.svg`, one per catalog slug, plus `<slug>-bronze.svg`, `-silver` and `-gold` for a badge with levels |
| Medallion generator | `app/tools/build_badge_medallions.py` (`--check` fails when a file is stale) |
| Hourly sweep | cron phase `badge-sweep` runs `run_badge_sweep` |
| Activity badges | `BADGE_METRIC_SQL` (one query per badge), `award_metric_badges`, `award_daily_spinner_badges`, `badges.longest_streak` |
| Oracle | `_sweep_repo_discussion_answers`, `answer_record_fact`, `BADGE_METRIC_SQL["oracle"]`; table `badge_answer_facts` (migration `0267_badge_answer_facts`) |
| Record badges | `sweep_badge_records` and its `_sweep_repo_pull_records` / `_sweep_repo_issue_records`, `BADGE_FACT_SQL`, `award_fact_badges`, `_record_web_merge_badge_fact` (called by the web merge in `app/src/route_handlers.py`); readers `pull_record_fact`, `issue_record_fact`, `co_author_emails`, `has_review_file` in `badges.py` |
| Merge fields in `pull.md` | desktop `PullStore::mergePull` and the landed-pull sweep in `desktop/src/PullStore.cpp`; web merges in `executeMerge`, `server/merge.go` |
| Node uptime totals | `_credit_node_uptime`, called by `record_online_sample`; table `node_uptime_totals` (migration `0265_node_uptime_totals`) |
| Tables | `badge_awards` (`tier` 0-3, `tier_at`), `badge_progress` (best value seen per badge and account); migration `0264_badge_tiers`. `badge_pull_facts`, `badge_coauthor_facts`, `badge_repo_sweeps`; migration `0266_badge_pull_facts` |
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

## Achievements page

`/dashboard/achievements` lists every catalog badge. Earned badges come first,
newest first, each with its level medallion, a level chip, progress to the next
level and the date. Locked badges follow, greyed with a lock and sorted by how
close the member is: a progress bar once there is progress, otherwise "First
level at N". Badges flagged `hidden_until_earned` in the catalog (Founding
Member and the hackathon awards, which can no longer be earned on demand) are
shown only once earned; `GET /api/badges` returns them with
`hiddenUntilEarned`. The sidebar links the page under Profile, and the profile
Badges card links to it.

Locked progress comes from `refresh_badge_progress`: each hourly sweep writes
the value of every account with `0 < value < first threshold` for each metric
badge, in one `INSERT ... SELECT`, only where it grew. `GET /api/badges/<name>`
returns those rows as `progress`. Stargazer records its progress on every
star, and Daily Spinner shows only "First level at 7" until it is earned.

## Badge pings

Every new badge and every level-up also sends a ping of kind `badge`
(`_send_badge_ping`), deduplicated per badge and level (`badge:<slug>:<level>`),
titled "You earned the Stargazer badge" or "Your Stargazer badge is now
Silver", linking to the achievements page. Its `meta` is
`{celebrate: "badge", badge, badgeName, tier}`, so the existing celebration
overlays play on the web, desktop, iOS and Android, and the Genie spider
cheers and announces the badge on the desktop and iOS. Android has no Genie
spider; it shows the celebration overlay. The kind sits under Money & rewards
in Pings settings, with a trophy icon, and sends no email of its own because
the badge email already goes out.

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

## Activity badges

Ten badges are earned from data the server already stores per account. The
hourly `badge-sweep` runs one query per badge (`BADGE_METRIC_SQL`); each
query yields `(account_bi, value)`, and only accounts at or above the first
threshold whose value beats their stored progress come back, at most
`BADGE_SWEEP_LIMIT` (25) per badge per run, highest first. A row must match
an active user account by name and blind index before
`_record_badge_progress` raises its level. A node's activity counts for the
user that owns it (`nodes.user_bi`); an unowned node earns nothing.

| Slug | Name | Value | Levels |
|---|---|---|---|
| `six_eyes` | Six Eyes | sum of the `reviews` column of the user's public contribution days | 1 / 16 / 64 / 256 |
| `brood` | Brood | forks of the user's public repositories owned by someone else | 2 / 16 / 64 / 256 |
| `fan_club` | Fan Club | followers | 10 / 50 / 250 / 1000 |
| `night_watch` | Night Watch | hours the user's nodes were online, summed across nodes | 24 / 168 / 720 / 8760 |
| `daily_spinner` | Daily Spinner | longest run of consecutive days with a contribution | 7 / 30 / 100 / 365 |
| `bug_catcher` | Bug Catcher | paid `issue_report` microjobs plus paid `issue` contributor rewards | 1 / 10 / 50 / 200 |
| `lift_off` | Lift Off | release downloads of the user's public repositories | 10 / 100 / 1000 / 10000 |
| `shopkeeper` | Shopkeeper | distinct other accounts using any of the user's listed apps or published community packages | 10 / 100 / 1000 / 10000 |
| `critic` | Critic | visible marketplace reviews with a helpful vote from another account | 1 / 10 / 50 / 200 |
| `bounty_hunter` | Bounty Hunter | paid microjobs plus paid contributor rewards | 1 / 5 / 25 / 100 |

Rules and limits worth knowing:

- **Contribution days** (Six Eyes, Daily Spinner) use the same source
  selection as the profile graph: one active generation per project, from
  the most recently captured public source, so a project mirrored in several
  places counts once. `reviews` is the desktop-signed per-day figure the
  graph shows; the server only checks that it is a bounded integer.
- **Daily Spinner** is the longest run ever, not the current one. Candidates
  are users whose count of active days beats their progress, least recently
  checked first, so a run that does not grow cannot starve other users.
- **Night Watch** history starts when `0261` is deployed: the older
  `online_hourly_nodes` keeps only 48 hours and at most 50 nodes per sample,
  so it is not used. Every 10-minute sample credits 10 minutes to each live
  node key (repository hosts, healthy HTTPS mirrors and present accounts),
  up to `NODE_UPTIME_MAX_KEYS` (2000) keys, before the 50-node cap applies.
- **Brood and Lift Off** join through `repositories.key_bi`. Fork parents
  are stored lowercased and download rows use the URL's case, so a
  repository whose name has capitals can miss forks or downloads.
- **Lift Off** downloads are anonymous. One release file fetched by the same
  User-Agent on the same day counts once, which blunts reload loops but does
  not prove distinct people.
- **Bug Catcher and Bounty Hunter** add two separate payment pipelines, so
  work paid both as a microjob and as a contributor reward counts twice.
- **Shopkeeper** leaves out first-party ForkMesh packages and the
  publisher's own use.
- The first sweeps after deploy backfill existing activity 25 accounts per
  badge per hour, and each award or level-up sends the badge email.

## Record badges

Five badges come from each repository's own `.forkmesh/` records, which the
server does not otherwise keep: who merged a pull request, when, whether
anyone reviewed it, and who co-authored it.

| Slug | Name | Earned by | Levels |
|---|---|---|---|
| `web_weaver` | Web Weaver | pull requests merged into repositories the author does not own (GitHub: Pull Shark) | 2 / 16 / 128 / 1024 |
| `many_webs` | Many Webs | distinct repositories, not the author's own, with a merged pull request (Open Sourcerer) | 2 / 8 / 32 / 128 |
| `quick_silk` | Quick Silk | closed an issue, or merged a pull request, within 5 minutes of it being opened (Quickdraw) | one |
| `carpet_ride` | Carpet Ride | merged a pull request that had no review, or merged it with the owner's review waiver (YOLO) | one |
| `eight_hands` | Eight Hands | merged pull requests whose commits name you in a `Co-authored-by` trailer (Pair Extraordinaire) | 1 / 10 / 24 / 48 |

### What the merge writes

A merge adds fields to the pull's `pull.md` header, outside the author's
signature like `status` and `mergeBase`:

- `mergedAt`: milliseconds since the epoch.
- `mergedBy`: the merging node's key. Only the desktop writes it; a web merge
  runs on a mirror that is never told who asked, so the relay stores the
  merging account itself (below). A pull the desktop finds landed outside
  ForkMesh gets `mergedAt` (the time the ledger noticed) but no merger.
- `coAuthors`: the sorted, lowercased emails from `Co-authored-by` trailers
  in the merged commits (or the mbox of a patch-backed pull), at most 16.

The desktop deletes a pull it closes, so a closed, unmerged pull leaves no
record and the pull side of Quick Silk is merges only. Issues already carry
signed `open` and `status` events with times and signers, so they need no
format change.

### The sweep

The hourly `badge-sweep` phase first runs `sweep_badge_records`:

1. **Which repositories**: public repositories whose newest
   `repo_state_history` pin is newer than the state they were last fully swept
   at (`badge_repo_sweeps.state_ts`), least recently swept first, at most
   `BADGE_RECORD_REPOS_PER_RUN` (4) per run. A web merge resets the
   repository's `state_ts` so the next sweep reads it.
2. **Pulls**: the `.forkmesh/pulls` tree on the `forkmesh/pulls` ledger
   branch, then one `blobs` read for at most `BADGE_RECORD_BATCH` (50) records
   that are not settled yet, least recently checked first. Per-pull branches
   that the index has not caught up with are left for a later sweep. A merged
   pull with a known merger and no review waiver costs one more `tree` read of
   its folder to look for `NNNN-review.md` event files, at most
   `BADGE_RECORD_REVIEW_CHECKS` (10) per repository per run.
3. **Issues**: the `.forkmesh/issues/closed` tree and one `blobs` read for at
   most 50 closed issues not yet stored. The pre-split layout (numbered
   folders directly under `.forkmesh/issues`) is not read.
4. **Settled**: an issue row, or a pull row that is merged, is never read
   again; an open pull is re-read on later sweeps of that repository. A
   repository's `state_ts` only advances when every pending record fit in the
   batch, every read answered and every review check ran, so a large or
   unreachable repository comes back on the next run. A failed read of the
   pull tree alone counts as "no pull ledger", because the gateway answers a
   missing `forkmesh/pulls` branch the same way it answers an outage.

Each record is one `badge_pull_facts` row keyed by `(repo_bi, kind, number)`
(the table name predates issues sharing it), so reading a record twice never
counts it twice. Keys are stored as written; the queries map them to
accounts through `nodes.pubkey` and `COALESCE(NULLIF(user_bi, ''),
node_bi)`, so a node linked to its account later still earns, and an
unowned node earns nothing. Rows from private repositories never count.

### How each badge counts

- **Web Weaver / Many Webs** (`BADGE_FOREIGN_MERGES_SQL`): merged pulls whose
  author account differs from the repository owner (a node-owned repository
  counts as its account's). Organisation repositories count for every author,
  members included. Web Weaver counts distinct `(repository, number)`, Many
  Webs distinct repositories.
- **Quick Silk**: a stored record whose close or merge time is 0 to 300,000 ms
  after it was opened. The earner is the closer: the signer of the issue's
  first closing `status` event, the desktop's `mergedBy`, or the account the
  relay recorded for a web merge. Open and close times come from different
  machines' clocks, so skew can move a borderline case either way.
- **Carpet Ride**: a merged pull with no review event of any kind, or one the
  owner merged on the web with `overrideReview`. The earner is the merger; a
  pull with no known merger earns nothing. Desktop merges made before
  `mergedBy` existed are never attributed.
- **Eight Hands**: each `coAuthors` email of a newly stored merged pull is
  matched to a user whose blind-indexed email equals it, whose stored email
  is verified and equal, and who is active. The pull's author is left out.
  One `badge_coauthor_facts` row per pull and account, so the value is the
  number of merged pull requests co-authored, as on GitHub.

Web Weaver, Many Webs and Eight Hands are `BADGE_METRIC_SQL` queries and go
through `_record_badge_progress` like the activity badges. Quick Silk and
Carpet Ride are one-level `BADGE_FACT_SQL` queries that `award_fact_badges`
passes to `_award_badge`, at most `BADGE_SWEEP_LIMIT` (25) accounts each per
run. Both kinds run after the record sweep in the same phase, so a record
read this hour can award this hour.

Not covered yet: pull records written by the phones or the MCP tool carry no
`mergedAt`, `mergedBy` or `coAuthors` until those writers add them, and the
new Go desktop (`desktop_go_new`) does not write them either.

## Oracle

`oracle` (2 / 8 / 16 / 32 accepted answers, GitHub's Galaxy Brain): replies
of yours that were marked as the answer to a Q&A discussion. Marking an
answer is described in [discussion-answers.md](discussion-answers.md).

The record sweep reads each changed repository's `.forkmesh/discussions`
tree (default branch). Every discussion folder's tree `oid` is stored in
`badge_answer_facts` (migration `0267_badge_answer_facts`, one row per
repository and discussion), so a folder is read again only when something in
it changed, at most 25 discussions (`BADGE_RECORD_BATCH // 2`) per
repository per run in one `blobs` read of `discussion.md` and `answer.md`.
`answer_record_fact` keeps the answer only for a Q&A category and a non-empty
target, and takes the answerer from `targetAuthor`. Clearing or moving an
answer rewrites the row, so the value can fall; the badge level never does.

`BADGE_METRIC_SQL["oracle"]` counts, per answerer account (through
`nodes.pubkey`), the discussions in public repositories whose answer they
wrote, leaving out answers to their own question (same key, or the same
account). A reply signed with a browser's per-browser key maps to no account
and earns nothing.

The first pull request, issue and discussion stay the existing Coder, QA and
Poster badges.

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
| `six_eyes` | the Genie peering through a gold magnifying glass |
| `brood` | a silk egg sac with baby Genies hatching |
| `fan_club` | the Genie with heart eyes and blush, hearts around it |
| `night_watch` | a crescent moon with a glowing lantern on a thread |
| `daily_spinner` | a flame ringed by a silk orbit |
| `bug_catcher` | a ladybird caught in a web, silk across it |
| `lift_off` | a rocket with a violet nose, the Genie's eyes in the porthole |
| `shopkeeper` | an open treasure chest spilling gems |
| `critic` | a parchment of stars and the Genie's feather as a quill |
| `bounty_hunter` | the Genie in a leather eye patch with a gold coin |
| `web_weaver` | the Genie in the middle of a glowing web |
| `many_webs` | a globe wrapped in web lines, the Genie dangling on top |
| `quick_silk` | the Genie dropping on a thread, stopwatch beside it |
| `carpet_ride` | the Genie in pink sunglasses on the magic carpet |
| `eight_hands` | two spider claws meeting in a high five |
| `oracle` | a crystal ball holding a galaxy, on a gold stand |

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
