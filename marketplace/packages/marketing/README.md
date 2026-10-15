# Marketing package

Marketing is an opt-in organization package for planning and measuring content
across up to 100 channels (social, owned, search, advertising, publishing,
community, commerce, video, audio, review, developer and event channels). It
provides a channel catalog, per-channel metric snapshots and health checks, a
shared content pipeline, campaigns, search keywords, an ads manager, a growth
scorecard, performance reports from the organization's Analytics, and channel
desks, on the web, the desktop and the phone apps.

Catalog inclusion means a channel is generally available, not that it is
approved or compliant in a particular market. Teams remain responsible for
platform terms and for the advertising, disclosure, email, SMS, privacy and
industry rules of every market they publish in.

## Data and security

Content, metrics, ads, experiments, funnel weeks, Reddit threads, posted
links, campaigns, keywords, ready posts, browser activity and settings
(including subreddit rules, the X keys and Mastodon tokens) are encrypted in
package-owned organization tables. Uninstall exports the organization snapshot
first, purges every row, and reverses the schema when the last organization
removes the package.

Marketing never connects to an ad account, holds no ad-platform credentials
and spends no money: ads are bought in the platform and results are logged by
hand. `test_worker_moves_no_money_and_calls_no_ad_or_payment_api` keeps the
worker's outbound calls to its feed reader, page reader, X client and the
declared social APIs. ForkMesh's servers never sign in to a channel; counts
that need a signed-in view are read by the person's own browser through the
ForkMesh browser extension.

Manifest permissions used: `organization.analytics.read`,
`organization.crm.read`, `organization.discord.read`,
`organization.discord.messages.read`, `account.ai.prompt`,
`external.social.post`, `external.social.follow`, `desktop.agents.run`.

## Channels

Each configured channel has metric snapshots and a health state computed by
the worker (`summary.channels[].health`, `summary.health`), so every surface
shows the same thing:

| State | Meaning |
|---|---|
| On track | Every check passes. |
| Needs attention | A check fails: no channel link, numbers older than 7 days, no post within twice the category's cadence, or a shrinking audience over 30 days. |
| Stale numbers | The latest snapshot is older than 14 days, or there is none. |
| Not set up | No channel link and no numbers. |
| Paused | The channel is switched off. |

Each category has a playbook (`channelPlaybooks`: goal, three numbers to
track, a cadence in days and tips). `health.guide` lists setup steps for
channels whose numbers need a particular source (Reddit, Email).

**Engagement rate** is engagements over the first of reach, impressions,
views or followers that the latest snapshot holds; `health.engagement`
reports `rate`, the numerator and denominator with labels, the `formula`, the
source and how many readings in a row carried the same pair (a sign that a
source stopped refreshing). Both numbers always come from one snapshot.

### Getting numbers

- **Refresh** (`POST /refresh`) reads what the server can: RSS/Atom feeds
  (a Blog's feed is discovered from its alternate link and posts are imported
  idempotently into the pipeline), public APIs (GitHub stars, forks and
  followers; Mastodon followers, following, posts and the followed-accounts
  list), the organization's Discord connector, Analytics (Google visits) and
  CRM (Email). Values a source cannot see carry forward; the snapshot source
  is `refresh`.
- **Update** uses the browser extension: it opens the channel link in a tab,
  reads the counts the page shows, and posts them to
  `POST /api/marketing/<org>/capture`. `/capture` accepts a page only from the
  channel's own hosts, refuses a read with no numbers, records a snapshot with
  `source: "extension"`, and stores the page profile (title, description,
  handle, up to five recent posts). Any member may update a configured
  channel; only owners and admins may add a channel or fill an empty link this
  way. Desktop and phone **Update** opens
  `/dashboard/marketing?org=<org>&page=channels&capture=<id>` in a browser;
  phones fall back to the manual **Log metrics** form.
- **Log metrics** (`POST /metrics`) records numbers by hand.

### Channel specifics

