# App Store Status package

`marketplace/packages/app-store-status` shows the iOS app as App Store Connect
sees it, on every client: the web dashboard (`/dashboard/app-store-status`),
the Qt desktop (rail page `builtin:app-store-status`), the iPhone app and the
Android app.

## Where the data comes from

Only the operator's desktop holds the App Store Connect API key
(`~/.appstoreconnect/private_keys/AuthKey_<id>.p8`, see
[../native/IOS_RELEASE.md](../native/IOS_RELEASE.md)). The relay never sees
the key and never calls Apple. Instead:

1. The **iOS App Store status** shortcut (`.forkmesh/shortcuts/ios-app-store-status.sh`)
   runs `tools/mac_adhoc_deploy.py --store-status`, which runs
   `native/ios/tools/asc_store_report.py --publish`.
2. The report collects everything into one snapshot (`collect()`), prints the
   familiar text report from it (`render()`), and with `--publish` PUTs the
   snapshot to `/api/app-store-status/snapshot`, signed with the desktop's
   identity key (`forkmesh_mcp_server.package_request`, proof
   `forkmesh-app-store-status-request-v1`). A publish failure is printed and
   never fails the report. `--json` prints the snapshot instead of the text.
   The same `--publish` run also sends each day's downloads, countries and
   devices, the versions and the reviews to Marketing's App Store channel
   (`/api/marketing/<org>/app-store`); see "App Store desk" in
   [../../marketplace/packages/marketing/README.md](../../marketplace/packages/marketing/README.md).
