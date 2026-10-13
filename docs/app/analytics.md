# Product analytics

ForkMesh measures its own site with the first-party `analytics` marketplace
package. It replaced PostHog on 2026-09-23 (`12317aa01`); no third party
receives analytics events, and every PostHog script, proxy route and
dependency entry was removed.

## Pieces

| Path | Role |
| --- | --- |
| `marketplace/packages/analytics/content/worker/analytics.py` | Server side: public ingest, insights, replay, flags, surveys; staged into the App Worker |
| `marketplace/packages/analytics/content/sdk/analytics-sdk.js` | Dependency-free browser SDK (pageviews, autocapture, identify/alias/groups, flags, surveys, errors, web vitals, heatmaps, masked session replay) |
| `marketplace/packages/analytics/content/web/` | Organization dashboard page at `/dashboard/analytics`; `page.js` is generated from `src/` |
| `marketplace/packages/analytics/tools/sync_assets.py` | Builds `content/web/page.js` and copies the SDK to `www/public/analytics-sdk.js`; `--check` runs in the package tests |
| `www/public/analytics.js` | Consent-gated controller loaded by ForkMesh's own pages |

## Routes

- `POST /api/analytics/_/capture` (event batches and replay chunks) and
  `/api/analytics/_/decide` (flags, surveys, recorder settings). Session-less;
  they authenticate with a project's public write key. `_` can never be an
  organization name, so these never collide with `/api/analytics/<org>/...`.
- Everything else under `/api/analytics/<org>/...` is an organization-member
  API behind the normal session, organization and package-install gates.
- `/api/client-errors` stays a separate App route (`app/src/app_runtime.py`)
  for browser error reports.

## ForkMesh's own site

`www/public/analytics.js` does nothing until the browser opts in
(`forkmesh.analyticsConsent.v1`). Do Not Track and Global Privacy Control turn
it off. After opt-in it lazy-loads `/analytics-sdk.js` with the project key
`site`, which resolves to the one project an instance administrator marked as
the site default (`site_default` column). Until such a project exists, capture
answers 401 and the SDK stays quiet.

Only public surfaces (`PUBLIC_SURFACES`: home, documentation, blog, public
pages) send paths, element text, heatmaps and replay. Dashboard, chat, World,
repository and account pages send only a coarse page category, because those
pages can show end-to-end encrypted chat, notes and private repository names.

## Storage