| Channel | Numbers and rules |
|---|---|
| Google (`google-search`) | `visits` (sessions from Google in 30 days, from Analytics) and `indexedPages` (the result count for `site:<host>`, read by the extension). The link defaults to `https://www.google.com/search?q=site:<host>`. No posting check. |
| Reddit | Refresh imports the subreddit's Atom feed only (Reddit refuses server reads of counts). Members, weekly visitors and weekly contributions come from the extension. Rate limits answer `reddit_rate_limited` (retried once on `old.reddit.com`, then no new request for 15 minutes); `reddit_refused`, `reddit_not_found`. |
| Email | Refresh calls `crm.email_channel_report` in-process: `outbound`, `replies`, `closedDeals` (won stages only) and `closedValue`. Source `zoho` or `crm`; `crm_not_installed` without CRM. |
| GitHub | `stars`, `forks`, `followers` from the public API (summed over the account's own repositories for an account link). Growth is measured by stars; there is no engagement rate. |
| Discord | Refresh uses `runtime.discord_guild` (member and online counts) and stores only the newest message time (`feed.lastPostAt`, never moves back) so the posting check sees Discord activity. Falls back to the public invite or widget. **Open** uses a public invite (`openUrl`). |
| Mastodon | Refresh records followers, following and posts, and keeps the followed handles (`mastodonFollowing`, up to 2000, never sent to clients) so they are not suggested. |
| Apple App Store (`app-store`) | Refresh reads the public App Store lookup (`https://itunes.apple.com/lookup?id=<id>&country=<cc>`, the id and store country come from the channel link): `rating`, `ratings` (rating count) and the store listing. `downloads`, `redownloads` and `updates` (30 days to the last reported day) come from App Store Connect through the App Store desk below. No posting check. Errors: `app_store_url_unsupported`, `app_store_app_not_found`. |
| Blog | Refresh imports the feed and records `posts` (every feed item seen so far, `marketing_feed_items`) as a snapshot, so a successful read counts as fresh numbers; before 2026-10-14 it recorded nothing and the "Numbers updated in the last 7 days" check failed however often the feed was read. Views come from Analytics (see Performance). |

Starter channels (`STARTER_CHANNELS`) are added to every organization unless
deleted. `DELETE /api/marketing/<org>/channels/<id>` (owners and admins)
removes a channel and its metric rows, records it in `removedChannels` so it
is not re-added, and keeps its content, ads and posted links.

## Dashboard

One command center from data ForkMesh already has, with a 7D/30D/90D range:

| Panel | Source |
|---|---|
| KPI strip: visits, conversions, audience, reach, engagement, posts, live campaigns | Analytics, snapshots, pipeline, campaigns |
| Web traffic by channel | `performance.channels` |
| Search keywords (tracked and from visits) | `marketing_keywords`, `performance.keywords` |
| Channels | `summary.channels` |
| Campaigns | `campaigns[].stats` |
| Marketing agent, Needs attention | `suggestions`, `POST /assistant`, ads attention, overdue pipeline items |
| Content flow, Ads, Post to X, This week, Reach chart, Landing pages, Referring websites | pipeline, `adsSummary`, `/x-post`, metrics, `performance.landing`, `referringSites` |

Overview panels can be reordered by dragging their headers (kept in
`localStorage` `forkmesh.marketing.overviewPanels` on the web and in desktop
settings).

### Campaigns

`marketing_campaigns` (at most 500 per organization): name, status (`idea`,
`planned`, `active`, `paused`, `completed`, `archived`), objective, dates, a
campaign tag (`slug`, used as `utm_campaign`), owner, landing page, channels,
audience, message, offer, hashtags, keywords, budget, targets, a checklist of
up to 40 steps, brief and notes. Content, ads and posted links join a campaign
by name, tag or landing page. Routes: `GET/POST /campaigns`,
`GET/PATCH/DELETE /campaigns/<id>`. Each campaign returns `stats` (content by
status, next post, ads and spend per currency, uncovered channels, checklist
progress, posts and their engagement). `campaignTemplates` offers prefilled
campaigns. **Copy brief** copies a Markdown brief; **Hand off** files it as a
task on the organization's board.

### Marketing agent

`overview.suggestions` lists rule-based next steps (missing campaigns,
schedules, landing pages or targets, stale channels, no keywords).
`POST /assistant` with `{campaignId?, question?}` asks ForkMesh AI
(`runtime.ai_json`, per-account quota) for up to 8 suggestions (content
drafts, checklist tasks, keywords, campaign ideas); content is added as a
draft, never posted.

### Search keywords

`marketing_keywords` (at most 500, blind-indexed so a term is tracked once;
409 `keyword_exists`): target page, intent, priority, campaign, notes and up
to 104 dated readings of position, impressions and clicks. **Import** accepts
a Search Console or Bing export or a plain list (`POST /keywords` with
`rows`).

### Posting to X

Owners and admins connect the organization's X app (OAuth 1.0a user context,
Read and write) by hand or guided by the browser extension.
`POST /x-connection` verifies the keys against `GET /2/users/me` before
storing them sealed; they are never returned or exported. `POST /x-post`
posts through `POST https://api.x.com/2/tweets` after a confirmation and
marks the pipeline item published. Members cannot post.

