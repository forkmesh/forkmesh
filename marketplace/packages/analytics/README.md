# Analytics

First-party product analytics for an organization's sites and apps, running
entirely on the ForkMesh instance that hosts it. No third-party service sees
a visitor, no IP address is stored, and every stored row is sealed with the
instance's data key.

| Area | What it does |
| --- | --- |
| Capture | Pageviews, pageleaves, autocaptured clicks/changes/submits, rage clicks, custom events, uncaught exceptions, web vitals, UTM and referrer attribution, offline queue with retry. |
| People | Anonymous visitors, `identify`, `alias`, person properties (`$set`, `$set_once`), merge of the anonymous history into the identified person, right-to-erasure delete. |
| Groups | `group(type, key, properties)`: companies, teams, workspaces. Unique-group maths, group breakdowns and filters. |
| Insights | Web overview, trends, funnels (ordered / strict / unordered, conversion window, breakdown, time to convert), retention, user paths, stickiness, lifecycle, live events, taxonomy. |
| Cohorts and actions | Static and behavioural cohorts; actions that name an autocaptured element by URL, text, or selector. |
| Session replay | DOM snapshot + mutations, mouse movement, clicks, scroll, input (masked), viewport. Player with speed, skip-inactivity, and an event timeline. |
| Heatmaps | Click, rage-click, and mouse-movement maps plus scroll depth per page. |
| Feature flags | Boolean and multivariate flags, percentage rollout, person-property and static-cohort targeting, JSON payloads. |
| Conversions | Conversion goals (any event, with property filters), conversion rate per visitor and per session against the previous period, by channel and landing page. |
| Experiments | A/B results per variant from flag exposures and a goal, with lift and a significance test. A website playbook sets up the instance's own goals, funnels and homepage experiment. |
| Surveys | Open text, rating, NPS, single / multiple choice, link. URL, flag, and event targeting. Results with NPS. |
| Errors | `$exception` grouping by fingerprint with users, sessions, and a jump to the replay. |
| Dashboards | Saved insights, dashboards of tiles, annotations, JSON export. |

Not included: a SQL console, a data warehouse or batch exports to other
systems, mobile-native replay, and server SDK packages (the capture endpoint
is a plain JSON POST, so any server can call it).

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/analytics.py`, `/api/analytics` | Organization-scoped, sealed with the instance data key. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/analytics`, every feature in the table above. |
| Desktop | `content/desktop/AnalyticsFeature.{h,cpp}`, `AnalyticsManage.cpp` | Rail page **Analytics** (1.3.0), signed with `forkmesh-analytics-request-v1`; Conversions and Experiments sections (1.7.0). |
| iOS | `native/ios/ForkMesh/Features/Packages/AnalyticsScreen.swift` | Overview only. |

The desktop page shows the web overview for one project: visitors,
pageviews, sessions, bounce rate and average session with the change against
the previous period of the same length, a visitors and pageviews chart, and
top pages, referrers, countries, devices, browsers and custom events, over the
last 24 hours, 7, 30 or 90 days. Below them (1.6.0) come **Campaigns** (source,
medium and campaign with visits, change against the previous period, bounce,
average time and the top landing page), **Landing pages** and **Search terms**,
each a `campaigns` query (`groupBy` `campaign`, `landing`, `term`, 15 rows,
`host` from the project's first allowed origin). They replace the desktop
Marketing package's Website tab, which showed the same Analytics data through
Marketing's `/performance`; Marketing kept only what needs its own records. It also installs the package for an
organization, creates the first project and copies the site snippet; the
top search box finds its projects and top pages.

Since 1.7.0 the header switches between **Overview**, **Conversions** and
**Experiments** (`AnalyticsManage.cpp`); the range buttons apply to all three.

- **Conversions**: the project's goals with converted visitors, rate, change
  against the previous period, converting sessions and session rate (one
  `conversions` query for up to 10 goals, pinned first); the selected goal by
  channel and by landing page; and a saved funnel picked from the project's
  funnel insights with visitors, conversion from the first and the previous
  step, drop-off and median time per step. **New goal**, **Edit** and
  **Delete** manage goals (event, one property condition, pinned; extra
  conditions set on the web are kept). Owners and admins get **Add website
  playbook** while the project has none applied.
- **Experiments**: every experiment with its flag, status, split and goal, and
  the selected one's results per variant (visitors, conversions, rate, lift,
  chance to beat control, and Baseline / Winning / Losing / Not yet
  significant), counted from its start to its end or now. Owners and admins can
  **Stop** an experiment (status `stopped`, `endedAt`, and its flag turned off
  so everyone sees control) or **Start** it again (flag back on), and create
  one with **New experiment**: a name, hypothesis, flag key, a test variant and
  its share of visitors, and a saved goal; it creates the flag (`control` plus
  the variant) and a running experiment.