Events are stored as sealed batches (`analytics_batches`, one row per browser
flush, later merged by the package's maintenance hook), not one row per event,
because Workers Free D1 bills rows read. Insights run in Python over opened
batches with a per-query row cap; results beyond it are marked `truncated`.
No IP address is stored. Tables: `analytics_projects`, `analytics_batches`,
`analytics_persons`, `analytics_aliases`, `analytics_groups`,
`analytics_recordings`, `analytics_recording_chunks`, `analytics_objects`,
`analytics_usage`, `analytics_pages`, `analytics_page_visitors`.

## Docs view counter and default install (2026-09-29)

Every generated documentation page (`/docs/<path>/`) shows "N views" in its
dateline: the number of unique visitors of that URL, read from the analytics
package's `analytics_pages` table through the public
`GET /api/analytics/_/pages?path=` route. Capture keeps the table current for
every `$pageview` (see the package README, Storage model). The public route
answers only for the site-default project and only for paths under `/docs`,
because other paths of an organization's sites are not public.

`docs_site.meta_html` always renders `<p class="dp-meta" data-docs-meta>`
(hidden by `:empty` when a page has no dateline) and `docs-page.js` appends
the count when it is at least 1; a failed or zero answer shows nothing. The
count covers only visitors who opted in to analytics, since nothing else is
captured.

The package is now default-on (`install.default: true`). For an
organization-scoped package that no longer means anything is installed:
`plugin_data._visible` lists a default package in the dashboard's Plugins
column (Analytics, `chart-line` icon) for every account that belongs to an
organization, and the Analytics page offers owners and admins the install for
an organization that has not installed it. Accounts in no organization do not
see it, since the page needs one.

Nothing is counted until an organization installs Analytics and an instance
administrator marks one of its projects `siteDefault`. On 2026-09-29 no
organization on the served stack had installed it.

## Turned on for forkmesh.com, consent prompt, Marketing (2026-10-06)

The user asked to turn Analytics on, make sure it works, and connect it to
marketing. Three things kept it from counting anything:

1. **No site project.** The `forkmesh` organization had the package installed
   but no project, so every capture answered `unknown_project`. On
   2026-10-06 the project `forkmesh.com` (`52b7991b594fdaa180ef6558b37c6212`)
   was created and marked the site default with signed requests from the
   operator's desktop identity (account `jett`, an instance administrator).
   Its allowed origins are `https://forkmesh.com`, `https://www.forkmesh.com`,
   `https://app.forkmesh.com` and `https://world.forkmesh.com`, so the public
   `site` key is refused from any other browser origin (checked:
   `origin_not_allowed`). Session replay stays off.
2. **Nobody was asked.** Consent could only be given on `/privacy`, so almost
   no visitor opted in. `analytics.js` now shows a small box on public
   surfaces (home, documentation, blog, other public pages) when this browser
   has made no choice: "No thanks" stores `denied` and the box never returns,
   "Allow analytics" stores `granted` and starts the SDK. It is not shown on
   private surfaces, on `/privacy` (which has its own controls), inside
   frames, to automated browsers, or when Do Not Track or Global Privacy
   Control is on. Analytics stays off until a visitor allows it; the box only
   asks. `app/tests/test_analytics_consent_prompt.py` drives the real script
   in Node. The privacy page now says the box exists and lists the `utm_*`
   campaign tags the SDK sends, which it previously did not mention.
3. **Marketing could not see it.** Analytics 1.1.0 adds the `campaigns`
   query and `campaign_report`, and Marketing 1.9.0 reads them on its
   Performance page and in the weekly report (see the package READMEs).
4. **Search terms were dropped.** The SDK always sent `utm_term`, but no
   report grouped by it. Analytics 1.2.0 adds the `term` group to
   `campaigns` and `campaign_report`: one row per normalized term (lowercase,
   single spaces) with its top three sources. Untagged visits from a search
   engine are `(not provided)`, since engines no longer pass the query.
   Marketing 1.10.0 shows them in its Search keywords panel.

After deploy, set the `forkmesh` organization's Marketing goals (Performance,
Goals). Useful ones for ForkMesh's own site: "Install click"
(`forkmesh_product_action`, `action` contains `install`) and "Proof opened"
(`forkmesh_product_action`, `action` contains `proof`). Sign-up pages are an
account surface, so their path is reported only as `/(account)`; count
signups on `/admin/acquisition`, not here.

This is separate from campaign attribution ([../attribution.md](../attribution.md)),
which is server-side, needs no opt-in, and still skips EU and EEA visitors.

Two failures in `app/tests/test_first_party_analytics.py` were fixed on the
way: `app/oauth-authorize.html` and `www/ios-install.html` did not load the
controller, and `tools/ecosystem_third_parties.json` lists PostHog as an
ecosystem entry, which is data and is now exempt from the "no PostHog"
check.

## Fuller overview (2026-10-09, Analytics 1.4.0)

The user asked for more on the Analytics page, starting with the https
referrals, more panels and charts, and a more compact layout. The overview
now has a Referrals card that counts sessions by external referring domain or
full referring URL (internal navigation is skipped), a Channels share bar,
sparklines on every KPI tile plus a Views / session tile, a weekday-by-hour
heatmap, new versus returning visitors, pages-per-session and session-length
histograms, page engagement (median time on page, scroll reached), and Screen,
Language and UTM medium picks. Spacing, card heads, tables and KPI tiles are
tighter across every tab, and long breakdown tables scroll inside their card.
The query fields are listed in the package README (Web overview). Referrer
URLs stay plain text, as the page never turns a recorded URL into a link.

## Security

The replay player rebuilds recordings inside a script-less sandboxed iframe and
drops event handlers, `javascript:` URLs, meta refreshes and frames.

## Traps

- Edit `content/sdk/` and `content/web/src/`, then run
  `python3 marketplace/packages/analytics/tools/sync_assets.py`; the copies
  under `www/public/` and `content/web/page.js` are generated.
- `analytics-sdk.js` is listed wherever `analytics.js` is: `app/tools/build_site_assets.py`,
  `app/tools/deploy_targets.py`, the World Worker's proxied paths.

## Related

- `marketplace/packages/analytics/README.md`
- `docs/attribution.md` (acquisition attribution, a separate first-party system)
- [App Worker home](README.md)

## Desktop page (2026-10-07)

