# FMTools

Activity, telemetry and the in-app log, notes, tasks, the health and wellbeing tools (Calorie
Counter, Pedometer, Water, Sleep, Sugar Break, Stretch Break, Food Guide, heart rate by tapping),
Glasses Alert, Eye Control's maths, the Speech Playground, the Docs library, bug reports, and the developer performance
overlay's text. `import FMTools` (re-import `FMBase` for `JSONObject`, `FMAccount` for
`PerformanceMonitorService`). Every view model is `@MainActor @Observable`; the SwiftUI views are
in `ForkMesh/Features/Tools`. The Password Vault's logic is FMVault's ([FMVault](FMVault.md)); only
its views and clipboard live in the Tools feature.

## App wiring (`ForkMesh/Features/Tools/ToolsFeature.swift`)

- `ToolsFeature.register()` (called last by AppServices) registers `AppLog`, `AppTelemetry`,
  `ToolsHooks` and `VaultClipboard` (`PasteboardVaultClipboard`: UIPasteboard, `.localOnly` plus
  `.expirationDate` when an expiry is given), starts sampling, follows `AppLifecycle.shared`, and
  runs a 60 Hz display-link frame probe while visible (`ToolsFeature.frameProbeEnabled = false`
  skips it). It does not create the VaultService.
- `ToolsFeature.bootstrapTelemetry() -> AppTelemetry` is idempotent; call it before building the
  HTTP client so requests are counted: `let http = telemetry.wrap(URLSessionHTTPClient())`.
- `ToolsHooks`: the Chat feature fills `relayLog`, `relayFrames`, `nodeActivity` and
  `syncNodeNow`; closures are read inside view bodies so observable reads re-render.

## Entry points (views)

`ActivityScreen`, `NotesScreen`, `NoteEditorScreen(noteId:)`, `TasksScreen`, `TaskDetailScreen`,
`CaloriesScreen`, `StepsScreen`, `WaterScreen`, `SleepScreen`, `SugarBreakScreen`,
`StretchScreen`, `SpeechScreen`, `VaultScreen`, `VaultShareScreen`, `VaultSettingsScreen`,
`TelemetryScreen(focusMetric:logCategory:)`, `TelemetryStrip`, `ReportBugSheet(screen:onFinished:)`,
`BugReportButton(onReport:)` (a 48 pt glass button the shell places in the prompt bar beside send),
`DevPerformanceOverlay` (debug builds). Screens are pushed inside the shell's NavigationStack.
The Logs Home package opens TelemetryScreen's Logs tab; the Diagnostics package opens its Stats tab.

## Telemetry (`AppTelemetry.swift`, `TelemetryLogic.swift`)

- `AppLog(capacity: 2000)`: a thread-safe ring of classified lines (`LogCategory`), with
  `captureProcessLogs()`, `entries(categories:since:)` and per-category counts.
- `AppTelemetry`: a 1 Hz sampler (`sampleNow`), frame and loop-lag recording from any thread,
  `wrap(HTTPClient)` (logs `METHOD host/path status in Nms`, never the query), per-metric
  history in second and minute rings, `series`/`summary` per range. RSS comes from
  `mach_task_basic_info.resident_size` (Darwin) or `/proc/self/statm` (Linux).
- Presentation helpers: `TelemetryFormat`, `telemetryDownsample`, `TelemetryChartGeometry`,
  `MetricCardState`, `TelemetryScreenModel`, `PerformanceLogText`.
- `PerformanceReport.text(...)`: the "Send diagnostics" text for an agent task, clamped to
  1,000–4,000 Unicode scalars. Sections in priority order: metric summaries, stalls by name (with
  the clock time of the newest), slow operations by name, stall culprits, HTTP endpoints (digit runs folded to `#`), MetricKit reports, recent events, busy log
  lines, a per-minute series, the stall log tail and the last app log lines. Over budget, the
  lowest-ranked sections are trimmed first and marked "... N lines trimmed to fit". Stalls have
  their own section so slow network operations cannot push them out of the report.
- `DiagnosticsBundle.lead`: the prompt sends the agent to `DiagnosticsBundle.playbook`
  ([docs/native/PERFORMANCE_CHECKS.md](../../../docs/native/PERFORMANCE_CHECKS.md)) first and asks
  it to add its check there, so each check starts from the earlier ones.

## Screens

Models take a `ToolsApiCall` closure (tests) or an `ApiService` (convenience inits per tool).

- **Activity**: `ActivityScreenModel` loads stats and leaderboards, shows recent log lines and the
  node-sync activity reported by Chat (`NodeActivitySnapshot`).