### Ready X post

`marketing_ready_posts` keeps one current post per organization and channel
(`x`) plus the last 30 closed ones. A special agent writes it; the package
stores and presents it.

- `GET /ready-post?channel=x` returns the post (text, X-counted `length`,
  optional image data URL and alt text, why, link, model, author,
  `intentUrl`) and history.
- `PUT /ready-post` saves a new post (PNG/JPEG/WebP image up to 960,000
  characters whose bytes match the type; https links only); a waiting post
  becomes `replaced`.
- `PATCH /ready-post` with `{id, status: "posted"|"skipped", url?}` closes it
  (409 `ready_post_changed` when the id is stale). It also closes as `posted`
  when `/x-post` or the extension's X import sees matching text.

## Posted links

`marketing_posts` (at most 2000 per organization) tracks links to posts on any
channel: `GET /posts` (with `postsSummary`), `POST /posts` (a link without a
channel takes it from the host; duplicates answer 409 `post_exists`),
`PATCH /posts/<id>` (counts add a history point; at most 120, readings within
30 minutes replace the last one, older readings are ignored), bulk import
`POST /posts` with `{"posts": [...]}` (at most 500; used by the extension),
and `DELETE /posts/<id>`. Links are normalised (X/Twitter status links become
`https://x.com/<handle>/status/<id>`). Optional `campaign` and up to four
`pbs.twimg.com` `images`. A posted link counts as a post in its channel's
health.

## Ads manager

Plans and measures paid placements on any catalog channel: channel,
objective, campaign, audience, creative, flight dates, lifetime or daily
budget in its own currency, and platform-reported spend, impressions, clicks,
conversions and revenue. The package derives CTR, CPC, CPM, CPA and ROAS,
pacing (on pace, fast, slow, over budget) and totals per currency and channel;
currencies are never added together. Self-serve channels link to their ad
console.

## Growth

- **Funnel scorecard**: one row per week (Monday, UTC) with visits, signups,
  activated, retained, referral signups and revenue, entered by hand (empty
  stays empty, never zero). Rates and their change are derived; bases under
  20 are flagged; the bottleneck is the rate furthest below its target.
- **Experiment backlog**: impact, confidence and ease scored 1–10; warnings
  for too many running experiments and passed review dates.
- **Link builder**: UTM-tagged links checked against the organization's
  allowlists of sources, mediums, campaigns and tracked domains.
- **Weekly report** (`GET /report`): Markdown with the scorecard, experiments,
  ads needing attention, content, channel to-dos, Reddit desk counts, campaign
  results from Analytics and referring websites.

## Performance

`GET /performance?from=&to=&project=` reads the organization's Analytics
in-process (`analytics.campaign_report`, after membership and install checks):
totals against the previous window, campaigns by UTM tags, ads matched to
visits by their landing-link tags, landing pages, results by subreddit, and
ranked "where to improve" findings (rules need at least 20 visits).
**Goals** (owners and admins, up to 5) define conversions as an Analytics
event with an optional property match. Without Analytics the page says what to
set up instead of showing zeros.

**Channel views.** Each enabled channel whose link is on a host Analytics
sees is sent as a page section (`analytics.campaign_report` `pages`), and the
response adds `channelViews` (`{<channel>: {host, path, views, visitors,
days}}`, only channels with views) and `pages` (views and visitors per page
under those links, at most 300). The desktop shows them as the **Views over
time** chart beside **Followers over time** on a channel page (recorded `views`
snapshots when Analytics has none), **Views** and **Visitors** columns for
each content item whose URL is one of those pages, a views headline on a
channel card with no audience number, and the sparkline on every channel card
(daily views when Analytics has them, else the headline number's snapshots
over the range).

