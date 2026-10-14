# FMTools

Activity, telemetry and the in-app log, notes, tasks, the Calorie Counter, bug reports, and the
developer performance overlay's text. `import FMTools` (re-import `FMBase` for `JSONObject`,
`FMAccount` for `PerformanceMonitorService`). Every view model is `@MainActor @Observable`; the
SwiftUI views are in `ForkMesh/Features/Tools`. The Password Vault's logic is FMVault's
(docs/FMVault.md); only its views and clipboard live in the Tools feature.

## App wiring (`ForkMesh/Features/Tools/ToolsFeature.swift`)

- `ToolsFeature.register()` (no arguments; AppServices calls it last). Registers `AppLog`,
  `AppTelemetry`, `ToolsHooks` and `VaultClipboard` (`PasteboardVaultClipboard`: UIPasteboard,
  `.localOnly` + `.expirationDate` when an expiry is given), starts sampling, follows
  `AppLifecycle.shared`, and runs a 60 Hz display-link frame probe while visible (feeds FPS/FRAME;
  set `ToolsFeature.frameProbeEnabled = false` before `register()` to skip it). It does NOT create
  the VaultService (AppServices does; the vault screens look it up with `Services.get`).
- `ToolsFeature.bootstrapTelemetry() -> AppTelemetry`: idempotent; creates and registers the log
  (capturing `Log.sink`) and the sampler. Call it BEFORE building the HTTP client to count
  requests: `let http = telemetry.wrap(URLSessionHTTPClient())` (returns `HTTPClient`).
- `ToolsFeature.hooks` / `Services.get(ToolsHooks.self)`: the Chat feature fills
  `relayLog: () -> [String]` (RelayService.log, oldest first, "HH:MM:SS  text"),
  `relayFrames: () -> Int` (frames received since launch), `nodeActivity: () -> NodeActivitySnapshot?`
  and `syncNodeNow: () -> Void`. Closures are read inside view bodies, so reading observable state
  in them re-renders the Activity screen.

## Entry points (views)

`ActivityScreen()`, `NotesScreen()`, `NoteEditorScreen(noteId: String? = nil)`, `TasksScreen()`,
`TaskDetailScreen(model:)`, `CaloriesScreen()`, `StepsScreen()`, `WaterScreen()`, `SleepScreen()`, `SugarBreakScreen()`, `StretchScreen()`, `SpeechScreen()`, `VaultScreen()`, `VaultShareScreen(vault:itemId:)`,
`VaultSettingsScreen(vault:clipboard:)`, `TelemetryScreen(focusMetric: = nil, logCategory: = nil)`,
`TelemetryStrip()` (opens Diagnostics in a sheet), `ReportBugSheet(screen: = "", onFinished: = nil)`,
`BugReportButton(onReport:)` (a round 48pt glass button, `FmGlassSurface`; the shell puts it in
the prompt bar just left of the send button, on iPhone and iPad alike, and presents
`ReportBugSheet` itself), `DevPerformanceOverlay()` (debug builds only; overlay the root). Screens
are pushed inside the shell's NavigationStack and set their own titles and toolbars.

The report button used to float over the tab content at the screen edge, draggable. Over the native shell it sat on top of lists and empty states, so
it moved into the prompt bar at the foot of every page, where it covers nothing and is always in
the same place. `HomeShell` opens `ReportBugSheet` with the open tab (`code`, `agents`, `chat`) as
the report's screen and shows the thanks in the shell's inline notice line under the top bar when a report is filed. The placement the draggable
button remembered (`SettingsService.bugButtonAnchor`, `bugButtonOnLeft`, `BugButtonGeometry`) is no
longer read by the app. This is PR #153, written for the `agent/native-gate2` shell (a bottom bar
with a prompt button, and a floating prompt button on iPad); main has neither, so the button sits
in main's prompt bar beside its trailing action, send.

## Telemetry (`AppTelemetry.swift`, `TelemetryLogic.swift`)

- `LogCategory` (`label`, `description`, `icon`, `index`, `classify(line)`), `LogCategoryInk.of(category, count:)`.
- `AppLog(capacity: 2000, now:)`: thread-safe ring. `add(category, message)`, `addLine(line)`
  (trims right, classifies), `captureProcessLogs() -> restore`, `entries(categories:since:)` (newest
  first), `countsSince(date)`, `totalLines`, `totalFor(category)`. `AppLogEntry.description` is
  the entry with an ISO 8601 time (`DartDateFormat.iso8601`), `.stamp()` is HH:mm:ss.
- `TelemetryMetric` (`label`, `short`, `title`, `description`, `unit`, `floor`, `peak`, `count`,
  `format`, `formatBare`), `TelemetryRange` (`label`, `seconds`, `minutes`, `usesMinutes`),
  `MetricHistory` (second ring + minute ring, `add`, `addGap`, `lastSeconds`, `lastMinutes(peak:)`).
- `AppTelemetry(log:monitor:relayFrames:sampleInterval:now:rssMegabytes:)`: `start(lifecycleState:)`,
  `appLifecycleChanged`, `stop`, `sampleNow()` (the 1 Hz tick; bumps observable `revision`),
  `recordFrame(Duration)` / `recordFrames([FrameTiming])` / `recordLoopLag` (any thread),
  `wrap(HTTPClient) -> HTTPClient` (logs `METHOD host/path status in Nms`, never the query),
  `latest(metric)`, `history(metric)`, `series(metric, range)`, `summary(metric, range)`, `uptime`.