Retention, replay, heatmaps, flag targeting rules and surveys stay on the web
page; **Open in browser** goes there.

## Storage model

Events are stored as sealed **batches**: one row per browser flush, merged
into rows of up to 1,500 events once their day is over (the package's
periodic hook). D1 bills rows read and written, so an insight over a month
opens a few hundred rows instead of tens of thousands, and the insight maths
runs in the Worker over the opened events. A query scans at most 60,000
events and says `truncated: true` when it stopped early.

Per-project daily quotas (events and replay bytes) and retention (events 90
days, replays 21 days by default) are settings; both have hard ceilings in
`analytics.py`.

Pageviews are also counted per URL in two plain tables, updated on every
capture: `analytics_pages` (project, path, `views`, `visitors`) and
`analytics_page_visitors` (one row per project, path and person). The path is
`$pathname` without its trailing slash, as in the paths insight. `visitors` is
recounted from the visitor rows after every write, so two flushes racing for
the same person never count twice. An identify moves the anonymous person's
rows onto the identified person, erasing a person deletes their rows and
recounts, and the rows live as long as the project (retention does not expire
them, so a returning reader is still one visitor).

## HTTP API

All paths are under `/api/analytics`.

### Public ingest (write key, no session, any origin)

Bodies are JSON sent as `text/plain` so a browser never needs a preflight.

`POST /_/capture`

```json
{
  "key": "fma_…",
  "sentAt": 1790000000000,
  "batch": [
    {
      "event": "$pageview",
      "distinctId": "0190…",
      "ts": 1790000000000,
      "uuid": "…",
      "sessionId": "0190abcd…",
      "properties": { "$current_url": "https://…", "$pathname": "/pricing" },
      "set": { "plan": "pro" },
      "setOnce": { "initial_referrer": "…" },
      "groups": { "company": "acme" }
    }
  ],
  "replay": {
    "sessionId": "0190abcd…", "distinctId": "0190…", "seq": 0,
    "startedAt": 1790000000000, "url": "https://…",
    "viewport": { "width": 1280, "height": 720 },
    "device": { "browser": "Firefox", "os": "Linux", "type": "Desktop" },
    "encoding": "gzip-b64", "events": "<string>",
    "activeMs": 9000, "clicks": 3, "keys": 12
  }
}
```

`batch` holds at most 100 events; `replay.events` at most 640 KiB. Both are
optional. Responses: `200 {ok, accepted, replayBytes}`, `401 unknown_project`
or `invalid_key`, `403 origin_not_allowed`, `413`, `429 quota_exceeded`. A
client stops for the page's lifetime on 401/403 and backs off on 429/5xx.