**Referring websites** come from the core `site_referrers` counter (all-time
visits per referring host to this instance, excluding the instance's own
hosts); hidden when the organization's site is on another domain.

## Reddit desk

The desk tracks Reddit threads the team might answer, on the Reddit channel
page.

- **Watch list** (settings `reddit`, owners and admins): up to 40 subreddits,
  40 searches and 10 of the team's Reddit accounts.
- **Thread log** (`marketing_reddit_threads`, at most 2000, post id
  blind-indexed; 409 `reddit_thread_exists`): link, title, note, status
  (`to_review`, `drafting`, `replied`, `skipped`), landing page, and the
  person's own **Rules read** and **Disclosed** checks; `replied` requires
  both and the reply's comment link. Optional browser-read `author`,
  `comments`, `score`, `postedAt` and `source`. Monitored threads keep view
  and score readings (at most 180 history points).
- **Tagged links**: `utm_source=reddit&utm_medium=social&utm_campaign=reddit_scout&utm_content=<subreddit>`.
- **Subreddit rules** (`POST/GET /reddit-rules`, any member): stored under
  `redditRules`, at most 30 rules per subreddit and 200 subreddits; settings
  writes keep them. **Copy rules for the scout** copies them as Markdown.
- **Where to post next** and **What to talk about** (`redditDesk.ideas`) are
  counted from the desk's own data: open threads, landed replies, rules on
  file, quiet subreddits, a 7-day gap after a reply, and recurring two-word
  phrases from recent threads. A subreddit where a post or reply of ours
  landed (5 points, 500 views or 3 comments) ranks higher (`landed`).
- **Draft with ChatGPT** on each "Where to post next" row opens
  `https://chatgpt.com/?hints=search&q=<prompt>` (`ideas.draftUrl` +
  `draftPrompt`, at most 5000 characters so the address stays under
  ChatGPT's URL limit). The prompt is built on the server from the desk:
  what ForkMesh is, links tagged for the subreddit (home and `/desktop`), the
  subreddit's rules on file (or its rules page if none), the posts logged
  there, and outcomes from the team's own Reddit posts (Posted links on the
  `reddit` channel, parsed for `/r/<name>/comments/`) and replied threads:
  what we already posted there, our best results and our weakest. It asks
  whether a self-promotional post is allowed at all, then for titles, a
  disclosed body, flair and timing, and a first comment. It improves as the
  team logs posts and their views and points: the next prompt learns from
  them. The Reddit desk payload reads the newest 200 Reddit posted links for
  this. ChatGPT gets only the prompt; nothing is sent to it by ForkMesh.

