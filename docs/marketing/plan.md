# ForkMesh marketing plan

The living plan the Marketing special agent keeps (charter:
[README.md](README.md)). This page defines its structure, metrics and
rules; the agent fills in the values on each run. Every number names its
source and the moment it was read. "No data" means nobody measured it; it
never means zero.

## North star

**Activated accounts:** accounts that have published at least one repository
(`first_mirror_created`, [../attribution.md](../attribution.md); "accounts
with at least one mirror" on `/product-analysis`).

- **Drivers.** New signups, the share of signups that publish a first
  repository, repositories per activated account, and accounts still active
  after 30 days.
- **Guardrails.** Bot-filtered sessions stay excluded; no fake or bought
  activity; signups with no session stay `unknown`, never guessed;
  unsubscribe and complaint counts once any email is sent; zero moderation or
  private-data incidents from a campaign.

## Funnel

| Stage | Event | Source |
| --- | --- | --- |
| Acquisition | `landing_view` sessions by first-touch channel, bot-filtered | `/admin/acquisition` (admin session only) |
| Signup | `signup_completed`; public total `usage.users.total` | `/admin/acquisition`; `https://forkmesh.com/nodeinfo/2.1` |
| Activation | `first_mirror_created`; first mirror within 7 days of signup | `/admin/acquisition`; "New accounts activated" on `/product-analysis` |
| Retention | Accounts seen in the last 30 days that signed up 30+ days earlier | "Returning accounts" on `/product-analysis` |
| Referral | Referral-link signups; `contribution_made` | Public `referrals` leaderboard; `/admin/acquisition` |
| Revenue | Money collected from paid plans | The finance review |

`/admin/acquisition` and `/product-analysis` need an admin browser session; a
signed device request is not accepted. Without an operator export, public
proxies stand in and are labelled as such. The weekly export is the Overview
figures of `/product-analysis` plus the first-touch channel table from
`/admin/acquisition`, or the Marketing package's report at
`/api/marketing/<org>/report`.

### Weekly values

One row per run: date, source of each figure, and the value for each stage.

## Positioning

Ideal user profiles, the problem each has, the alternatives they use, the
proof points (each traced to released code or docs) and the message for each
profile.

## Experiment backlog

| Idea | Stage | Impact | Confidence | Ease | Score | Status |
| --- | --- | --- | --- | --- | --- | --- |

Scores are impact × confidence × ease (1-10 each); the top of the backlog
runs first.

## Campaigns

One brief per active or finished campaign in `docs/marketing/campaigns/`,
with audience, problem, message, channel, call to action, owners, UTM-tagged
links on the `ATTRIBUTION_ALLOWED_*` allowlists, a success measure with a
baseline, start and end dates, a follow-up review, and the result.

## Email channel

The admin console's **Marketing email** section
([code](../app/marketing-email.md)) sends to accounts that already exist.

- **Audience.** Only accounts that turned on **Product updates and offers**
  (unchecked box at signup, or the switch in email settings) and have a
  verified address. Nobody is added by hand, imported or bought.
- **Types.** Product update (monthly), release note (only for a release that
  changes how people use ForkMesh), promotion (only with an operator-approved
  offer, its terms and end date in the email). At most three emails a month,
  never two in one week.
- **Rules.** The operator approves every send; Marketing prepares the draft,
  tagged link and test send. Links are tagged
  `utm_source=email&utm_medium=email&utm_campaign=product_update` (or
  `promotion`) with `utm_content=<yyyy-mm>` or the offer's slug. The tool
  adds the reason for sending, unsubscribe and settings links and the postal
  address. Pacing is about 12 emails an hour and 100 a day.

| Measure | Healthy | Stop and review |
| --- | --- | --- |
| Subscribers | Growing month over month | Shrinking two months running |
| Clicks ÷ sent | 2% or more | Under 1% twice running: change the content |
| Unsubscribes ÷ sent | Under 1% | Over 2%: pause and review |
| Complaints ÷ sent | Under 0.1% | 0.3% or more: stop the channel until the cause is found |
| Bounces ÷ sent | Under 2% | Over 5%: check suppression and DNS first |
| Signups and first mirrors from `utm_source=email` | Any | None after three sends: rethink the call to action |

With fewer than 50 subscribers, rates are reported as counts.

## Calendar

Launches, blog posts and events for the next eight weeks.

| Date | What |
| --- | --- |
| 16 October 2026 | Plugin Race and affiliate launch posts, tagged `#ForkMeshPluginRace` and `utm_campaign=plugin_race` ([brief](campaigns/plugin-race-and-affiliates-2026-10-16.md)) |
| 19 October 2026 | The Plugin Race opens |
| 18 December 2026 | The Plugin Race closes, unless all ten prizes are won first |

## Log

One dated entry per run.

- **2026-10-15.** The September Revenue Contest closed and moved to
  /hackathon/archive. /hackathon is now the Plugin Race, and the commission
  carries on as the affiliate program at /affiliates. Both launch together
  under one tracking hashtag, `#ForkMeshPluginRace`
  ([brief](campaigns/plugin-race-and-affiliates-2026-10-16.md)). Nothing is
  posted until the brief's gates pass.

- **2026-10-15.** Herald's ready X post is the core-retirement story from
  the 2026-10-14 blog. "Is your repo mirrored?"
  ([brief](campaigns/repo-mirrored-2026-10-09.md)): gate 1 passed (`/mirror`
  answers 200 with the right page); gate 2, the test-account copy and
  credential-free clone, has not been run, so the X draft stays unposted
  and Herald does not make it the ready post yet.

- **2026-10-30.** Herald reported X numbers stalled since 10-19 because X shows
  "not connected". Read at 16:11 UTC, that mixes two things: "connected" is
  only the API keys for posting from Marketing; the numbers come from the
  browser extension's X profile sync, last run 2026-10-30 16:11 UTC (217
  followers; the five 10-29 posts have 18 to 96 views). The 10-19 post went
  out with the operator's own text as status 2111971537762893976 (22 views, 2
  likes, 1 repost); its ready-post record was closed with no URL, which is why
  it looked unmeasured. The 10-21 "agents context" post still reads 0 views:
  the sync only updates posts on screen, so scroll the profile past 10-21
  before **Sync now**. RM1: gate 1 still passes, gate 2 still needs the
  operator's test-account copy (steps in the
  [brief](campaigns/repo-mirrored-2026-10-09.md)). Plugin Race: three of six
  gates now pass; the race has been open since 10-19 with 0 entrants and no
  launch post, so the owner's three gates are the campaign's bottleneck
  ([brief](campaigns/plugin-race-and-affiliates-2026-10-16.md)).