- **Notes**: `NotesListModel` (`NotesFilter` with counts), `NoteEditorModel` (actions return
  `ToolsOutcome{notice, close}`; autosave, conflicts, `NoteFormatting`, `NoteLinkRef`, export),
  `NoteHistoryModel`, `NoteShareModel` (suggestions, roles, access link), `NotePresence` and
  `NoteRoomModel` (the note socket). Behaviour and reasons:
  [`docs/native/notes.md`](../../../docs/native/notes.md).
- **Tasks**: `TasksListModel` (Mine/Open/Done with icons and counts, `loadMore`),
  `TaskDetailModel`, `TaskFormModel`, `TaskCheckinModel`. People filter: Everyone, each member
  (you first, then A to Z), Bot (agent assignees) and Unassigned, each with its open count; same
  rules as the web board (`docs/app/task-board-filters.md`).
- **Bug reports**: `ReportBugModel(screen:build:signedIn:api:)` (`submit()`), `BugSeverity.all`.
  The admins' Reported tab is `BugReportInboxModel`. A cancelled list load is not an error:
  SwiftUI cancels the view's `.task` when the tab or sheet goes away, and a `.refreshable` when
  the list redraws under it (the loading bar appearing is enough). `ApiService.bugReportInbox`
  answers `BugReportInbox.cancelledLoad`, the model keeps its state and stays unloaded so the
  next appearance loads again, and pull-to-refresh runs the load in its own `Task`. Before this
  the cancelled request read "Could not reach ForkMesh. Check your connection and try again.",
  was marked loaded, and never retried (2026-10-17: the relay had no inbox route yet, answered
  404, and the phone then cancelled its next request). A 404 without `report_missing` or
  `attachment_missing` means the relay has no inbox route, and the sheet says to update it.

### Health and wellbeing

Each tool has a model over its package API, an optional Apple Health bridge protocol implemented
by `HealthKitBridge` (`ForkMesh/Platform/HealthKitBridge.swift`) and faked in tests, and per-device
switches named in `HealthPrefs` (`health.steps.enabled`, `health.water.enabled`,
`health.calories.enabled`, `health.sleep.enabled`, `health.sleep.writeHealth`,
`health.sugarBreak.enabled`, `health.stretch.enabled`, `health.foodGuide.enabled`).

- **Apple Health plumbing** (`HealthModels.swift`): `HealthReconcile.plan(desired:existing:)` turns
  the entries a tool should have in Health and the samples ForkMesh wrote earlier (entry id and
  version in sample metadata) into saves and deletions; `HealthReconcile.imports` picks other
  apps' samples not yet logged; `HealthDates.sampleDate` never dates a sample in the future.
  `HealthFeature` runs syncs from the HealthKit observer query (hourly background delivery) and on
  foreground. `AppleHealthGuide` holds the per-tool reads/writes disclosure shown in each tool and
  under Settings > Legal > Apple Health.
- **Calories** (`CaloriesModel`, `/api/calories`): diary, goals, search, portions, photo
  analysis, challenges and charts; results are shown inline as `ToolsNotice`. Writes the last 14
  days to Health as dietary energy and macros when enabled.
  - Quick add: the overview carries `quickFoods` (My foods, at most 60) and `commonFoods`.
    `CaloriesQuick` parses typed meals (meal, day, weight, parts split on `,` `;` `+` `&` "and",
    amounts as numbers, fractions such as `1/2` before decimals, `3x`, or words) and matches them
    against My foods, frequent and common foods. `logTyped(text)` logs a weigh-in, matched
    entries (`source: "quick"`), or sends the text to `POST /analyze` (`source: "text"`).
  - Receipts: `readReceipt(dataURL:)` (`POST /receipt`); the photo is re-encoded as JPEG (1600,
    1280, 1024, 800 px) until the data URL fits the 1.6 MB limit.
  - While the screen is visible it registers a `ShellPromptTarget` so the shell's prompt bar
    logs typed meals.