ForkMesh's servers do not read Reddit listings, threads or rules. People log
threads by hand, or the browser extension reads pages they open (or walks the
watch list at a reader's pace in one tab) and sends what it read only when
asked. Nothing on the desk posts, votes or sends anything to Reddit.

## App Store desk

The `app-store` channel has its own page (`?page=app-store`, "Open the App
Store desk" on the channel card) with downloads over time and the store's
other numbers, on the web, the desktop channel page, iPhone and Android.

**Where downloads come from.** App Store Connect only gives downloads to an
API key with access to Sales and Trends (the Admin, Finance or Sales role;
Sales is the least it needs), and that `.p8` stays on the computer that
runs `native/ios/tools/asc_store_report.py`. The server never holds it and
never calls App Store Connect. With `--publish` the report reads App Store
Connect's daily SUMMARY sales reports (`/v1/salesReports`, the vendor number
from `ASC_VENDOR_NUMBER`) and PUTs them to
`/api/marketing/<org>/app-store`, signed with the desktop's identity key
(`forkmesh_mcp_server.marketing_request`). The organization is `--marketing
ORG`, `FORKMESH_MARKETING_ORG`, or the only organization with Marketing
installed. It asks Marketing for the last stored day and sends from three
days before it (late reports get corrected), or backfills up to 365 days
(Apple keeps daily reports for a year) from the first App Store version's
month, in batches of 120 days. A missing report for a day less than two days
old is left out, since Apple publishes a day's report the next morning,
Pacific time; an older missing report means no sales and is sent as zeros.
A failure is printed and never fails the report. The iOS App Store status
shortcut runs it, so the desk updates whenever the store status does.

**Storage.** `marketing_app_store_days` keeps one sealed row per day
(downloads, redownloads, updates, and first-time downloads by country and by
device), at most 800 days; older rows are dropped on each send.
`marketing_app_store` keeps one sealed row per organization: the app (id,
name, bundle id), the newest App Store versions and customer reviews from
the report, the last public lookup, and who sent the numbers when. Sending
numbers for a different app id clears the stored days. Each send also
records a metric snapshot (source `app-store-connect`) with the 30-day
counts, so channel health, the channel card and the weekly snapshots see
them; an identical send within 12 hours records none.

**API.** `GET /api/marketing/<org>/app-store` (any member) returns
`{ok, canPublish, appStore}`. `appStore` has `app`, `lookup`, `versions`,
`reviews`, `publishedAt`, `publishedBy`, `fresh` (sent within 7 days),
`firstDay`/`lastDay`, `days` (every day from the first to the last, gaps as
zeros, for the chart), `total`, `bestDay`, and `windows` for 7, 30, 90 and
365 days. Each window ends on `lastDay`, not today, and carries the three
counts, `previous` (the same span before it), `comparable` (the stored days
reach back over the previous span, so a change is meaningful), the top 10
`territories` and `devices`, and `territoryCount`. `PUT` (owners and admins)
takes `{app: {id, name, bundleId}, days: [{day, downloads, redownloads,
updates, territories, devices}], versions, reviews}`; at most 400 days per
request, each within the last 800 days. It adds the `app-store` channel with
`https://apps.apple.com/app/id<id>` as its link if the organization does not
have one.

**What the desk shows.** Downloads, redownloads and updates for 30 days, 90
days or a year with the change against the span before (only when
`comparable`), rating and ratings count, all-time downloads, best day, a bar
chart per day (per week for a year) with a choice of downloads, redownloads
or updates, top countries and devices with their share of downloads, recent
reviews, the store listing (version, release date, genre, price, minimum
iOS) and the newest App Store versions with their review state.

Not covered: App Store impressions, product page views and conversion rate
need App Store Connect's Analytics Reports API (asynchronous report
requests); proceeds are left out because Marketing moves and reports no
money.

## Other channel tools

- **Mastodon people to follow** (`GET /mastodon?topics=`): suggestions,
  followers, people who engaged with recent posts, hashtag timelines and
  account searches for up to ten topics, ranked by topic matches and
  engagement, excluding already-followed accounts (relationships check plus
  the kept following list); 40 returned. `POST /mastodon` `{"follow": id}`
  (owners and admins) follows with the channel's write-only access token
  (`read` and `write:follows` scopes); audited as `marketing.mastodon.follow`.
- **Suggested emails** (`GET /email-suggestions`): up to 12 CRM contacts to
  reply to, follow up (14+ days) or introduce, from
  `crm.email_suggestions`, with filled templates and a Zoho Mail or Gmail
  compose link. Nothing is copied into Marketing.
- **Discord posts** (`GET /discord-posts`, members and up): the last 6 days of
  messages in the connector's chosen public channels (10 channels, 50
  messages each) through `runtime.discord_recent_messages`; read on demand,
  re-read when older than 60 seconds, never stored. The desktop Discord page
  charts them as **Posts over time** (posts per day over those 6 days) next to
  Views over time; the chart only covers the window the posts endpoint reads.
- **Browser activity** (`POST/GET /activity`): lines the extension writes
  (`read`, `sent`, `failed`), private to the writing account, newest 40 kept.
  Every write rings the writer's devices with the payload-free account event
  `package:marketing`, so open dashboards reload.
- **Special agents on channel pages**: a desktop special agent whose definition
  lists the channel in `marketingChannels` appears on that channel's page with
  Open, Run now and a message line.
- **AI context** (every channel: the desktop channel page's head and the web
  channel card): copies one Markdown prompt to paste into Claude or another AI
  chat. It holds the instructions (diagnose, find what worked, three changes
  with the number to watch, three drafts, "Notes for next time") and the
  channel's data: settings, numbers now and a range ago, history, Analytics
  views, posted links (desktop), content, Discord posts, ads, campaigns,
  checks and the category playbook. To make the advice improve on each run,
  each copy saves the channel's numbers on this device (desktop `QSettings`
  `marketing/aiContext/<org>/<channel>`, web `localStorage`), and the next
  copy adds a "Since the last copy" section with then → now so the AI can
  check its earlier predictions. Nothing is sent to a server.