- `TelemetryFormat.compactCount/uptime/ago`, `telemetryDownsample(values, size, peak:)`,
  `TelemetryChartGeometry` (`scaleTop`, `indexAt`, `x`, `y`, `segments`), `MetricCardState`
  (`points`, `slots`, `value(scrub:)`, `when(scrub:)`, `footer`, `scrubIndex(x:width:)`),
  `TelemetryScreenModel` (range, focus, tab, categories, pause, `visibleEntries`, `caption`,
  `copyText()`), `PerformanceLogText` (overlay copy text, card lines, `clampIndicator`).
- `PerformanceReport.text(telemetry:context:metricKit:stallLog:now:timeZone:limit:)` (`@MainActor`):
  the shell prompt's "Send diagnostics" text, at most `limit` Unicode scalars, clamped to
  `minChars` (1,000) ... `maxChars` (4,000) so it fits an 8,000-character agent task with the
  user's own words. `context` is the app's device and screen facts as key/value pairs; sections,
  most useful first, are metric summaries over 5 m and 1 h, the top `topRows` slow events by name
  (a kind equal to its name, `frame`, is said once), stall culprits (`finishedDuring` /
  `inFlight`), HTTP endpoints over the last five minutes (digit runs folded to `#`), two MetricKit
  reports one line per line, the newest `recentEvents` slow events, the busiest log lines, a
  per-minute series for the last `recentMinutes`, the stall log tail, the last `logLines` app log
  lines and log volume, every row clipped. Over budget, `shrink` trims the lowest ranked sections
  first (down to 3 lines each, then 1, then 0), marking each with "... N lines trimmed to fit"; the
  stall log and app log lose their oldest lines. `fit` cuts only what is still over. Empty
  sections say `none`.

## Screens

- `ToolsApiCall = @MainActor (method, suffix, body?) async throws -> JSONObject`; models take one
  (tests) or an `ApiService` (convenience inits: `notesApi`, `tasksApi`, `caloriesApi`, `stepsApi`,
  `waterApi`, `sleepApi`, `sugarBreakApi`, `stretchApi`).
- Activity: `ActivityScreenModel(stats:leaderboards:hooks:)` / `(api:hooks:)`; `reload()`, `stats`/
  `leaderboards: ActivityLoad` (`.loading`, `.failed(dartErrorText)`, `.loaded`), `visibleLogs:
  [ActivityLogLine]` (12-hour stamp, icon, tone), `showAllLogs`, `node: NodeActivityPresentation?`;
  statics `fundsGroups`, ... `NodeActivitySnapshot` is what the Chat feature
  reports (mode, connection, counts with pre-pluralized labels, applier totals).
- Notes: `NotesListModel`, `NoteEditorModel` (`save/setVisibility/delete/discard/refreshMeta`
  return `ToolsOutcome{notice: ToolsNotice?, close}`), `NoteHistoryModel`, `NoteShareModel`.
- Tasks: `TasksListModel` (filters Mine/Open/Done, `loadMore`), `TaskDetailModel` (`actions:
  [TaskAction]`, `perform`, `complete(note:)`, `checkin`, `sendReply`, `delete`, `facts`),
  `TaskFormModel`, `TaskCheckinModel`, `TaskText` (error sentences, labels).
- Task board people and icon tabs (2026-10-06, same rules as the web board in
  `docs/app/task-board-filters.md`): `filterTabs: [TaskFilterTab]` gives each status tab a Material
  icon (`person_outline`, `radio_button_checked`, `check_circle_outline`) and a count; the screen
  draws them through `FmSegmentTab(icon:)` with "Open, 3" as the VoiceOver label. `people:
  [TaskPerson]` is Everyone, each member (you first, then A to Z, case-insensitive, deduplicated),
  Bot (`assigneeKind` agent, bot, claude or codex) and Unassigned (also a user task with no
  assignee), each with its open-task count regardless of the status tab. `selectPerson` toggles
  (the selected one again means Everyone) and moves Mine to Open; `selectFilter(0)` resets the
  person to Everyone. Open and Done counts follow the selected person; Mine always counts the
  actor's own open tasks. A person missing from a reloaded board falls back to Everyone. Member
  avatars reuse `MessagesPersonAvatar` (one cached `/api/accounts` read per name, no polling).
- Calories: `CaloriesModel(api: CaloriesAPI, timeZone:, now:)`; `notice: ToolsNotice?` (message and
  `isError`) carries every result sentence, shown inline on the screen (no toasts, 2026-10-09); sheet models `CaloriesEntryDraft`, `CaloriesSuggestionsModel`, `CaloriesSearchModel`,
  `CaloriesPortionModel`, `CaloriesGoalsModel`, `CaloriesChallengeDraft`, `CaloriesBoard`;
  `CaloriesText` (days, numbers, usernames, tz offset), `CaloriesPhoto` (data-URL rule; the app
  re-encodes too-large photos), chart geometry `CaloriesRing`, `CaloriesDayChart`, `CaloriesWeightChart`.
- Calories quick add and typed logging (`CaloriesQuick.swift`, calories 2.3.0): see below.
- Steps: `StepsModel(api: StepsAPI, health: HealthBridge?, prefs:, timeZone:, now:)` for the
  Pedometer package (`/api/steps`); `load(syncIfDue:)`, `install()` (turns Apple Health on when it
  is available), `enableHealth()`, `disableHealth()`, `syncNow(days:)`, `startLive()`/`leave()`
  (live motion count while the screen is open, one sync on leaving when it ran ahead),
  `addSteps(day:steps:distance:)`, `saveGoal(_:units:)`; presentation `stepsText`, `ringRatio`,
  `detailLine`, `sourceLine`, `stats`, `bars`, `goalRatio`; `StepsText` (grouped counts, km/mi,
  12-hour clock, `gait`).