`403 origin_not_allowed` comes from the project's allowed origins. An entry is
either an exact origin (`https://example.org`) or a wildcard
(`https://*.example.org`) that allows every subdomain on the same scheme and
port but not the bare domain, which is listed on its own. A wildcard needs at
least two labels after `*.` and is refused over a shared suffix where
strangers own the subdomains: two-label country suffixes (`*.co.uk`,
`*.com.au`) and hosting platforms (`*.github.io`, `*.pages.dev`,
`*.vercel.app`, `*.herokuapp.com` and the rest of `SHARED_SUFFIXES` in
`worker/analytics.py`). A deeper wildcard under them (`*.me.github.io`) is
allowed. Requests without an `Origin` header (server-side callers) are not
checked; the write key is their credential. The desktop reads a wildcard
entry's domain as the project's host.

Reserved event names and the properties the insights read:

| Event | Properties |
| --- | --- |
| `$pageview` | `$current_url`, `$pathname`, `$host`, `$referrer`, `$title`, `utm_*`, `$browser`, `$os`, `$device_type`, `$viewport_width`, `$viewport_height` |
| `$pageleave` | as above plus `$max_scroll_y`, `$page_height`, `$duration_ms` |
| `$autocapture` | `$event_type` (`click`/`change`/`submit`), `$el_text`, `$elements` (array of `{tag, id, classes, attrs, nth}` from target up), `$x_ratio` (0..1 of viewport width), `$page_y` (px from page top) |
| `$rageclick` | `$x_ratio`, `$page_y`, `$el_text`, `$elements` |
| `$heatmap` | `$heatmap_points`: `[[xRatio, pageY], …]` sampled mouse positions |
| `$exception` | `$exception_type`, `$exception_message`, `$exception_stack`, optional `$exception_fingerprint` |
| `$web_vitals` | `$web_vitals_LCP`, `_CLS`, `_INP`, `_FCP`, `_TTFB` |
| `$identify` | `$anon_distinct_id`; person properties in `set` / `setOnce` |
| `$create_alias` | `alias` |
| `$groupidentify` | `$group_type`, `$group_key`, `$group_set` |
| `$feature_flag_called` | `$feature_flag`, `$feature_flag_response` |
| `survey shown` / `survey dismissed` / `survey sent` | `$survey_id`, `$survey_response`, `$survey_response_1`, … |

`POST /_/decide` with `{key, distinctId, personProperties?}` returns

```json
{
  "ok": true,
  "flags": { "new-onboarding": true, "pricing-test": "variant-b" },
  "payloads": { "pricing-test": { "price": 12 } },
  "surveys": [ { "id": "…", "name": "…", "questions": [], "targeting": {}, "appearance": {} } ],
  "replay": { "enabled": true, "sampleRate": 100, "minDurationMs": 2000,
              "maskAllText": false, "maskAllInputs": true, "recordCanvas": false },
  "autocapture": true, "heatmaps": true, "captureErrors": true,
  "capturePerformance": true
}
```

The literal key `site` resolves to the one project an instance administrator
marked `siteDefault`; it is what the instance's own pages send.

`GET /_/pages?path=/docs/…` answers `{"ok": true, "path", "views",
"visitors"}` for the `site` project. Only paths under `PUBLIC_PAGE_PREFIXES`
(`/docs`) are answered; anything else is `404 page_not_public`, and `404
unknown_project` until a site project exists. The documentation pages show
`visitors` as their view count.

### Member API (session, organization member, package installed)

