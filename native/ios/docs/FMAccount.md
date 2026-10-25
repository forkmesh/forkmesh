# FMAccount

Settings, relays, auth and the session, performance monitor, stall log, trust lists, sync
status. `import FMAccount` (re-import `FMBase`/`FMNet`/`FMCrypto` for their types). Storage is
FMBase's synchronous `KeyValueStore` / `SecureStore`, and every call is synchronous.

Also here: the Kotlin app's SecurityBootstrap. Owner crypto and room seal live in FMCrypto
(docs/FMCryptoAdditions.md).

## Observation and listeners

`SettingsService`, `RelayDirectory`, `AuthService` are `@MainActor @Observable` classes. Getters
backed by preferences read an observable `revision` (bumped by every change), so SwiftUI
re-renders on any change. Code outside SwiftUI uses a listener surface:
`addListener { ... } -> ListenerToken`, `removeListener(token)` (callbacks are `@Sendable`, run on
the main actor; use `MainActor.assumeIsolated` inside). A callback that only starts a task writes
`Task { @MainActor in ... }` directly: a lone `Task { ... }` as the whole body of an
`assumeIsolated` closure becomes its generic result, which Swift 6.2+ rejects as ambiguous.
`PerformanceMonitorService` is the
exception: thread-safe (lock), not main-actor.

Static keys and pure static helpers (`SettingsService.canonicalServerUrl`, `AuthService.sessionKeyFor`,
`AuthService.messageFor`, `...Key` constants) are `nonisolated`: usable from any context.
`create`/`load`/instances need the main actor.

