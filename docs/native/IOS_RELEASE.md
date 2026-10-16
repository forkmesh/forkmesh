# Native iOS release pipeline

The App Store app **ForkMesh** (`com.forkmesh.forkmesh`) ships from
`native/ios`. This page covers the Codemagic workflows in `codemagic.yaml`,
the helper scripts in `native/ios/tools/` and `tools/`, the store rules the
app must follow, and the Flutter-to-native migration. Building on the office
Mac instead of Codemagic is in [MAC_BUILD.md](MAC_BUILD.md).

## Version

`native/ios/Config/Version.xcconfig` holds `MARKETING_VERSION` and
`CURRENT_PROJECT_VERSION` for every target (an extension or watch app whose
version differs from the app's is rejected at upload).

- `ios-native-release` builds with one past the highest build number App Store
  Connect holds for the app, or the xcconfig's number if higher, written into
  its clone only. `asc_bundle_ids.py latest-build` reads the highest numeric
  `version` among the 200 most recent builds.
- `ios-native-adhoc` also goes one past the last ad hoc build, counted on the
  build Mac in `~/ci/ios-adhoc-build` (`FORKMESH_ADHOC_BUILD_FILE`).
- A failed upload spends its build number.

## `ios-native-release`

Started by hand or through the Codemagic API. Variable group
`app_store_connect`: `ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_API_KEY_P8`.

1. **Register the bundle ids** (`tools/asc_bundle_ids.py ensure`). Increased
   Memory Limit is not in the API's capability list; the step prints a reminder
   to tick it once in the developer portal.
2. **Pick the next build number** (`FORKMESH_IOS_VERSION`, `FORKMESH_IOS_BUILD`).
3. Generate the project, fetch the watchOS platform, resolve Swift packages,
   run the ForkMeshCore tests.
4. **Archive** per `FORKMESH_NATIVE_SIGNING`:
   - `manual` (default): unsigned archive, entitlements stamped with `codesign`
     (`*native_embed_entitlements_manual`); the export signs. Mints no
     development certificate.
   - `xcode`: Xcode automatic signing with the API key
     (`-allowProvisioningUpdates`), archived with
     `FORKMESH_APP_ENTITLEMENTS=Debug` because a development profile refuses
     `aps-environment=production`. Each run on a fresh machine mints a
     development certificate, and the account has a cap.
5. **Export for the App Store** with cloud signing (`Config/ExportOptions.plist`).
6. Checks: push entitlements and the notification extension (`*push_check`),
   widgets and watch app keychain groups (`*native_watch_check`), every
   bundle's profile and entitlements (`*native_show_profiles`).
7. **Upload** with `altool`.
8. **TestFlight internal testing** (`tools/asc_testflight.py internal`): waits
   for processing (VALID), sets What to Test, adds the build to every internal
   group without "access to all builds", checks the internal state.
9. **External TestFlight** only with `FORKMESH_IOS_TESTFLIGHT_EXTERNAL=1`. Beta
   App Review fails with `BETA_CONTRACT_MISSING` until the Account Holder
   accepts the pending agreement in App Store Connect.
10. **App Store submission** only with `FORKMESH_IOS_APP_STORE=1`.

| Variable | Default | Effect |
|---|---|---|
| `FORKMESH_NATIVE_WATCH` | `1` | `0` leaves the watch app out |
| `FORKMESH_NATIVE_WIDGETS` | `1` | `0` leaves widgets and Live Activities out |
| `FORKMESH_NATIVE_SHARE` | `1` | `0` leaves the share extension out |
| `FORKMESH_NATIVE_AUTOFILL` | `1` | `0` leaves the AutoFill credential provider out |
| `FORKMESH_NATIVE_SIGNING` | `manual` | `xcode` = automatic signing at archive |
| `FORKMESH_IOS_TESTFLIGHT_EXTERNAL` | `0` | `1` also submits for Beta App Review |
| `FORKMESH_IOS_APP_STORE` | `0` | `1` picks an acceptable version and submits for App Review |
| `FORKMESH_IOS_WHATS_NEW` | `TESTFLIGHT_WHATS_NEW` | App Store What's New text |
| `FORKMESH_IOS_UPLOAD_DRY_RUN` | `0` | `1` validates and uploads nothing |
| `FORKMESH_NATIVE_CORE_TESTS` | `1` | `0` skips the macOS ForkMeshCore step |
| `TESTFLIGHT_INTERNAL_GROUPS` | all internal | comma-separated group names |
| `FORKMESH_TESTFLIGHT_WAIT_MINUTES` | `60` | processing wait |

`tools/asc_testflight.py status VERSION BUILD` prints a build's state. When a
build stays `NOT_FOUND` after upload, read the upload record
(`GET /v1/apps/<app>/buildUploads`) before waiting longer: a server-side
validation failure appears only there.

## Store configuration guard

`ForkMeshCore/Tests/FMApiTests/StoreConfigTests.swift` checks the committed
project files on Linux so store mistakes fail in `swift test`:

- The `ForkMesh` target (selected by bundle id) has exactly Debug and Release;
  Release signs with `ForkMesh-Release.entitlements` (`aps-environment`
  production), Debug with `ForkMesh-Debug.entitlements`.
- The app and `NotificationService` list the same keychain access group. The
  extension reads the session token and the chat device key
  (`chat/e2ee/x25519PrivateV2`, service `com.forkmesh.forkmesh.secure`) from it
  to open chat pushes, so the app's items must land in that shared group.
- `NotificationService` links only the `ForkMeshPushCore` product (FMBase,
  FMCrypto, FMPush).
- `APNS_BUNDLE_ID` in `app/src/apns.py` matches the app's bundle id.
- `ForkMesh-Info.plist` answers `ITSAppUsesNonExemptEncryption`.
- On iPhone the app is portrait only; the `~ipad` list keeps all four
  orientations, which iPad multitasking requires.
- No `PrivacyInfo.xcprivacy` is named in the project file (synchronized groups
  include them). Each manifest declares every required-reason API category its
  bundle's sources use (for example DiskSpace for `LocalModelStore`). The share,
  AutoFill, widget and watch extensions have their own manifests.
- No `IntentDescription` literal contains "iphone", "ipad" or "apple watch"
  (App Store validation error 90626).
- Widgets, the watch app and AutoFill ship without an App Group.

## App Store rollout for a tagged release

`tools/forkmesh-release-host.py` starts `ios-native-release` for the newest
release candidate, on the office Mac when it answers ([MAC_BUILD.md](MAC_BUILD.md))
and on Codemagic otherwise ([`docs/release/README.md`](../release/README.md)).
It sets `FORKMESH_IOS_APP_STORE=1` and passes the annotated tag's message as
`FORKMESH_IOS_WHATS_NEW` and `TESTFLIGHT_WHATS_NEW`.

- **Marketing version**: `tools/asc_appstore.py version <xcconfig version>`
  returns the xcconfig's version while App Store Connect has not released it,
  otherwise one patch past the highest released or approved version (a version
  string cannot be reused after approval). Written into the clone only.
- **Submit** (`tools/asc_appstore.py submit VERSION BUILD WHATS_NEW`) waits for
  VALID, then prepares the one App Store version that may be in flight: creates
  one if none exists, cancels a waiting or in-review submission so the newest
  release replaces it, or renames an editable version. It sets release type
  "after approval", attaches the build, writes What's New into every
  localization, writes the App Review notes (`REVIEW_NOTES`,
  `HEALTH_REVIEW_NOTES`), and submits, reusing an open review submission.

To submit `main` without a tag, run
`.forkmesh/shortcuts/deploy-native-ios-app-store-mac.sh`
(`tools/mac_adhoc_deploy.py --app-store --execute`; see [MAC_BUILD.md](MAC_BUILD.md)).

Releasing a version locks every older install out until it updates: the app
compares itself with the App Store's current version on launch and shows a
blocking update screen ([NAVIGATION.md](NAVIGATION.md), "Forced update").

Rejections and missing metadata fail the submit step with App Store Connect's
error; the release host retries after an hour. `asc_appstore.py status` shows
versions and submissions; `asc_appstore.py plan VERSION` is a dry run.

## Store status shortcut

`.forkmesh/shortcuts/ios-app-store-status.sh` runs
`tools/mac_adhoc_deploy.py --store-status` (read-only) and prints:

- the four newest App Store versions (build, state, release type);
- the four newest App Review submissions, marking states that need action (the
  API does not return the reviewer's message text);
- downloads, redownloads and updates from the Sales and Trends summary reports
  (`GET /v1/salesReports`), which need `ASC_VENDOR_NUMBER` and a key with Sales
  access;
- the three newest TestFlight builds and every TestFlight group;
- the newest TestFlight feedback and App Store reviews (`--feedback N`).

Credentials: `ASC_KEY_ID` and `ASC_ISSUER_ID` from the environment, the
desktop's Actions variables, or the office Mac; the private key at
`~/.appstoreconnect/private_keys/AuthKey_<ASC_KEY_ID>.p8`.

The shortcut also publishes the same data (`asc_store_report.py --publish`) to
the **App Store Status** package page, so the web, desktop, iPhone and Android
show it laid out as a page. `--json` prints that snapshot instead of the text.
It also sends daily downloads (with countries and devices), the versions and
the reviews to the Marketing package's App Store channel, which charts
downloads over time; `--marketing ORG` (or `FORKMESH_MARKETING_ORG`) names the
organization when more than one has Marketing installed.
See [../marketplace/app-store-status.md](../marketplace/app-store-status.md).

## TestFlight feedback to tasks

`.forkmesh/shortcuts/testflight-feedback-to-tasks.sh` runs
`tools/testflight_feedback_tasks.py`:

- Each screenshot or crash feedback without a task gets one (unassigned,
  "TestFlight: <comment>", with build, device and date, screenshots attached).
  The last details line `TestFlight feedback id: <screenshot|crash> <id>`
  prevents duplicates.
- When that task is completed, the next run deletes the feedback in App Store
  Connect (the only state change the API allows). A 404 counts as done.
- Screenshots are downloaded (Apple's links expire) and re-encoded as JPEG to
  fit the 64 KiB task request cap (`BODY_MAX_BYTES`); needs Pillow.
- Tasks are created through `task_request` in `tools/forkmesh_mcp_server.py`,
  signed with the node identity. `--dry-run` and `--repository OWNER/NAME` are
  supported.

## App Review: nothing paid on iOS

Under guidelines 3.1.1 and 3.1.3(b) an iOS app may only show or unlock
something bought elsewhere if the same thing is sold by In-App Purchase, and
the app sells nothing. Memberships are sold on the web
(`docs/app/payments-and-plans.md`). Rule for the iOS app:

- no membership, tier, price, plan or paid/free label;
- nothing a membership pays for is unlocked or managed (the Ads screen is the
  review queue only, shown to ad reviewers);
- no button that sends or requests money (no tips, treasury or wallet; no
  `wallet.` control family);
- no link to pages that list prices (the app links the privacy policy, not the
  docs library);
- credits and tip ping kinds are hidden (`PingKindGroup.hiddenKinds`);
- the store listing never mentions membership or tips.

Allowed: showing sponsored messages, read-only bounty and funding records,
the user's own usage and costs with other providers, and notification
preferences. Before adding a screen, check it shows no price, plan or tier,
does not depend on `role_grants` of scope `platform`, and moves no money.
Android and the desktop are not bound by this.

## App Review: Apple Health is named in the app

The app uses HealthKit (not CareKit) in seven optional Market tools:
Pedometer, Water Intake, Calorie Counter, Sleep, Sugar Break, Stretch Break and
Food Guide (`ForkMesh/Platform/HealthKitBridge.swift`). Guideline 2.5.1
requires the UI to identify it:

- Each tool's Apple Health card ends with `AppleHealthDisclosure`
  (`ForkMesh/Features/Tools/AppleHealthViews.swift`): a "Works with Apple
  Health" mark, what the tool reads and writes, and a guide button.
- Settings > Legal > Apple Health opens `AppleHealthGuideScreen` with no tool
  installed and links `/privacy#apple-health` (`LegalLinks.appleHealth`).
- The text is `AppleHealthGuide` in
  `ForkMeshCore/Sources/FMTools/AppleHealthGuide.swift`, one `AppleHealthUse`
  per `AppleHealthTool`; `AppleHealthGuideTests` fails on a missing entry. Keep
  it in sync with `www/docs/mobile/apple-health.md`,
  `www/templates/pages/privacy.html` and the usage strings in
  `Config/ForkMesh-Info.plist`.

## AutoFill

`ForkMeshAutoFill` (`com.forkmesh.forkmesh.AutoFill`) fills vault logins in
Safari and other apps, inserts a username or password into any field, and is a
Credential Exchange import destination for Apple Passwords.

- The sealed cache, its key and the app links live in the keychain group
  `$(AppIdentifierPrefix)com.forkmesh.forkmesh`, so no App Group is needed.
- `asc_bundle_ids.py ensure` registers the extension with
  `AUTOFILL_CREDENTIAL_PROVIDER` and enables the same capability on the app
  (which uses `ASCredentialIdentityStore` and `ASSettingsHelper`).
- `Config/ForkMesh-Info.plist` lists `ASCredentialExchangeActivityType` under
  `NSUserActivityTypes`; the extension declares `SupportsCredentialExchange`
  and `SupportedCredentialExchangeVersions` (`1.0`). The app receives the
  import token as an `NSUserActivity` (`VaultCredentialExchange`).
- Credential Exchange code is under `#if compiler(>=6.2)` and `.app` service
  identifiers under `#if compiler(>=6.3)`, so older Xcode builds the rest.

## Widgets, Live Activities and the watch

The Home Screen widgets, Live Activities (one summary of agents, shortcuts and
the timer, and walks; drawn by the widget extension) and the watch app ship in
every build. The App Store
Connect API cannot create or assign App Groups, so they share data through
keychain groups, which every profile allows:

- The app writes the widget snapshot and the widgets leave pending taps in
  `742ZANCB57.com.forkmesh.forkmesh`, service `com.forkmesh.forkmesh.surfaces`
  (`SurfaceContainer.sharedItems()`, `KeychainSharedItemStore` in FMBase),
  readable after first unlock.
- The watch app leaves its complication summary in
  `742ZANCB57.com.forkmesh.forkmesh.watchkitapp` (`WatchGroup.shared()`).
- `*native_watch_check` fails the build if an exported widget or watch bundle
  lacks its keychain group.

A watch installs the app from the iPhone's Watch app, or for an ad hoc build
only once the watch is a registered device.

## `ios-native-adhoc`

The same archive and checks, exported `release-testing` for the team's
registered devices. It installs over a TestFlight or App Store copy, so it is
how to test an upgrade on a phone. Register devices with:

```sh
cd native/ios
ASC_KEY_ID=… ASC_ISSUER_ID=… python3 tools/asc_bundle_ids.py register-device "Name" <UDID>
python3 tools/asc_bundle_ids.py devices     # id, class, status, UDID, name
python3 tools/asc_bundle_ids.py has-watch   # exit 0 when an enabled watch is registered
```

An ad hoc profile covers only devices registered when it is minted, so build
again afterwards. A watch lists as platform IOS, class APPLE_WATCH.

## App icon

`tools/render_logo.py` draws the app icon from `logo.svg` into every file in
`AppIcon.appiconset/Contents.json` and the 1024 px watch icon, rendering each
size at its own pixel size, with no alpha channel (required for the 1024 px
icon). Run it after changing `logo.svg` (needs pycairo and Pillow);
`--preview out.png` renders one icon. The PNGs are committed. The in-app
`Logo.imageset` holds `logo.svg` with `preserves-vector-representation`.

## Flutter-to-native migration

`FMAccount/FlutterMigration.swift`, run by
`ForkMesh/App/FlutterMigrationRunner.swift` from `AppServices.start` before any
store is built. Tests: `FMAccountTests/FlutterMigrationTests.swift`.

Flutter left secrets in flutter_secure_storage (generic-password items,
service `flutter_secure_storage_service`, account = key,
`kSecAttrAccessibleWhenUnlocked`, group `742ZANCB57.com.forkmesh.forkmesh`) and
preferences as `UserDefaults` keys `flutter.<key>`. Key names are identical to
the native ones, so the migration copies each key into the Keychain service
`com.forkmesh.forkmesh.secure` and `fm.<key>` preferences.

- **Active project.** When none is named, `relays/active` is set to
  `app.forkmesh.com`, so the upgraded user stays signed in.
- **Idempotent.** `migration/flutter/done` stops later runs. Until then each
  launch retries and a Flutter value wins over a native one (a native value
  may come from a background launch that could not read the locked keychain).
- **Deferred, not guessed.** An unreadable Flutter keychain or a failed write
  returns `deferred` with nothing marked.
- **Verified.** Every copied value is read back; counts and keys are kept
  under `migration/flutter/`.
- **Kept until verified.** Nothing is deleted until
  `FlutterMigrationRunner.retireWhenVerified` sees the relay accept the
  migrated session (`GET /api/accounts/sessions`); then the copied Flutter
  items and preferences are deleted. Failed deletes retry.
- **Files** need no move: the container paths are the same, and the
  repository index's absolute paths are rebased on load.
- **Push** credential items keep the Flutter names (service
  `com.forkmesh.forkmesh.push`, items `session` and `session@<host>`).

## Open items

- External TestFlight needs the Account Holder to accept the pending
  agreement.
- Remove `com.forkmesh.nativeapp` from the server's APNs allowlist
  (`APNS_NATIVE_BUNDLE_ID` in `app/src/apns.py`) once no token uses it, and
  delete the unused `com.forkmesh.nativeapp*` App IDs.
- App Store Connect metadata: watch screenshots and privacy labels for photos,
  microphone and speech.