| Method and path | Purpose |
| --- | --- |
| `GET /` | Organizations the caller belongs to and whether each installed the package |
| `GET /{org}` | Projects, role, `canManage`, `instanceAdmin`, vocabularies |
| `POST /{org}/projects` | Create a project (owner/admin) |
| `GET/PATCH/DELETE /{org}/projects/{id}` | Read (with 30-day usage), update settings, delete with all data |
| `POST /{org}/projects/{id}/rotate-key` | Replace the write key |
| `POST /{org}/projects/{id}/playbook` | Add the website playbook (owner/admin); see Conversions |
| `POST /{org}/projects/{id}/query` | Run an insight; see below |
| `GET/POST /{org}/projects/{id}/objects?kind=` | List / create saved objects |
| `GET/PATCH/DELETE /{org}/projects/{id}/objects/{objectId}` | One saved object |
| `GET /{org}/projects/{id}/persons?search=&identified=1&before=` | People |
| `GET/DELETE /{org}/projects/{id}/persons/{personId}` | One person with recent events and replays / erase |
| `GET /{org}/projects/{id}/groups?type=` | Groups |
| `GET /{org}/projects/{id}/recordings?before=&minDurationMs=&personId=&errors=1` | Replays |
| `GET/DELETE /{org}/projects/{id}/recordings/{sessionId}?afterSeq=` | Replay chunks (paged) / delete |
| `GET /{org}/projects/{id}/export` | Events as JSON |

Saved object kinds: `insight` `{name, description, query, pinned}`,
`dashboard` `{name, description, tiles: [{insightId, width 1-4}], pinned}`,
`cohort` `{name, static, distinctIds, match: all|any, conditions}`,
`action` `{name, steps: [{event, url, urlMatching, selector, text, properties}]}`,
`annotation` `{content, date}`, `flag` `{key, name, active, conditions:
[{properties, rolloutPercent, variant}], variants: [{key, percent}], payloads}`,
`experiment` `{name, flagKey, goal, status, startedAt, endedAt}`, `survey`
`{name, active, questions, targeting, appearance}`, `goal` `{name,
description, event, properties, pinned}` (kept in the project settings, see
Conversions).

Every query body carries `kind`, `from`, `to` (ms), and optionally `filters`,
`filterTestAccounts`, `tzOffsetMinutes`. The response is
`{ok, kind, from, to, truncated, scannedEvents, result}`.

A **filter** is `{type: event|person|group|cohort, property, operator, value}`
with operators `exact is_not icontains not_icontains regex not_regex gt gte lt
lte is_set is_not_set`; a cohort filter's `value` is the cohort id. A
**series** is `{event, label?, math, mathProperty?, groupType?, properties:
[filter]}` where `event` is a name, `*`, or `action:<id>` and `math` is one of
`total dau sessions unique_group sum avg min max median p90 p95 p99`.