The marketplace card for Analytics showed a disabled **Catalog only** button
("No compatible runtime is available in this client") because the manifest
had no `desktop` entrypoint. Version 1.3.0 adds `builtin:analytics`, a native
rail page in `content/desktop/AnalyticsFeature.cpp`, so the package installs
from the desktop Market like Marketing or CRM. The page calls the existing
organization API with signed requests (`REQUEST_PROOF` in `analytics.py` was
already in place) and needs no server change. Its icon is the vendored
`pulse` octicon; `graph` is the Stats page. Its calls are listed in
`desktop/resources/network/endpoint-catalog.json`.

## Unhandled rejections name their reason (2026-09-28)

`error_log` held `client unhandledrejection [home] Unhandled promise rejection`
(2026-09-22..23) and the same on `[account]` (2026-09-18..20) with no stack and
no source. The reporter only kept a reason that was an `Error` instance or a
string, so every other reason (`undefined`, a DOM `Event`, a `Response`, a
plain object, an error from another realm) collapsed into that one fixed text
and the rows could not be traced. They date from before `12317aa01`, when those
pages also loaded PostHog's third-party SDK; none has been reported since it
was removed, and the current home and account scripts catch every promise they
start.

`analytics.js` now reports what the reason was:

- an `Error` keeps its message and stack; one with an empty message is
  reported as `<name> with no message`;
- an error-like object (string `message` plus `name` or `stack`, for example
  one thrown in an iframe) is reported like an `Error`;
- anything else becomes `Unhandled promise rejection: <shape>`: `undefined`
  or `null`, `<type> value` for primitives, `<EventType> "<type>" from <tag>`
  for events, `Response status <n>`, or `<Constructor> with keys a, b` plus
  a string or numeric `code` / string `error` field.

The text is stable for one cause: it names types, property names and codes,
never raw values, because `error_log` groups rows by their exact message and a
varying value would split one cause into many groups. It goes through the same redaction
and the 500-character message cap as every other client error.
`app/tests/test_client_error_rejection_reasons.py` drives the real reporter in
Node with each reason shape.

### Salvage of `fix/web-server-errors` (2026-09-28)

That unmerged 2026-09-27 branch (`1ec346b3d1`) was compared with `main`:

- Rejection descriptions and the `api-client.js` WebSocket URL fix: superseded
  by `78ab261d5b` (this section, and `status.html` catching a throwing
  constructor). Its stand-in closed socket for an unusable URL is not needed
  once `websocketURL` always returns `ws:`/`wss:`. Its check of
  `event.defaultPrevented` guarded against handlers that call
  `preventDefault()` on `unhandledrejection`; no page has one.
- World GPU warm-up without `compileAsync`: done by `714c6690ca`. World lazy
  modules: `78ab261d5b` retries the failed extra once, which replaces the
  branch's whole-page reload.
- Calories answering a missing AI binding: `cbd9c93b46` answers 503 with the
  expected-degraded header inside the package. The branch's host-wide rule
  that marked any package's 5xx `missing_binding`/`ai_unavailable` answer as
  expected was dropped: packages decide that themselves, as calories does.
- `login.js` catching a failed login run: dropped. It showed a generic hint
  and swallowed the rejection, so the cause would never reach `error_log`, and
  no login rejection has been reported.
- `resolveOrigin()` falling back to `app.forkmesh.com` for a non-HTTP page:
  dropped; `api-client.js` is only served over HTTP(S).
- A World session that ended hidden: ported
  ([../world/README.md](../world/README.md#world-crash-reports-after-2026-09-24)).

## Extension messaging failures are not stored (2026-10-04)

The ping of 2026-10-03 21:42 opened error group 520:
`client unhandledrejection [repository] Invalid call to runtime.sendMessage(). Tab not found.`
That text is Safari's WebExtension runtime rejecting a content script's
`browser.runtime.sendMessage()` when its tab is gone. No first-party web
script calls `runtime.sendMessage`; only the ForkMesh browser extension in
`marketplace/packages/browser-extension` does, and it runs as an extension,
not as a page script. The rejection has no stack, so `injectedOnlyOrigin`
cannot attribute it to the extension. The exact message is now matched as
non-actionable in both places: `knownNonActionableBrowserError` in
`www/public/analytics.js` drops it before sending, and
`_client_error_is_non_actionable` in `app/src/entry.py` accepts it without
storing it, for reporters that are still cached in browsers. Group 520 can be
deleted from `/admin?table=error_log`.