- **Steps** (`StepsModel`, `/api/steps`): Health step sync at most every 15 minutes, live motion
  count while the screen is open, goals and charts.
  - Walking scene: the figure's speed follows live cadence from the accelerometer
    (`StrideMeter`: 50 Hz magnitude, gravity removed with a 1 s low-pass, peaks above an
    adaptive threshold of 45% of recent peak height and at least 0.08 g, steps at least 0.25 s
    apart; cadence over the last six steps; running detected by cadence and peak height with
    hysteresis). Without an accelerometer it falls back to the pedometer's cadence. Stride is
    the pedometer's calibration when available, else today's distance over steps. Reduce Motion
    shows the figure standing.
  - Walks: `WalkDetector` turns Core Motion activity into walks (30 s of confident walking to
    start, 3 minutes of other activity to end). `WalkActivityController` shows each walk as a Live
    Activity (`WalkActivityAttributes`, `ForkMeshWidgets/WalkLiveActivity.swift`), catches up from
    Core Motion history when HealthKit wakes the app, and keeps a live pace (`WalkPace`) that
    drives the top bar's walking chip.
  - Heart rate by tapping (`HeartTapModel`): at least 5 taps, a 2.5 s pause restarts, the last 12
    intervals kept, intervals more than 30% off the median dropped, 30–220 bpm accepted; saved to
    Health only (`HKMetadataKeyWasUserEntered`), never sent to the account.
- **Water** (`WaterModel`, `/api/water`): presets, typed amounts, goals, Health reconciliation;
  `WaterText` formats ml, L, fl oz and cups.
- **Sleep** (`SleepModel`, `/api/sleep`): `SleepDetector` infers a night from Core Motion
  activity minus ForkMesh's own foreground time; `healthNights` sums Health sleep samples;
  `SleepSchedule` assigns times to nights (boundary five hours after the wake time);
  `SleepHealthSync` posts both sources at most every 15 minutes. `SleepModels.collect` runs
  `healthNights` and `SleepDetector.detect(nights:...)` (sorts the rest samples once for every
  night) in a detached task, and `HealthKitBridge` takes Core Motion's activity history on its
  own `OperationQueue`: up to seven days of activity rows used to be mapped and scanned per
  night on the main thread at every resume.
- **Sugar Break** (`SugarBreakModel`, `/api/sugar-break`, reads pass `?day=<local today>`):
  start a break (1–365 days, default 30), +1 skips, slips, undo, end early; Health Dietary Sugar is
  read-only and suggests a slip. `SugarContract` builds the shareable no-sugar contract rendered
  to a PNG.
- **Stretch Break** (`StretchModel`, `/api/stretch`): routines decode into Codable structs with
  defaults; `StretchMath` interpolates poses and returns the figure's joints and segments;
  `StretchRun` is the session state machine (pause, back, skip, spoken cues). Sessions of at least
  30 active seconds are logged (`source: "ios"`), and saved to Health as workouts
  (`HKWorkoutBuilder`) when enabled.
- **Food Guide** (`FoodGuideModel`, no server API): reads the Calorie Counter goal and today's
  total, and with Health enabled 14 days of nutrient sums. `FoodNeeds` flags nutrients under 80% of
  the daily value or limits over it; `FoodMealPlan` splits the remaining calories over the meals
  ahead; `FoodPlanner` suggests recipes by diet flags, calorie fit, nutrient gaps and goal.

### Glasses Alert (`GlassesWatchModels.swift`, package `glasses-alert`)

Detects Meta smart glasses from Bluetooth advertisements; no API.

- `GlassesSignatures.match` keeps adverts whose manufacturer data starts with company ID
  `0x01AB`/`0x058E` (Meta) or `0x0D53` (Luxottica), or whose name has a glasses keyword, and whose
  RSSI clears the `GlassesRange` floor (near -65, room -75, wide -90 dBm). `GlassesTracker` alarms
  once per new identifier with a 30 s cooldown and forgets devices after 120 s.
- `GlassesWatcher` (app) scans with `CBCentralManager` (foreground only; the screen keeps the
  device awake while watching). The alarm loops a sound in the `.playback` category with haptics,
  a time-sensitive notification and a full-screen view for 30 s.
- `GlassesHoming` estimates closeness, trend and a bearing by binning RSSI by compass heading into
  12 sectors (the body shades the signal from behind). `GlassesCensus` lists other nearby devices
  with one of 25 `NearbyKind`s, classified by product-name keywords, then GAP appearance (Android
  only), standard service UUIDs, then company IDs. `app/tests/test_native_glasses_icons.py` checks
  both platforms list the same kinds and icons.
- `GlassesRadarChip` in the top bar shows a radar (`GlassesRadarLayout`: distance from closeness,
  a fixed hashed angle per device, real bearing for followed glasses).
- Prefs: `glassesAlert.range`, `glassesAlert.watching`, `glassesAlert.keepAwake`.

### Phone Security (`PhoneSecurity*.swift`, package `phone-security`)

The phone checkup; the check table and severity choices are in
[`docs/marketplace/phone-security.md`](../../../docs/marketplace/phone-security.md).