| kind | Request fields | `result` |
| --- | --- | --- |
| `overview` | — | `totals`, `previous` (`visitors pageviews sessions bounceRate averageSessionMs events`), `timeseries` (a trends result), `sessionSeries` (`bounceRate averageSessionMs`, aligned to `timeseries.labels`), `breakdowns` (`pages referrers sources mediums campaigns browsers os devices screens languages countries entryPages exitPages events`, counted per pageview, and `channels referringDomains referringUrls`, counted per session; each `[{value, count}]`), `visitorKinds` (`new returning`), `sessionDepth`, `sessionLength` (`[{value, count}]` histograms), `hours` (7×24 pageviews, Monday first, in `tzOffsetMinutes`), `engagement` (`[{value, count, medianMs, scroll}]` from page leaves), `webVitals` |
| `trends` | `series`, `interval`, `breakdown {type, property, groupType}` | `labels` (bucket start ms), `series: [{name, breakdown, data, total}]` |
| `funnel` | `steps` (series), `windowSeconds`, `order`, `breakdown` | `steps: [{name, count, conversion, stepConversion, dropoff, medianTimeMs, averageTimeMs}]`, `breakdown?` |
| `retention` | `targetEvent`, `returningEvent`, `period`, `periods`, `retentionType` | `rows: [{start, size, values, percentages}]` |
| `paths` | `pathType`, `startPoint`, `endPoint`, `stepLimit`, `edgeLimit` | `edges: [{source: "1_/a", target: "2_/b", value}]`, `dropoffs`, `sessions` |
| `stickiness` | `series`, `interval` | `labels` (1..n), `series: [{name, data, total}]` |
| `lifecycle` | `series`, `interval` | `labels`, `series` named `new returning resurrecting dormant` |
| `events` | `event`, `personId`, `sessionId`, `limit`, `before` | `events`, `nextBefore` |
| `sessions` | — | `sessions: [{id, actor, start, end, durationMs, pageviews, events, entry, exit, errors}]` |
| `heatmap` | `path`, `type: click|rageclick|move|scroll`, `viewportMin`, `viewportMax` | `points: [[xPercent 0-99, pageY, count]]`, `scroll: [{y, reached}]`, `pageviews`, `pageHeight` |
| `errors` | — | `groups: [{fingerprint, type, message, count, users, sessions, firstSeen, lastSeen, stack, url, sessionId}]` |
| `taxonomy` | — | `events`, `properties`, `personProperties` as `[{value, count}]` |
| `propertyValues` | `property`, `type` | `values` |
| `experiment` | `flagKey`, `goal` (series) | `variants: [{variant, exposures, conversions, conversionRate, lift, probabilityToBeatControl, significant}]` |
| `survey` | `surveyId` | `shown dismissed sent responseRate questions` |
| `campaigns` | `goals` (up to 5 series), `groupBy: campaign|content|source|landing|term`, `host`, `limit` (≤200) | `totals`, `previous` (`sessions visitors pageviews bounces bounceRate averageSessionMs conversions conversionRate goals`, plus `taggedSessions taggedShare` on `totals`), `rows: [{key, source, medium, campaign?, content?, landing?, term?, topSources? (term only), …totals, previousSessions, previousConversions, tagged, topLanding}]`, `goals` (labels), `rowCount` |
| `conversions` | `goals` (up to 10 series), `interval` | `totals`, `previous` (`visitors sessions`), `goals: [{name, conversions, sessions, events, rate, sessionRate, previous: {conversions, sessions, rate}}]`, `labels`, `series: [{name, data, total}]` (converting visitors per bucket), `channels` and `landing` (`[{value, sessions, visitors, conversions: [per goal], rates: [per goal]}]`, at most 12 and 20 rows) |

### Conversions and the website playbook (1.7.0)

A **goal** is one event, or an action, with optional property filters: a
sign-up click, a copied install command, a finished form. Goals use the
objects API (`kind=goal`) but are stored in the project's sealed settings
(`settings.goals`, at most 20), not in `analytics_objects`: that table's
`kind` CHECK constraint lists the older kinds, and widening it would mean
rebuilding the table on every existing instance.

The `conversions` query counts, for each goal, the visitors who reached it
(`conversions`, `rate` = over all visitors of the range), the sessions with a
goal event (`sessions`, `sessionRate`), and the goal events. `previous` is the
window of the same length just before. Each session is credited to its
landing pageview's channel (the overview's classes) and landing path. The web
page shows it as the **Conversions** tab (`#analytics/conversions`).

The **website playbook** (`SITE_PLAYBOOK`, version `PLAYBOOK_VERSION`) holds
what the instance's own pages need, keyed by a stable id:

| Key | Object |
| --- | --- |
| `goal.install-intent`, `goal.signup-intent`, `goal.account-created`, `goal.proof-opened` | Goals on `forkmesh_product_action` with `stage` `install`, `signup`, `account`, `interest` |
| `funnel.home-install` | Saved funnel: homepage pageview → install intent within a day |
| `funnel.home-account` | Saved funnel: homepage pageview → signup intent → account created within a week |
| `flag.home-hero-headline` | Multivariate flag, `control` 50 / `concrete` 50, active |
| `experiment.home-hero-headline` | Running experiment on that flag with install intent as its goal |

The package's periodic hook applies it to the site-default project once per
playbook version (at most once an hour per isolate); owners and admins can
apply it to any project with `POST …/playbook`. The applied keys are kept in
`settings.playbook` (`{version, applied, appliedAt}`, exposed as
`project.playbook` with `latest`), so an item someone deleted or edited is
never added again, and a flag whose key already exists is kept as it is.
Created objects are attributed to `website-playbook`.