- Glasses Alert (`GlassesWatchModels.swift`, package `glasses-alert`, route `.glassesAlert`, path
  segment `glasses-alert`): finds Meta smart glasses from Bluetooth adverts, no API.
  `GlassesSignatures.match(advert, range:, now:)` keeps an advert whose manufacturer data starts
  with company ID `0x01AB`/`0x058E` (Meta) or `0x0D53` (Luxottica), or whose name contains a
  glasses keyword, and whose RSSI is at least the `GlassesRange` floor (near -65, room -75 default,
  wide -90; 127 "unknown" is dropped). `GlassesTracker` alarms once per new identifier and not again
  within `cooldown` (30 s) of the last alarm; it forgets a device after 120 s unseen. The app side
  is `GlassesWatcher` (`ForkMesh/Platform/GlassesWatcher.swift`): `CBCentralManager` scanning with
  no service filter and duplicates allowed. That only works in the foreground, so the screen turns
  the idle timer off while watching (`glassesAlert.keepAwake`). The alarm loops
  `desktop_power_alarm.wav` in the `.playback` category, so the ring switch does not mute it,
  with a looping Core Haptics pattern (`kSystemSoundID_Vibrate` without Core Haptics), a
  time-sensitive notification and a full-screen `GlassesAlarmView`, for 30 s. Prefs:
  `glassesAlert.range`, `glassesAlert.watching` (resume when the screen opens).
  `GlassesHoming` (`GlassesHoming.swift`) follows the strongest sighting and reports a
  `GlassesHomingReading`: smoothed RSSI, `closeness` (-95 dBm 0, -45 dBm 1), `trend` (last 3 s
  against 4–12 s ago, ±3 dB), and a `bearing`: samples are binned by compass heading into 12
  sectors, and the strongest sector, weighted with its neighbours, is where the glasses probably
  are (the body shades the signal from behind). It needs 3 sectors with 2 samples each and a 3 dB
  spread; `confidence` grows with spread and coverage. `heading(rotation:)` takes the horizontal
  direction of the phone's top plus its back; `rotation(up:magnetic:)` builds that matrix the way
  Android's `getRotationMatrix` does. `GlassesCensus` (`GlassesCensus.swift`) keeps the other
  devices heard (`NearbyKinds.kind` from name keywords, then company ID) for 45 s. It smooths
  RSSI and sorts strongest first so the compact three-column glass grid remains stable; each tiny
  tile shows kind, name, identifier, highlighted distance and colour-coded strength. The watcher
  feeds both, reads the heading from `CMMotionManager` device motion (`.xMagneticNorthZVertical`,
  gravity and calibrated field) only while something is followed. The device-kind glyphs
  (`bluetooth`, `headphones`, `lightbulb`…) and the screen's own icons were missing from
  `MaterialIconCodepoints`, so every tile drew `help_outline`; they are in the table now.
  `NearbyKind` has 25 kinds, each with its own icon: phone, tablet or e-reader, computer,
  headphones, hearing aid, speaker, TV, watch, other smart glasses, fitness sensor, health monitor,
  scale, keyboard/mouse/remote, game controller, tracker tag or beacon, car, scooter or e-bike, drone,
  camera, printer, smart lock, smart home, light, appliance, and plain Bluetooth.
  `NearbyKinds.kind` tries, in order: name keywords (specific product names such as `iphone` and
  `airpods` first, so an owner's name in "Sam's iPhone" cannot win), the GAP appearance category
  (Android only, because CoreBluetooth does not expose it), standard 16-bit service UUIDs (heart
  rate and cycling are fitness, HID is input, glucose and blood pressure are health, weight scale,
  LE Audio and ASHA/HAS hearing aids, Eddystone and Tile beacons, exposure notification is phone),
  then company IDs. The watcher passes listed and service-data UUIDs through `shortUuid`. A later
  nameless advert can upgrade a device that is still `.other`. `app/tests/test_native_glasses_icons.py`
  checks every kind's icon is in this table and in the Android font, and that both platforms list
  the same kinds.
  Tapping a tile opens `NearbyDeviceSheet`: `GlassesWatcher.track(_:)` points a second
  `GlassesHoming` (`trackState`) at that device, re-followed every homing tick with
  `seenAt` = now so a quiet device reads "Signal lost" rather than being dropped, and
  `tracked` drives a `GlassesHomingCard` (distance guess, closeness, arrow); `untrack()` runs
  when the sheet closes. While watching, `GlassesRadarChip` sits in `ShellTopBar` just right of
  the project logo, a 44 pt glass circle like it, and opens Glasses Alert. Suggestion mode's
  contact and channel chips and the pocket button follow it (docs/native/GENIE.md, "Suggestion
  mode in the top bar" and "Pocket mode"); `WalkActivityController` is `@Observable` so its
  `moving` flag drives the pocket row's walking figure. While the pedometer reports steps,
  `StepsWalkChip` sits right of the radar: the Pedometer's walking scene (`StepsWalkScene`,
  figure centred) scaled into a 34 pt circle (`StepsWalkBadge`) in the same 44 pt glass, and
  opens the Pedometer. The user asked for the walking person in a small circle up there. It replaced the
  floating `GlassesWatchPill` ("Watching, 48 other devices"), which covered page content: the
  user asked for a small radar with a dot per device instead. `GlassesRadarLayout`
  (`GlassesRadarLayout.swift`) places the dots: distance from the centre is `1 - closeness`
  (0.2–0.92 of the radius). BLE gives no direction for a passing device, so each one keeps a
  fixed angle from an FNV-1a hash of its identifier and only moves in and out. Glasses being
  followed with a compass bearing sit at their real arrow angle, relative to where the phone
  points. Glasses dots are larger and red, and the radar and its count badge (glasses plus
  census) turn red while glasses are near or the alarm rings. The sweep stops under Reduce Motion.
- Speech Playground (`SpeechModels.swift`, package `speech`, route `.speech`): text to speech on
  the device, no API. `SpeechPlaygroundModel(engine: SpeechEngine, prefs:, language:)`;
  `load()` reads the voices and, unless the saved `speech.voice` is still installed, picks
  `SpeechVoices.best` (natural before novelty/Eloquence, the phone's language before others, then
  Personal > Premium > Enhanced > Standard, exact region last). `toggle()` speaks, pauses and
  resumes; `stop()`, `preview(voice)` (a short line, tapping again stops), `select`,
  `useSample`, `clear`, `tuningChanged()`/`resetTuning()` (speed 0.5–2×, pitch 0.5–2, stored as
  `speech.speed`/`speech.pitch`), `requestPersonalVoice()`, `renderAudio() -> URL?`.
  `highlight: SpeechHighlight?` splits the spoken text around the word the engine reports
  (UTF-16 range; previews never highlight). `SpeechTuning.utteranceRate` maps speed onto
  `AVSpeechUtterance.rate` (0.5× → 0.25, 1× → 0.5, 2× → 0.75). The app's engine is
  `AppleSpeechEngine` (`ForkMesh/Platform/SpeechSynthesis.swift`): AVSpeechSynthesizer on a
  `.playback`/`.spokenAudio` session that ducks other audio and is released when speech ends
  (unless dictation is listening); stale finish/cancel callbacks of a replaced utterance are
  ignored by identity. `SpeechRenderJob` writes `synthesizer.write` buffers to an AAC `.m4a` in a
  fresh temporary folder for the share sheet. `FakeSpeechEngine` (`SpeechTests.swift`) fakes the
  engine. See `marketplace/packages/speech/README.md`.
- Steps walking scene: right under Today, `StepsScreen` shows a figure walking along a scrolling
  path (`StepsWalkScene`). Its speed follows `StepsModel.cadence`, the live steps per second.
  While the screen is open the cadence comes from the accelerometer in real time
  (`HealthBridge.startMotionSamples`, `CMMotionManager` at 50 Hz, no permission prompt; issue
  #1305): `StrideMeter` (`StrideMeter.swift`) takes the acceleration magnitude, removes gravity with
  a 1 s low-pass, smooths it, and counts a step at each peak above an adaptive threshold (45 % of
  the recent peak height, at least 0.08 g; the signal must dip below gravity between steps and steps
  are at least 0.25 s apart). Cadence is the rate over the last six steps (needs three); with no
  step for 2.5 step gaps (1–2 s) it drops to 0, so the figure stops as soon as you do. The meter also
  tells running from walking: running starts at 132+ steps/min with peaks of 1 g or more, or 156+
  steps/min with 0.6 g, and ends below 120 steps/min or 0.75 g (hysteresis, so a fast walk does not
  flicker). `StepsModel.running` then names the gait "Running" whatever the cadence, the figure
  switches to its running pose (`StepsWalkClock.run` eases toward 1), and without a running signal a
  fast cadence reads as a brisk walk. While the accelerometer is on, the pedometer's cadence is
  ignored. Without an accelerometer (or in tests that do not fake one) the cadence falls back to the
  pedometer's `currentCadence` (`HealthStepDay.cadence`, live updates only), else the step count's
  rise since the previous update when that came 1–12 s earlier; with no update for
  `cadenceStaleAfter` (6 s) it drops to 0, and 140+ steps/min reads as running. The figure eases to
  a standstill, after which the timeline pauses so a still screen draws nothing. Stride, knee lift,
  arm swing and lean grow with cadence, and the ground moves at the speed the planted foot travels,
  so feet do not slide. `paceLine` names the gait (under 90 steps/min strolling, under 115 walking,
  else brisk; running as above), the steps per minute, the speed (`speedText`, always mph whatever
  the profile's units): cadence times `strideM`, and, until the daily goal is met, the minutes left
  to reach it at the current pace (`minutesToGoal`: steps still to go over steps per minute, rounded
  up, e.g. "42 min to 10,000"). The stride is Apple's own calibration when the
  pedometer reports it (`1 / (currentPace × currentCadence)`, kept separately for walking and
  running), else today's distance over today's steps (0.72 m before 100 steps), times 1.35 when
  running. Reduce Motion shows the figure standing.
- Heart rate by tapping (`HeartTapModels.swift`, issue #1191): `StepsScreen`'s Heart rate card
  feeds `HeartTapModel(bridge: HeartRateBridge?, now:)`. `tap()` records the time; a pause over
  `HeartTap.resetAfter` (2.5 s) starts over, and only the last `maxIntervals` (12) intervals are
  kept. `HeartTap.bpm` needs `minTaps` (5) taps, drops intervals more than 30% off the median (a
  missed or doubled beat), averages the rest, and rejects rates outside 30–220. `save()` asks for
  Heart Rate write access (`authorizeHeartRate`) and writes one sample spanning the taps
  (`saveHeartRate`, `HKMetadataKeyWasUserEntered`); the reading is not sent to the account.
  `HeartRateBridge` is the protocol `HealthKitBridge` implements and `FakeHeartRate`
  (`HeartTapTests.swift`) fakes. The model's tests run on Linux; the card and the HealthKit write
  have not run on a device.
- Walks (`WalkDetector.swift`): while step syncing is on, the app's `WalkActivityController`
  (`ForkMesh/Features/Tools/WalkActivityController.swift`) feeds Core Motion activity samples
  (`HealthKitBridge.startWalkDetection`) into `WalkDetector`. Confident walking or running held for
  `startAfter` (30 s) starts a walk dated from when the motion began; being still, driving or
  cycling for `endAfter` (3 min) ends it at the moment it stopped, so a pause at a crossing does not
  split a walk. Low-confidence and unknown samples are ignored. A sample dated before the app
  resumed catches up at once, in either direction. On resume the controller drops a start that was
  only pending (`forgetPendingStart`), since its timer would otherwise fire before the fresh sample
  and show a walk that already stopped; the restarted updates redeliver the current activity with
  its own start date, so a walk still under way is not lost. `WalkProgress` holds the walk's own pedometer
  count (a second `CMPedometer` counting from the walk's start) and today's steps before it.
  The controller shows each walk as a Live Activity (`WalkActivityAttributes` in FMSurfaces,
  `ForkMeshWidgets/WalkLiveActivity.swift`): steps in the Dynamic Island's compact view, and steps,
  elapsed time, distance and today's total when expanded and on the Lock Screen. iOS only lets an
  app start a Live Activity from the foreground, so a walk detected while ForkMesh is suspended
  appears the next time it is opened; once shown, the count updates while the app runs and the
  activity goes stale after 5 minutes without an update. When HealthKit's hourly step delivery
  wakes the suspended app, `catchUp()` replays the Core Motion history since the walk began
  (`WalkDetector.replay`) and refreshes the count from the pedometer, so a walk that ended in
  the background ends its activity then rather than counting up until ForkMesh is next opened.
  It ends ten minutes after the walk does, showing the final count, and immediately when step
  syncing is turned off. While the app is in the foreground with step syncing on, the
  controller also keeps a live pace (`WalkPace`): a third `CMPedometer`
  (`HealthKitBridge.startPaceSteps`, counting from now) reports steps and the current cadence,
  falling back to steps over time when Core Motion gives no cadence. Below `minCadence`
  (0.5 steps/s) is shuffling, not walking, and the pace drops to zero after `staleAfter`
  (10 s) without an update. It publishes `cadence`, `running` and `stepping`; `moving` is
  `stepping || walking`, so the top bar's walking chip and the pocket lock screen react within
  a few seconds instead of waiting for `WalkDetector`. It stops when the app is paused or
  hidden (`appPaused`) and restarts on resume. Distance uses the
  Pedometer profile's units (`HealthPrefs.stepsImperial`, saved on every load), else the locale.
- Water: `WaterModel(api: WaterAPI, health:, prefs:, timeZone:, now:)` for Water Intake
  (`/api/water`); `add(milliliters:drink:source:)`, `addTyped(_:drink:)`, `delete(_:)`,
  `saveGoal`, `enableHealth`/`disableHealth`, `reconcileHealth()`; `presets`, `rows`, `bars`,
  `goalLine`, `summaryLine`; `WaterText` (ml, L, fl oz, cups).
- Sleep: `SleepModel(api: SleepAPI, bridge: SleepBridge?, prefs:, timeZone:, now:)` for the Sleep
  package (`/api/sleep`); `load(syncIfDue:)`, `install()` (turns tracking on), `enableTracking()`,
  `disableTracking()`, `setWritesHealth(_:)`, `syncNow(nights:)`, `addNight(night:bedAt:wakeAt:awakeMinutes:)`,
  `removeAddedNight(_:)`, `saveGoal(minutes:bedtime:wakeTime:)`; presentation `durationText`, `ringRatio`,
  `detailLine`, `sourceLine`, `stats`, `bars`, `goalRatio`; `SleepText` (durations, 12-hour clock).
  `SleepDetector` (`SleepDetector.swift`) is pure: `detect(night:rest:usage:schedule:timeZone:now:)`
  turns Core Motion activity (`SleepRestSample`) minus ForkMesh's own foreground time
  (`SleepUsageLog`) into a night; `healthNights` sums Apple Health sleep samples per night;
  `healthWrites` and `reconcile` plan the samples written back. `SleepSchedule` keys a time to the
  night it belongs to (the wake day; the boundary is five hours after the wake time).
  `SleepHealthSync.runIfDue` posts the last nights for both sources (`phone`, `health`) at most every
  15 minutes; `SleepBridge` is the protocol `HealthKitBridge` implements and `FakeSleep`
  (`SleepTests.swift`) fakes. Package README: `marketplace/packages/sleep/README.md`.
- Sugar Break (`SugarBreakModels.swift`, package `sugar-break`, route `.sugarBreak`, path segment
  `sugar-break`): `SugarBreakModel(api: SugarBreakAPI, health: SugarBridge?, prefs:, timeZone:, now:)`
  over `/api/sugar-break` (every read passes `?day=<local today>`); `load()`, `install()`,
  `startBreak(lengthDays:)` (default `selectedLength`, the profile's `defaultLengthDays`, 30, clamped
  to 1–365), `skip()` (the +1; counted at once through `pendingSkips` until the overview comes back),
  `slip()`, `undo(_:)`, `endEarly()`, `deleteBreak(_:)`, `uninstall()`. Presentation: `headline`
  ("Day N of L"), `progressRatio`, `progressLine`, `remainingLine`, `resultLine` (a finished or ended
  break), `todayLine`, `stats` (+1 total, sugar-free days, streak, best streak, days left), `days`
  (`SugarBreakDay`: status `sugar-free`/`slip`/`open`/`upcoming`, Health grams), `events` (today's,
  newest first), `history`, `lengths` (`SugarBreakText.length`: "1 month (30 days)"). Apple Health is
  read-only and off until `enableHealth()` (`health.sugarBreak.enabled`): `SugarBridge.sugarGrams(from:to:)`
  returns Dietary Sugar grams per local day over the break, shown beside each day, and
  `healthTodayLine` / `suggestsSlip` offer a one-tap slip when Health shows sugar today. `FakeSugar`
  (`SugarBreakTests.swift`) fakes the bridge. The screen's +1 button plays `FmHaptics.commit()`.
  `SugarContract` (`SugarContract.swift`) is the shareable no-sugar contract: `startDay`/`endDay`
  (`setStart`/`setEnd` keep end ≥ start), `signers` (clamped to 1–12, resizing `names`; names cut to
  40 characters), `dateLine` ("Oct 11 – Oct 31, 2026", one date for a single day), `lengthLine`,
  `terms` (one sentence, "I" for one signer, "We" for more), `fineprint`, `lines` (name or
  "Signature" under each line), `message` (the share text) and `fileName`.
  `SugarBreakModel.contract(signers:)` defaults it to today through the active break's end, or today
  plus `selectedLength`. The app's `SugarContractSheet` edits it and renders `SugarContractCard`
  (fixed light paper colours, 360 pt wide at 3x) to a PNG shared with `ShareLink`.
- Stretch Break (`StretchModels.swift`, package `stretch`, route `.stretch`, path segment `stretch`):
  `StretchModel(api: StretchAPI, health: StretchBridge?, prefs:, timeZone:, now:)` over `/api/stretch`
  (every read passes `?day=<local today>`). The overview and the routine library decode into Codable
  structs (`StretchOverview`, `StretchLibrary`, `StretchRoutine`, `StretchStep`, `StretchMotion`,
  `StretchFigure`) that fall back to defaults for missing fields. `StretchMath.pose(_:at:fields:)`
  interpolates a motion's frames with the smoothstep in the package README, and
  `StretchMath.body(_:pose:seconds:tap:)` returns the example person's joints and the draw-ordered
  `segments` in the 200 by 264 figure box; the screen only scales them into a SwiftUI `Canvas` inside a
  `TimelineView(.animation)`. `StretchRun` is the session state machine: `begin()`,
  `advance(by:now:)`, `pause(at:)`/`resume(at:)` (paused time is kept out of `activeSeconds` and
  recorded as `pauses`), `back()` (restarts the step after 3 seconds, otherwise goes to the previous
  one), `skip(now:)`, `stop(at:)`. It returns `StretchCue`s: the start line `"<title>. <say>"`, the
  `half` cue at the halfway mark of steps of 20 seconds or more, `"Next: <title>"` 3 seconds before the
  end of steps of 15 seconds or more with a following step, and the finish line. `shouldLog` is true when
  the routine completed or at least 30 active seconds were done; `logBody(day:health:)` is the
  `POST /api/stretch/sessions` body with `source: "ios"`. The model speaks cues through a
  `StretchSpeaker` (start and finish lines interrupt, the others queue; a cue raised while paused waits
  for resume), saves the voice switch with `PUT /profile {voice}`, and on logging saves a
  `StretchWorkout` through `StretchBridge` first when `health.stretch.enabled` is on, posting
  `health: true` only when Health accepted it. `HealthKitBridge` writes it with `HKWorkoutBuilder`
  (Flexibility, or Mind and Body for `lymph`, pause and resume events so the duration is the active
  time) and reads the last 7 days of Flexibility and Mind and Body workout minutes for `healthLine`. The
  screen's `StretchVoice` uses `AVSpeechSynthesizer` with the Speech package's chosen voice and speed
  (`SpeechPrefs`), or `SpeechVoices.best`, and keeps the screen awake while a session runs.
  `FakeStretchHealth` and `FakeStretchSpeaker` (`StretchTests.swift`) fake the bridge and the voice.
- Food Guide (`FoodGuideModels.swift`, `FoodCatalog.swift`, package `food-guide`, route `.foodGuide`,
  path segment `food-guide`; no server API): `FoodGuideModel(api: FoodGuideAPI?, health:
  NutritionBridge?, prefs:, recipes:, products:, timeZone:, now:)`. `load()` reads the Calorie
  Counter overview for the goal and today's total (`FoodGuideAPI.caloriesOverview`, any error means
  no Calorie Counter) and, with `health.foodGuide.enabled` on, 14 days of daily nutrient sums from
  `NutritionBridge.nutrientDays` (`HealthKitBridge` sums 14 `HKQuantityType`s with
  `dailySums`). `FoodNeeds.averages` keeps past days of at least 800 kcal and nutrients seen on 3 or
  more of them; `FoodNeed.isLow` is under 80% of the FDA daily value, `isHigh` a limit (sodium, sugar,
  saturated fat) over it. `FoodBudget` is the Calorie Counter goal or 2,000 kcal, eaten the larger of
  the two totals; `FoodMealPlan.plans` splits what is left over the meals still ahead (25/35/40,
  ending 10:30, 15:00, 21:00) and plans past ones for tomorrow. `FoodPlanner.suggest` filters
  recipes by `FoodDiet` flags, scores calorie fit, each gap's share of the meal's daily value, the
  `FoodGoal`, and a stable per-day jitter, and pages three per meal with no recipe in two meals
  (`moreIdeas` turns the page). Products are ALDI US items with USDA values per 100 g in
  `FoodNutrient.allCases` order; recipes are web pages with grams per serving. Goal and diets live in
  `foodGuide.goal` and `foodGuide.diets`. `FakeNutrition` (`FoodGuideTests.swift`) fakes Health.
- Apple Health (`HealthModels.swift`): `HealthBridge` is the protocol the app's
  `HealthKitBridge` (`ForkMesh/Platform/HealthKitBridge.swift`) implements and tests fake
  (`FakeHealth` in `HealthTests.swift`). `HealthReconcile.plan(desired:existing:)` turns the
  entries a client should have in Health and the samples ForkMesh wrote before (entry id and version
  in the sample metadata) into saves and deletions; `HealthReconcile.imports` picks other apps'
  samples not yet logged. `HealthDates.sampleDate` puts a sample at the entry's time on its day, else
  noon, never in the future. `StepsHealthSync.runIfDue` posts daily step totals at most every 15
  minutes unless forced; `HealthFeature` (app) calls it from the HealthKit observer query
  (background delivery, hourly) and on every return to the foreground. `HealthPrefs` names the
  per-device switches (`health.steps.enabled`, `health.water.enabled`, `health.calories.enabled`,
  `health.sleep.enabled`, `health.sleep.writeHealth`, `health.sugarBreak.enabled`, `health.stretch.enabled`, `health.foodGuide.enabled`).
  `CaloriesModel.attachHealth(_:prefs:)` / `enableHealth()` / `reconcileHealth()` write the last
  14 days of the diary as dietary energy and macros.
- Apple Health disclosure (`AppleHealthGuide.swift`): `AppleHealthGuide.uses` has one
  `AppleHealthUse` (`name`, `reads`, `writes`, `account`) per `AppleHealthTool`, with the badge,
  summary and promises text. The app's `AppleHealthDisclosure(tool:)` shows it in each tool's Apple
  Health card and `AppleHealthGuideScreen` shows all of it from Settings > Legal > Apple Health
  (`docs/native/IOS_RELEASE.md`, "App Review: Apple Health is named in the app").
- Bug reports: `ReportBugModel(screen:build:signedIn:api:)` (`submit() -> Bool`, `problem`,
  `attached`), `BugSeverity.all`, `BugButtonGeometry` (`resting`, `clamp`, `release`).

## Calories: Quick add, receipts and the prompt bar

Calorie Counter 2.3.0 lets the web Prompt dock log typed meals and adds a Quick add panel and
receipt reading (`marketplace/packages/calories/README.md`). The iOS app does the same.

- The overview carries `quickFoods` (My foods: at most 60, `source` `saved` or `receipt`) and
  `commonFoods` (the Worker's built-in list, with `category`). `CaloriesQuickFood(json:)` reads
  both; `json` is the entry body (name, brand, portion, calories, macros), `savedJSON` adds
  `source: "receipt"` so a re-saved list keeps receipt foods marked, `meta` is the row subtitle.
- `CaloriesQuick` is the pure port of the web page's parser: `meal`, `day` (yesterday, today,
  tonight, this morning), `weight` (kg, lb, or the profile's unit when none is typed), `singular`,
  `normalise`, `parts` (splits on `,` `;` newline `+` `&` and "and" after dropping meal, day and
  "I had" words), `match(text, foods:)` (amounts as numbers, fractions such as `1/2`, `3x`, or words
  such as "half a" and "a couple of"; macros scaled and rounded to 0.1, calories to whole numbers;
  nil when any part is unknown or an amount is not in (0, 20]), `frequent(entries:mine:)` (names
  logged more than once in the overview's whole-diary `entries`, not in My foods, top 8),
  `sections` and `filter`, and the sentences `loggedMessage`, `receiptMessage`, `receiptError`,
  `estimateError`. The fraction alternative is tried before the decimal one, so `1/2` is a half
  and not `1` followed by `/2` (the web page had that bug and was fixed the same way).
- `CaloriesModel` adds `myFoods`, `frequentFoods`, `commonFoods`, `quickSections`, `quickFilter`,
  `quickMeal` (time-of-day guess until changed), `quickHeader`, `isMyFood`, `quickAdd(food)` (one
  serving, `source: "quick"`, on the screen's day), `starFood` and `removeMyFood`
  (`PUT /quick-foods` with the whole list), `readReceipt(dataURL:)` (`POST /receipt`; 429, 422 and
  503 read as sentences), `receiptUnavailable`, and `logTyped(text) async throws -> String`: a
  weigh-in goes to `PUT /weights/{day}`, text that matches My foods, frequent or common foods posts
  one entry per part with `source: "quick"`, anything else goes to `POST /analyze` as a description
  and is logged with `source: "text"`; the meal comes from the text or the time of day, the day
  from the text or the screen's day, and the diary reloads. Failures throw `ShellPromptFailure`.
- `CaloriesPhoto.receiptDataURL(jpeg:)` and `receiptSides`: the app draws a receipt photo as a JPEG
  at most 1600 px on its long side (then 1280, 1024, 800) until the data URL fits the Worker's
  1.6 MB image limit.
- App: `CaloriesQuickAddSheet` (CaloriesSheets.swift) opens from the Today tab's Quick add button:
  meal picker, filter, Read receipt (PhotosPicker), then My foods (× removes), You log often and
  Common foods (☆ saves); a tap logs one serving. A sheet rather than an inline section because the
  common list is about thirty rows and would push the diary off screen. While the Calories screen
  is on screen and the package is installed, it registers a `ShellPromptTarget` (FMApi, see
  FMApiViewModels.md) so the shell's prompt bar logs typed text through `logTyped`, and removes it
  when the screen goes away.

## Docs: the public documentation library (`Docs/DocsLibrary.swift`, `Docs/DocsMarkdown.swift`)

The Docs package (`marketplace/packages/docs`, route `.docs`, path segment `docs`, package id
`docs`, App hub group next to People) shows on iPhone the same public pages forkmesh.com/docs
serves, and every dependency with its GitHub repository and top three authors. There is no server
API: the app ships `ForkMesh/Resources/DocsLibrary.json` (schema `forkmesh-docs-library-v1`,
written by `marketplace/tools/docs_mobile_bundle.py`; regenerate it there, never edit it by hand).
The model follows the desktop's `DocsLibrary.cpp` so the two behave the same.

- `DocsLibrary(data:) throws` (`DocsLibraryError`: `unreadable`, `wrongSchema`, `empty`) decodes
  leniently: a malformed entry is dropped, not the whole file. Pages keep the bundle's order; a
  path that is absolute, contains `..` or repeats is skipped. `areas` (the last is
  `dependencies`, which has no pages), `documentAreas`, `pages(in:)`, `page(path:)`,
  `previous`/`next`, `homePath`, `sourceURL(repositoryPath)`.
- Dependencies are sanitised as on desktop: `repository`, `profile` and `projects` survive only as
  `https://github.com/...` (profile and projects fall back to the login's GitHub pages), `website`
  only as http(s), logins must look like GitHub logins, at most three authors, `usedBy` only
  relative paths. `dependencyGroups` groups by ecosystem in order of first appearance with the
  desktop's labels (`ecosystemLabel`); `summary`, `versionLabel`, `authorsLabel`,
  `DocsAuthor.commitsLabel`/`projectsLabel` are the row texts.
- `search(query, limit:)` needs every whitespace token in the title, description, path or body
  and scores like the desktop (title phrase +200, title token +150 at the start or +100 elsewhere,
  description +25, path +15, body occurrences up to 10). The snippet is the first Markdown line with
  the phrase, else the first body token, without its leading `#>|*+-` or list number and inline
  markup, cut to 140 characters around the hit; the description stands in when there is none.
  `searchDependencies(query)` matches title, name, description, ecosystem, repository and author
  names and logins, exact and prefix title matches first.
- `resolve(from:href:) -> DocsTarget?` mirrors `DocsLibrary::resolve`: `#x` is `.anchor`; a
  relative link joined to the page's folder (`.`/`..` folded, query dropped, percent-decoded) is
  `.page` when it names a page or a folder whose `README.md` is one; `//host` and http(s)/mailto are
  `.external`, `/abs` is on `webOrigin`, a path under `www/public/` is the website file, and any
  other repository path opens the source tree. Other schemes resolve to nil. `linkURL(from:href:)`
  turns a target into the URL the view puts on a link (`fmdocs://page?path=..&fragment=..`,
  `fmdocs://anchor?fragment=..`, or the external URL) and `DocsLibrary.target(for:)` reads it back.
- `DocsMarkdown.blocks(markdown)` is a block parser: ATX and setext headings with GitHub anchors
  (`anchor(_:)`: lowercase, letters, digits, `_` and `-` kept, whitespace to `-`; repeats get `-1`,
  `-2`), paragraphs, list items with nesting depth from indentation and task boxes, quotes, fenced
  and indented code (nothing inside a fence is Markdown), pipe tables, rules; HTML comments and
  tag-only lines are dropped. `DocsInline.runs(text)` splits inline Markdown into `DocsRun`s (bold,
  italic, code, strike, link href) and drops HTML tags; `DocsMarkdown.plain` is the bare text.
  Inline parsing lives here rather than in `AttributedString(markdown:)` so it is tested on Linux
  and links can be rewritten before SwiftUI sees them.
- App (`ForkMesh/Features/Docs`): `DocsScreen()` loads the bundle once (`DocsBundle.shared`, off the
  main thread) and lists areas, or page hits and dependencies while searching; `DocsPageScreen`
  renders the blocks in a `ScrollViewReader`, intercepts link taps with an `OpenURLAction` (anchor
  scrolls, page pushes another page, external goes through `referenceOpenResult`), has Previous and
  Next at the foot (they replace the page in place) and Open on web/Share in the toolbar;
  `DocsDependenciesScreen` and `DocsDependencyScreen` show the dependency list and one dependency.
  Inline styles are `InlinePresentationIntent`, which Linux Foundation lacks, so `DocsText` is only
  built by Xcode.

## Surprises

- The vault's quick unlock uses `ForkMesh/Platform/LocalAuthVaultDeviceAuth.swift` (AppServices);
  `VaultPlatform` stays `NoVaultPlatform` (no autofill extension exists), so Vault settings show
  the "not supported" sentence.
- RSS comes from `mach_task_basic_info.resident_size` (Darwin) or /proc/self/statm (Linux).
- The vault screen's flow tests are FMVault's `VaultScreenFlowTests`; the bug report API tests
  belong to FMApi.