## Activity pings

Every successful change raises a `plugin_activity` ping (`ACTIVITY_PINGS` in
`content/worker/marketing.py`): content, numbers, App Store numbers,
settings, channels, ads, experiments, funnel, Reddit threads and rules,
campaigns, keywords, the X connection, X posts, Mastodon follows, refreshes,
captures, the ready post and posted links. Reads, the AI assistant and link building raise none, and
neither does a failed request.

- **Who**: the member who acted ("By you in acme") and the organization's
  owners and admins (at most 10 in all). Pings open `/dashboard/marketing`.
- **Browser plugin** (`POST /activity`): `read` lines ping only the writer;
  `sent` lines also ping owners and admins; `failed` lines ping the writer.
  The line's text is the ping body.
- **Syncs ping only for something new**: an extension post sync
  (`POST /posts {"posts": [...]}`) pings only when it added posts ("2 new
  channel posts found"); refreshed numbers on known posts stay quiet.
- **Bursts fold**: edits, number logs (App Store ones too), refreshes,
  captures, Reddit thread adds and `read` lines share one ping per member,
  action and item for 10 minutes. The first rings the phone; later ones update that ping in the
  inbox without a banner (`push=False`, an in-process memo of 512 keys).
- Pings follow each person's Pings settings under "App package activity".

## Surfaces

- **Web**: Dashboard, Performance, Channels (cards with health, playbooks,
  desks and actions), Ads manager and Growth.
- **Desktop** (`MarketingDashboard.cpp`, `MarketingPages.cpp`,
  `MarketingCommon.cpp`): the same five sections from the same API, a channel
  bar with one chip per channel (each with a sparkline), a page per channel
  (KPIs, next steps, desk, health, followers and views charts, playbook, posts
  and content with views per item, ads, recorded numbers, and
  channel-specific panels), and a square per channel in the status rail. When
  nothing changes for an hour the desktop raises a reminder ping for the
  channel that most needs attention. **Hand off** is web-only; the desktop
  copies the brief.
- **iOS and Android**: under Business in the You hub, with Overview, Content,
  Campaigns, Channels, Ads, Growth, Reddit and Performance. They read
  `GET /api/marketing/<org>` and write through the same routes; each record's
  fields are described in `MarketingForms`. A settings `PUT` always carries
  the full channel list. Extension captures open in a desktop browser.

### Web page states and controls

The web view (`content/web/view.html`, `page.js`) renders inside the
dashboard shell and uses its shared `fm-*` components rather than its own, so
the states that only appear on an event match the rest of the dashboard:

- The page error and notice lines are `fm-flash` (danger and info); the text
  goes into their `-text` child so the icon stays. The traffic setup prompt,
  the Connect Analytics prompt, the extension banner, the X guide and the
  settings' plugin data box are flashes too, with actions as `fm-btn-sm` on
  the right. An agent error is a danger flash (a warning one when the agent is
  busy).
- Every dialog and drawer uses the `fm-dialog` header, body and footer; the
  close button is an invisible icon button and the footer holds Delete on the
  left and Cancel and the confirm button on the right.
- The header's More menu is an `fm-popover` with `fm-menu-item` rows.
- Empty tables, charts and lists are `fm-blankslate` with one muted icon, a
  title, a muted line and at most one small button.
- Status pills (content, ads, experiments, campaigns, Reddit threads, channel
  health) are outlined `fm-label`s coloured by status: success for published,
  active, running and replied; attention for review and paused; blue for
  planned.
- Green is kept for one create button per section (New content on the
  dashboard, New ad, New experiment, Add channels, Log thread, Install) and
  for dialog confirm buttons. Panel, card and row buttons are neutral
  `fm-btn-sm`; the selected tab underline, today's date and the top
  experiment score use the blue accent instead of green.
- The view hides every `.hidden` element with `!important`, because the
  shell's component rules load after the utility sheet and would otherwise
  show a hidden flash or button.
- Confirmations still use the browser's `confirm()`.

## Package tests

```sh
python3 -m pytest -q marketplace/packages/marketing/tests
```