**The homepage experiment.** The hero headline "Your Code, Your World." is a
slogan; the `concrete` variant says what the product does: "Git hosting no
single provider can take away." The hypothesis is that naming the outcome
raises the share of homepage visitors who copy the install command or open a
download (install intent). Only visitors who allowed analytics are assigned:
everyone else, and anyone whose flag is off, sees `control`. Stop the
experiment at a significant result (the experiment panel marks it at 95% with
at least 100 visitors per arm) or after four weeks, then turn the flag off or
make the winner the default copy.

### Recordings (1.5.0)

The web page calls session replay **Recordings** (tab `#analytics/recordings`;
`#analytics/replay` still works). Each recording keeps `pages`: the URLs the
visitor moved through, in order, one entry per change (a reload of the same
page is not repeated), at most 50. The server appends the `url` of every replay
chunk, so a recording that spans several page loads lists each of them; older
recordings without `pages` fall back to `startUrl`. The list shows the trail and
its length; the player's "Pages viewed" card lists each page change from the
recording's `meta` events with its time and seeks there on click.

When replay is off for the project, the tab says so and offers owners and
admins a "Turn on recordings" button, which PATCHes only `replay.enabled`.

### Motion view and desktop playback (1.8.0)

`GET .../recordings/<sessionId>?afterSeq=<n>&view=motion` pages through the
same chunks, but the server unpacks each one (`gzip-b64` or `json`) and
answers `encoding: "motion"` with `events` as a JSON array holding only
`meta` (`href`, `width`, `height`, `pageHeight`), `full` (`doc`, `x`, `y`),
`move`, clicks (`click`, `dblclick`, `contextmenu`, `touchstart` with `x`,
`y`), `scroll` (`id`, `x`, `y`, `h`), `resize`, `input` (empty) and `custom`
(`name`, `url`). Serialized nodes, mutations and input values are never
included. A chunk that cannot be decoded comes back with no events. The
desktop player uses this view. Recordings also carry `country`, the visitor's
two-letter country from the first chunk.

The SDK records the document height: `pageHeight` on `meta` and `h` on window
`scroll` events.

### Web overview (1.4.0)

The overview's referral views count **sessions** by their landing pageview's
`$referrer`, skipping a referrer on the page's own `$host` (with or without
`www.`), so internal navigation never shows up as a referral.
`referringDomains` holds the bare host; `referringUrls` the full `http(s)` URL
without its fragment (the SDK has already dropped the query when
`maskUrlQuery` is on), up to 20 rows. `channels` classes each session the way
`campaigns` does and names the class: Direct, Organic search, Social,
Referral, Paid (`cpc ppc paid display …`), Email (`email newsletter`),
Affiliate, or Campaign for any other tagged medium. The older per-pageview
`referrers` list stays for the desktop and iOS pages.

`visitorKinds.returning` counts visitors of the range who were also seen in
the previous period of the same length; nothing older is loaded. `hours` uses
the query's `tzOffsetMinutes` (the browser's offset on the web page).
`engagement` reads `$pageleave`: the median `$duration_ms` (capped at six
hours) and the average share of the page scrolled into view.

The web page shows these as a sparkline under each KPI tile, a Channels share
bar next to the traffic chart, a Referrals card (Domain / Full URL), a
weekday-by-hour heatmap, a new-and-returning split, pages-per-session and
session-length histograms, and a page engagement table. Referrer URLs are
printed as text, never as links: a visitor controls `$referrer`.

### Campaign results (1.1.0)

`campaigns` attributes each **session** to its first touch: the `utm_*`
properties the SDK copies onto every event of a session, or, for an untagged
session, the host of the landing page's referrer, classed as `organic`
(search engines), `social` (a fixed list of networks and communities) or
`referral`. A referrer on the project's own `host` counts as `(direct)`.
Untagged sessions carry `campaign = "(none)"`. A session converts when any
event in it matches one of the `goals`; with no goals `conversions` and
`conversionRate` are `null`, never 0. The previous window has the same
length and ends where the chosen one starts. Bounces use the overview's
definition (one pageview, no interaction, under ten seconds).