Test trap (Linux XCTest): a synchronous `@MainActor func testX()` crashes discovery ("Could not cast
value of type ... @MainActor"). Make main-actor test methods `async`.

## ServerURL (`ServerURL.swift`)

- `ServerURL.parse(settings.serverUrl)` (or `settings.server`): `scheme`, `host` (lower-case, IPv6
  without brackets), `port` (nil when absent or the scheme default; ws:80 keeps its port).
- `.http(path, query: [(k, v)])` -> `"https://host[:port]/path?q"` (`http` for a `ws` relay);
  `.httpURL(path, query:) -> URL?`; `.webSocket(path, query:)` (`wss`, or `ws` for ws/http);
  `.httpOrigin`.
- `DartURI.encodeComponent`, `encodeQueryComponent` (space -> +), `encodePath` (keeps valid %XX),
  `queryString([(k, v)])`, `decodePercent`. Path segments with user text: `"/api/accounts/\(DartURI.encodeComponent(name))"`.

## SettingsService (`SettingsService.swift`)

- Build: `SettingsService.create(prefs:secureStore:env:clock:)` (runs the startup migrations
  and loads the relay directory); `SettingsService(prefs:secureStore:relays:env:clock:)` skips them
  (tests). `env: AppEnv = .prod`.
- Relay: `serverUrl` (the ACTIVE relay's ws URL; everything derives from it), `server: ServerURL`,
  `relays: RelayDirectory`, `supports(RelayFeature) -> Bool`, `setServerUrl(_)` (compat path:
  adds/activates a relay, no handshake; empty resets), `resetServerUrl()`, `hasServerUrlOverride`,
  `defaultServerUrl`, `worldUri: URL?`. Static `canonicalServerUrl(_)`,
  `isRetiredLocalWorkerServerUrl(_)`, `migrateLocalWorkerServerUrl(prefs)`.
- Values: `displayName`, `room` (default "world-general"), `passphrase`,
  `githubToken`, `gitlabToken` + `set...(_)`, `clearSecrets()`.
- `themeMode: ThemeMode` (`.system/.light/.dark`), `accent: AccentChoice` (`.blue/.purple/.teal`,
  `.label`, `fromName`), setters. Raw values are the stored names. The app's own `FmAccent` maps
  with `FmAccent.fromName(settings.accent.rawValue)`.
- `serveAsNode` / `setServeAsNode`; `showSponsoredMessages`, `sponsoredMessagesHidden`,
  `hideSponsoredMessages()` (7 days); iOS UDID: `iosUdid`, `iosUdidProduct`, `iosUdidVersion`,
  `beginIosUdidRead() -> nonce`, `acceptIosUdidLink(IosUdidLink) -> Bool`.
- `legacyProject` (the per-project `ProjectLegacyMarker`, settled once by `create` or on first read:
  legacy when the project shows earlier use), `settleLegacyProject()`; `signInOn` / `setSignInOn`
  (the per-device Sign in package, default `legacyProject`); `aquariumOn`.
- Keys are `static let ...Key` constants ("server/url", "profile/name", "appearance/themeMode"...).
- `prefs` and `env` are public for sibling services.

## Relays (`RelayProfile.swift`, `RelayFeatures.swift`, `RelayDirectory.swift`)

- `RelayProfile` (value, equality by `id` = normalized origin `host[:port]`): `label`, `serverUrl`,
  `addedAt/lastUsedAt/verifiedAt: Int64`, `verifiedRev`, `builtIn`, `features`; `httpOrigin`,
  `host`, `isLoopback`, `subtitle`, `logo` (file name, "" for none), `hasInstance` (false for a
  project made from a name alone: `named(_:serverUrl:)`, id `local-<slug>-<random>`),
  `copyWith(...)`, `tryParse(input, label:)`,
  `normalizeOrigin(_) -> String?` (IPv6 refused), `fromJSON/toJSON`.
- `RelayFeature` (raw value = wire name; `core`, `label`, `fromWireName`, `coreFeatures`,
  `learnableFromPath(path)`), `RelayFeatureSet` (`.declared(set)`, `.undeclared(missing)`, `has`,
  `hasCore`, `missingCore`, `markMissing`, `isDeclared`, `fromJSON`, `fromVersionBody(JSONObject)`).
- `RelayDirectory` (keys `relays/list`, `relays/active`): `relays`, `active`, `activeId`,
  `activeServerUrl`, `activeFeatures`, `hasMultiple`, `byId`, `contains`, `add(_)` (never switches),
  `addNamed(_:logo:)` (instance-less, on the build's relay URL), `setLogo(id, logo)`,
  `useUrl(_)`, `rename`, `markFeatureMissing(id, feature)` (call on a 404 whose path
  `RelayFeature.learnableFromPath` maps), `markVerified(id, rev:, features:)`, `setActive(id)`,
  `remove(id) -> Bool` (not built-in, not active), `fallbackFor(id)`.

## AuthService (`AuthService.swift`, models in `AuthModels.swift`)

- Build: `AuthService.create(settings:identity:http:prefs:secureStore:performanceMonitor:deviceInfo:deviceLabelProvider:deviceTimestampProvider:clock:)`
  (migrates, loads every relay's session). `identity: DeviceIdentity?` (FMCrypto; nil -> login
  throws `identity_unavailable`). `prefs`/`secureStore` default to the settings' stores.
- State: `session: AuthSession?`, `isAuthenticated`, `isGuest`, `hasAppAccess`, `relayScope`
  (active relay id), `sessionForRelay(id)`, `isSignedInOnRelay(id)`, `signedInRelayIds`,
  `revokedBannerPending`, `signupUrl`.
- Actions: `login(identifier:password:totp:) async throws -> AuthSession` (mobile-node bind),
  `logout() async`, `handleUnauthorized(code:)` (call from the API layer on a Bearer 401 with the
  Worker's `error`), `checkNodeName(_) async -> AccountAvailability`, `refreshAvatar() async`,
  `authenticatePreview()`, `deleteAccount(password:) async throws`, `enterGuestMode()`,
  `exitGuestMode()`, `clearRevokedBanner()`, `adoptActiveRelay()` (after a relay switch),
  `forgetRelay(id)`.
- Errors: `AuthException(code, message)` (`.code`, `.message`; `AuthService.messageFor(code)`);
  network failures surface as FMNet `HTTPError`.
- Models: `AuthSession` (`sessionToken` is the Bearer token; `canOwnerSign`,
  `isMobileNode`, `desktopDevices`, `mobileDevices`, `thisDevice`, `fromJSON(_, now:)`, `toJSON`),
  `AccountDevice` (`fromJSON` accepts snake_case and CSV capabilities; `displayName`, `kindLabel`,
  `statusLabel`, `capabilitySummary`, `capabilityLabel`, `shortId`, `isRevoked`),
  `AccountSession.fromJSON` (GET /api/accounts/sessions rows), `AccountAvailability`.
- `DeviceInfo(platform:modelName:model:systemName:systemVersion:vendorId:)`: the app target fills it
  from UIDevice; `.label` is the default device label ("iPhone 15 Pro · iOS 19.0").
- Keys: `auth/session/<relayId>` (secure), `auth/guestMode/<relayId>`, `auth/revokedBanner/<relayId>`.

## PerformanceMonitorService (`PerformanceMonitor.swift`)

- `init(enabled:threshold:stallThreshold:heartbeatInterval:maxEvents:logSink:stallLog:now:elapsedForTest:heartbeatQueue:)`;
  durations are Swift `Duration` (`.milliseconds(500)`).
- `track(name, details: JSONObject) { async work } async rethrows -> T` (records slow operations,
  adds `error` on throw), `timeSync(name) { sync work }`, `recordFrameDelay(...)`,
  `recordSlowEvent(...)`, `clear()`.
- `events: [PerformanceEvent]` (newest first; `kind`, `name`, `duration`, `durationMs`,
  `timestamp`, `details`, `stackTrace`, `isStall`), `stallCount`, `stallLogPath`, `onHeartbeatLag`.
- The app feeds it: `installStallMonitors(frameSource: FrameTimingSource?, lifecycleState:)`,
  `appLifecycleChanged(AppLifecycleState)`, `uninstallStallMonitors()`. `FrameTimingSource` is a
  protocol (`startReporting(handler)`, `stopReporting()`) delivering `[FrameTiming]`.
  Frame timings are dropped while the heartbeat is stopped and when their span reaches back
  before the heartbeat last started, so time in the background is never a late frame.
- `FrameTiming.lateCallback(interval:period:)` turns two display-link timestamps into a frame
  timing: nothing unless the gap is over two periods, otherwise `buildDuration` = gap minus one
  period and `vsyncOverhead` = the period (logged as `vsyncMs`). The app's `DisplayLinkFrameSource`
  ticks at 10 Hz, so it used to report the whole gap: one skipped tick after a 100-120 ms hold of
  the main thread read as a "200 ms" stall and fed the "dropped-frame hitches" alert (the
  1.4.1 (255) report from an iPhone 15 Pro Max was all `frame 200-221ms buildMs=100-121`). Only
  the time past the expected tick counts now, so a stall means the main thread really missed
  `stallThreshold`.
- `track` keeps only the caller's return addresses (`Thread.callStackReturnAddresses`) while the
  operation runs and symbolicates them (`dladdr`, Darwin) only when it turns out slow. It used to
  call `Thread.callStackSymbols` on the main actor for every API request, fast or not, and a
  resume fires a dozen or more at once.
- `StallLogFile.forDevice(build:)` (existing) is the on-disk log. `tail(maxBytes:) async` returns the
  end of `stalls.log.1` followed by `stalls.log`, starting on a whole line, read on the log's own
  queue; it reads earlier launches too and never creates the file.

## SecurityBootstrap (`SecurityBootstrap.swift`)

`SecurityBootstrap.load(secureStore:prefs:)` -> `.identity: DeviceIdentity?` (FMCrypto `Identity`,
seed migrated off prefs) and `.ownerKeys: OwnerKeys?` (`OwnerCrypto`); a corrupt device key logs
and stays nil instead of crashing. `.register()` puts both in `Services`. Call off the main thread
at startup, right after `SettingsService.create`, then build `AuthService` with `.identity`.

## FreshInstallReset (`FreshInstallReset.swift`)

Deleting the app from an iPhone removes its UserDefaults and files, but iOS
keeps its Keychain items. So that a reinstall asks the user to sign in again,
the first launch of a new install erases those leftovers.

`FreshInstallReset(prefs:eraseKeychain:).run()` is the first step
of `AppServices.start`, before anything reads
the Keychain (`StartupMaintenance.eraseLeftoversOfADeletedApp`, with
`KeychainBackend.eraseAll`, which deletes every item of every class in all of
the app's access groups: the session, identity, owner keys, vault copy, push
credentials and share drop).

- `install/installed` set: `.alreadyInstalled`, nothing happens.
- No marker, but the app's `fm.` preferences hold anything: an update, which
  keeps its login. The marker is
  set: `.upgraded`.
- No marker and no preferences: a new install. `install/erasePending` is set,
  the Keychain is erased, then the marker replaces it: `.erased`. If the erase
  fails (the phone still locked after a reboot), it returns `.deferred`, and
  the pending flag makes the next launch erase again, even though the
  preferences are no longer empty.

## PermissionPacing (`PermissionPacing.swift`)

Device-wide keys that keep a fresh install free of permission prompts
(`docs/native/PERMISSIONS.md`). `settle(prefs, freshInstall:)` runs once at boot
(`SettingsService.settlePermissionPacing`, fresh when the active project still
needs its starter pack): a fresh install stores `permissions/intro` = `pending`;
an update stores `seen` and `permissions/nearby-opened` = true, so nothing
changes for people who already answered the old launch prompts. Later calls
keep the first answer. `SettingsService.needsPermissionsIntro` and
`finishPermissionsIntro` drive the intro screen; `reasons`, `title`, `summary`
and `footnote` are its text. `nearbyAllowed` / `markNearbyOpened` drive the
mesh hold.

## Sync status and trust lists (conformance ports; pure functions over `JSONObject`)

- `SyncStatus.evaluate(evidence) -> JSONObject` (forkmesh-sync-status-v1: `state`, `label`,
  `severity`, `current`, counts, `lastConfirmedAt`, `lagMs`, `error`, `action`, `summary`, in a
  fixed key order), `SyncStatus.mirrorsPayloadEvidence(payload)`, `SyncStatus.mirrorsPayloadStatus(payload)`
  for `GET /api/repo/<o>/<r>/mirrors`.
- `TrustLists.resolve(documents: [String], now:, follows:, overrides:) -> JSONObject`
  (forkmesh-trust-resolution-v1), `TrustLists.verifyDocument(raw) throws -> JSONObject`
  (`TrustListError.code`), `canonicalJson`, `documentId(raw)` / `documentId(utf16:)` (lone
  surrogates, which a Swift String cannot hold), `listId(issuer, name)`. Both are pinned to
  `conformance/*/v1` (all 24 trust-list vectors, both input orders).

## FallbackSecureStore (`FallbackSecureStore.swift`)

`FallbackSecureStore(backend: ThrowingSecureBackend)`
is a `SecureStore` that moves reads/writes to memory after the first platform failure
(`isUsingFallback`), while deletes keep reaching the platform store. The app's `KeychainStore`
swallows errors today; wrapping a throwing Keychain backend in this gives the fallback behaviour.

## Other

- `IosUdidLink.tryParse(URL|String)`, `newNonce()`, `pageUri(serverUrl, nonce) -> String`,
  `IosUdidLink.scheme/host/pagePath`.
- `LegalLinks.terms`, `.privacy`, `.appleHealth` (the privacy policy's `#apple-health` section) (URLs; open with `openURL`).

## Not here

RelayProbe (`/api/version` handshake) and RelaySwitchController are not ported in this module;
`RelayFeatureSet.fromVersionBody`, `RelayDirectory.add/markVerified/setActive` and
`AuthService.adoptActiveRelay/forgetRelay` are the pieces they call. The account screens' view
logic (Kotlin `account/*Logic.kt`) belongs with the Account feature views.
