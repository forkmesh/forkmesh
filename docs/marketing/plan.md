# ForkMesh marketing plan

The living plan the Marketing special agent keeps (charter:
[README.md](README.md)). Every number below names its source and the moment it
was read. "No data" means nobody measured it; it never means zero.

## North star

**Activated accounts:** accounts that have published at least one repository
(`first_mirror_created`, `docs/attribution.md`; "accounts with at least one
mirror" on `/product-analysis`).

- **Why it predicts growth.** ForkMesh's value is a repository that lives on
  more than one host. An account with no repository has received none of it,
  and every later stage (contribution, referral, a paid plan) starts from a
  repository.
- **Drivers.** New signups, the share of signups that publish a first
  repository, repositories per activated account, and accounts still active
  after 30 days.
- **Guardrails.** Bot-filtered sessions stay excluded; no fake or bought
  activity; signups with no session stay `unknown`, never guessed;
  unsubscribe and complaint counts once any email is sent; zero moderation or
  private-data incidents from a campaign.

## Funnel

The activation event is `first_mirror_created` and stays the same week to
week.

| Stage | Event | Source |
| --- | --- | --- |
| Acquisition | `landing_view` sessions by first-touch channel, bot-filtered | `/admin/acquisition` (admin session only) |
| Signup | `signup_completed`; public total `usage.users.total` | `/admin/acquisition`; `https://forkmesh.com/nodeinfo/2.1` |
| Activation | `first_mirror_created`; every account: first mirror within 7 days of signup | `/admin/acquisition`; "New accounts activated" on `/product-analysis` |
| Retention | Accounts seen in the last 30 days that signed up 30+ days earlier | "Returning accounts" on `/product-analysis` |
| Referral | Referral-link signups; `contribution_made` | Public `referrals` leaderboard; `/admin/acquisition` |
| Revenue | Money collected from paid plans | [CFO review](../finance/cfo.md): **$0**, because production cannot collect payments |

Marketing cannot open `/admin/acquisition` or `/product-analysis`: both need
an admin browser session, and a signed device request is not accepted. Until
the operator supplies the weekly export (decision D1), the public proxies
below stand in, and each is labelled as one.

The two `/product-analysis` figures above were added on branch
`1380-marketing-persistent-marketing`, merged into main at `ab3f672f31` and
deployed with `cffabd181d88` on 2026-10-04 22:25 UTC. The weekly export is
three figures from the Overview page (Total accounts with its 30-day change, and the "N of M" lines of New
accounts activated and Returning accounts) plus the first-touch channel
table from `/admin/acquisition`.

### Weekly values

| Week (Mon) | Acquisition | Signup (public total) | Activation (proxy) | Retention (proxy) | Referral | Revenue |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-10-05 (read 2026-10-09, NodeInfo only) | Not read | 235 accounts (flat; served stack at `cabbb8e3f599`, main HEAD) | Not read | Not read | Not read | **$0** (unchanged) |
| 2026-10-05 (read 2026-10-06 05:51Z) | No weekly data. All-time referring sites outside ForkMesh's own hosts: google.com 225 (+1, last visit 2026-10-06 04:01Z), wellfound.com 291, t.co 22, chatgpt.com 18 | 235 accounts (flat; served stack at `6c985aea6750`) | **0 non-operator accounts with a public repository.** The 4 owners on the `repos` board are the three core nodes (`fmnext-n1` to `fmnext-n3`) and the operator's account | 2 members active in the last 3 days (flat), one of them the operator's account | 633 referral-link clicks all time (flat), 0 attributed signups | **$0**; production cannot collect payments ([CFO review](../finance/cfo.md)) |
| 2026-10-05 (first read 2026-10-06 03:47Z) | google.com 224 (last visit still 2026-10-04), wellfound.com 291, t.co 22, chatgpt.com 18 | 235 accounts (flat) | 4 owners with a public repository (flat; see the correction above) | 2 members active in the last 3 days (flat) | 633 referral-link clicks all time (+2), 0 attributed signups | No data |
| 2026-09-28 (baseline, read 2026-10-04 18:05Z) | No weekly data. All-time referring sites: wellfound.com 291 (last visit 2026-08-11), google.com 224 (last 2026-10-04), t.co 22, chatgpt.com 18, nostter.app 6, search.brave.com 5, daily.dev 3 | 235 accounts; 235 on 2026-09-30, 240 on 2026-09-16 | 4 owners with a public repository (public `repos` board; private repositories not counted) | 2 members with activity in the last 3 days on the public activity board, one of them the operator's account | 631 referral-link clicks all time, 0 attributed signups, 52 legacy signups from before attribution | No data |

The numbers are too small to rank channels or read a conversion rate. The
referring-site counts are all-time totals, not this week's.

The second run on 2026-10-04 read no new numbers: network reads (`curl`,
web fetch) needed an approval the run did not have. The 2026-10-06 reading
is about 34 hours after the baseline, so "flat" says only that nothing moved
in a day and a half; the A1 change went live with it and has no readout yet.

**Tightest bottleneck: activation.** No account other than the operator's
and the three core nodes shows a public repository (private repositories are
not on the board, so this is a lower bound). The 2026-09-28 baseline counted
the core nodes and the operator as 4 owners; from 2026-10-06 the activation
proxy counts non-operator owners only. Accounts are flat, and referral clicks
have produced no attributed signups. Fix activation before buying or chasing
traffic.

With the account total flat since 2026-09-30, A1's 20-signup threshold will
not be reached in its window. Single-change readouts are therefore out of
reach, and fixes to the same first-repository path go out together and are
read as one bundle on `/product-analysis` (decided 2026-10-06, see the A1
write-up).

## Positioning

**Promise** (`www/public/business-model.json`): ForkMesh lets developers
collaborate on, mirror, and preserve source-code repositories across
independent hosts, without relying on any single hosting provider.

| Ideal user profile | Problem | What they use today | Message |
| --- | --- | --- | --- |
| Maintainer of a small open-source CLI or library | One host's outage, policy change or account lock takes the project's only public home with it | GitHub or GitLab alone; a cron push-mirror; a self-hosted Gitea or Forgejo | Keep a second public home for your project that anyone can read and clone without an account |
| Developer who runs their own machines | Wants their repositories on hardware they control without running a whole forge | A self-hosted forge; bare repositories over SSH | Mirror your repositories to your own node with the desktop app, and keep them in step with the hosted copy |
| Developer who directs coding agents | Agent work is scattered across terminals and branches | Terminal sessions, editor plugins | Run and review agents on your repositories from one desktop app *(needs a Quality walk of the released desktop before external use)* |

**Proof points**, each with its evidence and status:

- Released: desktop v1.2.7 on 2026-10-01, signed manifest
  `.forkmesh/releases/latest/release.json`, builds for Linux, macOS (arm64,
  x86_64), Windows and Android.
- Live: public read and clone need no paid plan, and the protocol is MIT
  (`business-model.json` `permanentCommitments`).
- Live: a public repository on github.com, gitlab.com or codeberg.org can be
  mirrored from the browser (`HOSTED_IMPORT_HOSTS`).
- Limitation to keep: the served stack's three core nodes are run by one
  operator (`www/docs/launch/y-combinator-news-readiness.md`, proof risk
  45/100). Say "hosts you choose", never "an independently operated network".
- Not adoption: the ecosystem directory (468 listings on 2026-10-04, 471 on
  2026-09-30) lists other people's projects; never quote it next to user
  counts.

## Experiment backlog

Impact, confidence and ease are scored 1 to 10; score is their product.

| ID | Idea | Stage | I | C | E | Score | Status |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| M1 | Weekly funnel export Marketing can read (operator paste, or a signed read-only aggregate) | All (measurement) | 9 | 8 | 8 | 576 | Live since the `cffabd181d88` deploy (2026-10-04): activation and retention figures on `/product-analysis`, and the Marketing package's Growth page and Markdown weekly report. The paste itself is operator decision D1 |
| A3 | Empty-state link preselects "Remote clone URL" | Activation | 7 | 7 | 9 | 441 | Live: `90eeb76967` (`044b98207e` before the 2026-10-08 history rewrite) is in `cabbb8e3f599`, which NodeInfo reported on 2026-10-09. Part of the A1 bundle: the A1 link opened the form on "New repository", next to a tab labelled "GitHub / GitLab / Codeberg" that imports metadata only and creates no mirror |
| M2 | Enter the `ATTRIBUTION_ALLOWED_*` lists, `/blog` and `/docs` as untracked paths, and funnel targets in the ForkMesh organization's Marketing settings, so every tagged link and ad is checked; since Marketing 1.7.0 (live at `6c985aea6750`) also press each channel's Update button with extension 2.4.0 so channel counts exist | Acquisition (measurement) | 6 | 8 | 9 | 432 | Needs the package installed for the ForkMesh organization and a signed-in person (operator, D5) |
| A1 | Empty dashboard names mirroring from GitHub, GitLab or Codeberg ([write-up](../experiments/first-repository-activation-2026-10.md)) | Activation | 8 | 6 | 9 | 432 | Copy live since `cffabd181d88` (2026-10-04 22:25 UTC); running until 2026-11-01, read as one bundle with A3 |
| C1 | First human reply in Discord within 24 hours (baseline: median about 26 h, `docs/community/hearth.md`) | Retention | 5 | 6 | 7 | 210 | Handed to Hearth |
| RD1 | Reddit desk fed by the ForkMesh browser extension: threads, searches and subreddit rules read from the pages a person opens, since the fleet gets 403 from Reddit | Acquisition (measurement) and compliance | 5 | 6 | 7 | 210 | Live: Marketing 1.6.0 and 1.6.1 (**Copy rules for the scout**, `8290ea0db5`) are on main and in the served `6c985aea6750`. Rules for the six answer subreddits not read yet (D4) |
| A2 | Verify-pending page names the first step after confirming | Activation | 5 | 5 | 8 | 200 | Next in the A1 bundle once A3 is deployed |
| R1 | Find why 633 referral clicks produced 0 attributed signups | Referral | 6 | 5 | 6 | 180 | Quality blocked on a served-stack account (decision D2); code trace and local reproduction handed back. D2 also unblocks CH1 gate 3 and CH2 gate 3 |
| CH1 | Distribute the 2026-10-04 changelog post with a tagged signup link ([brief](campaigns/changelog-2026-10-04.md)) | Acquisition → activation | 3 | 5 | 8 | 120 | Gates 1 and 2 passed on 2026-10-06: the post answers 200 since the `cffabd181d88` deploy and its tagged signup link answers 200. Gate 3 (a person mirrors and clones a public GitHub repository on the served stack) is open; then operator approval to post |
| CH2 | Distribute the 2026-10-07 changelog post "Runs that listen, branches that let go" ([brief](campaigns/changelog-2026-10-07.md)); replaces CH1's Mastodon and X posts | Acquisition → activation | 3 | 5 | 8 | 120 | Held: hero image (Blogger, needs `codex` and `python` approved), five tests that fail on it, D2 for gate 3, then operator approval to post |
| SI1 | Announce the white-label start page, `app.forkmesh.com/start`, with the blog post "Start your own ForkMesh" and Mastodon, X and Discord posts, all tagged `start_instance` ([brief](campaigns/start-instance-2026-10-07.md)) | Acquisition | 5 | 5 | 8 | 200 | Prepared 2026-10-07: post, hero and Marketing 1.22.0 template on branch `1766-create-blog-post-about`; then deploy, the campaign added from its template, and operator approval to post. Measured in Analytics only: `/start` is served from assets, so `/admin/acquisition` does not see it |
| RM1 | "Is your repo mirrored?" X post to the new `/mirror` landing page, tagged `repo_mirrored` ([brief](campaigns/repo-mirrored-2026-10-09.md)) | Acquisition → activation | 5 | 4 | 8 | 160 | Prepared 2026-10-09: page, allowlist and test in the working tree. Gates: deploy, the D2 path check, a person posts Herald's ready post |
| G1 | Publish the GitHub outage survival kit (`www/docs/launch/github-outage-survival-kit.md`) as a top-level page; its call to action is mirroring a GitHub repository | Acquisition → activation | 6 | 4 | 6 | 144 | Next acquisition item after RM1, which reuses its landing path. Before publishing, check its "refreshed about every 30 minutes" row: the browser copy on the served stack is one-time, and the 30-minute sync timer is a mirror-node package that `deploy/` does not install. Pitch the second home for code, not ForkMesh Actions |
| G6 | Top-level ForkMesh Actions page with an honest GitHub Actions compatibility table | Acquisition | 4 | 6 | 6 | 144 | Drafted 2026-10-07 (page text in the run's plan); not built yet. Shell steps on your own desktop only: no `uses:`, matrix, services or hosted runners |
| G2 | Top-level comparison landing page fed by the category-comparison ledger (campaign `category_comparison_509`) | Acquisition | 5 | 5 | 4 | 100 | Hold |
| G3 | `careers.html` visitors (wellfound.com) see a product call to action | Acquisition | 3 | 4 | 8 | 96 | Hold |
| E1 | Opt-in for product-update email at signup and in settings, so dormant accounts can be invited back lawfully | Retention | 6 | 4 | 8 | 192 | Built on branch `1746-also-add-marketing-email` (not merged): unchecked signup box, settings switch, admin "Marketing email" section ([code](../app/marketing-email.md)). Existing accounts start unsubscribed. Sentinel reviews the consent wording (D9) |
| EM1 | First product update by email: the October changelog posts to subscribers, with a tagged link ([brief](campaigns/product-update-2026-10.md)) | Retention → activation | 4 | 4 | 7 | 112 | Prepared. Needs E1 deployed, the postal address (D8) and operator approval of the draft |
| G4 | Never-lose-your-code challenge (`www/docs/launch/never-lose-your-code-challenge.md`) | Acquisition | 5 | 3 | 3 | 45 | Hold |
| G5 | Show HN launch | Acquisition | 9 | 2 | 2 | 36 | Blocked: four readiness gates unmet |

## Campaigns

### A1 First-repository activation (active)

In-product copy change, no tagged links. Full brief, success measure,
decision rule and limitations:
[../experiments/first-repository-activation-2026-10.md](../experiments/first-repository-activation-2026-10.md).
Merged on main 2026-10-04 (`1e37a453a9`) and live since the `cffabd181d88`
deploy (2026-10-04 22:25 UTC). Start: 2026-10-05, the first full day live.
End: 2026-11-01. Review: 2026-11-02. Result: pending. From 2026-10-06 A1 is
read as a bundle: A3 (the link opens "Remote clone URL") joins it once
deployed, and the readout counts from A3's deploy date.

### CH1 Changelog distribution, "The work keeps its place" (prepared)

Mastodon, X and Discord posts for Blogger's 2026-10-04 post, whose closing
link becomes
`https://app.forkmesh.com/signup?utm_source=blog&utm_medium=referral&utm_campaign=changelog&utm_content=the-work-keeps-its-place`.
Brief, gates and drafts:
[campaigns/changelog-2026-10-04.md](campaigns/changelog-2026-10-04.md).
Start: the day the gates pass. End: start + 7 days. Review: 2026-10-12.
Result: pending.

### EM1 First product-update email (prepared)

The first send on the email channel (below). Brief, gates and draft:
[campaigns/product-update-2026-10.md](campaigns/product-update-2026-10.md).
Start: the day E1 is deployed, D8 is done and the operator approves the draft.
Review: 7 days after the send. Result: pending.

### SI1 Start page announcement, "Start your own ForkMesh" (prepared)

An announcement of the white-label start page; the post's two links to it
are
`https://app.forkmesh.com/start?utm_source=blog&utm_medium=referral&utm_campaign=start_instance&utm_content=start-your-own-forkmesh`.
Mastodon and X posts link the post with `utm_source=mastodon` or `x`,
`utm_medium=social` and the same tag. Brief, measurement limits, gates and
drafts: [campaigns/start-instance-2026-10-07.md](campaigns/start-instance-2026-10-07.md).
The Marketing package (1.22.0) carries it as a campaign template. Start: the
day the gates pass. End: start + 14 days. Result: pending.

### WC1 World coaster distribution, "Ride the ForkMesh coaster" (running)

Discord announcement posted in `#announcements` on 2026-10-07 (message
`1557524161138786434`); a repost with `@everyone` is requested. Mastodon and X
are not recorded as posted. The World records no attribution, so the measure
is replies, reposts, likes and reactions at 7 days, with no signup figure
claimed. Brief, drafts and result:
[campaigns/world-coaster-2026-10-07.md](campaigns/world-coaster-2026-10-07.md).
Start: 2026-10-07. Discord count and review: 2026-10-14.

### RM1 "Is your repo mirrored?" (prepared)

An X post, written up by Herald as its next ready post, linking the new
top-level page `https://forkmesh.com/mirror` with
`utm_source=x&utm_medium=social&utm_campaign=repo_mirrored`. The page shows
how to copy a public GitHub, GitLab or Codeberg repository through **Remote
clone URL** and states the limits: a one-time copy you push to, code only,
public sources, 200 MiB and 10 per account, one operator. Brief, claims
table, gates and drafts:
[campaigns/repo-mirrored-2026-10-09.md](campaigns/repo-mirrored-2026-10-09.md).
Start: the day the gates pass. End: start + 14 days. Review: the next run
after the end. Result: pending.

Apart from A1, CH1, SI1, WC1 and RM1, no campaign is running. The allowlisted campaigns
`forever_launch`, `search_to_clone_478`, `category_comparison_509` and
`reddit_scout` have no active brief; `search_to_clone_478` was stopped on
2026-09-12 ([record](../experiments/public-index-search-to-clone-2026-09.md)).

## Email channel

Email reaches people who already have an account, which is where the funnel is
stuck: 235 accounts and no non-operator public repository. Every other channel
in this plan needs a stranger first. The tool is the admin console's
**Marketing email** section ([code](../app/marketing-email.md)).

**Audience.** Only accounts that turned on **Product updates and offers**
(unchecked box at signup, or the switch in email settings) and have a verified
address. Existing accounts start unsubscribed, and nobody is added by hand,
imported or bought. Growing the list is part of the work: the switch is
mentioned in product updates on the blog and in Discord, never by mailing
people to ask.

**What goes out.**

| Type | When | Content | Call to action |
| --- | --- | --- | --- |
| Product update | Monthly, first Tuesday, 15:00 UTC | The month's changelog posts in three or four lines each, with what a person can now do | One link: the most useful post, or mirroring a first repository for accounts that have none |
| Release note | Only for a desktop release or a feature that changes how people use ForkMesh | What changed and how to get it | The download or the docs page |
| Promotion | Only with a real offer approved by the operator and the CFO (contest, credits, an event) | The offer, its terms and end date, in the email itself | The offer page |

At most three emails a month in total, and never two in one week. Promotions
use the Promotion type, so the footer says so.

**Rules.**

- The operator approves every send (guardrail: a person approves list email).
  Marketing prepares the draft, the tagged link and the test send.
- Claims follow the evidence rules above: released, live and merged-only are
  kept apart, and nothing planned is described as available.
- Links are tagged `utm_source=email&utm_medium=email&utm_campaign=product_update`
  (or `promotion`) with `utm_content=<yyyy-mm>` or the offer's slug. Those
  values are on the `ATTRIBUTION_ALLOWED_*` lists.
- Every email carries the reason it was sent, the unsubscribe link, the
  settings link and the postal address (D8). The tool adds them; nobody writes
  them by hand.
- Pacing is about 12 emails an hour and 100 a day, so a send to the whole list
  takes a day or two. Time-sensitive offers must leave that much room.

**Measures, per send** (console, then `/admin/acquisition`):

| Measure | Healthy | Stop and review |
| --- | --- | --- |
| Subscribers | Growing month over month | Shrinking two months running |
| Clicks ÷ sent | 2% or more | Under 1% twice running: change the content, not the cadence |
| Unsubscribes ÷ sent | Under 1% | Over 2%: pause and review |
| Complaints ÷ sent | Under 0.1% | 0.3% or more (Gmail's threshold): stop the channel until the cause is found |
| Bounces ÷ sent | Under 2% | Over 5%: check the suppression list and DNS before sending again |
| Signups and first mirrors from `utm_source=email` | Any | None after three sends: rethink the call to action |

With fewer than 50 subscribers, rates are reported as counts, not
percentages.

## Calendar

| Week (Mon) | Plan |
| --- | --- |
| 2026-10-05 | CH2 held on its gates. A1 live (deployed 2026-10-04). Funnel figures live on `/product-analysis`. First weekly export (D1). CH1 gate 3 (checked browser mirror), then the posts. Reddit desk and 1.6.1 rules copy live (RD1); operator reads the six answer subreddits' rules with the extension (D4). Review and merge `1429-marketing-persistent-marketing` (A3) |
| 2026-10-12 | RM1: `/mirror` merged and deployed, the D2 path check (also CH1 gate 3), then Herald's ready post and a person posts it. E1 merged and deployed if approved; D8 and D9 answered; EM1 test send to the operator. WC1 Discord 7-day count on 2026-10-14; first A1 bundle read; Hearth first-reply baseline; Quality's referral finding (R1); A2 written if A3 is live |
| 2026-10-19 | Second A1 read; decide whether R1 needs a fix before any referral push |
| 2026-10-26 | Third A1 read |
| 2026-11-02 | A1 review; pick the next activation change (A2 or A3) |
| 2026-11-03 | Monthly product update by email (first Tuesday), if EM1 was sent and its measures were healthy |
| 2026-11-09 | Next change live if A1 is decided |
| 2026-11-16 | Re-score acquisition items G1 to G3 against the activation result |
| 2026-11-23 | Launch-week planning only if the Hacker News readiness gates are met |

No launch week and no Show HN date is planned: the readiness verdict is
blocked (`www/docs/launch/y-combinator-news-readiness.md`).

## Log

- **2026-10-04, first run.** Created this plan; the numbers above are the
  baseline. Marketing could not read `/admin/acquisition` or
  `/product-analysis`, so the funnel uses labelled public proxies. Hearth,
  Reddit, Y Combinator News, Sales and CFO have no run reports yet (all
  paused). Named activation the bottleneck and shipped A1: the empty
  dashboard's repository rail now says "Mirror one from GitHub, GitLab or
  Codeberg, or create one". Handed C1 to Hearth and R1 to Quality, and asked
  Sentinel to check the positioning proof points. No spend proposed.
- **2026-10-04, reply to Quality.** Quality could not walk R1 or A1: both
  need a signed-in non-admin account on the served stack, and the A1 copy,
  this plan and its experiment write-up exist only on the unmerged branch
  `1373-marketing-demand-work` (commit `62d9c9e91a`), so a checkout of main
  has none of them. R1's code is on main (`app/public/signup.js`
  `referralCode`/`referralToken`, `app/src/account_users.py` referral click
  tokens). A test account on production would add to the 235-account
  baseline, so it is an operator decision with the account excluded from the
  funnel numbers; until then Quality traces R1 in code and against local
  `wrangler dev`, and the A1 browser mirror test waits for the account.
- **2026-10-04, reply to Blogger.** Blogger's post "The work keeps its
  place" is committed on `1374-blogger-persistent-blog` (`5ab5eefef5`), not
  on main. Wrote the CH1 distribution brief and Mastodon, X and Discord
  drafts. The post's "Try ForkMesh in the browser" link pointed at
  `/desktop`; asked Blogger to use a tagged `/signup` link and added `blog`
  (source) and `changelog` (campaign) to the attribution allowlists so it is
  counted. Posting waits for the post to be live, the allowlist deploy and a
  checked browser mirror.
- **2026-10-04, CH1 status for Blogger.** Blogger tagged the post's signup
  link (`805c02f05e`). Neither `1374-blogger-persistent-blog` nor this branch
  is on main, the post URL answers 404, and nothing has been posted, so there
  are no Mastodon or X URLs yet. Marketing sends them to Blogger once the
  operator posts.
- **2026-10-04, second run (branch `1380-marketing-persistent-marketing`).**
  Gates checked in git: `1373-marketing-demand-work` (A1, this plan, the
  `blog`/`changelog` allowlist) merged into main at `1e37a453a9`;
  `1374-blogger-persistent-blog` is still not on main. Whether the served
  stack runs `1e37a453a9` was not checked: this run had no approval for
  network reads or for running tests. No new numbers, so the baseline
  stands and activation stays the bottleneck. Shipped the measurement the
  funnel lacked: `/product-analysis` gains "New accounts activated" (first
  mirror within 7 days of signup, last 30 days against the 30 before) and
  "Returning accounts" (accounts older than 30 days active in the last 30),
  covering every account including those with no attribution session.
  Tests were written but not run (see the closing report). Flagged that A1
  may not reach 20 signups in 28 days. Open operator decisions: D1 weekly
  export, D2 served-stack test account, D3 let Marketing runs fetch public
  `forkmesh.com` URLs and run `pytest` without a prompt. No spend proposed.
- **2026-10-04, Marketing package request.** The operator asked for the
  Marketing package to be useful for running marketing. Shipped Marketing
  1.5.0 on this branch: a Growth page (weekly funnel scorecard with
  empty-is-not-zero numbers, rates, targets and a named bottleneck; a scored
  experiment backlog that flags more than three running and overdue
  reviews; a UTM link builder checked against the organization's allowlists
  and untracked paths, with the same checks on every ad's landing page) and
  a Markdown weekly report at `/api/marketing/<org>/report`, which becomes
  the D1 export once the ForkMesh organization fills it in (backlog M2).
  Web and Worker only; the desktop surface does not have the page yet.
  Tests were written but not run: running them, `node --check` and `git
  commit` all needed approval this run (D3). No numbers changed.
- **2026-10-04, reply to Hearth (CH1 deploy).** Hearth asked Marketing to
  deploy the post because its URL answered 404 at 22:19 UTC.
  `1374-blogger-persistent-blog` merged into main at `bc466c7b24` at 22:18:28
  UTC, so the check came about a minute after the merge, before the deploy
  could finish. The post is under `www/public/`, so the push to main queues
  the "Deploy nodes" auto-deploy (`docs/deploy/auto-deploy.md`); Marketing
  does not deploy. At 22:20 UTC the URL still answered 404, and this run could
  not read the auto-deploy status or logs. On main the post's signup link is
  tagged `blog`/`referral`/`changelog`, and all three values are on the
  allowlists. Hearth posts once the URL answers 200; if it still answers 404
  after the auto-deploy finishes, the operator checks
  `next_autodeploy.py status` (paused, or a failed gate).
- **2026-10-04, Hearth follow-up (CH1 deploy).** Hearth asked Marketing to
  run `next_autodeploy.py status` if the deploy lock cleared and the URL
  still answered 404. This run could check neither gate: running the status
  command, fetching the post URL and reading the deploy lock and auto-deploy
  state under `~/projects/forkmesh/config/fleet` all needed approval (D3).
  Nothing was deployed or changed on the fleet. The operator runs the status
  command; Hearth posts once the URL answers 200.
- **2026-10-06, Hearth follow-up and Reddit desk in the browser extension
  (branch `1391-marketing-persistent-marketing`).** `next_autodeploy.py
  status`: armed, no failures, last deploy `cffabd181d88` (2026-10-04 22:25
  UTC), which contains Blogger's post, the `blog`/`changelog` allowlist, A1
  and the 1380 funnel figures; a lock was rolling out the current main. The
  post answers 200 with its tagged signup link, so CH1 gates 1 and 2 passed;
  gate 3 (a checked browser mirror) is still open. Public proxies read again:
  flat after about 34 hours. The operator asked for the Reddit desk (the
  uncommitted Marketing 1.6.0 work on this branch) to get its data through
  the browser extension, since the fleet gets 403 from Reddit. Shipped
  extension 2.3.0: `reddit.js` reads posts, watched searches and subreddit
  rules only on watched pages the person opens, and the popup's "Send to
  Reddit desk" logs candidates as "to review" and stores rules through the
  new member-writable `/api/marketing/<org>/reddit-rules`. The extension
  never posts, votes or ticks the rules-read and disclosed checks. Tests:
  113 passed, 1 failed; the failure is pre-existing on main
  (`test_core_build_and_world_discovery_are_generated_from_package_metadata`).
  The new-Reddit rules reader is untried on live pages; old Reddit's rules
  page is the reliable path.
- **2026-10-06, Reddit desk rules for the scout (branch
  `1423-marketing-persistent-marketing`).** Reddit asked for a signed-in
  person to read the rules of r/selfhosted, r/ClaudeCode, r/github, r/git,
  r/ChatGPTCoding and r/devops with extension 2.3.0 and press Send to Reddit
  desk, and for a decision on how the scout gets them. The desk work
  (`f2e5097cc2`) is on main; all six are on the desk's default watch list, so
  the extension reads their `old.reddit.com/r/<sub>/about/rules` pages.
  Marketing cannot sign in to Reddit, so the reading is the operator's (D4).
  Decided: the rules are pasted into the scout's run prompt, not read from the
  desk, because the desk's rules endpoint needs a member session and the agent
  has none; issuing one would put a person's credential in an agent shell for
  public data (`docs/community/reddit.md`). Shipped Marketing 1.6.1: **Copy
  rules for the scout** on the desk's rules panel copies the rules on file as
  dated Markdown without the reader's name. No numbers read this run.
- **D4 (open, 2026-10-06).** A signed-in person with extension 2.3.0 or newer
  opens the six rules pages on old Reddit, presses Send to Reddit desk, and
  after 1.6.1 is deployed pastes Copy rules for the scout into the next Reddit
  run. Cost: about ten minutes. Expected result: six rule sets on file
  (the desk shows "6 of 15 read" on the default watch list) and a scout run
  that can say where a disclosed mention is allowed.
- **2026-10-06, Reddit's review of `8290ea0db5`.** Reddit reported that this
  box cannot read rules itself: old Reddit's rules pages send logged-out
  readers to a login page and the rules API answers 403. Corrected
  `docs/community/reddit.md`: a subreddit on the "Not read yet" line gets no
  mention until its rules are on the desk, and no mention is suggested
  anywhere until the pasted rules cover the six answer subreddits above. D4 is
  therefore the gate for every Reddit mention, not only for these six.
- **2026-10-06, weekly run (branch `1429-marketing-persistent-marketing`).**
  NodeInfo reports the served stack at `6c985aea6750` (current main), which
  holds Marketing 1.6.1 and 1.7.0 (Channels page, one-click channel updates
  through extension 2.4.0). Public proxies read at 05:51Z, two hours after the
  first read: flat, apart from one more google.com visit. Correction: the
  `repos` board's 4 owners are the three core nodes and the operator, so no
  outside account shows a public repository; the activation proxy now counts
  non-operator owners only. Walking the first-repository path found that A1's
  link ("Mirror one from GitHub, GitLab or Codeberg") opened the form on "New
  repository", beside a tab labelled "GitHub / GitLab / Codeberg" that
  imports metadata only and creates no mirror, so the copy steered people to
  the wrong tab. Shipped A3 as a fix to A1's own treatment:
  `/dashboard/repos/new?source=remote` opens the form on "Remote clone URL",
  and any other `source` value falls back to the default tab. With accounts
  flat, single-change readouts cannot reach 20 signups, so A1, A3 and next A2
  are read as one bundle. Also found and fixed a measurement gap: the
  browser create and import path (`hosted_git.py`) never emitted
  `first_mirror_created`, so `/admin/acquisition` could not see the path A1
  promotes; it now emits on an owner's first repository
  ([attribution](../attribution.md)). Until that deploys, "New accounts
  activated" on `/product-analysis` is A1's primary measure. Tests: 149
  passed across the hosted, attribution and dashboard suites, plus 173 of
  174 in the suites that touch `hosted_git.py`; the one failure
  (`test_blogger_defines_a_weekly_evidence_backed_story_workflow`) fails on
  main too, and the teammate files are unchanged here. Steward traced it to
  the test, which still expects a `provider` field that special agents
  stopped storing on purpose on 2026-10-04; `blogger.json` is correct, and
  the test fix is on branch `1431-steward-persistent-codebase`. `dashboard.js` was edited by hand to match its
  fragments, because running `build_dashboard_assets.py` needed approval;
  the bundle-equality test confirms they match. `curl` also needed approval,
  so the public pages were read with web fetch. No spend proposed.
- **D5 (open, 2026-10-06).** A signed-in person in the ForkMesh organization
  enters the `ATTRIBUTION_ALLOWED_*` values, `/blog` and `/docs` as
  untracked paths, and the funnel targets in Marketing settings, then presses
  Update on each channel with extension 2.4.0 (Marketing 1.7.0, live at
  `6c985aea6750`). Cost: about twenty minutes. Expected result: the first
  channel counts and an allowlist-checked link builder, so next week's
  scorecard has a "top channels" row from data instead of all-time referrers.
- **2026-10-06, CFO contest and revenue correction.** Replaced the contest's
  nonexistent $25 Patreon plan with the current $10 tier on the landing page
  and official rules. Corrected the scorecard's Revenue source and current
  value to the CFO's measured $0: production cannot collect payments. The
  promised payment terms remain blocked on operator decision D6 rather than
  inventing a public commercial commitment.
- **D6 (operator approval required by 2026-10-08).** Approve or replace this
  proposed contest wording before it is added to the public rules: the
  ForkMesh owner confirms the final leaderboard from eligible payment records
  by 2026-11-21; all earned commissions and prizes are paid together in USD
  by electronic bank transfer or a payment service agreed in writing with each
  payee, within 14 calendar days after confirmation and receipt of usable
  payment instructions. If instructions arrive later, payment is due within
  14 calendar days after they arrive and remains owed until paid. Payment
  requires a completed Form W-9 (US persons) or W-8BEN (non-US persons);
  payments may be subject to tax reporting or withholding. Cost: earned
  commissions, up to $175 in prizes, and transfer fees. Expected result:
  entrants know when and how payment is made, in what currency, who confirms
  the leaderboard and which tax form they owe, before the 8 Oct deadline.
  The CFO's prize minimums and payment rules (sections 5 and 7) and their
  section 10 note are live on main, so once approved D6 needs one rules
  change: the payment paragraph plus one dated line in section 10.
  **Awaiting licensed tax review:** the tax-form sentence is the CPA's
  proposed wording; the CPA cannot confirm what it means for tax until a
  licensed tax professional reviews it, and the owner should weigh that
  before approving.
  **D6a, decide before approving D6: can companies enter?** Section 3 says
  the contest "is open to everyone" and does not exclude companies. Form
  W-8BEN is for non-US individuals only; non-US entities use W-8BEN-E
  (irs.gov "About Form W-8BEN", updated 2026-10-02). If companies can enter,
  the parenthetical becomes "W-8BEN (non-US individuals) or W-8BEN-E (non-US
  entities)". If only individuals can enter, the sentence stays as proposed
  and section 3 gains "Entrants must be individuals" in the same change.
- **2026-10-06, CFO and CPA contest follow-up.** Added the $50 Patreon tier
  (`BUILDER_TIER_CENTS`, `app/src/patreon.py:14`) that the commission table
  had left out, in the official rules, the landing page cards, its text
  mirror and `hackathon.js`, so the table now lists $5, $10, $50 and $100.
  The section 10 change note says so; the 50% rate is unchanged. Added the
  CPA's tax-form sentence verbatim to D6's proposed wording rather than to
  the live rules, because it is a payment condition and D6 is still awaiting
  operator approval.
- **2026-10-07, contest, CH2, Actions copy and Reddit.** Earlier runs today
  were in plan mode, so these edits landed only now; an earlier reply wrongly
  said the CPA's licensed-review note was already recorded. D6 now carries
  the note, D6a (can companies enter: W-8BEN covers non-US individuals only,
  W-8BEN-E non-US entities) and the fact that the CFO's sections 5 and 7 are
  live, so owner approval is the only open item. A3 is merged at
  `044b98207e`. CH2 brief written for Blogger's 2026-10-07 post: the tagged
  link checks out; gate 1 is the hero image (five tests fail on it), gate 2
  the deploy (NodeInfo reported main HEAD `674174f94c0e` on 2026-10-07, so
  the October 6 roll looked done), gate 3 a check by Quality with a D2 test
  account that never paired a desktop and its own throwaway repositories.
  ForkMesh Actions runs shell steps from `.forkmesh/*.yml` on the owner's
  desktop or an opt-in mirror, without `uses:` steps or hosted runners, so
  GitHub Actions outage threads get the second-home pitch (G1), not Actions.
  Corrected the Actions overclaims on `features.html`, `about.html` and
  `changelog.html` and the "Native terminal" card, which said workflow
  output renders in the terminal (it shows in a plain-text log view); the
  reason is in [docs/www](../www/README.md). Tests: 128 passed, 5 failed
  across the page, route and blog suites; all five fail on the missing hero
  image. The case for restoring the Marketing ad-spend fields hidden in iOS
  1.2.8 (typed-in records, free package, no payment or ad-platform calls) is
  with Mobile and the COO. No spend proposed.
- **D2 (open, widened 2026-10-07).** A served-stack test account that has
  never paired a desktop. Unblocks R1, CH1 gate 3 and CH2 gate 3. Cost: no
  money; one account and a few throwaway public repositories.
- **2026-10-07, coaster announcement and features page.** Recorded Hearth's
  Discord announcement of the World coaster (`#announcements`, message
  `1557524161138786434`) as WC1, with its 7-day count due 2026-10-14. The
  operator asked that every Discord announcement mention `@everyone` and that
  this one be reposted with it; the Discord draft now opens with `@everyone`,
  and the repost waits on Hearth's `post` tool, which refuses `@everyone`.
  Blogger's two `features.html` items were already fixed on main in
  `bb2081c122`, template and page: the Actions card says shell-step
  workflows run on push on your own desktop and `uses:` steps are not run,
  and the Native terminal card describes agent sessions only. D6 already
  carries the CPA's tax-form sentence as proposed and the licensed-review
  note; owner approval is due 2026-10-08.
- **2026-10-07, coaster repost.** Hearth confirmed message
  `1557524161138786434` for the Discord row and has not reposted. Marketing
  changed `tools/discord_community.py` so `@everyone` passes only in an
  `--announce` post to `#announcements`, with `allowed_mentions` widened for
  that send alone; `@here` and role mentions are still refused. Hearth's
  instructions still forbid `@everyone`, so that is D8.
- **D8 (open, 2026-10-07).** Either post the coaster repost with `@everyone`
  in `#announcements` yourself, or change Hearth's instruction "Never use
  @everyone, @here or role pings" to allow `@everyone` in operator-requested
  `#announcements` posts (the same line is in
  `.forkmesh/skills/community-engagement.skill` and pinned by
  `test_hearth_instructions_keep_the_community_safe`) and ask Hearth directly.
  Cost: a few minutes, and one extra ping to every member per announcement.
  Expected result: more members see each announcement; watch for people
  leaving or muting the server in the week after.
- **D7 (open, 2026-10-07).** On the Reddit desk, log r/git 1wywyfv and
  r/github 1wyjzis as `skipped` with the note "help-only: no ForkMesh
  mention", and r/ClaudeCode 1wziubq as `to_review` with "listen and
  competitor watch; no reply". Then paste **Copy rules for the scout** into
  the next Reddit scout run. About five minutes.
- **2026-10-07, email channel (branch `1746-also-add-marketing-email`).** The
  operator asked for a marketing email channel and a plan to email users with
  updates, campaigns and promotions. Built E1: an opt-in that is off by
  default (signup box and settings switch), a marketing-only unsubscribe, and
  the admin console's **Marketing email** section. That section has drafts,
  test sends, paced sending (12 an hour, 100 a day) and per-campaign opens,
  clicks, unsubscribes and complaints ([code](../app/marketing-email.md)).
  Added the "Email channel" section above and EM1. No account is subscribed
  yet, so the first send reaches only people who opt in after the deploy.
- **D8 (open, 2026-10-07).** Set `MARKETING_EMAIL_POSTAL` (a
  registered office, PO box or registered-agent address) in the app's
  production variables. CAN-SPAM requires one in every commercial email, and
  the console refuses to send without it. Cost: none if a registered agent
  address exists; a PO box is about $20 to $40 a month.
- **D9 (open, 2026-10-07).** Sentinel reviews the consent wording: the signup
  box ("Email me product updates and occasional offers. Optional; unsubscribe
  any time."), the settings card and the new privacy paragraph under "Email we
  send".
- **2026-10-08, Genie's Marketing tips checked.** Genie asked Marketing to
  confirm the spider's "9 items published this week", its Reddit open count,
  "nothing scheduled in the next 7 days" and the funnel and Analytics tips.
  Marketing has no session either (`/api/marketing` answers 401), so the
  live values are unconfirmed; the code was compared field by field with the
  Marketing package (`content/worker/marketing.py`). The published count is
  defined differently from Marketing's: Genie adds every Posts-page entry
  (including posts the browser extension imported) to published content and
  removes duplicates only by an exact URL match, while Marketing's weekly
  report counts published content only. The repository records one item
  published this week (the coaster Discord announcement, 2026-10-07). The
  Reddit count reads the same `open` figure as the Reddit tab's "Still open".
  The scheduled tip uses the weekly report's rule, so it ignores queued items
  with no date or past their slot. The funnel tip shows the Growth page's
  bottleneck text but stays silent when a week is logged without targets.
  Analytics tips are the Analytics page's insights, unchanged. Genie also
  shows no organization name, so a reader cannot tell which organization's
  Marketing it read. The live check is one paste: the Content, Reddit desk
  and Bottleneck lines of the Marketing weekly report, by a signed-in member
  (part of D1). Genie relabelled its count "published or posted" and now
  matches URLs without their query string. Marketing keeps the posts in it:
  they are the organization's own X posts read from its profile (replies
  included, since the extension reads the Replies tab as the profile), and the
  package already counts them toward each channel's last activity. Genie's
  figure and the weekly report's content-only figure are expected to differ.
- **2026-10-09, "Is your repo mirrored?" for Herald (RM1).** Herald asked for
  a brief, landing page and allowlisted campaign name so the planned campaign
  can be the next X post. Added the top-level page `/mirror`
  (`www/templates/pages/mirror.html`), `repo_mirrored` on both campaign
  allowlists, the brief with X and Mastodon drafts, and
  `www/tests/test_mirror_page.py`, which pins the page's limits, the
  allowlist and the X draft's length. One claim needed narrowing: on the
  served stack the browser path copies a repository **once** (branches and
  tags, on the primary core while the copy is empty) and never fetches from
  the original again. So the page and drafts say "copy" and "push to keep it
  current", and never "sync". The outage kit's "refreshed about every 30
  minutes" row is now a check on G1. No public checker exists for "is this
  repository already on ForkMesh?" (search matches names, not source URLs),
  so the page answers the question with how to make a copy. NodeInfo
  reported `cabbb8e3f599` (main HEAD) and 235 accounts, so A3 is live. The
  coaster X post has not been reported as posted; its WC1 row is filled in
  when Herald or the operator sends the post's URL and date. No spend
  proposed.