`groupBy: term` (1.2.0) groups sessions by their `utm_term`, lowercased with
single spaces, and lists each term's top three `source / medium` pairs in
`topSources`. Untagged sessions from a search engine are `(not provided)`;
other sessions without a term are `(none)`.

Other packages read it in-process, without a session or HTTP hop:
`await analytics.campaign_report(runtime, org_bi, query)` returns
`{status: not_installed|no_project|ok, project, projects, lastEventAt, from,
to, truncated, scannedEvents, results: {<groupBy>: <campaigns result>}}`.
`query` is a `campaigns` query plus `project` (an id; the site default, else
the oldest project, when absent) and `groups` (several groupings computed over
one event load). The caller is responsible for its own membership check;
Marketing calls it only after resolving the member's organization. The
Marketing package's Performance page is the first caller.

With `daily: true` (or a list of group names), each selected campaign row also
includes `days` (`[[UTC day start in ms, pageviews], ...]`). Marketing uses the
organic Google-referrer rows for the Google channel's views-over-time chart;
tagged Google campaigns remain paid traffic rather than search-referrer traffic.

With `pages` (up to 20 `{key, host, path}` sections) the report also returns
`results.pages`: page views and visitors per `host` + `path` (`rows`, at most
`pageLimit`, default 200, cap 500; `www.` is dropped from hosts and trailing
slashes from paths), and per section `views`, `visitors` and `days` (`[[UTC
day start in ms, views], ...]`) for every page view on that host at or under
that path (`/` takes the whole host). Marketing passes one section per channel
link so a Blog channel gets views per post and per day.

## Browser SDK

`content/sdk/analytics-sdk.js` is dependency-free and exposes
`window.ForkMeshAnalyticsSDK`:

```js
const analytics = ForkMeshAnalyticsSDK.init({
  key: "fma_…",                       // or "site" on the instance's own pages
  apiHost: "https://forkmesh.com",    // default: the script's own origin
  persistence: "localStorage",        // "localStorage" | "memory"
  autocapture: true, capturePageview: true, capturePageleave: true,
  sessionRecording: true, heatmaps: true, captureErrors: true,
  surveys: true, respectDoNotTrack: true,
  sanitizeProperties: (properties, eventName) => properties,
  beforeSend: (event) => event,       // return null to drop
});
analytics.capture("plan upgraded", { plan: "pro" });
analytics.identify("user-42", { email: "a@example.com" }, { first_plan: "free" });
analytics.alias("user-42-legacy");
analytics.group("company", "acme", { seats: 40 });
analytics.setPersonProperties({ plan: "pro" });
analytics.register({ app_version: "2.1" });   // super properties
analytics.isFeatureEnabled("new-onboarding");
analytics.getFeatureFlag("pricing-test");       // "variant-b"
analytics.getFeatureFlagPayload("pricing-test");
analytics.onFeatureFlags((flags) => {});
analytics.startSessionRecording(); analytics.stopSessionRecording();
analytics.optOut(); analytics.optIn(); analytics.reset(); analytics.shutdown();
```

Elements (and their subtrees) marked `data-fm-no-capture` are ignored by
autocapture; `data-fm-mask` masks text in replays and `data-fm-block` replaces
the element with a same-sized placeholder. Password fields and anything typed
into an input are always masked.

`tools/sync_assets.py` assembles `content/web/page.js` from
`content/web/src/replay-player.js` and `content/web/src/page.ui.js`, and copies
the SDK to `www/public/analytics-sdk.js` for the instance's own pages. Run it
after editing either; `--check` (run by the tests) fails on drift.