- `PhoneSecurityFacts` holds what the app's `PhoneSecurityProbe` read; a `nil` field makes its
  check Unavailable. `PhoneSecurityScanner.scan` is pure, so tests feed it fixtures.
- `PhoneSecurityCatalog` lists the checks, `PhoneSecurityPaths` the jailbreak paths, system links,
  injected-library markers and injection variables, `PhoneSecurityExplanations` the plain-language
  text, and `PhoneSecurityPolicy.newestIOSMajor` the newest iOS major (bump it with each release).
- `PhoneSecurityReport` sorts worst first and gives the filters, counts, category list, search,
  headline and the desktop's summary wording.
- `PhoneSecurityProbe` (app) reads LocalAuthentication, notification settings, the Info.plist,
  `CFNetworkCopySystemProxySettings`, dyld's image list, `sysctl` for `P_TRACED`, `lstat` and
  file existence, and permission statuses. The sandbox test creates a uniquely named file in
  `/private` and unlinks it at once; it only succeeds on a jailbroken phone.
- `PhoneSecurityScreen` (app) shows the headline, filters with counts, a category menu, search and
  a detail sheet whose fix opens ForkMesh's Settings page or its notification settings, or lists
  steps for system settings iOS cannot link to.

### Eye Control (`EyeControlModels.swift`, package `eye-control`)

Experimental gaze cursor; design and thresholds are in
[`docs/marketplace/eye-control.md`](../../../docs/marketplace/eye-control.md).

- `EyeGaze.screenPoint` intersects the averaged eye ray (face anchor in camera space) with the
  screen plane and converts metres to points with `EyeScreen`. `EyeTransform`/`EyeVector` are
  plain-Double stand-ins for simd so the maths tests on Linux.
- `EyeCursorEngine` applies `EyeCalibration` (affine, least-squares `fit`), smooths with
  `EyeOneEuro`, freezes during a blink and returns `.click` at the point from just before it
  (`EyeBlinkDetector`, `EyeBlinkLength`). `EyeCalibrationRun` walks nine dots.
- `EyeTracker` (app) runs `ARFaceTrackingConfiguration` and draws the cursor window;
  `EyeClicker` turns a click into a control press or `accessibilityActivate()`.
- Prefs: `eyeControl.smoothing`, `eyeControl.blinkLength`, `eyeControl.calibration`.

### Speech Playground (`SpeechModels.swift`, package `speech`)

On-device text to speech. `SpeechPlaygroundModel` picks `SpeechVoices.best` (natural voices
first, the phone's language first, then Personal > Premium > Enhanced > Standard) unless the saved
`speech.voice` is installed; speak, pause, resume, preview, speed and pitch (0.5–2, stored as
`speech.speed`/`speech.pitch`), word highlighting, and `renderAudio()` to an AAC `.m4a` for
sharing. `AppleSpeechEngine` uses `AVSpeechSynthesizer` on a ducking `.spokenAudio` session.
See `marketplace/packages/speech/README.md`.

## Docs library (`Docs/DocsLibrary.swift`, `Docs/DocsMarkdown.swift`)

The Docs package (route `.docs`) shows the public documentation and dependency list offline. The
app ships `ForkMesh/Resources/DocsLibrary.json` (schema `forkmesh-docs-library-v1`, generated by
`marketplace/tools/docs_mobile_bundle.py`; never edit by hand). The model follows the desktop's
`DocsLibrary.cpp`.

- `DocsLibrary(data:)` decodes leniently (malformed entries dropped; absolute, `..` or repeated
  paths skipped). Areas, pages, previous/next, home path, source URLs.
- Dependencies are sanitised: GitHub URLs only for repository, profile and projects, http(s)
  websites, valid logins, at most three authors, relative `usedBy` paths.
- `search(query)` requires every token and scores title, description, path and body like the
  desktop; snippets are cut to 140 characters around the hit.
- `resolve(from:href:)` turns links into anchors, pages (folders resolve to their `README.md`),
  website files or external URLs; `linkURL` encodes them as `fmdocs://` URLs for the view.
- `DocsMarkdown.blocks` is a Linux-tested block parser (headings with GitHub anchors, lists with
  task boxes, quotes, code, pipe tables); `DocsInline.runs` handles inline markup so links can be
  rewritten before SwiftUI sees them.

## Notes

- The vault's quick unlock uses `ForkMesh/Platform/LocalAuthVaultDeviceAuth.swift`;
  `VaultPlatform` stays `NoVaultPlatform` in the main app.
- The vault screen's flow tests are FMVault's `VaultScreenFlowTests`; the bug report API tests
  belong to FMApi.
