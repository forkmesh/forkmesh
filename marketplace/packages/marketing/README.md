# Marketing package

Marketing is an opt-in organization package for planning and measuring content
across up to 100 channels used by marketing teams worldwide. The catalog covers
social, owned, search, advertising, publishing, community, commerce, video,
audio, review, developer, and event channels that are broadly available.
It provides a searchable Add Channels page, one-click dashboard setup,
per-channel metric snapshots, a shared content pipeline, channel links, and
matching web and native desktop surfaces.
A configured Blog website is
discovered through its RSS/Atom alternate link; saving or refreshing the
channel reads its feed and idempotently imports published posts into the
content pipeline together with the feed title, description, and last-read time.

The catalog was reviewed on September 2, 2026. Ordering starts with the
platforms holding the largest global audiences in
[DataReportal's Digital 2025 platform rankings](https://datareportal.com/reports/digital-2025-sub-section-top-social-platforms)
and [Similarweb's most-visited social and community sites](https://www.similarweb.com/top-websites/computers-electronics-and-technology/social-networks-and-online-communities/),
then broadens into specialist channels. Channels outside the product's policy
scope were removed.

## Channels page and one-click updates

The **Channels** page (web) groups every configured channel by category. Each
card shows the audience and its 30-day change, engagement rate, posts in the
last 30 days, the last post, rating and reviews when the channel has them, the
page's own title or handle, its most recent posts, and a health badge:

| State | Meaning |
|---|---|
| On track | Every check passes. |
| Needs attention | A check fails: no channel link, numbers older than 7 days, no post within twice the category's cadence, or a shrinking audience over 30 days. |
| Stale numbers | The latest snapshot is older than 14 days, or there is none. |
| Not set up | No channel link and no numbers. |
| Paused | The channel is switched off. |

The worker computes health (`summary.channels[].health`, `summary.health`) so
web, desktop and phones show the same thing, and the weekly report gains a
**Channels** table with a to-do list of failing checks. Each category carries
a playbook (`channelPlaybooks`: goal, three numbers to track, a cadence in
days and tips) shown beside the group on the web and in the Status tooltip on
the desktop.

**Update** sits on every channel, on the dashboard table, the Channels cards,
the desktop channel table and the phone apps' Channels list. It needs the
ForkMesh browser extension (2.4.0 or later): the extension opens the saved
channel link in a new tab, reads the counts the page shows (followers,
following, posts, likes, comments, shares, views, reviews and the rating,
from the page's structured data, description and visible labels), posts them
to `POST /api/marketing/<org>/capture`, closes the tab and returns to
Marketing, which reloads. The first update on a site asks Chrome once for that
one site on the extension's own page; the popup's **Allow channel sites**
grants every configured channel at once. **Update stale channels** runs the
updates one after another.

The desktop and the phone apps cannot talk to a browser extension, so their
**Update** opens `/dashboard/marketing?org=<org>&page=channels&capture=<id>`
in the default browser; the page starts the same update there. Phone browsers
cannot run Chrome extensions: on a phone the page says so and opens the manual
**Log metrics** form for that channel instead.

`/capture` accepts a page only from the channel's own site (the catalog's
`hosts` for that channel, or the host of its saved link), refuses a read with
no numbers, keeps reach, clicks and impressions from the previous snapshot,
records a snapshot with `source: "extension"`, and stores the page profile
(title, description, handle, up to five recent posts, what matched) on the
channel as `capture`. Any member may update a configured channel; only an
owner or admin may add a channel this way, and only they fill an empty
channel link from it. ForkMesh's servers never sign in to or fetch the
channel: the person's own browser reads the page they asked for.

### Desktop channel rail

On the desktop each configured channel also gets a square in the channel
status rail beside the special agents (`docs/desktop/agents-ui.md`). The square
shows the channel's main count (stars for GitHub, else followers, else views,
else reach, else posts in the last 30 days, compacted as 12.8k), a border in its health colour and a
badge with the number of failing checks. Paused channels sort last. A click
opens the channel's page on the Marketing dashboard (below). A right-click
lists the numbers, how old they are, the category's goal, and next steps
derived from the same health checks.

While the package is installed on the desktop the rail always has at least
one square: until the channels arrive, or when there are none to show, a
**Marketing** square (count —) says why: Loading channels, Could not load
(it retries in the background after 30 s, doubling up to every 15 minutes), No organization yet, Not installed for
this organization, or No channels turned on. A click opens the Marketing page,
and the square never raises the channel reminder.

| Check or state | Next step | What choosing it does |
|---|---|---|
| No channel link | Add the link | Selects the channel's row and focuses its link field |
| Numbers older than 7 days | Refresh the numbers, or update them with the browser extension | Runs the channel's Refresh, or the extension Update when it has none |
| Numbers older than 7 days | Log the numbers by hand | Opens Add metrics for that channel |
| No post within twice the cadence | Post on the channel | Opens New content as a draft for that channel |
| Audience shrank over 30 days | The category's first playbook tip | Opens the channel's page |
| Every check passes | Plan the next post, plus a rotating playbook tip | Opens New content as a draft |
| Paused | Turn it back on (owners and admins) | Ticks the channel's Active box, which saves |
| Has a link | Open the channel | Opens the link in the browser |

Choosing a step opens the Marketing page, which reloads, and a Blog that was
never read starts a second reload. The dashboard table now keeps the selected
channel row, and focus in a channel's link field, across every rebuild, so
the row a step picked is still selected when the numbers arrive. Every step
except Add the link opens the channel's page; Add the link goes to its row
in Manage channels.

When no channel's count, state or failing checks change for an hour, the
desktop raises a ping that stays until it is dismissed, naming the channel
that most needs attention and its first next step; a click opens that
channel's page. Stale numbers come first, then a failing check, then a channel
that is not set up, and the most failing checks breaks a tie
(`docs/desktop/agents-ui.md`, "Channel reminder").

### Desktop channel page

Clicking a channel opens its own page in place of the dashboard: its square
in the rail, its tile in **Channels**, its name in **Manage channels**, or
its chip in the channel bar.

The channel bar sits under the section switcher on every Marketing page,
so another channel is one click away from wherever you are. Since
2026-10-07 it is also where the Dashboard shows its channels: the big
**Channels** panel of tiles is gone and each chip carries the tile's
numbers in two lines, so the overview no longer costs a screen of
scrolling.

- **All channels** comes first and opens the Dashboard; it counts the
  active channels, those on track and those that need a look.
- One chip per channel follows, active channels first and the largest
  audience first. Line one: brand mark, name, the main count with its
  30-day change, the health dot and **⋯**. Line two: what the count is,
  engagement rate, posts in 30 days, the channel-specific facts the tile
  showed, the last post, posts queued or scheduled for it, a warning when
  its numbers are a week old or more, and the numbers-guide step
  (`setup 2/4`). Phrases do not break across lines.
- **⋯** or a right-click opens the tile's actions: Open, Log, Refresh
  (greyed while it runs or when it cannot), Update (extension) and Edit.
  The tooltip gives the health checks, where the numbers came from and the
  next guide step in full.
- **+ Add channel** and **Manage** sit in the **All channels** chip
  (1.26.0; the catalog size is the tooltip).

Paused channels are greyed. The open channel's chip, or **All** on the
Dashboard, is highlighted. A Marketing agent "needs a look" suggestion now
opens that channel's page.
**‹ All channels** goes back, as do changing the organization and any link
that scrolls the dashboard to a panel. The page reloads with the rest of
Marketing and stays open across reloads; a channel that drops out of the
summary closes it.

- **Header**: the channel's mark and name, category, health and link, with
  Open, Refresh, Update (extension), Log numbers and New post.
- **Special agent**: the special agent attached to the channel, with Open,
  Run now and a message line; hidden when none is attached
  ([Special agent on a channel page](#special-agent-on-a-channel-page-1210)).
- **KPIs**: the channel's main count (the tile's) with its 30-day change
  and a sparkline over the 7D / 30D / 90D range, reach, engagement rate,
  posts in 30 days against the cadence, the last post, and how old the
  numbers are.
- **Next steps**: the rail's steps for the channel, as links.
- **Desk** (titled with the channel, e.g. **X desk**) and, on Reddit, the Reddit desk
  ([Channel desks](#channel-desks-1360)).
- **Health**: each check and the numbers guide, ticked or crossed.
- **Channel settings**: active, display name, link and the last refresh,
  read-only; **Edit in Manage channels** goes to the editable row.
- **Over time**: a line chart of Followers, Reach, Engagements or Views.
- **Playbook**: the category's goal, what to track and its tips.
- **Posts and content**, **Ads**: the channel's own items; double-click
  edits one, **New post** drafts one for the channel. Posted links are the
  first table in Posts and content, with **Add link** beside New post
  ([One row of work on a channel page](#one-row-of-work-on-a-channel-page-1270)).
- **People to follow** (Mastodon): one scrolling column beside Posts and
  content, skipping anyone the account already follows.
- **Discord posts in the last 6 days** (Discord): what was posted in the
  channels the organization's Discord connector reads, newest first
  ([Discord posts on the channel page](#discord-posts-on-the-channel-page-1300)).
- **Recorded numbers**: every snapshot, newest first, with its source, in
  the right column under **Searches to check**.

The rail tile carries `opensPage` in `NativePackageRailTile`; the host then
calls `openRailTile` before showing the package page
(`docs/desktop/agents-ui.md`).

### Discord through the organization's connector

Refreshing the Discord channel reads the organization's own Discord connector
first (the bot the owner authorized under the organization's Discord
settings). The host capability `runtime.discord_guild(org_bi)` returns the
connected server's name, description, vanity invite, approximate member count
(recorded as followers) and approximate online count (recorded as reach). It
needs the `organization.discord.read` permission in the manifest, the package
installed in that organization, a current owner grant and the bot token; the
bot token never reaches the package. A refresh also reads the chosen
channels' messages of the last 14 days and keeps only the time of the newest
one, so the posting check sees them
([Discord messages count as posting](#discord-messages-count-as-posting-1331));
the channel page's post list reads them live and stores none
([Discord posts on the channel page](#discord-posts-on-the-channel-page-1300)).

When the organization has no connector, the refresh falls back to the public
invite (`discord.gg/<code>`) or server widget read from the channel link. When
the connector is set up but failing (bot removed, access lost, rate limited)
and the public read fails too, the connector's error is reported, since that
is the one to fix.

The channel's **Open ↗** link (web, desktop) follows each channel's
`openUrl` from the settings, not the raw channel link. A
`discord.com/channels/<server>/<channel>` link only works for someone already
signed in to Discord and otherwise lands on Discord's login page, so for
Discord `openUrl` is a public invite: the channel link itself when it is a
`discord.gg/<code>` or `discord.com/invite/<code>` invite, otherwise the
invite the last refresh recorded as the feed's `siteUrl`. A refresh records
the server's vanity invite, an invite channel link, or the server widget's
`instant_invite` (read through the widget when the connector reports no
vanity code). Without any of these the link stays the channel link; turn on
the server widget's invite channel or save an invite link to fix it.

### Google (1.11.0)

The catalog's `google-search` channel is named **Google** and tracks two
numbers, kept on every snapshot beside the others:

| Field | What it counts | Where it comes from |
|---|---|---|
| `visits` | Sessions that arrived from Google in the last 30 days: untagged visits referred by `google.com` or a country domain (`google.co.uk`), plus visits tagged `utm_source=google` (paid included). Gmail and Google News referrals are not counted. | The organization's Analytics, same project and site host as the Performance page |
| `indexedPages` | Pages Google lists for `site:<your host>` | The browser extension's **Update**, which opens the channel link and reads Google's result count |

When the channel has no link, it gets
`https://www.google.com/search?q=site:<host>`, the host being the Analytics
site host, else the host of the attribution landing page, else (1.24.1) the
host of the ForkMesh instance itself (`forkmesh.com` on forkmesh.com), with
`www.` dropped. The link is filled whenever settings are read, so a fresh
organization's Google channel is set up and refreshable without a save.
**Refresh** (desktop, `POST /refresh` with `channel: google-search`) records
`visits` only and carries `indexedPages` forward; it answers
`analytics_not_installed`, `analytics_no_project` or `analytics_unavailable`
when Analytics cannot be read. **Update** (extension) records the page count
and, in the same snapshot, the current `visits`. Both can also be logged by
hand in **Log metrics** (Visits, Pages on Google).

Google answers searches from servers with a block or a script-only page, so
the count is read where every other channel's page is read: in the person's
own signed-in browser. On a Google results page the extension reads
`#result-stats` ("About 1,230 results"); when Google hides it and the results
fit on one page, it counts the result links instead. It reads nothing else
from the page, so numbers in other sites' snippets never become followers or
likes. The count is Google's estimate, not an exact index size; Search
Console's Pages report is the exact one.

Google is on every organization's dashboard by default, beside Blog, Reddit,
X, YouTube and Mastodon (`STARTER_CHANNELS`; the legacy five-channel
`DEFAULT_CHANNELS` stays as it is for old tables), so it has a square in the
desktop channel rail without being added. It can be paused like the others.
Google has no posts, so its health skips the "Posted in the last N days"
check.

Where the numbers show:

| Surface | Google |
|---|---|
| Web | Tile main number is visits (else pages on Google); the card adds **Visits · 30D** and **Pages on Google** |
| Desktop table | **Visits · 30d** and **Pages on Google** columns for every channel; the Status tooltip names both |
| Desktop rail | Square counts visits in 30 days (else pages on Google); details list both. Its steps are **Refresh visits from Google** and **Count pages on Google with the browser extension** (opens the web Update) |
| iOS and Android | The Channels row reads "N visits from Google in 30d · N pages on Google" instead of followers and posts |

### Reddit channel numbers (1.12.0)

Reddit gives ForkMesh's servers no counts: the Reddit channel's **Refresh**
reads the subreddit's public Atom feed, which imports its posts but carries no
member or visitor numbers (`docs/community/reddit.md`). The numbers come from
the browser extension (2.6.0 or later), so the channel guides people there.
The worker adds `health.guide` to the Reddit row of `summary.channels`, a
list of steps with `id`, `label`, `action` and `done`:

| Step | Done when | Action |
|---|---|---|
| `link`: add the subreddit's link (`/r/<name>/`, or a `/user/<name>/` profile) | The saved link is a subreddit or user address on `reddit.com` | **Add link** opens Manage channels (owners and admins) |
| `extension`: install the browser extension and sign in | A snapshot was ever read by the extension (the web page also ticks it once the extension answers) | **Get the extension** opens the Extension page |
| `update`: press Update | The latest extension snapshot is under 7 days old | **Update** |

The web Channels card shows the steps under its checks until all are done,
with the action on the first step not done. On the desktop the rail square's
"numbers are stale" steps come from the same list (the link step only for
owners and admins; the others open the web Update), instead of a Refresh
that cannot read counts. The desktop dashboard's Reddit tile shows the
next step ("Get numbers · step N of 3: …", all steps in its tooltip), labels
its main number Members, lists weekly visitors and weekly contributions, and
has an **Update** button beside Refresh: before, a channel with a Refresh
got no Update on its tile, so Reddit's tile had no way to its counts. The
Manage channels table shows the same guide: the Status cell adds "get
numbers: step N of 3" until every step is done, and its tooltip lists the
steps with ✓ or ✗, explains that Refresh only imports Reddit's posts, and
gives members, weekly visitors and weekly contributions. On the Reddit row
the Reach, Followers and Engagements cells say what they hold, the Refresh
and Update tooltips say which one reads the counts, and a Refresh that read
no counts ends its status message by pointing at Update. Every other channel
gets an empty `guide`.

On a subreddit page the extension reads only the subreddit header: members
(`followers`), weekly visitors (`reach`) and weekly contributions
(`engagements`), from the header's attributes or its visible labels, or the
reader count in old Reddit's sidebar. It skips the rest of the page, so post
scores and comment counts on the listing never become likes or replies.
`/capture` accepts `reach` and `engagements` for this, so the card's
engagement rate is weekly contributions over weekly visitors (named on
screen since 1.32.0, with what to do when they stop refreshing). The card labels
them Members, Weekly visitors and Weekly contrib., the desktop says members
and lists both weekly numbers, and a Reddit page with no counts (a wrong link,
or Reddit's bot check) explains how to fix it.

### Email: outbound and closed deals from CRM and Zoho (1.13.0)

The catalog's `email` channel is named **Email** (it was "Email newsletter";
the id stays) and is a starter channel, so every organization's dashboard and
desktop rail show it. The CRM package already owns the organization's Zoho
Mail connection, its send log and its pipelines, so Email reads its numbers
from there instead of keeping a second Zoho connection: package secrets and
outbound origins belong to the package that calls them, so Marketing could
not refresh Zoho's token itself.

**Refresh** (`POST /refresh` with `channel: email`, no channel link needed)
calls `crm.email_channel_report(runtime, org_bi, since)` in-process, the same
way Google calls Analytics, with `since` 30 days back, and records:

| Field | What it counts |
|---|---|
| `outbound` | Outbound email in the last 30 days: CRM sends and linked messages that are not inbound and not draft, sending, failed or rejected; founders outreach sends that were accepted; and Zoho Mail messages synced from the Sent folder, skipping any already counted as a linked CRM message (same Zoho message id) |
| `replies` | Inbound messages linked to contacts in the last 30 days |
| `closedDeals` | CRM contacts whose stage is a won stage: `won`, `closed-won`, `customer`, `renewal`, `committed`, `signed`, `paid` or `active`. A bare `closed` is not counted, because the Partnerships pipeline uses it for a partnership that ended |
| `closedValue` | The sum of those contacts' deal values |

The snapshot's `source` is `zoho` when CRM has Zoho Mail connected, else
`crm`; the channel's feed link becomes the organization's Zoho Mail web app.
Without CRM installed the refresh answers `crm_not_installed`. The manifest
lists `organization.crm.read` for it.

Email's health swaps the link check for **Zoho Mail connected in CRM** and the
posting check for **Email sent in the last 30 days**, and checks that closed
deals held or grew over 30 days instead of followers (`closedDealsChange`).
`health.guide` walks through installing CRM, connecting Zoho Mail in CRM,
moving won contacts to a won stage, and pressing Refresh.

| Surface | Email |
|---|---|
| Web | Tile main number is emails sent in 30 days, with closed deals, won value and replies; **Refresh** and **CRM** on the tile; guide steps link to CRM |
| Desktop table | **Emails sent · 30d** and **Closed deals** columns; the Status tooltip adds replies and won value |
| Desktop rail and tile | Square counts emails sent in 30 days; details list closed deals and their value; Refresh works without a link and is part of Refresh all |
| Desktop channel page | KPIs: emails sent, closed deals (worth), replies |
| iOS and Android | The Channels row reads "N emails sent in 30d · N closed deals · N won" |

### GitHub: stars, forks and followers (1.14.0)

The user asked for GitHub's stars, followers and forks in the marketing
counts. Before, the GitHub refresh folded them into two numbers: `followers`
held the account's followers, or the repository's stars when the link was a
repository, and `engagements` held stars plus forks. Each now has its own
field on every snapshot, read from the public GitHub API (a token in the
channel's settings only raises the rate limit):

| Field | Account link (`github.com/forkmesh`) | Repository link (`github.com/forkmesh/forkmesh`) |
|---|---|---|
| `stars` | Stars summed over the account's own repositories (forks of other projects skipped, the 100 most recently pushed) | The repository's stars |
| `forks` | Forks summed over the same repositories | The repository's forks |
| `followers` | The account's followers | The owner's followers (one more API call) |

`engagements` stays stars plus forks, but GitHub has no engagement rate:
stars plus forks over followers came out as thousands of percent. For a project on GitHub, stars are the
audience, so GitHub's health measures growth by stars: `health.starsChange`
is the 30-day change in stars, and **Audience growing over 30 days** uses it
once a 30-day-old snapshot has stars. `followersChange` is still the
followers' change, except for a repository link whose old snapshot has no
stars: that snapshot's followers were stars, so the change is left out rather
than reported as a drop. The weekly report's Followers cell for GitHub adds
"(N stars, N forks)", and the Marketing agent's brief carries both.

| Surface | GitHub |
|---|---|
| Web | Tile main number is stars with their 30-day change, plus followers and forks; the channel card lists Stars and Forks; a refresh notice reads "N stars · N forks · N followers" |
| Desktop table | **Stars** and **Forks** columns; the Status tooltip lists stars, forks, followers and the stars' 30-day change |
| Desktop rail and tile | Square counts stars; details list stars (with their change) and forks before followers |
| Desktop channel page | KPIs: stars (with change and sparkline), followers, forks; the chart gains **Stars** and **Forks** (opening on Stars) and **Recorded numbers** gains Stars and Forks columns, both shown only for GitHub |
| Log numbers | Desktop's dialog shows Stars and Forks when GitHub is the channel, and the web dialog has Stars (GitHub) and Forks (GitHub), so a hand-logged GitHub snapshot no longer saves them as zero |
| iOS and Android | The Channels row reads "N stars · +N in 30d · N forks · N followers · …" |

## Dashboard: one command center (1.10.0)

The Dashboard page now puts everything on one compact screen, modelled on a
classic digital-marketing wallboard but built only from data ForkMesh already
has (its own Analytics, channel snapshots, the pipeline and the Ads manager):

| Panel | What it shows | Source |
|---|---|---|
| KPI strip | Visits and conversions with change against the previous window, audience (sum of followers) with a sparkline, reach and engagement rate, published and scheduled posts, live campaigns | Analytics, metric snapshots, pipeline, campaigns |
| Web traffic by channel | Organic search, paid search, social, email, referral, display, direct and other: share of visits with a marker at the previous window's share, visits, change, conversion rate, average time, bounce rate, pages per visit, and a total row | `performance.channels` |
| Search keywords | **Tracked**: the keywords the team wants to rank for, with position and its change, impressions, clicks and measured visits. **From visits**: search terms Analytics recorded | `marketing_keywords`, `performance.keywords` |
| Channels | One tile per channel with its brand mark, main number (followers, subscribers, views, reach, else posts), 30-day change, engagement, posts in 30 days, last post, a health dot, and Open, Log and Update; plus an Add a channel tile | `summary.channels` |
| Campaigns | Cards with dates and time elapsed, message, bullet bars of visits and conversions against their targets, published content and checklist, channel chips (dashed when nothing is planned on that channel), next scheduled post, open checklist items, spend and budget | `campaigns[].stats`, Analytics by `utm_campaign` |
| Marketing agent | Rule-based next steps, and **Suggest** for AI suggestions from the Marketing special agent | `suggestions`, `POST /assistant` |
| Content flow, Ads, Post to X | Pipeline stages and warnings; ad spend, CTR, ROAS and pacing of live ads; the X composer | pipeline, `adsSummary`, `/x-post` |
| Content pipeline, This week, Reach chart, Top landing pages | The full table and calendar, the channel trend chart, and the most visited landing pages | pipeline, metrics, `performance.landing` |

The date range (7D, 30D, 90D) drives the KPI strip, the traffic table, the
campaign visit bars and the keyword visits; the dashboard reads
`/performance` for that window itself.

### Campaigns

`marketing_campaigns` (sealed, at most 500 per organization) holds a name,
status (`idea`, `planned`, `active`, `paused`, `completed`, `archived`),
objective, dates, a campaign tag (`slug`, used as `utm_campaign`), owner,
landing page, channels, audience, message, offer, hashtags, search keywords,
budget and currency, visit and conversion targets, a checklist of up to 40
steps, a brief and notes. Content and ads join a campaign when their campaign
field is its name or tag; an ad also joins when its landing link carries the
tag. `GET/POST /api/marketing/<org>/campaigns` and
`GET/PATCH/DELETE /campaigns/<id>`; every member may write. Each campaign
comes back with `stats` (content by status, next scheduled post, ads and
spend per currency, channels with no content, checklist progress, time
elapsed, days left and a tagged landing link).

`campaignTemplates` carries the campaign on forkmesh.com/hackathon (the
September Revenue Contest, 15 September to 15 October 2026 UTC: 50% of each
new customer's first month and $100, $50 and $25 prizes). Until an
organization has a campaign from that page, the Campaigns panel shows it as a
template card; **Add this campaign** opens the form already filled in. Since
1.22.0 a second template carries the start-page announcement
([Start page campaign template](#start-page-campaign-template-1220)).

**Copy brief** copies the campaign as Markdown in the format of
`docs/marketing/campaigns/`. **Hand off** puts an `@Marketing:` task with that
brief on the organization's task board (`POST /api/tasks`, department
`marketing`, assigned to the agent queue, repository `forkmesh/forkmesh`);
when the board refuses, the brief is copied instead.

### The Marketing agent's suggestions

`overview.suggestions` lists rule-based next steps (`_campaign_suggestions`):
no campaign, a planned campaign past its start, an active one past its end,
nothing scheduled, a listed channel with no content, no landing page, no
target, ending within 7 days, channels that are stale or need attention, and
no tracked keywords. Each has an action that opens the right form.

`POST /api/marketing/<org>/assistant` with `{campaignId?, question?}` asks
ForkMesh AI (`runtime.ai_json`, the host's per-account quota) to answer as the
Marketing special agent, from a summary of the campaign (or every open
campaign), channels and keywords. Up to 8 suggestions come back, each a
`content` draft with ready copy and a day offset, a checklist `task`, a
`keyword` or a `campaign` idea; the page adds them with one click (content as
a draft, never posted). The manifest declares `account.ai.prompt`.

### Search keywords

`marketing_keywords` (sealed, at most 500 per organization, the term
blind-indexed so it is tracked once: a second add answers 409
`keyword_exists`) keeps a term, the page that should rank, intent, priority,
campaign, notes and up to 104 dated readings of position, impressions and
clicks. **Import** accepts a pasted Search Console Queries export, a Bing
keyword report or a plain list; known keywords get a reading for that date
and new ones are added (`POST /keywords` with `rows`). Search engines no
longer pass the query in the referrer, so organic positions come from those
tools; Analytics adds the terms it can measure (`utm_term`, see Analytics
1.2.0) and counts untagged search-engine visits as `(not provided)`.

### Posting to X

Owners and admins connect the organization's X app under **Post to X →
Connect**, either with **Guide me with the extension** (below) or by hand, with
the app's API key and secret and an access token and secret
for the posting account (OAuth 1.0a user context, app permission Read and
write). `POST /x-connection` checks the keys against `GET /2/users/me` before
storing them in the sealed settings; they are never returned (clients see
`settings.x`: connected, username, who connected it, last post) and are not
in exports. `DELETE /x-connection` removes them. `POST /x-post` with
`{text, contentId?}` posts through `POST https://api.x.com/2/tweets` and, when
the pipeline item is on the X channel, marks it published with the post's
link. The page counts characters as X does for links, warns past 280, and
asks for confirmation before each post. Members cannot post. The manifest
declares `external.social.post`.

### Desktop: the same page

The desktop Marketing page was planned as the web dashboard in native form,
but it had grown into four tabs of wide tables. Since 2026-10-06 it is one
scrolling page laid out like the web command center, from the same overview
and `GET /performance` reads:

| Web panel | Desktop |
|---|---|
| Six KPI tiles with the 7D / 30D / 90D range | Same tiles and range switch; Audience has its sparkline |
| Web traffic by channel, Search keywords | Same tables: share bars with the previous-period tick, Tracked / From visits keywords, Add and Track open the keyword dialog |
| Channels tiles | The channel bar above the page (since 2026-10-07): one compact chip per channel with the tile's numbers, and Open, Log, Refresh, Update (extension) and Edit under **⋯** or a right-click |
| Campaigns, Marketing agent | Campaign cards with flight, target bars, channel coverage, open checklist items, Edit, Draft post, Ask agent, Copy brief, Copy link and templates; the agent panel runs `POST /assistant` and adds its drafts, tasks and keywords |
| Content flow, Ads, Post to X | Same; clicking a stage filters the pipeline. Connect opens the X keys dialog; posting asks first |
| Content pipeline, This week | Searchable pipeline with status and channel filters, and the seven-day schedule |
| Reach by channel, Top landing pages | Line chart with the Reach / Engagements / Followers switch and landing-page bars |

### Desktop: Needs attention and the compact Dashboard

Since 2026-10-07 the desktop Dashboard opens with a full-width **Needs
attention** panel under the KPI strip, so the first screen says what to do
before it shows numbers. It gathers, most urgent first:

- the overview's rule-based suggestions ([The Marketing agent's
  suggestions](#the-marketing-agents-suggestions)): campaigns past their
  start or end, nothing scheduled, uncovered channels, missing landing
  pages or targets, channels that are stale or need a look, no keywords;
  these moved here from the Marketing agent panel, which now holds only the
  question box and the agent's answer;
- every ad the Ads manager's **Needs attention** panel lists (over budget,
  flight ended, start date passed first; then spending fast or slow and
  stale results), from the same `adAttention()` check;
- pipeline items past their scheduled time and not marked published, and
  items in review for more than 3 days, one row each.

Each row has its action (Open, Draft, View, New, Add). The first six show;
**Show all N** in the panel header expands the rest, and the header counts
the urgent and total items. With nothing to review the panel is one line.
Like every overview panel it can be dragged by its header; it starts first,
also for an order saved before it existed.

The same change tightened the whole page so more fits above the fold
without dropping anything: the compact KPI strip (the channel page's), compact
panel headers and bodies, 8 px gaps between panels, 26 px table rows, a
210 px chart, a shorter X composer, tighter campaign cards, ad facts and
week rows, and smaller page margins.

A switch under the title moves between the same five sections as the web:
**Dashboard**, **Performance**, **Channels**, **Ads manager** and **Growth**.
The Reddit desk was a sixth section from 2026-10-06 until 1.36.0 moved it
onto the Reddit channel page ([Channel desks](#channel-desks-1360)). Each
section brings its own buttons to the right of the switch, as the web header
does.

| Web section | Desktop |
|---|---|
| Performance | 7D / 28D / 90D window, Analytics project picker, totals with change against the previous window, Campaigns, Where to improve, Ads measured by Analytics and Landing pages; **Goals** edits the conversion goals, default project and site host |
| Channels | Health tiles (on track, needs attention, stale, not set up, audience change), status, search and category filters, channel tiles grouped by category with each category's playbook, **Update stale**, then **Manage channels** and **Add channels** |
| Ads manager | Spend, impressions, clicks, conversions and ROAS per currency, spend by channel (a bar filters the table), needs attention, and the ads table with status, search and channel filters |
| Growth | Funnel scorecard, the latest week's funnel with the bottleneck, the link builder with allowlist checks, the experiment backlog with status filters, the weekly funnel table, **Weekly report** (copy or save the Markdown), **Targets** (link-tracking allowlists and funnel targets) and **Log week** with Fill visits from Analytics |
| Reddit channel page | This week's counts, the watch list (Watch list edits it), Before you reply, subreddit rules with **Copy rules for the scout**, and the thread log with Log thread, Edit, Open on Reddit and Copy tagged link; a thread logged twice opens the existing one; the Status column shows a monitored thread's views (1.15.0) |

The dashboard also connects X (**Post to X → Connect**, the same
`POST`/`DELETE /x-connection` as the web) and imports keywords (**Search
keywords → Import**, the same parser and `POST /keywords` rows). **Manage
channels** and **Add channels** live on the Channels section; the dashboard's
links and the rail's Add the link step switch to it. A click on a channel
tile or on a channel's name in Manage channels opens the
[channel page](#desktop-channel-page). Panels sit three across and fall back
to two, then one, as the window narrows. The only web feature the desktop
leaves to the browser is **Hand off**: it needs the task board API, which
native packages cannot call, so **Copy brief** puts the same Markdown brief
on the clipboard instead. **Open on web** opens the section that is showing.
The panel widgets are in `MarketingDashboard.cpp`, the five added sections
and their dialogs in `MarketingPages.cpp`, and the helpers both use in
`MarketingCommon.cpp`. The phone apps carry the same sections (see
[Phone apps](#phone-apps)).

## Ads manager

The Ads manager plans and measures paid placements on any of the 100 catalog
channels — a YouTube pre-roll, a Reddit promoted post, a newsletter slot, a
trade-show booth. Each ad carries its channel, objective, campaign, audience,
creative copy, flight dates, a lifetime or daily budget in its own currency,
and the totals its platform reports: spend, impressions, clicks, conversions,
and attributed revenue. The package derives CTR, CPC, CPM, CPA, and ROAS,
compares spend with what the flight should have used by now (on pace,
spending fast or slow, over budget), and totals results per currency and per
channel; amounts in different currencies are never added together.

Channels bought in a self-serve console (Google Ads for YouTube and Search,
Meta Ads Manager, Reddit Ads, X Ads, LinkedIn Campaign Manager, Microsoft
Advertising, and others) link to that console; every ad can also keep a direct
link to itself there. Other channels are marked as booked directly. Marketing
never connects to an ad account, holds no ad-platform credentials, and spends
no money: ads are bought in the platform and results are logged by hand.
`test_worker_moves_no_money_and_calls_no_ad_or_payment_api` keeps that true:
the worker's outbound calls stay in its feed reader, page reader and X
client, its only API hosts are `api.github.com` and `api.x.com`, and it names
no payment or ad-platform API. iOS 1.2.8 hides the budget and spend fields to
lower App Review 3.1.1 risk; this test is part of the evidence for showing
them again.

Catalog inclusion means the channel is generally available, not that it is
approved or compliant in any particular market. Teams remain responsible for
platform terms and for the advertising, disclosure, review, email, SMS,
privacy, and industry rules of every market they publish in.

## Growth

The Growth page (1.5.0) runs the measure-decide-ship loop the ForkMesh
Marketing special agent follows (`docs/marketing/README.md`) on the web
surface:

- **Funnel scorecard.** One row per week (Monday, UTC) with the north star,
  visits, signups, activated, retained out of an eligible base, referral
  signups and revenue, copied by hand from the team's own analytics with a
  note of the source. A field left empty stays empty: it is shown as a dash
  and never counted as zero. The page derives signup, activation, retention
  and referral rates and their change from the week before, marks a rate as
  too small when its base is under 20, and names the bottleneck as the rate
  furthest below the target an owner or admin set. With no targets it names
  none.
- **Experiment backlog.** Ideas scored 1 to 10 for impact, confidence and ease;
  the score is their product and the top three ideas are highlighted. Each
  experiment keeps its stage, hypothesis, audience, success measure,
  baseline, target, dates, review date, decision and result. The page warns
  when more than three run at once, when a review date or an end date has
  passed, and when an open experiment has no score.
- **Link builder.** Builds a UTM-tagged link and checks it against the
  organization's allowlists of sources, mediums and campaigns, its tracked
  domains and the paths its analytics does not record. The Ads manager runs
  the same checks on every ad's landing page.
- **Weekly report.** `GET /api/marketing/<org>/report` returns the scorecard,
  the backlog's top three, running experiments and reviews due, ads that need
  attention and the week's content as Markdown, so the numbers can be pasted
  into a plan, an issue or a special agent's run.

The desktop has the same Growth section (see
[Desktop: the same page](#desktop-the-same-page)); it reads the same API.

## Performance

The Performance page (1.9.0) answers "which posts, ads and campaigns work,
and where should we improve" from the organization's own **Analytics**
package, so nothing is copied by hand:

- **Totals** for the chosen window (7, 28 or 90 days) against the window
  before: visits, visitors, conversions, bounce rate and the share of visits
  that carry a campaign tag.
- **Campaigns.** One row per `utm_source / utm_medium / utm_campaign` (or
  referrer for untagged visits) with visits, change, bounce rate,
  conversions, conversion rate, spend per visit from the Ads manager, and the
  page most visits land on. A campaign missing from the allowlist is flagged.
- **Ads measured by Analytics.** Each ad is matched to visits by the UTM tags
  of its landing link (every tag the link has must match; `utm_content`
  narrows it to one creative). It shows logged clicks against measured
  visits, measured conversions, and cost per visit and per conversion in the
  ad's own currency. Spend is the ad's logged lifetime spend, so choose a
  window that covers the flight.
- **Landing pages**, and on the web **Reddit desk results** (visits per
  subreddit, through the `utm_content=<subreddit>` tag on each thread's
  link). The desktop shows them on the Reddit channel page as **Results by
  subreddit** (1.36.0).
- **Where to improve.** Ranked findings computed in the worker
  (`_performance_insights`): setup gaps, no goal, untagged traffic over half
  of visits, traffic down 30% or more, a campaign converting at under half
  or over 1.5 times the site rate, campaigns or landing pages where most
  visits bounce or nothing converts, ads with untagged links, spend without
  visits or a wide click-to-visit gap, the cheapest conversion per currency
  and the best subreddit. Rules need at least 20 visits.

**Goals** (owners and admins, up to 5) define a conversion: an Analytics
event name, optionally one property that is or contains a value, for example
`$pageview` with `$pathname` containing `/signup`, or ForkMesh's
`forkmesh_product_action` with `action` containing `install`. Goals, the
default project and the site host are stored in the settings under
`analytics`. The funnel dialog's **Fill visits from Analytics** copies the
week's visits into the scorecard and names the source.

`GET /api/marketing/<org>/performance?from=&to=&project=` returns the same
data. The weekly report gains a **Campaign results (Analytics)** section for
the last seven days, so the Marketing special agent's run reads measured
results. When Analytics is not installed or has no project, the page, the
API and the report say what to set up instead of showing zeros.

Marketing reads Analytics in-process (`analytics.campaign_report`) after its
own membership and install checks, and only for the member's organization;
the manifest lists `organization.analytics.read` for it.

## Reddit desk

The Reddit desk (1.6.0) is where the team keeps the Reddit threads it
might answer, so the work the Reddit scout special agent
(`docs/community/reddit.md`) briefs is tracked in one shared place:

- **Watch list.** Up to 40 subreddits and 40 searches (each at most 120
  characters), kept in the organization's settings under `reddit` and edited
  by owners and admins only. Each one is a link that opens Reddit's newest
  posts in the person's own browser. It starts from the scout's watchlist.
- **Thread log.** One row per Reddit post (`marketing_reddit_threads`, at most
  2,000 per organization, the post id blind-indexed so a thread is logged
  once: a second log answers 409 `reddit_thread_exists` with the existing
  id). Each row keeps the link (any `www.`, `old.` or bare `reddit.com`
  thread address, stored as `https://www.reddit.com/r/<sub>/comments/<id>/`),
  the title, a note, a status (`to_review`, `drafting`, `replied`,
  `skipped`), the forkmesh.com page the tagged link lands on, and two checks
  the person ticks: **Rules read** and **Disclosed**. `replied` needs both
  checks and the address of the reply, which must be a comment in the same
  thread. Every member can log and edit threads.
- **Tagged link.** Each thread gets a forkmesh.com link tagged
  `utm_source=reddit&utm_medium=social&utm_campaign=reddit_scout` and
  `utm_content=<subreddit>`, checked against the attribution allowlists like
  every other link.
- **Subreddit rules.** The rules of each watched subreddit, with when and by
  whom they were read, or a link to the rules page when nobody has read them
  yet. **Copy rules for the scout** (1.6.1) copies every rule set on file as
  Markdown (subreddit, read date, rules page, numbered rules, and the watched
  subreddits not read yet, without the reader's name) for pasting into a
  Reddit scout run prompt, since the scout has no member session on the desk.
- **Weekly report.** The report's Reddit section counts threads logged,
  replied and skipped in the last 7 days, lists the replies, and says how many
  watched subreddits have rules on file.
- **Where to post next** and **What to talk about** (1.32.0), and **Walk the
  watch list**, which has the browser extension read every watched page at a
  reader's pace. Both are described under "Reddit clues, the walk, and what
  the engagement rate divides by".

### Why the browser reads Reddit

ForkMesh's servers cannot read what the desk needs: Reddit answers their
requests for listings, searches, threads and subreddit rules with 403 (its
JSON endpoints and its OAuth API without an approved app, 2026-10-02, see
`docs/community/reddit.md`), and its public Atom feeds, which the Reddit
channel refresh still reads, carry no comment counts, scores or rules. The
desk therefore never fetches Reddit on the server. People either log a thread by hand or let the
ForkMesh browser extension (2.3.0 or newer) read the Reddit pages they open
themselves: watched subreddit listings, searches for a watched query, threads
in a watched subreddit, and watched subreddits' rules pages. The extension
sends what it read only when the person presses **Send to Reddit desk** in its
popup (`marketplace/packages/browser-extension/content/extension/README.md`).

The browser-read fields on a thread are optional: `author` (a Reddit
username), `comments` (0 or more), `score` (may be negative), `postedAt`
(milliseconds) and `source` (`manual` by default, `extension` when the
extension sent it). A count nobody read stays empty, never zero. The
extension never sets **Rules read**, **Disclosed** or the reply: those are the
person's own statements, made on the desk.

Rules are stored in the organization's settings row under `redditRules`,
keyed by subreddit (at most 30 rules a subreddit, titles up to 200 and
descriptions up to 2,000 characters, at most 200 subreddits, oldest dropped
first). `POST /api/marketing/<org>/reddit-rules` with `{subreddit, rules}`
replaces one subreddit's rules (an empty list means the page had none) and
`GET` lists them; any member may call it, because reading a public rules page
is not a settings change. Every other settings write keeps the stored rules,
so the settings `PUT` (owners and admins) cannot replace or drop them, and the
settings payload does not carry them; the overview's `redditDesk.subreddits`
does.

### Monitored threads (1.15.0)

The user asked to add threads and watch their views through the extension
(2026-10-06; the first was their own reply in r/devops's weekly
self-promotion thread). A thread on the desk can be **monitored**:

- `monitored` (true or false) and `commentUrl`, the comment to follow. A
  comment link pasted as the thread address (`/comments/<id>/comment/<cid>/`
  or `/comments/<id>/<slug>/<cid>/`) sets it and the thread address drops the
  comment; with no comment set the reply is followed. The thread projection
  carries `monitorCommentUrl` and `monitorCommentId`, the one being followed.
  Before 1.15.0 a `/comment/<cid>/` link was stored with `comment` as the
  slug, an address that does not open the thread.
- A reading is a `PATCH` with `checkedAt` (milliseconds) and any of `views`,
  `score`, `comments`, `commentViews`, `commentScore`. It sets the latest
  numbers and adds a point to `history` (at most 180 points; a reading within
  30 minutes of the last point replaces it, so a thread read on every visit
  does not fill the history in a day). A reading older than the last point is
  ignored, since two browsers may send out of order.
- The web desk has a **Monitored** tab, a **Monitor views with the browser
  extension** box and a **Comment to monitor** field in the thread dialog, and
  shows each monitored thread's views with the change over the last day (or
  since the first read), the comment's points and views, and when it was read.
  The desktop desk does the same, because the user asked for it on the
  desktop too: a **Monitored** tab in the thread log, the views and their
  change in the Status column with the full reading in the row's tooltip,
  **Monitor a thread…** (paste a thread or comment link; an already logged
  thread is switched to monitored), and the thread dialog's **Monitor views
  with the browser extension** box, **Comment to monitor** field and last
  reading. The desktop cannot read Reddit itself: the numbers still come from
  the extension. `redditDesk.summary.monitored` counts them, and the weekly
  report lists each monitored thread's last numbers.

Views come only from the extension. Reddit shows a post's or comment's view
count only to the account that posted it, and not to logged-out readers (a
logged-out fetch of the r/devops thread, 2026-10-06, had scores and comment
counts but no views), so the numbers are read in the person's own signed-in
tab. A count the page did not show stays empty.

Nothing on the desk posts, votes or sends anything to Reddit.

## Posted links (1.16.0)

The user asked to add links to posts on any marketing channel by hand on the
desktop and track them, and for X to scroll the profile in the browser and
have the extension read every post's stats and add the links (2026-10-06).
Monitored Reddit threads already did this for Reddit only; posted links do it
for every channel.

- `GET /posts` lists them (newest posted first, at most 2000 per
  organization) with `postsSummary`: the total, summed `views`, `likes`,
  `replies`, `reposts` and `engagements` (likes + replies + reposts), and the
  same per channel. The overview carries `posts` and `postsSummary` too.
- `POST /posts` adds one: `url` (a link typed without `https://` gets it),
  `channel` (optional: without it the channel comes from the link's site, so
  x.com, twitter.com, linkedin.com, youtube.com and the other channel hosts
  need none; a newsletter or blog link needs one, else
  `post_channel_required`), `title`, `note`, `postedAt` and any of the four
  counts. Links are compared after cleanup (lowercase host, no fragment or
  trailing slash; X and Twitter status links become
  `https://x.com/<handle>/status/<id>` without `?s=` tags), and an existing
  one answers 409 `post_exists` with `postId`.
- `PATCH /posts/<id>` edits it. Counts work like a monitored Reddit thread's
  reading: `checkedAt` (now when absent) and the counts set the latest numbers
  and add a `history` point (at most 120; within 30 minutes the last point is
  replaced; an older reading is ignored). An empty count keeps the old one.
- `POST /posts` with `{"posts": [...]}` (at most 500) is the extension's
  import: each entry is added, or updates the counts of the link already
  there without replacing a title or posting date someone typed. Entries with
  an unusable link or no channel are counted as `skipped`. The answer has
  `created`, `updated`, `skipped`, `full` and `postsSummary`.
- `DELETE /posts/<id>` stops tracking it.
- A posted link counts as a post in its channel's health: `postsLast30d`
  and `lastPostAt` include its `postedAt` (a link whose address is also a
  published content item counts once), and a channel with links is listed
  in the summary even when it is not set up.

On the desktop the Channels page has an **Add link** button and a **Posted
links** panel listing every channel's links with their posting date, views,
likes, replies, reposts, when they were last read and who added them
(**Extension** for imported ones). Each channel page has its own **Posted
links** panel with **Add link** preset to that channel. Double-click a row to
edit its numbers, open it or delete it. Any member may add, edit and delete
links. The desktop only edits by hand: the X numbers come from the extension
(2.9.0), which reads the posts on the profile as it is scrolled and sends them
when **Sync now** is pressed in its popup.

## X following and post images (1.17.0)

The user asked for the X stats to keep track of the following count too, and
for the extension to read the most recent posts on the profile along with
their images (2026-10-07).

- The X snapshot already carried `following`; the desktop's X channel page
  now shows it: a **Following** card (in place of Reach, which the profile
  does not show) with its change over the chosen range, a **Following**
  choice on the channel chart, and a **Following** column in **Recorded
  numbers**. Any other channel whose snapshots carry `following` gets the
  chart choice and the column too.
- A posted link has `images`: at most four `https://pbs.twimg.com/` addresses
  (photos, video posters and link-card images; anything else is dropped). The
  extension's import sets them; an import entry without images keeps the ones
  already there, since X loads images lazily and a quick scroll can miss them.
- The desktop's **Posted links** tables show a link's first image as a
  thumbnail, fetched through the desktop's own network manager (so the
  desktop firewall decides) and kept in memory only; the row's tooltip says
  how many images the post has.

Content, metrics, ads, experiments, funnel weeks, Reddit threads, posted links, campaigns, keywords and settings (with the subreddit rules and the X keys) are encrypted
in package-owned organization tables. Uninstall exports the organization snapshot first, purges every row,
and reverses the schema when the last organization removes the package.

## Connect X with the browser extension (1.18.0)

The user asked for the X key entry to guide them through X's pages with the
browser extension and to automate as much as possible (2026-10-07). The
Connect X dialog (web and desktop) now leads with **Guide me with the
extension** / **Guide me in the browser**; typing the keys stays below it.

- Web: the button asks the extension (2.12.0 or newer, through `dashboard.js`)
  to start; the dialog's status line follows the guide (`marketing-x-setup-update`:
  Read and write set, which keys have been read, checking, refused), and on
  success the page reloads `GET /x-connection`, closes the dialog and shows
  the account. Without the extension, or with an older one, the line says
  what to install. `?xsetup=1` opens the dialog and starts the guide; the
  desktop opens that link.
- Desktop: **Guide me in the browser** closes the dialog, opens the web page
  with `xsetup=1`, and polls `GET /x-connection` every five seconds (twenty
  minutes at most, stopping when the organization changes) until it reports a
  connection newer than the one before; then the X card shows the account.
- The extension's guide is described in its README
  ([Connect X for posting](../browser-extension/content/extension/README.md#connect-x-for-posting-2120)).
  It saves through the same `POST /x-connection`, so the server's check
  against X and the admin-only rule are unchanged.

## Mastodon people to follow and compact channel pages (1.19.0)

The user asked for the desktop's Mastodon page to suggest people to follow,
follow them in one click, list searches to check for more people with links
to them, and for the channel pages to be more compact with more information
(2026-10-07).

- `GET /api/marketing/{org}/mastodon?topics=a,b` returns `mastodon`:
  `server` and `account` (from the Mastodon channel URL, else
  `mastodon.social`), `hasToken`, `canFollow` (a token and an owner or admin),
  `topics` (the requested ones, then `opensource`, `git`, `foss`,
  `selfhosted`, `devops`, `programming`; eight at most), `suggestions`,
  `searches` (per topic a `tagUrl` and a people `peopleUrl` on the home
  server), `places` (the server's trending posts, trending hashtags and
  profile directory, and Fedi.Directory) and `errors`. It reads on demand
  only, when the Mastodon page opens or **Find more** is pressed: with a token
  `/api/v2/suggestions`, then for the first three topics the hashtag timeline
  and an account search, and the relationships of the ranked people so anyone
  already followed or requested is left out; without a token the active
  profile directory instead of the server's suggestions. Bots, moved,
  suspended and limited accounts, accounts with no posts and the channel's
  own account are dropped. People are ranked by how many topics they match, a server
  suggestion, follower count (by order of magnitude) and recent activity,
  with a small penalty for locked accounts; 24 at most. 1.29.0 widens the
  searches and returns 40 (see below).
- `POST /api/marketing/{org}/mastodon` `{"follow": "<account id>"}` (owners
  and admins) follows from the channel's account with its access token and
  answers `relationship`: `following`, or `requested` for a locked account.
  Errors: `mastodon_access_token_required`, `mastodon_access_token_invalid`
  (401/403), `mastodon_account_not_found`, `mastodon_rate_limited`. Every
  attempt is audited as `marketing.mastodon.follow`; the manifest declares
  `external.social.follow`.
- The token is the Mastodon channel's write-only `accessToken`, made on the
  server under Preferences, Development, New application with the `read` and
  `write:follows` scopes. Without one, **Follow ↗** opens the person's profile
  on the home server, and the panel offers **Add an access token**.
- The desktop's topics are up to four of the most used tags on Mastodon
  content, then up to four single-word tracked keywords.
- A Mastodon refresh now also records `following` and `posts` from the
  account.
- A clicked **Follow** greys out at once and stays greyed: **Following…**
  while the request runs, then **Following** or **Requested**, all as a
  disabled primary button so the theme's grey disabled style applies
  (2026-10-07, the user asked for it). Without a token, **Follow ↗** opens the
  profile and the button becomes a greyed **Opened ↗** until the
  organization changes; the name link still opens the profile.
- Every channel page is more compact: a smaller header with the back arrow,
  the account's own title and bio, paused state and last refresh; eight
  compact KPI cards (adding **Per post**, engagements per post read, and
  **Pipeline**, the channel's content items); tighter panels and spacing;
  the chart capped at 210 px; **Health and settings** replaces the separate
  Health and Channel settings panels (settings moved to the header and an
  **Edit** button); Ads sits beside Playbook and content spans the page.

## Referring websites (1.20.0)

The user asked for the site's HTTP referrals on the Marketing page, because
they hold real numbers while Analytics has almost none (2026-10-07).

- The overview (`GET /api/marketing/{org}`) gains `referringSites`: `status`,
  `host` (the instance's public host from `runtime.public_origin()`, without
  `www.`), `siteHost` (the organization's Analytics host, else its default
  landing page host), `sites` and `visits` (all-time totals) and `board`, the
  top 12 hosts with `visits`, `firstAt`, `lastAt` and the last referring `url`
  (only `http(s)`, else empty).
- The rows come from the core `site_referrers` table, the same counter behind
  `/api/referrals/sites` and the `referring-sites` board on `/api/leaderboards`:
  one visit per top-level page load whose `Referer` is another website
  (`record_site_referral` in `app/src/entry.py`). The numbers are all time,
  not windowed, so the 7D/30D/90D range does not change them.
- The table counts visits to this ForkMesh site, not to an organization's own
  site. `status` is `other_site` (and the board empty) when the organization
  set a site host outside the instance's domain; the panel is then hidden. An
  unset host, the instance host or one of its subdomains shows the board.
  `unavailable` covers a host runtime without `public_origin` or without the
  table.
- Web: a **Referring websites** panel stacked under **Top landing pages**,
  bars per host with the total in the header; a host links to its last
  referring page. Desktop: the same panel in the same column; clicking a row
  opens that page.
- The weekly report adds **Referring websites (all time)** with the totals and
  a host, visits and last visit table, so the Marketing special agent reads
  them without the public board.

## Special agent on a channel page (1.21.0)

Issue 1764 asked for a channel with a special agent attached to show that
agent on the channel's page, so the operator can work with it there
(2026-10-07).

- A special agent is attached to a channel when its definition lists the
  channel id in `marketingChannels` (`docs/desktop/agents-ui.md`, "Special
  agents on Marketing channel pages"). The checked-in Reddit, Hearth, Y
  Combinator News and Blogger agents are attached to `reddit`, `discord`,
  `hacker-news` and `blog`.
- The desktop channel page then shows a **Special agent** panel across the
  page under the KPIs (**Special agents** when several are attached): the
  agent's name and specialty, its status, schedule and last activity, and its
  focus. **Open** shows the agent's page, **Run now** starts a run (disabled
  while it works or when it is paused), and the message line sends a message
  like the prompt box on its card: it steers a run in progress or starts one
  with the message as the task. An agent with a built-in workflow has no
  message line. The result goes to the status line and the panel redraws.
- The panel is hidden when no agent is attached. The agents come from the
  repository picked on the Special agents page, through the host's
  `specialAgents` and `specialAgentAction`, which the manifest's new
  `desktop.agents.run` permission unlocks (`docs/marketplace/README.md`).
  The web, worker and phones are unchanged: special agents live on the
  desktop.
- The operator asked for the channel chips along the top of the desktop
  page to show a little face for the attached special agent (2026-10-07).
  A chip with an attached agent shows its round 16 px face after the
  channel name (up to three when several share a channel), and the chip's
  tooltip and accessible description name them. The face is the one the
  Special agents rail wears: the host adds `face`, a base64 PNG of
  `teammateFacePixmap` clipped round, to every `specialAgents` row, cached
  per avatar, provider and model. The channel page's panel shows the same
  face in place of the initials, which stay as the fallback when a row has
  no `face`. `agentsOnChannel` is the one match on `marketingChannels` for
  both.

## Start page campaign template (1.22.0)

The operator asked for a blog post about the white-label start page,
`app.forkmesh.com/start`, with a marketing campaign linked to it
(2026-10-07).

- `START_INSTANCE_TEMPLATE` is the second entry of `campaignTemplates`:
  "Start your own ForkMesh", tag `start_instance`, objective `launch`, status
  `planned`, 8 to 21 October 2026 UTC, the post
  `forkmesh.com/blog/start-your-own-forkmesh/` as source and landing page,
  channels blog, Mastodon, X and Discord, and a six-step checklist. The brief
  and drafts are in
  [docs/marketing/campaigns/start-instance-2026-10-07.md](../../../docs/marketing/campaigns/start-instance-2026-10-07.md).
- No visit or conversion target: nothing measured `/start` before, so the
  numbers are set after the first week instead of guessed.
- The web Campaigns panel numbered template cards by their place among the
  templates still shown but looked them up in the full list, so with the
  contest campaign already added, **Add this campaign** on the second card
  opened the contest. Each card now keeps its index in the full list. The
  desktop passes the template itself and was not affected.

## Posted links join a campaign (1.23.0)

The operator asked to make the September Revenue Contest campaign active,
tie the forkmesh.com/hackathon link to it and track the Discord message and
X posts already sent about it (2026-10-07).

- A posted link has an optional `campaign` (a campaign's name or tag), set
  with `POST`/`PATCH /posts` and kept when the extension re-reads the post's
  numbers. A link joins a campaign when that field names it, when its address
  carries the campaign's tag as `utm_campaign`, or when it is the campaign's
  landing page (host and path, ignoring the query and a trailing slash).
- Each campaign's `stats` gain `posts`, `postViews`, `postEngagements` and
  `postChannels`; a channel with a linked post no longer counts as
  uncovered. The web card lists the links and their engagements, the desktop
  card has a **Posted links** row, and the desktop **Add link** dialog has a
  **Campaign** picker. The phone apps do not show or set the field yet.
- The contest template stays `active` with /hackathon as source and landing
  page. Its offer now names the $5, $10, $50 and $100 plans of the official
  rules (it still said $25), and its checklist records the Discord
  announcement as done and adds two open steps: add the X, Discord and Reddit
  announcements under Posted links with this campaign, and write a blog post
  (no contest post exists in `www/public/blog`). An organization that already
  added the campaign keeps its own copy: change its status and checklist on
  the campaign itself.
- `revenue_contest` is on `ATTRIBUTION_ALLOWED_CAMPAIGNS` and `discord` on
  `ATTRIBUTION_ALLOWED_SOURCES` (`app/wrangler.toml`, `app/src/attribution.py`),
  so a tagged contest link that reaches the attribution hook is not stored as
  `other`.

## Movable overview panels (1.24.0)

The user asked to leave forkmesh.com out of Referring websites, to move that
panel up and to let the overview panels be moved (2026-10-07).

- `referringSites` leaves out the instance's own hosts in the board and the
  totals: the instance host, its subdomains and its parent zone with theirs
  (`app.example.org` also drops `example.org` and `world.example.org`, but
  never a two-label suffix such as `co.uk`). The core counter skips the host
  a page was served from, but a page served under another host name, such as
  a core node's own name, counted forkmesh.com subdomains as referrers.
- **Referring websites** is its own panel again, right under **Channels** on
  the web and under **Search keywords** on the desktop, beside **Campaigns**;
  **Top landing pages** stands alone at the end.
- Every overview panel can be dragged by its header onto another panel, which
  it takes the place of; the target shows a dashed outline. Buttons and fields
  in a header still work. The order is kept per browser in `localStorage`
  (`forkmesh.marketing.overviewPanels`) and per desktop in `QSettings`
  (`market/packages/marketing/data/overviewPanels`); panels added later slot
  in after their default neighbour. On the web, touch does not start a drag,
  so swiping a header still scrolls; Escape cancels. Both scroll the page when
  the pointer nears its top or bottom edge.

## Suggested emails on the Email channel (1.25.0)

The user asked for the Email channel to list emails suggested for the team
to send, each with one click that opens Zoho Mail with it filled in
(2026-10-07).

- `GET /api/marketing/{org}/email-suggestions` (any member, like CRM's own
  contact list) calls `crm.email_suggestions(runtime, org_bi, now)` in-process
  and answers `email`: `provider` (CRM's mail provider, `zoho` unless CRM is
  set to Gmail), `zohoConnected`, `total` and `suggestions`, twelve at most.
  Without CRM it answers 409 `crm_not_installed`. It reads on demand only,
  when the Email channel page opens, on **Check again**, and after an Email
  Refresh; nothing is stored in Marketing, so no contact data is copied out of
  CRM's sealed rows.
- Each suggestion is a CRM contact with an email address: `contactId`,
  `name`, `email`, `company`, `flow`, `stage`, `nextStep`, `kind`, `reason`,
  `at`, `subject`, `body` and `composeUrl`. Kinds, in order:
  - `reply`: a reply linked in the last 60 days that is newer than both the
    last outbound message to that contact and "last contacted", whatever the
    stage. Subject `Re: <their subject>`; newest first.
  - `follow_up`: last contacted 14 or more days ago, using CRM's Follow-up
    nudge template; longest silence first.
  - `intro`: never contacted, in the Investment or Partnerships pipeline,
    using CRM's Investor outreach or Partnership template; oldest first.
  Contacts with a next step come first within a kind and the reason names it.
  Contacts at a won stage (the Email channel's closed-deal stages) or an ended
  one (`passed`, `rejected`, `closed`, `inactive`, `hired`, `lost`,
  `closed-lost`, `churned`, `declined`, `unsubscribed`, `do-not-contact`) get
  no follow-up or intro.
- Templates are filled in: `[name]` becomes the contact's first name,
  `[company]` their company, `[your name]` CRM's sender name when one is set,
  and `{fromEmail}` the sender address. Other `[bracketed]` parts stay for the
  sender, and both surfaces say so.
- `composeUrl` is Zoho Mail's mailto handler on the data center of CRM's Zoho
  connection, `https://mail.zoho.<dc>/zm/comp.do?ct=<mailto:address?subject=…&body=…>`
  (the same handler CRM's address links use, now with subject and body), or
  Gmail's `view=cm` compose URL when CRM's mail provider is Gmail. Sending
  happens in the mailbox, so the message lands in its Sent folder and the
  Email channel counts it on the next Zoho sync.

| Surface | Suggested emails |
|---|---|
| Desktop channel page | A full-width **Suggested emails** panel on Email: initials, name, address and company, kind, reason and when, subject (the body in the tooltip), and **Open in Zoho Mail ↗**. A clicked button greys out as **Opened ↗**; **Check again** reloads and clears that |
| Web | The Email card on the Channels page lists the same rows with **Open in Zoho Mail ↗** links |

## Compacter channel pages, no followed people (1.26.0)

The user marked up a screenshot of the desktop Mastodon page and asked for
every channel page to be more compact, and for people already followed to
stop being suggested (2026-10-08).

- The page header is one row: the organization, the section switcher and
  its tools, **Refresh** and **Open on web ↗**. The "Marketing" title and
  the reach, engagements, published and queued line are gone; those totals
  are the organization picker's tooltip. The status line under it only
  shows when there is something to say (an error, a refresh in progress, a
  setup hint); the routine "ForkMesh Blog · N posts · last read …" line is
  no longer shown.
- **+ Add channel** and **Manage** moved into the **All channels** chip, so
  the dashed chip at the end of the channel bar is gone; the catalog size
  is the Add channel tooltip.
- Each channel chip is tinted like that channel's square in the right rail:
  the same tone colour (green on track, amber needs attention, red stale,
  blue neutral, grey dashed when paused) at the rail's fill strength, from
  `packageRailToneColor`. The open channel keeps a primary-coloured border.
- On a channel page the header (mark, name, facts, bio and actions) is a box
  in the right column beside the chart, with **Next steps** under it, so
  the KPIs come first. **New post** sits beside the name and the other
  actions wrap below the bio, which is cut at 220 characters (the tooltip
  has it all).
- **People to follow** is a grid of compact cards, three across on a wide
  window, each with its **Follow** button next to the name instead of at the
  far edge of the panel. **Searches to check** and **Browse** lay their
  links out in columns. Email's **Suggested emails** uses the same cards.
- People already followed are left out. With a token the worker already
  dropped anyone the relationships call reports as followed or requested;
  now it keeps only the 40 people it checked, so an unchecked person cannot
  slip through. Without a token it looks up the channel's account
  (`/api/v1/accounts/lookup`) and reads its public following list (the
  first 80, `/api/v1/accounts/{id}/following`) and drops those handles; a
  hidden list or a failed read just skips the filter. On the desktop a
  person followed from the page disappears from the grid at once; when
  everyone found is followed the panel says so.

## One row of work on a channel page (1.27.0)

The user marked up the desktop Mastodon page again and asked for the three
extra columns of people to go, for the posts to take that space, and for
the same page on every other channel (2026-10-08).

- **People to follow** is one narrow column that scrolls inside the panel,
  so the people no longer push the rest of the page down. The cards are the
  same compact cards, with **Follow** next to the name.
- **Posts and content** sits beside it in the space the other three columns
  of people used, and **Recorded numbers** moved into the right column under
  **Searches to check**. On a channel with no people panel (every channel
  but Mastodon) **Posts and content** takes the whole width beside that
  column, so every channel page has the same shape.
- **Posted links** is no longer its own panel: its **Add link** button is in
  the Posts and content header, its table sits at the top of that panel
  above the content table, and the panel's hint counts both ("41 items ·
  3 links"). The line above the links table is the old empty-state hint
  (X still names the extension's Sync now).
- A person the account already follows is dropped whatever the list length:
  with a token the worker now checks relationships for every candidate it
  ranked (up to 80, in batches of 40) instead of only the first 40, and
  anyone the check did not cover — a failed or partial relationships call —
  still goes through the public following list, which is now read with the
  token so a hidden list is visible.

## Discord posts on the channel page (1.30.0)

The user asked for the Discord channel page to list the last posts, to see
what was posted in the last 6 days (2026-10-08).

- `GET /api/marketing/{org}/discord-posts` (owners, admins, maintainers,
  members and auditors; guests get 403 `organization_member_required`)
  calls the host capability `runtime.discord_recent_messages(org_bi, 6)` and
  answers `discord`: `days`, `since`, `channels` (names), `posts`,
  `truncated` and `contentMissing`. Each post has `id`, `channel`, `author`,
  `bot`, `content`, `postedAt`, `edited` and `url` (the message's
  `discord.com/channels/<server>/<channel>/<message>` link).
- The host reads the channels the owner chose in the organization's Discord
  connector, only those that are still public (the connector's own chat
  rule), ten at most, the latest 50 messages of each, and keeps those posted
  since `now - 6 days` (the time comes from the message id). `truncated` is
  set when a channel's 50 messages all fall inside the window, so older ones
  were not read. `contentMissing` means every post came back without text:
  the bot lacks Discord's Message Content intent.
- It needs the new `organization.discord.messages.read` permission, the
  package installed in the organization and a current owner grant, like
  `organization.discord.read`. Errors: `discord_connector_not_connected`,
  `discord_connector_not_allowed`, `discord_channel_required` (no chosen
  channel is public), `discord_secret_required` (409), and
  `discord_authorization_required`, `discord_guild_missing`,
  `discord_rate_limited`, `discord_unavailable` (502).
- Nothing is stored: the list is read when the Discord channel page opens,
  on **Check again**, and after a Discord Refresh.
- Desktop: the **Discord posts in the last 6 days** panel on the Discord
  channel page is a table of Posted, Channel, Author and Message (the whole
  text in the tooltip); double-clicking a row opens the message in Discord.
  Web: the Discord card on the Channels page lists the same posts with
  **Open ↗** links, scrolling past 320px.

## Followed Mastodon handles are kept (1.28.0)

The user reported that **People to follow** still suggested people the
account already follows, and asked for the follower count refresh to keep
the usernames too so they are never suggested again (2026-10-08). The live
check read only the first 80 accounts of the following list, so anyone
followed earlier came back.

- A Mastodon refresh, beside `followers`, `following` and `posts`, now
  reads the whole following list (`/api/v1/accounts/{id}/following`, 80 a
  page, following the `Link: rel="next"` header, 25 pages and 2000 handles
  at most), with the channel's token when it has one, and keeps the handles
  in the settings record as `mastodonFollowing`: `account` (the channel's
  handle), `handles` (each `user@server`, lowercase) and `readAt`. A full
  read replaces the list; a read cut short by the page limit or a failed
  page adds to the kept list instead; a failed first read leaves it as it
  was. A refresh never fails because of this list.
- `GET /api/marketing/{org}/mastodon` drops every kept handle before
  ranking, so the relationships check and the 24 places go to people not
  yet followed. When nothing is kept for the channel's current account (a
  first visit, or the profile URL changed) it reads the whole list once and
  keeps it. It still reads the newest page live when a candidate was not
  covered by the relationships call, to catch a follow made since the last
  refresh.
- The list is never sent to clients: `settings` leaves it out, settings
  saves keep it, and the organization export carries it as
  `mastodonFollowing`.
- The host's `outbound_text` now returns the response's `Link` header as
  `link` when there is one (`app/src/entry.py`), so packages can follow
  paginated APIs.

## Wider Mastodon search for people to follow (1.29.0)

The user asked for the Mastodon channel to find more people to follow with
better searches, and reported that the recorded numbers were not all coming
in: the channel page showed Following as 0 and every refresh as "logged"
(2026-10-08). A refresh built its snapshot from the reach, follower and
channel counters only, so the `following` and `posts` a Mastodon refresh
reads were dropped.

- People to follow now reads, all at once: the server's suggestions (40,
  with a token); the account's followers (40) and the people who boosted
  or liked its three most engaged recent posts; the hashtag timeline (40)
  and an account search (40) for each of the first five topics; one
  hashtag timeline for topics six to ten (`any[]`); the server's trending
  posts (40), kept only when a post carries a topic tag; and the active
  profile directory (40), kept only when the person's name or bio names a
  topic once there is a token. Only the server suggestions, hashtag
  timelines and account searches report errors; the rest are best effort.
- Topics: the requested ones, then the account's featured hashtags and,
  with a token, the hashtags it follows, then the defaults; ten at most.
- Every person's name, bio and username are matched against the topics, so
  a bio that says `#selfhosted` or "self-hosted" counts as a match. A topic
  of six letters or more also matches with spaces and dashes removed.
- Reasons and ranking: `follows you` (6), `boosted your post` and `liked
  your post` (5 each), each `#topic` (3), a server suggestion (4), then
  follower count by order of magnitude, recent activity and the locked
  penalty as before. Up to 120 people go through the relationships check
  (three batches, read together) and 40 are returned.
- A channel refresh now records `following`, `posts` and the other social
  counters (carried forward when the source cannot see them, like reach),
  keeps the last rating, and marks its snapshot with source `refresh`.
  Older refresh snapshots (note "Channel refresh", no source) read back as
  `refresh` too. The desktop shows it as "channel refresh" and the web as
  "by a channel refresh".
- The desktop's **Recorded numbers** table has a **Posts** column, and
  hides every number column (but Followers) that is zero in every
  snapshot, so a channel shows only the numbers it actually records.

## Ready X post (1.31.0)

The user asked for the X content section to always have a post ready to go,
written by Claude Opus 5.5 from past posts, their replies and engagement, the
current marketing plans and what will get attention, with the text and image
there to copy and a one-tap post from a phone (2026-10-08). Herald, the
special agent on the `x` channel, writes it
([docs/marketing/x-ready-post.md](../../../docs/marketing/x-ready-post.md));
this package stores it and puts it in front of people.

- `marketing_ready_posts` keeps one sealed row per organization and channel
  (only `x` for now): the current post and the last 30 closed ones. The table
  is in `DATA_SCHEMA_UP`/`DOWN`, the organization export (`readyPosts`, with
  the image), `org_has_data` and the purge.
- `GET /ready-post?channel=x` (owners, admins, maintainers, members and
  auditors; guests get 403 `organization_member_required`) answers
  `readyPost`: `channel`, `needsPost`, `post` and `history`. `post` has `id`,
  `text`, `length` (each link counted as 23, as X does), `imageDataUrl`,
  `hasImage`, `imageAlt`, `why`, `link`, `model`, `author`, `createdAt` and
  `intentUrl` (`https://x.com/intent/post?text=…`, which fills the X composer
  or the X app; an intent cannot carry an image). Each `history` entry has
  `id`, `text`, `why`, `status` (`posted`, `skipped` or `replaced`),
  `createdAt`, `closedAt`, `closedBy` and `url`.
- `PUT /ready-post` (any member but auditors and guests) saves a new post:
  `text` (required, at most `X_POST_LIMIT`), `imageDataUrl` (a PNG, JPEG or
  WebP data URL of at most 960,000 characters whose bytes match the type:
  `invalid_ready_post_image`, `ready_post_image_too_large`), `imageAlt`,
  `why`, `link` (https only: `invalid_ready_post_link`) and `model`. A post
  still waiting moves to the history as `replaced`.
- `PATCH /ready-post` with `{id, status: "posted"|"skipped", url?}` closes the
  current post. An `id` that is no longer current answers 409
  `ready_post_changed` with the fresh `readyPost`; nothing to close answers
  404 `ready_post_not_found`. A `url` is cleaned like a posted link.
- The post closes as `posted` by itself when `POST /x-post` sends text that
  matches it, and when the extension's X import adds a new post whose text
  matches (`readyPostClosed` in the import answer). Matching ignores links,
  case, punctuation and a trailing ellipsis, and accepts a common start of 40
  characters or more, because the extension may read a shortened text.
- The overview carries `readyPosts.x`, the same payload without
  `imageDataUrl`, so the text shows at once and clients fetch the image.

| Surface | Ready post |
|---|---|
| Desktop | A full-width **Ready to post** panel above Posts and content on the X channel page: the image (up to 360 x 450), the selectable text, length out of 280, who wrote it and why; **Copy text**, **Copy image**, **Post on X ↗** (puts the image on the clipboard and opens the composer with the text, so the image is one paste away), **Mark posted** and **Skip**, and **Check again**. With no post ready, and Herald attached to `x`, not paused and not running, the page starts Herald with "The ready X post was posted (skipped). Write the next one." once per organization every 30 minutes (kept in the desktop settings), and checks every 45 seconds while it runs so the new post appears; **Write one now** starts it by hand |
| Web | A **Ready to post** block on the X card of the Channels page: image, text, length, why and who wrote it; **Copy text**, **Download image**, **Post on X ↗** (on a touch screen that can share files, the system share sheet with the image and the text; elsewhere the X composer with the text), **Mark posted** and **Skip** (members and up). Empty, it shows the last closed post |
| iOS | **Ready for X** first in the Content section. **Post to X** opens the share sheet with the image and the text; finishing it in an app that posts (not Copy, Save Image, Print, Save to Files, Assign to Contact, Reading List or Markup) marks the post posted. **Copy text**, **Copy image**, **Mark posted** and **Skip** |
| Android | **Ready for X** first in the Content section. **Post to X** copies the text, then shares the image (written to the app cache, through the `composer.files` FileProvider's new `marketing-share/` path) with the text, straight to the X app when it is installed and through the chooser otherwise. **Copy text**, **Save image** (Pictures/ForkMesh, Android 10 and later), **Mark posted** and **Skip** |

## The Discord post list is read again on every visit (1.30.1)

The user posted in Discord and the Discord channel page did not change
(2026-10-08). The list was read once per organization and then kept for the
life of the window, so a page that had already been opened kept showing the
posts of its first read; only **Check again** or a Discord **Update** read
Discord again.

- The desktop and the web now read the list whenever the Discord channel
  page is drawn and the list in hand is more than 60 seconds old, so
  leaving the page and coming back, or any reload of the dashboard, picks
  up what was posted meanwhile. Reads inside that minute reuse the list, so
  redrawing the page cannot flood Discord; **Check again** and a Discord
  **Update** still read it straight away.
- A failed read also counts as a read, so a connector error does not retry
  on every repaint; the error note stays until the next read.
- The list says when it was read ("Read just now", "Read 3 min ago" on the
  desktop, `read 3 minutes ago` on the web card), so an empty list is
  readable as current rather than stale.

## Reddit clues, the walk, and what the engagement rate divides by (1.32.0)

### Where to post next, and what to talk about

The Reddit desk answered "which threads did we log" but never "where should
we go next" or "what do people keep asking about". Two sections now do,
beside Watch on the web, the desktop and the weekly report
(`redditDesk.ideas`). Nothing in them is generated: every line is counted
from this desk's own watch list and thread log, and names the evidence.

**Where to post next** ranks every watched subreddit by what the desk
already knows, highest first:

| Weight | Signal |
|---|---|
| 4 each | Open threads (to review or drafting) waiting for an answer |
| 3 each | Replies that landed there |
| 2 | The subreddit's rules are on file |
| 1 | Nothing logged there for 14 days |

Each row carries the counts, how long ago a thread was last logged, a reason
("2 open threads waiting for an answer", "3 replies landed here; the room
knows you", "Nothing logged here yet; read it before you post") and the
things in the way. A row is **ready** only when the rules are on file and
nothing blocks it. Two blockers are reported:

- the rules have not been read yet, with a link to the rules page;
- a reply landed there less than 7 days ago, the gap between mentions in one
  subreddit that `docs/community/reddit.md` asks for.

**What to talk about** lists topics, each a link to Reddit's newest results
for it:

- every search on the watch list, with how many logged threads match it;
- every two-word phrase that comes back in two or more threads logged in the
  last 90 days and is not already covered by a watched search. Stop words
  ("the", "how", "anyone", …) never form a phrase.

A topic shows the threads behind it (up to three, with their status and the
subreddits they came from) and up to two of the organization's own published
posts from the last 180 days whose title matches, so there is something to
link to. A topic with no threads behind it is one nobody has logged yet, and
says so. The weekly report gains **Where to post next** (the ready
subreddits) and **What to talk about** (the topics with threads).

A second fix came with them: the report's "Watched subreddits with rules on
file" line never printed once a thread existed, because the loop above it
reassigned `desk` to the week's summary.

### Browser activity (1.33.0)

The user asked for the desktop app to respond when they refresh Reddit and
other pages with the browser extension, and to see activity about what they
were doing. `POST /api/marketing/<org>/activity` stores one line from the
extension (`kind` of `read`, `sent` or `failed`, a `text`, and the `site`,
`page` and https `url`), sealed, in `marketing_browser_activity`. Lines belong
to the account that wrote them: only that account reads them, from
`GET /api/marketing/<org>/activity` or as `browserActivity` in the overview
(the newest 12). The newest 40 per account are kept.

Every write to Marketing (an activity line, a Reddit desk send, a capture, a
refresh and any other non-GET request) rings the writer's own devices with
the payload-free account event `package:marketing`
(`runtime.notify_account`). The desktop passes a `package:<id>` event to that
package's page through `NativePackageWidget::packageChanged`. Marketing then
reloads its overview within half a second and shows the last five lines of
the day under the header, as **From your browser**. A desktop that is not
running catches up when the page next loads.

### Walk the watch list

**Walk the watch list** (web: beside Watch on the Reddit desk; extension
popup: in the Reddit desk section) asks the browser extension (2.15.0) to
walk the watch list in one tab, so a desk that was empty until somebody
browsed Reddit fills itself:

1. it refreshes the watch list from Marketing and builds the stops: every
   watched subreddit's newest posts, every watched search over the last
   week, and the rules page of every watched subreddit with no rules on file
   (at most 24 stops);
2. it opens **one** tab and visits the stops one after another, reusing that
   tab;
3. on each page `reddit-walk.js` paces the page: it scrolls in bursts of
   five to eleven small steps with 28–90 ms between them, pauses 0.3–0.9 s
   between bursts, sometimes scrolls a little back up, and glides the
   pointer along a curved path onto one to three of the posts on screen,
   resting 0.3–1.1 s on each. Reddit loads its listings lazily and renders
   parts of a card on hover, so a page nobody scrolls holds almost nothing;
   the pauses are also why one walk reads a subreddit about as often as a
   person would instead of hammering the site;
4. it then asks `reddit.js` (`fm-reddit-scan`) to read the rendered page the
   way it reads a page you opened yourself, waits 2.5–6 s, and moves on;
5. at the end it runs the same **Send to Reddit desk** as the popup and
   reports what was logged.

The walk never clicks, types, submits, votes or posts: `reddit-walk.js`
dispatches pointer and mouse movement only, and
`test_the_clues_the_walk_and_the_engagement_formula_reach_every_surface`
fails if a click, a key, a form event, a request or any page write appears
in it. It is not a way around Reddit's bot check: it sends no spoofed
headers and hides nothing, it runs in the person's own signed-in browser,
and a page Reddit refuses is simply one the walk reads nothing from. Only
the person starts it, **Stop the walk** ends it after the page it is on, and
two walks never run at once (`reddit_walk_running`).

`reddit.js` keeps its promise not to touch Reddit: the pacing lives in its
own file, injected by the background for one page of a running walk.
`https://www.reddit.com/*` and `https://old.reddit.com/*` moved into
`host_permissions` for that injection; `reddit.js` was already a declared
content script on both, so Chrome's install warning does not change.

### What the engagement rate divides by

The engagement rate was `engagements / (reach or followers)`. It silently
used a different denominator per channel and, on the same channel, per
snapshot: X records impressions and YouTube records views, but both fell
through to the follower count, and a Reddit snapshot that read members but
no weekly visitors swapped a 1,840-visitor denominator for a 5,321-member
one, moving the rate by two orders of magnitude with nothing on screen
saying so.

The base is now the first of **reach, impressions, views, followers** that
the latest snapshot actually holds, and `health.engagement` says which one
it was:

| Field | Meaning |
|---|---|
| `rate` | The percentage, also kept as `health.engagementRate` |
| `engagements`, `engagementsLabel` | The numerator and what it counts on this channel |
| `base`, `baseValue`, `baseLabel` | The denominator's key, value and name |
| `formula` | The whole sum, e.g. "212 weekly contributions ÷ 1,840 weekly visitors × 100" |
| `source`, `observedAt` | Which reader took the snapshot, and when |
| `unchangedSince`, `readings` | How far back the same two numbers go |

Both numbers always come from one snapshot. The labels follow the channel:
Reddit says weekly contributions, weekly visitors and members; YouTube says
likes and subscribers; X names the five counts it adds up.

The formula shows in the web channel card's **Eng. rate** tooltip and the
dashboard tile's facts, in the desktop channel detail (the Engagement KPI
reads "of weekly visitors" instead of the old, often wrong, "of reach") and
in the Manage channels Status tooltip. Because a source reports only what it
can see and everything else carries forward, `readings` counts how many
snapshots in a row carried the same pair: more than one adds "The same two
numbers have come back since <date> (N readings), so they may not be
refreshing" — which is what a Reddit page that stopped showing its weekly
counts looks like.

### Reddit's rate limit on Refresh

A Reddit **Refresh** that Reddit rate-limited failed with the raw
`channel_http_429`, which no surface could explain, and pressing Refresh
again made it worse. Now:

- 429 is named `reddit_rate_limited`, 403 `reddit_refused`, 404
  `reddit_not_found`;
- on either of the first two the refresh retries the same feed on
  `old.reddit.com` (added to the package's `outboundOrigins`), a separate
  front end that is often not limited at the same moment. Any other failure
  stops there instead of asking Reddit twice;
- the channel's feed state keeps `lastErrorAt` beside `lastError` — when
  that error was **first** recorded, not when it last repeated — and a Reddit
  refresh inside 15 minutes of a rate limit answers `reddit_rate_limited`
  without touching Reddit at all. Pressing Refresh again during the wait
  therefore cannot extend it;
- the desktop reads all four codes, and any other `channel_http_<status>` as
  "the site answered <status>", instead of printing the code.

Reddit's counts never came from the servers anyway, so the message points at
**Update**, which reads them in your own browser.

## Discord messages count as posting (1.33.1)

The user reported the Discord channel still said **Needs attention** although
messages had been posted there (2026-10-09). The posting check ("Posted in the
last 6 days", twice the Communities cadence) only looked at published content,
posted links and the browser extension's capture. A Discord refresh read the
member counts and no message, and the post list on the channel page reads
messages live without storing them, so the check never saw a Discord message.

- A Discord refresh through the organization's connector now also calls
  `runtime.discord_recent_messages(org_bi, 14)` (14 days is the host's
  limit) and stores the newest `postedAt` as `feed.lastPostAt` on the
  channel. Nothing else from the messages is kept.
- `feed.lastPostAt` never moves back: a refresh that reads no message, or
  whose message read fails (`discord_channel_required`, rate limit), keeps
  the time already stored, and the refresh still records the counts.
- Channel health takes the newest of published content, posted links, the
  capture and `feed.lastPostAt`, so **Last post** and the posting check
  follow the server's own messages after an **Update** or **Refresh**.

## Your Reddit accounts (1.34.0)

The user's Reddit profile did not update from the browser extension, and they
asked for each link they posted with its views, for more than one account
(2026-10-12). The Reddit watch list (`settings.reddit`) now also holds
`accounts`: up to 10 Reddit usernames, written by owners and admins through
`PUT /settings` like the subreddits and searches. A name may be typed as
`name`, `u/name` or a profile link; the error codes are
`invalid_reddit_account` and `too_many_reddit_accounts`. A settings `PUT`
that leaves `accounts` out keeps them.

The desk payload lists them as `redditDesk.accounts`
(`{name, url}`, the url being the profile's **Posts** tab), and the web desk
shows them under **Your accounts, their posts and views**, with a field in the
**Watch list** dialog. The browser extension (2.18.0) reads those profiles and
records each post and comment in **Posts** (`POST /posts`, channel `reddit`)
with its views, score (as likes) and comment count (as replies); no new route
or table was needed.

## Deleting a channel (1.35.0)

The user asked for a way to delete a marketing channel (2026-10-12). Before
this, a channel could only be marked inactive. A settings save also re-added
the seven starter channels (`STARTER_CHANNELS`) whenever they were missing, so
dropping one from the list did nothing.

- `DELETE /api/marketing/<org>/channels/<id>` is for owners and admins. It
  removes the channel from `settings.channels`, adds its id to
  `settings.removedChannels`, and deletes its `marketing_metrics` rows.
  Content, ads, posted links, feed items and the ready post stay. The channel
  stays off the dashboard even though content or posted links still name it.
- A settings save never re-adds a starter channel listed in
  `removedChannels`. A save that lists the channel again, such as
  **Add channels** or an extension capture, takes it off that list. The
  worker keeps `removedChannels` itself and ignores it in a `PUT /settings`
  body.
- The **Delete** button is on each card on the web Channels page, on each
  row of the web **Marketing channels** settings dialog, and on the desktop
  channel page. Only managers see it, and it asks for confirmation first.
  The phone apps have no button yet. Their full-list settings save keeps
  deleted channels deleted.

## Channel desks (1.36.0)

The user asked to move everything on the Reddit desk into the Reddit channel,
to give every other channel something similar, to keep it compact and full of
information, and to take the Reddit desk out of the navigation (2026-10-13).

- The section switch no longer has **Reddit desk**, on the desktop or the web.
- Desktop: every channel page has a desk panel (**X desk**, **Reddit desk**…) under the chart:
  one row of small tiles, computed from data the overview already carries, so
  it adds no request. **Next post due** (last post plus the category's
  cadence), **Queued** (the next queued or scheduled item, and drafts),
  **Posted links** (count, views, likes, replies), **Top post**, **Campaigns**
  (planned or active campaigns listing the channel, and how many still have no
  post on it), **Talk about** (their hashtags and keywords), **Referrals**
  (all-time referring-site visits from the catalog `hosts` of the channel and
  its link's host), **Analytics visits** (the window's top sources matching
  the channel id or hosts), and **Ads**. A tile with nothing to say is left
  out. **Tag a link** opens the Growth link builder with the channel as
  `utm_source` and `social` (or `email`) as `utm_medium`.
- Desktop, Reddit: the desk adds **Threads · 7D**, **Waiting on you** (open and
  monitored), **Rules on file** and **Best subreddit**, and the page carries
  the old desk's panels, tightened: **Thread log** with **Watch** (subreddits,
  searches and your accounts, one line each) and **Before you reply** beside
  it; **Where to post next** (two lines a subreddit, with tagged visits when
  Performance has them), **What to talk about** and **Subreddit rules** (rule
  titles on one line; **Copy rules for the scout** still copies the full
  text) in one row; then **Results by subreddit**, moved from Performance.
  Opening the Reddit page reads Performance once if it has not been read.
  The long explanations moved into the panels' tooltips.
- Web: each card on the Channels page has the same compact desk row (post
  due, queued, campaigns, live ads). The Reddit card adds threads this week,
  open threads, rules on file and the first places to post, with **Open the
  Reddit desk**, which shows the desk page with **‹ All channels** back.
  `?page=reddit` still opens it, and the desktop's **Open on web** from the
  Reddit channel page goes there.
- The phone apps keep their Reddit section for now.

## Phone apps

The iOS and Android apps manage everything the desktop does, under Business
in the You hub. They show eight sections: Overview, Content, Campaigns,
Channels, Ads, Growth, Reddit and Performance.

They read `GET /api/marketing/<org>` once and write through the same routes
as the desktop:

- content, campaigns, ads, experiments, keywords, Reddit threads and posted
  links (`POST`, `PATCH`, `DELETE` on `/<resource>[/<id>]`);
- funnel weeks (`POST /funnel`);
- hand-logged numbers (`POST /metrics`);
- channel refreshes (`POST /refresh`);
- settings (`PUT /settings`);
- the X connection and posts, and the ready X post (`GET`/`PATCH /ready-post`);
- the assistant, the report, performance, and tagged links.

Each record's fields are described once in `MarketingForms` on each
platform, which builds the body the worker expects. A settings `PUT` always
carries the full channel list, because the worker rebuilds it from the
request. `currency` is only sent when typed, because the worker refuses an
empty one.

Only the browser extension's page capture stays in a desktop browser.
"Update with the browser extension" opens this page there. Logging numbers
by hand works on the phone. The native apps' README (`docs/native/README.md`,
2026-10-07) lists each section's actions.

## Package tests

```sh
python3 -m pytest -q marketplace/packages/marketing/tests
```