3. Every client reads `GET /api/app-store-status` and renders the snapshot
   natively. Pages show when it was published; to refresh, run the shortcut
   again (from the desktop's Shortcuts page or from a phone).

### Desktop fallback (iPhone)

When the server cannot provide the page, the iPhone asks the paired desktop
instead, so the desktop's App Store Connect credentials still answer. The phone
falls back when `GET /api/app-store-status` fails with a "not there" code
(`not_found`, `http_404`, `method_not_allowed`, `http_405`, or a
not-installed code) or answers with no snapshot. Other failures (offline, 5xx)
do not trigger the fallback. Without a paired desktop the phone does not try.

The request is `stats.appStoreStatus` on the desktop's owner-only `stats.`
control family (LAN or relay). The desktop finds a checkout holding
`native/ios/tools/asc_store_report.py` (`forkmesh/forkmesh` first) and runs it
there with `--json`. It passes `ASC_KEY_ID`, `ASC_ISSUER_ID` and
`ASC_VENDOR_NUMBER` from its Actions variables, kills the run after 110 s, and
caches a good answer for 60 s. Concurrent requests share one run. It replies
`{ok, source: "desktop", published, publishedAt, snapshot}`; a failure is
`app_store_report_failed` (with the report's last stderr line),
`app_store_report_unreadable` or `app_store_report_missing`. Nothing is
published to the server: the snapshot goes only to the phone that asked. The
page then reads "From your desktop <time>" instead of "Published <time>". If
the desktop fails too, the phone shows the server's message followed by "Your
desktop could not fetch it either: <reason>". An older desktop answers
`unsupported`, which reads "This desktop's ForkMesh is too old for that.
Update it."

The snapshot is account-scoped (`dataLifecycle.scope: "account"`): only the
account that published it can read it. It is sealed with the relay's key in
`app_store_status_snapshots`, removed by package uninstall
(`purge_account_data`) and by account deletion.

## API

All routes take a session (cookie or bearer) or the signed `node`/`ts`/`sig`
query of a desktop.

| Route | Answer |
|---|---|
| `GET /api/app-store-status` | `{ok, actor, published, publishedAt, snapshot}`; `snapshot` is `null` until the first publish |
| `PUT /api/app-store-status/snapshot` | stores the body (at most 256 KB) after `clean_snapshot`; `{ok, publishedAt}` |
| `DELETE /api/app-store-status/snapshot` | forgets the snapshot |
| `GET /api/app-store-status/export` | `{ok, filename, export}` |
| `GET /api/app-store-status/screenshots/<id>` | `{ok, id, mime, width, height, data}` (`data` is base64); `404 screenshot_not_found` until uploaded |
| `PUT /api/app-store-status/screenshots/<id>` | stores one tester screenshot (body as the GET answer, at most 3 MB of picture); `409 screenshot_not_in_snapshot` unless the current snapshot lists that id |

The API exists only once a deploy carrying this package has reached the app
nodes. Until then the app's catch-all answers `404 {"error":"not_found"}`
(the package never answers `not_found` for `GET /api/app-store-status`). On
2026-10-15 the iPhone showed "App Store Status is unavailable / not found"
because production was still on the previous release: the iPhone mapped only
`http_404` to "This server does not offer App Store Status yet.", not the
`not_found` code the server actually sends. It now maps `not_found`,
`http_404`, `method_not_allowed` and `http_405` to that sentence.

## Snapshot format (`forkmesh-app-store-status-v1`)

The worker keeps only these fields, clamps strings and list lengths, and
drops anything else. Dates are App Store Connect ISO 8601 strings
(`2026-10-08T18:29:00Z` or with an offset); clients format them locally.

```json
{
  "format": "forkmesh-app-store-status-v1",
  "generatedAt": 1760000000000,
  "app": {"id": "6752000000", "bundleId": "com.forkmesh.app"},
  "attention": ["1.4.0 was rejected: Apple's message is in App Store Connect -> App Review"],
  "versions": [
    {"version": "1.4.0", "build": "19", "releaseType": "AFTER_APPROVAL",
     "createdAt": "2026-09-16T09:45:00Z",
     "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}
  ],
  "submissions": [
    {"id": "s1", "submittedAt": "2026-10-08T18:29:00Z",
     "status": {"state": "UNRESOLVED_ISSUES", "label": "Unresolved issues", "tone": "action"},
     "items": [{"name": "1.4.0", "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}]}
  ],
  "downloads": {
    "available": true, "note": "", "since": "2026-08",
    "total": {"downloads": 18, "redownloads": 2, "updates": 7},
    "recent": [{"days": 7, "downloads": 1, "redownloads": 0, "updates": 2},
               {"days": 30, "downloads": 104, "redownloads": 0, "updates": 2}]
  },
  "builds": [
    {"version": "1.4.0", "build": "19", "uploadedAt": "2026-10-05T12:51:00Z", "expired": false,
     "processing": {"state": "VALID", "label": "Valid", "tone": "good"},
     "internal": {"state": "IN_BETA_TESTING", "label": "In beta testing", "tone": "good"},
     "external": {"state": "WAITING_FOR_BETA_REVIEW", "label": "Waiting for beta review", "tone": "pending"},
     "betaReview": {"state": "", "label": "Not submitted", "tone": "neutral"},
     "groups": ["forkmesh-team"]}
  ],
  "groups": [
    {"name": "outside", "internal": false, "testers": 1, "allBuilds": false,
     "publicLinkEnabled": true, "publicLink": "https://testflight.apple.com/join/x",
     "publicLinkLimit": 0, "feedbackEnabled": true}
  ],
  "feedback": [
    {"kind": "screenshot", "createdAt": "2026-09-01T03:06:00Z", "from": "t@example.com",
     "device": "iPhone16_2 27.0", "comment": "Show time as 5:45pm",
     "screenshots": [{"id": "3f1c2a9b8d7e6f5a4b3c2d1e", "width": 1179, "height": 2556}]}
  ],
  "reviews": [
    {"createdAt": "2026-10-01T10:00:00Z", "rating": 5, "title": "Great", "territory": "USA",
     "body": "Works well"}
  ],
  "unavailable": {"feedback": "ERROR: App Store Connect GET answered 404"}
}
```

`status.tone` is the only thing a client colours by:

| Tone | Meaning | Colour role |
|---|---|---|
| `action` | Apple wants something from us (rejected, invalid binary, missing export compliance, unresolved issues) | destructive |
| `pending` | waiting on Apple or processing (waiting for review, in review, processing, waiting for beta review) | warning |
| `good` | live or usable (ready for distribution, valid, in beta testing, approved) | success |
| `neutral` | anything else (prepare for submission, not submitted, expired) | muted |

A feedback row's `screenshots` lists at most 4 pictures the tester attached,
each `{id, width, height}`: `id` is 24 lowercase hex characters, `width` and
`height` are Apple's pixel size. The picture itself is not in the snapshot; see
"Tester screenshots" below.

`label` is the human text a client shows; `state` is the raw App Store
Connect value for tooltips and logs. `unavailable` maps a section key
(`versions`, `submissions`, `downloads`, `builds`, `groups`, `feedback`,
`reviews`) to the error that kept it empty; a client shows it inside that
section. `downloads.available` is false with a `note` when the vendor number
is not set or the app has no App Store version yet.

## Tester screenshots

TestFlight screenshot feedback carries the pictures the tester attached
(`attributes.screenshots` on `betaFeedbackScreenshotSubmissions`: a signed
`url`, `width`, `height` and `expirationDate`). The iOS in-app bug report on
this page asked to see them (task fca77d15, 2026-10-16), so version 1.1.0
shows each one as a thumbnail under its feedback row on every client, and a
tap or click opens it full size.

Apple's URLs are signed and expire, and the relay never calls Apple, so the
desktop copies the pictures to the server:

1. `collect()` gives every picture an id, the first 24 hex characters of
   SHA-256 over the feedback id and the picture's position, so the same
   picture keeps its id across runs. The row keeps Apple's `url` next to it.
2. `publish()` PUTs the snapshot without the URLs (`public()`). The answer's
   `missingScreenshots` lists the ids the snapshot names that the server does
   not hold yet; the worker also deletes stored pictures the new snapshot no
   longer names.
3. For each missing id the report downloads Apple's URL, shrinks it to a JPEG
   of at most 3 MB and 2048 px with Pillow (without Pillow a JPEG or PNG under
   3 MB is sent as it is), and PUTs it to `/screenshots/<id>`. A failed
   download or upload is printed ("uploaded 1 of 2 new tester feedback
   screenshots") and never fails the report.

The worker keeps each picture sealed in `app_store_status_screenshots`
(account, id), checks the bytes really are a JPEG or PNG, and refuses ids the
current snapshot does not list, so a client cannot store anything else there.
Forgetting the snapshot, uninstalling and account deletion remove the
pictures too. The export does not include them.

`--json` keeps Apple's `url` on each picture, so the iPhone's desktop fallback
loads them straight from Apple while the links are fresh; every other read
comes from `GET /screenshots/<id>`. A picture that is not uploaded yet shows a
broken-picture tile and "This screenshot has not been uploaded yet. Run the
iOS App Store status shortcut on your desktop again."

Viewers: the web page opens a dialog with Previous/Next (and arrow keys) and a
Save link; the desktop opens a dialog with Previous, Next and Save…; the
iPhone opens a full-screen pager with pinch and double-tap zoom and Share; the
Android app opens a full-screen dialog with pinch and double-tap zoom and
previous/next buttons. Every client fetches a picture once and keeps it for
the session.

## Page layout (every client)

1. Header: "App Store Status", the app's bundle id, and "Published <relative
   time>" from `publishedAt`. Empty state before the first publish: explain
   that the desktop publishes it when the iOS App Store status shortcut runs.
2. `attention` lines as one destructive banner, when non-empty.
3. App Store versions, then App Review submissions with their items.
4. Downloads: three figures (downloads, redownloads, updates) for the total
   and for each `recent` span.
5. TestFlight builds (processing, internal, external, beta review, groups).
6. TestFlight groups (internal first; testers; every/chosen builds; public
   link; feedback on/off).
7. Tester feedback with its screenshot thumbnails, then customer reviews
   (stars from `rating`).

## Files

- Package: `manifest.json`, `content/worker/app_store_status.py`,
  `content/web/{view.html,page.js}`, `tests/test_app_store_status.py`.
- Desktop: `content/desktop/AppStoreStatusModel.{h,cpp}` (pure parsing and
  formatting, tested by `content/desktop/tests/test_app_store_status.cpp`)
  and `AppStoreStatusFeature.{h,cpp}` (the read-only rail page; `refresh()`
  is one GET, `exportData()` uses `/export`). The firewall entries are in
  `desktop/resources/network/endpoint-catalog.json`.
- Desktop fallback: `stats.appStoreStatus` in
  `desktop/src/MainWindowLocalControlStats.cpp`; its pure parts
  (`appStoreStatusReply`, `appStoreReportError`) are in `LocalControlStats.cpp`,
  tested by `desktop/tests/test_local_control_stats.cpp`.
- Report: `native/ios/tools/asc_store_report.py`; tests
  `tools/test_asc_store_report.py` (which also checks that
  `clean_snapshot` keeps every field the report writes).
- iPhone: `FMPackages/AppStoreStatusModels.swift` (tolerant decoding and the
  loader with the desktop fallback), `Features/Packages/AppStoreStatusScreen.swift`
  (which supplies the `stats.appStoreStatus` call through `DesktopControlHub`), route
  `.appStoreStatus` (Insights group of the You hub). The shortcut detail
  page for `ios-app-store-status.sh` has an "Open App Store Status" button
  when the package is installed.
- Android: `core/.../market/AppStoreStatus.kt` and
  `app/.../feature/appstorestatus/` (`AppStoreScreenshots.kt` holds the
  thumbnails and the viewer), route `Route.AppStoreStatus`.
- Screenshots on iPhone: `AppStoreScreenshotLoader` in
  `AppStoreStatusModels.swift`; the thumbnails and viewer are at the end of
  `AppStoreStatusScreen.swift` and reuse `AgentZoomableImage`.
