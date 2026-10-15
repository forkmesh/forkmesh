# Product analytics

ForkMesh measures its own site with the first-party `analytics` marketplace
package. No third party receives analytics events.

## Pieces

| Path | Role |
| --- | --- |
| `marketplace/packages/analytics/content/worker/analytics.py` | Server side: public ingest, insights, replay, flags, surveys |
| `marketplace/packages/analytics/content/sdk/analytics-sdk.js` | Dependency-free browser SDK (pageviews, autocapture, identify, flags, surveys, errors, web vitals, heatmaps, masked replay) |
| `marketplace/packages/analytics/content/web/` | Organization page `/dashboard/analytics`; `page.js` is generated from `src/` |
| `marketplace/packages/analytics/content/desktop/AnalyticsFeature.cpp` | Desktop page (`builtin:analytics`), signed requests |
| `marketplace/packages/analytics/tools/sync_assets.py` | Builds `page.js` and copies the SDK to `www/public/analytics-sdk.js`; `--check` runs in tests |
| `www/public/analytics.js` | Consent-gated controller loaded by ForkMesh's own pages |

## Routes

- `POST /api/analytics/_/capture` and `/api/analytics/_/decide`:
  session-less, authenticated by a project's public write key and its allowed
  origins. `_` can never be an organization name.
- `GET /api/analytics/_/pages?path=`: public unique-visitor count, only for
  the site-default project and only for `/docs` paths (shown as "N views" on
  documentation pages).
- Everything else under `/api/analytics/<org>/...` is behind session,
  organization membership and package install.
- `/api/client-errors` is a separate App route for browser error reports.

## Consent and privacy

- `www/public/analytics.js` does nothing until the browser opts in
  (`forkmesh.analyticsConsent.v1`). Do Not Track and Global Privacy Control
  turn it off. On public surfaces with no stored choice it shows a small
  Allow / No thanks box; never on private surfaces, the privacy page, frames
  or automated browsers.
- After opt-in it loads `/analytics-sdk.js` with the project key `site`,
  resolving to the project an instance administrator marked `site_default`.
  Without one, capture answers 401.
- Only public surfaces (`PUBLIC_SURFACES`) send paths, element text, heatmaps
  and replay. Private pages (dashboard, chat, World, repositories, account)
  send only a coarse category, because they can show encrypted content and
  private repository names.
- No IP address is stored.
- Campaign attribution ([../attribution.md](../attribution.md)) is a
  separate server-side system.

## Storage

Events are stored as sealed batches (`analytics_batches`, one row per browser
flush, merged later by the package's maintenance hook). Insights run over
opened batches with a per-query row cap and mark results `truncated`. Tables:
`analytics_projects`, `analytics_batches`, `analytics_persons`,
`analytics_aliases`, `analytics_groups`, `analytics_recordings`,
`analytics_recording_chunks`, `analytics_objects`, `analytics_usage`,
`analytics_pages`, `analytics_page_visitors`.

The package is default-on (`install.default: true`): it is listed for every
organization member, and owners and admins are offered the install. The
`campaigns` query and `campaign_report` (with a `term` grouping) feed the
Marketing package.

## Client error reports

`analytics.js` reports unhandled rejections with a stable description of the
reason (an `Error`'s message and stack, or a shape such as `undefined`,
`Response status <n>`, `<Constructor> with keys a, b`), never raw values, so
`error_log` groups one cause together. Known non-actionable browser messages
are dropped by `knownNonActionableBrowserError` and by
`_client_error_is_non_actionable` on the server.

## Recordings

The package's Recordings tab (session replay) plays back a visit: the page as
the visitor saw it, their mouse movement, clicks, scrolling, and each page they
moved to, next to the analytics events of the same session. Recordings exist
only for visitors who allowed analytics, only on public surfaces, and only
after a project's owner or admin turns them on (off by default; the tab offers
a "Turn on recordings" button). Typed text is always masked. Each recording
keeps the ordered list of pages viewed, so the list shows the visitor's path
and the player can jump to each page change. Details are in the package
README under "Recordings".

## Security

The replay player rebuilds recordings inside a script-less sandboxed iframe
and drops event handlers, `javascript:` URLs, meta refreshes and frames.
Recorded referrer URLs are shown as plain text, never links.

## Dashboard page components

The package view (`content/web/view.html`, `content/web/src/page.ui.js`)
uses the dashboard shell's shared components instead of its own: `fm-btn`
(`fm-btn-sm` in rows, toolbars, cards and dialogs), `fm-flash` for the error,
saved and truncated-results notices, `fm-dialog` for the editor dialog, the
person drawer and the data dialog, `fm-blankslate` for empty, loading and
error states, `fm-label` for badges, and `fm-popover` for chart tooltips.
Green (`fm-btn-cta`) is kept for each tab's one create action and the dialog
confirm; Apply, Show and Play are neutral. The good, warning and bad status
colours come from the theme's success, attention and destructive roles. The
browser `confirm()` prompts are left as they are.

## Traps

- Edit `content/sdk/` and `content/web/src/`, then run
  `python3 marketplace/packages/analytics/tools/sync_assets.py`.
- `analytics-sdk.js` must be listed wherever `analytics.js` is
  (`app/tools/build_site_assets.py`, `app/tools/deploy_targets.py`, the
  World's proxied paths).

## Tests

`app/tests/test_first_party_analytics.py`,
`app/tests/test_analytics_consent_prompt.py`,
`app/tests/test_client_error_rejection_reasons.py`, and the package tests.
