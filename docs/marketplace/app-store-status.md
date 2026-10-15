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
3. Every client reads `GET /api/app-store-status` and renders the snapshot
   natively. Pages show when it was published; to refresh, run the shortcut
   again (from the desktop's Shortcuts page or from a phone).

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
     "device": "iPhone16_2 27.0", "comment": "Show time as 5:45pm"}
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

`label` is the human text a client shows; `state` is the raw App Store
Connect value for tooltips and logs. `unavailable` maps a section key
(`versions`, `submissions`, `downloads`, `builds`, `groups`, `feedback`,
`reviews`) to the error that kept it empty; a client shows it inside that
section. `downloads.available` is false with a `note` when the vendor number
is not set or the app has no App Store version yet.

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
7. Tester feedback, then customer reviews (stars from `rating`).

## Files

- Package: `manifest.json`, `content/worker/app_store_status.py`,
  `content/web/{view.html,page.js}`, `tests/test_app_store_status.py`.
- Desktop: `content/desktop/AppStoreStatusModel.{h,cpp}` (pure parsing and
  formatting, tested by `content/desktop/tests/test_app_store_status.cpp`)
  and `AppStoreStatusFeature.{h,cpp}` (the read-only rail page; `refresh()`
  is one GET, `exportData()` uses `/export`). The firewall entries are in
  `desktop/resources/network/endpoint-catalog.json`.
- Report: `native/ios/tools/asc_store_report.py`; tests
  `tools/test_asc_store_report.py` (which also checks that
  `clean_snapshot` keeps every field the report writes).
- iPhone: `FMPackages/AppStoreStatusModels.swift` (tolerant decoding and the
  loader), `Features/Packages/AppStoreStatusScreen.swift`, route
  `.appStoreStatus` (Insights group of the You hub). The shortcut detail
  page for `ios-app-store-status.sh` has an "Open App Store Status" button
  when the package is installed.
- Android: `core/.../market/AppStoreStatus.kt` and
  `app/.../feature/appstorestatus/`, route `Route.AppStoreStatus`.
