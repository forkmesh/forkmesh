# Native iOS release pipeline

The App Store app **ForkMesh** (`com.forkmesh.forkmesh`, App Store Connect app
6803060735) ships from `native/ios` since 1.4.0. This page explains the
Codemagic workflows in `codemagic.yaml`, the helper scripts in
`native/ios/tools/`, and the Flutter-to-native migration. The source files
carry no comments; this is where the reasons live.

## Version

`native/ios/Config/Version.xcconfig` holds `MARKETING_VERSION` and
`CURRENT_PROJECT_VERSION` for every target (an extension or watch app whose
version differs from its app's is rejected at upload).

- 1.4.0 was chosen above every version the retired Flutter app used: App
  Store Connect's highest Flutter build is 1.2.3 (16), the last Flutter source
  said 1.3.1+16, and the 1.1.0 train is closed (1.1.0 is live on the App
  Store; an App Store version 1.2.1 is waiting for review).
- 2026-10-09: the xcconfig moved to 1.4.1 for the next App Store version
  after 1.4.0.
- `ios-native-release` builds with one past the highest build number App
  Store Connect holds for the app (Flutter builds included), or the xcconfig's
  number if that is higher, and writes it into its clone only.
- `ios-native-adhoc` does the same and also goes one past the last ad hoc
  build, which the build Mac counts in `~/ci/ios-adhoc-build`
  (`FORKMESH_ADHOC_BUILD_FILE`). Ad hoc builds never reach App Store Connect,
  so without that counter every one of them was build 17 (the xcconfig's
  number) until 2026-09-28. On Codemagic, which keeps no counter, repeated
  ad hoc builds still share App Store Connect's number plus one.
  `asc_bundle_ids.py latest-build` reads that number from the API; it takes
  the highest numeric `version` among the app's 200 most recently uploaded
  builds.

## `ios-native-release`

Started by hand (or through the Codemagic API). Variable group
`app_store_connect`: `ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_API_KEY_P8`.

1. **Register the bundle ids** (`tools/asc_bundle_ids.py ensure`): the app,
   `.NotificationService` (both first registered for the Flutter app; Push on
   the app),
   `.watchkitapp` and `.watchkitapp.widgets` (APP_GROUPS on). Increased
   Memory Limit is not in the API's capability list and the app does not
   carry it for now (`native/ios/docs/FMLocalAI.md`); listed again, the step
   prints a reminder to tick it once in the developer portal.
2. **Pick the next build number**, exported as `FORKMESH_IOS_VERSION` and
   `FORKMESH_IOS_BUILD` for later steps.
3. Generate the project, fetch the watchOS platform, resolve the Swift
   packages, run the ForkMeshCore tests.
4. **Archive** (`*native_archive_signed`), per `FORKMESH_NATIVE_SIGNING`:
   - `manual` (default): an unsigned archive, entitlements stamped on with
     `codesign` by the next step (`*native_embed_entitlements_manual`); the
     export signs. Mints no development certificate.
   - `xcode`: Xcode automatic signing through the App Store Connect key
     (`-allowProvisioningUpdates`, the key in a 0600 temp file). The archive
     is development-signed, and a development profile refuses
     `aps-environment=production`, so the app is archived with
     `FORKMESH_APP_ENTITLEMENTS=Debug` (the generator's switch between
     `Config/ForkMesh-Debug.entitlements` and `-Release`); the App Store export
     re-signs with the distribution profile. **Measured 2026-09-24 (build
     6ab4e02cfdb508134c7c5d4f):** it does *not* register the App Group. The
     app and extension provisioned, but both watch targets failed with
     "Authentication failed: Make sure a bearer token was provided" and "No
     profiles for 'com.forkmesh.forkmesh.watchkitapp'": registering or
     assigning an App Group is not open to API-key authentication, in Xcode
     either. The run also minted a development certificate ("Created via
     API", VUNTA38MKW); every `xcode` run on a fresh Codemagic machine mints
     another, and the account has a cap.
5. **Export for the App Store** with cloud signing (`Config/ExportOptions.plist`).
6. Checks: push entitlements and the notification extension (`*push_check`),
   the Home Screen widgets and the watch app, each signed with the keychain
   group it shares its data through (`*native_watch_check`), then every
   bundle's profile and entitlements printed (`*native_show_profiles`).
7. **Upload** with `altool`.
8. **TestFlight internal testing** (`tools/asc_testflight.py internal`): waits
   until App Store Connect has processed the build (VALID), sets What to Test
   (the commit message without trailers, or `TESTFLIGHT_WHATS_NEW`), adds the
   build to every internal group without "access to all builds"
   (`forkmesh-team` has it) and checks the internal state
   (`IN_BETA_TESTING` / `READY_FOR_BETA_TESTING`).
9. **External TestFlight** only with `FORKMESH_IOS_TESTFLIGHT_EXTERNAL=1`
   (`app-store-connect publish --testflight`, group
   `TESTFLIGHT_BETA_GROUPS`, default `forkmesh-external-team`). It is off
   because Beta App Review failed with `BETA_CONTRACT_MISSING`, an agreement
   only the Account Holder can accept in App Store Connect (Business).
10. **App Store submission** only with `FORKMESH_IOS_APP_STORE=1`, which the
    release host sets for every tagged release (below).

Switches, set in the workflow or when starting a build:

| Variable | Default | Effect |
|---|---|---|
| `FORKMESH_NATIVE_WATCH` | `1` | `0` leaves the watch app out of the IPA |
| `FORKMESH_NATIVE_WIDGETS` | `1` | `0` leaves the Home Screen widgets and Live Activities out of the project |
| `FORKMESH_NATIVE_SHARE` | `1` | `0` leaves the "Send to ForkMesh" share extension out of the project |
| `FORKMESH_NATIVE_AUTOFILL` | `1` | `0` leaves the ForkMesh Vault AutoFill credential provider out of the project |
| `FORKMESH_NATIVE_SIGNING` | `manual` | `xcode` = Xcode automatic signing at archive |
| `FORKMESH_IOS_TESTFLIGHT_EXTERNAL` | `0` | `1` also submits for Beta App Review |
| `FORKMESH_IOS_APP_STORE` | `0` | `1` picks a version the App Store accepts and submits the build for App Review (the release host sets it) |
| `FORKMESH_IOS_WHATS_NEW` | `TESTFLIGHT_WHATS_NEW` | the App Store's What's New text |
| `FORKMESH_IOS_UPLOAD_DRY_RUN` | `0` | `1` validates with altool and uploads nothing |
| `FORKMESH_NATIVE_CORE_TESTS` | `1` | `0` skips the macOS ForkMeshCore step, for a release whose core passed on Linux while a macOS-only test harness crash is open |
| `TESTFLIGHT_INTERNAL_GROUPS` | all internal | comma-separated group names |
| `FORKMESH_TESTFLIGHT_WAIT_MINUTES` | `60` | processing wait |

`tools/asc_testflight.py status VERSION BUILD` prints a build's processing,
internal and external state at any time.

## Store configuration guard

Some mistakes only App Store Connect or a real handset would catch: a wrong
`aps-environment` or bundle id gives a build that installs and runs normally
except that pushes never arrive, and a missing required-reason API
declaration comes back as `ITMS-91053` after upload.
`ForkMeshCore/Tests/FMApiTests/StoreConfigTests.swift` checks the committed
project files on Linux, so these fail in `swift test` first. It replaces the
Flutter `mobile/test/ios_store_config_test.dart` from 2026-08-31, which was
dropped with the Flutter iOS app.

- The `ForkMesh` target (selected by `PRODUCT_BUNDLE_IDENTIFIER`, so a renamed
  id fails instead of passing vacuously) has exactly Debug and Release. Release
  signs with `ForkMesh-Release.entitlements` (`aps-environment` production),
  Debug with `ForkMesh-Debug.entitlements` (development).
- The app and `NotificationService` list the same keychain access group, or the
  extension cannot read the session token the app leaves for it. Since
  2026-10-13 the extension also reads the chat device key
  (`chat/e2ee/x25519PrivateV2`, service `com.forkmesh.forkmesh.secure`) to
  open chat pushes, so the app's own keychain items must keep landing in that
  shared group (they are written without an explicit group, which picks the
  first `keychain-access-groups` entry).
- `NotificationService` links the `ForkMeshPushCore` product (FMBase, FMCrypto,
  FMPush), linked by `tools/generate_project.rb`;
  `StoreConfigTests.testTheNotificationExtensionLinksOnlyThePushCore` checks
  the project, the generator and `Package.swift` agree.
- `APNS_BUNDLE_ID` in `app/src/apns.py` is the app's bundle id. The server sends
  it as `apns-topic` for tokens registered without a known bundle id, and the
  native app registers with `Bundle.main.bundleIdentifier`.
- `ForkMesh-Info.plist` answers `ITSAppUsesNonExemptEncryption`. The value is a
  store decision (`false` since 2026-09-18,
  `mobile/docs/app-store-submission-checklist.md`), so only its presence is
  checked; `tools/asc_testflight.py` checks it again before upload.
- On iPhone the app is locked to portrait: `UISupportedInterfaceOrientations`
  lists only `UIInterfaceOrientationPortrait` (since 2026-10-09). The layouts
  are built for a tall screen, and the Genie spider's gravity then always
  reads in the portrait frame. The `~ipad` list keeps all four orientations,
  which iPad multitasking requires.
- No `PrivacyInfo.xcprivacy` is named in the project file. The manifests reach
  their bundles through the synchronized folder groups, and a name in the
  project would be a membership exception that leaves one out.
- Each manifest declares every required-reason API category that its bundle's
  Swift sources use: the app bundle is `ForkMesh`, `Shared`, `SharedSurfaces`
  and `ForkMeshCore/Sources`, and the extension bundle is `NotificationService`,
  `Shared` and the FMPush, FMCrypto and FMBase sources it links. Each category is found by a short list of API names. The
  test's first run found DiskSpace undeclared: `LocalModelStore.availableCapacity`
  reads free space to refuse a model download that would not fit (`E174.1`)
  and shows it in Local AI settings (`85F4.1`). SystemBootTime (`35F9.1`) is
  still declared from the Flutter manifest although no native source uses it.
  Declaring more than the code uses is allowed.

The share extension ships in every build and has its own manifest
(`ForkMeshShare/PrivacyInfo.xcprivacy`, `UserDefaults` `CA92.1`;
[CAPTURE_MARKUP.md](CAPTURE_MARKUP.md), "The iOS share extension hand-off").
The AutoFill extension ships in every build too and has its own manifest
(`ForkMeshAutoFill/PrivacyInfo.xcprivacy`, `UserDefaults` `CA92.1`, for the
FMBase code it links). The widget extension (`ForkMeshWidgets/PrivacyInfo.xcprivacy`)
declares what the app declares, since it links the same core; the watch app
and its widgets (`ForkMeshWatch/`, `ForkMeshWatchWidgets/`) declare
`UserDefaults` `CA92.1`. `StoreConfigTests` checks all three like the others.

## Upload rejected: an App Intent description named the iPhone

Build 1.4.1 (21), uploaded 2026-10-11, never appeared in App Store Connect.
`altool` printed "UPLOAD SUCCEEDED" and the TestFlight step waited an hour on
`NOT_FOUND`; the reason was only in the upload record
(`GET /v1/apps/6803060735/buildUploads`, state `FAILED`): error 90626,
"Invalid Siri Support. App Intent description ... cannot contain "iphone"".
The text was the watch app's `NewAgentIntent.description`
(`ForkMeshWatch/NewAgentIntent.swift`), added with Siri on the watch after
build 20. It now says "through your phone".

`StoreConfigTests.testNoAppIntentDescriptionNamesAnAppleDevice` fails when an
`IntentDescription` literal contains "iphone", "ipad" or "apple watch". A
build number is spent by a failed upload: the next build is 22.

When a build stays `NOT_FOUND`, read the upload record before waiting longer.

## App Store rollout for a tagged release

`tools/forkmesh-release-host.py` starts `ios-native-release` for the newest
release candidate from v1.2.6 on: on the office Mac when it answers
([MAC_BUILD.md](MAC_BUILD.md)), on Codemagic otherwise
([`docs/release/README.md`](../release/README.md), iOS). It sets
`FORKMESH_IOS_APP_STORE=1` and passes the annotated tag's message as
`FORKMESH_IOS_WHATS_NEW` and `TESTFLIGHT_WHATS_NEW`. Two steps change:

- **Pick the next build number** also picks the marketing version with
  `tools/asc_appstore.py version <xcconfig version>`: the xcconfig's
  `MARKETING_VERSION` while App Store Connect has not released it, else one
  patch past the highest version it has released or approved. A version
  string can never be reused once Apple approved it, and a release must not
  wait for someone to bump the xcconfig, so the next build after 1.4.0 goes
  live is 1.4.1 unless the xcconfig says more. The number is written into the
  clone only, like the build number.
- **Submit to the App Store** (`tools/asc_appstore.py submit VERSION BUILD
  WHATS_NEW`) waits until the build is VALID, then prepares the one App Store
  version that may be in flight:
  - none in flight: creates the version;
  - one waiting for or in review: cancels that review submission and waits
    until the version is editable again, so the newest release replaces it;
  - one editable (prepare for submission, rejected, developer rejected,
    metadata rejected, invalid binary): renames it to this version.

  It sets the release type to "after approval", attaches the build, writes
  What's New into every localization, and submits: an open review submission
  (ready for review, or unresolved issues after a rejection) is reused, its
  items for this version marked resolved; otherwise a new one is created. The
  version goes live when Apple approves it. On 2026-09-28 App Store Connect
  held 1.0 and 1.1.0 (live) and 1.2.1 (the last Flutter build, rejected, in an
  unresolved submission), so the first automated release renames 1.2.1 to
  1.4.0 and resubmits that submission with the native build.

To submit `main` without a tag, run the shortcut
`.forkmesh/shortcuts/deploy-native-ios-app-store-mac.sh`
(`tools/mac_adhoc_deploy.py --app-store --execute`): the same steps on the
office Mac, with What's New from `--whats-new` or a generic line
([MAC_BUILD.md](MAC_BUILD.md), "The App Store shortcut").

Rejections and metadata Apple asks for (screenshots, privacy labels, the
open items below) fail the submit step with App Store Connect's error; the
release host retries after an hour, and `asc_appstore.py status` shows the
versions and review submissions. `asc_appstore.py plan VERSION` prints what
`submit` would do without changing anything.

## Store status shortcut (2026-10-09)

`.forkmesh/shortcuts/ios-app-store-status.sh` answers "is the iOS app
waiting for review, did Apple send feedback, and which TestFlight groups are
on?" without opening App Store Connect. It runs
`tools/mac_adhoc_deploy.py --store-status` on the desktop. The report only
reads; it changes nothing in App Store Connect. It prints:

- **App Store versions**: the four newest, each with its build number, state
  (`WAITING_FOR_REVIEW`, `IN_REVIEW`, `REJECTED`, `READY_FOR_DISTRIBUTION`, …)
  and release type.
- **App Review submissions**: the four newest, each with its state, its
  submitted date and the state of each item in it. `UNRESOLVED_ISSUES`,
  `REJECTED` and the other states that need a fix end in "<-- needs action".
  The App Store Connect API does not return the text of App Review's message.
  The report says to read it under App Review in App Store Connect.
- **TestFlight builds**: the three newest uploads, each with its processing
  state, internal and external testing state, Beta App Review state, and the
  groups it is in.
- **TestFlight groups**: every group, internal ones first, with its number of
  testers, whether it gets every build, whether its public link is on (and
  the link), and whether tester feedback is on.
- **Feedback**: the five newest TestFlight screenshot and crash reports (who
  sent each one, the device and the comment), and the five newest App Store
  customer reviews. `--feedback N` in `asc_store_report.py` changes the
  count. If an endpoint fails, its section says "unavailable" and the rest
  of the report still prints.

Credentials: this desktop's Actions variables may set `ASC_KEY_ID` and
`ASC_ISSUER_ID`; exported variables take precedence. When neither location supplies both,
the shortcut reads the two IDs from the office Mac over its authenticated build connection.
Its private key must be at `~/.appstoreconnect/private_keys/AuthKey_<ASC_KEY_ID>.p8`.
The script reports a configuration error if the values or local key are missing.

First run, 2026-10-09: 1.4.0 (19) `REJECTED`, submission `f00cc601…` in
`UNRESOLVED_ISSUES`, internal group `forkmesh-team` (every build), and
external group `forkmesh-external-team` with its public link on.

## TestFlight feedback to tasks (2026-10-09)

`.forkmesh/shortcuts/testflight-feedback-to-tasks.sh` runs
`tools/testflight_feedback_tasks.py`. Each run does two things:

- **New feedback becomes a task.** It reads every screenshot and crash
  feedback from `GET /v1/apps/{app}/betaFeedbackScreenshotSubmissions` and
  `.../betaFeedbackCrashSubmissions` (with `include=build,tester`, so the task
  names the build number and the tester). Each feedback without a task gets
  one in `forkmesh/forkmesh`, unassigned: the title is "TestFlight: " and the
  comment, the details hold the comment, tester, device, build and date, and
  the screenshots are attached as images. The last line of the details,
  `TestFlight feedback id: <screenshot|crash> <id>`, is how later runs find
  the task again, so a feedback is never filed twice (a trashed task counts
  too).
- **A done task marks its feedback done.** App Store Connect has no
  "resolved" state for tester feedback; the only change the API allows is
  `DELETE /v1/betaFeedback{Screenshot,Crash}Submissions/{id}`. So when the
  task carrying a feedback's id is completed, the next run deletes that
  feedback from App Store Connect, which removes it from the TestFlight
  feedback list. A trashed task deletes nothing. A delete that answers 404
  (already gone) counts as done; any other failure is printed and the run
  exits 1.

`--dry-run` lists the tasks it would add and the feedback it would delete,
and changes nothing. `--repository OWNER/NAME` files the tasks elsewhere.

Screenshots: Apple's screenshot links (`tf-feedback.itunes.apple.com`) need
no API token and expire a couple of weeks after the feedback, so the tool
downloads them and attaches the bytes rather than linking them. A task create
request is capped at 64 KiB (`BODY_MAX_BYTES` in
`world/src/world_office_tasks.py`), well under the 256 KiB per image the Tasks
page otherwise allows, so each screenshot (up to four) is re-encoded as a JPEG
small enough to fit: a 1290x2796 iPhone screenshot lands at about 472x1024,
quality 55, around 42 KB. This needs Pillow (`python3-pil`). A screenshot that
cannot be downloaded or shrunk still files the task, with a line saying so.

Tasks are created through `task_request` in `tools/forkmesh_mcp_server.py`,
signed with this node's identity, because `create_task` there does not pass
`attachments`. Credentials for App Store Connect are the same as the store
status shortcut's (above).

First dry run, 2026-10-09: five screenshot feedbacks (2026-08-30 to
2026-09-01), one screenshot each, no crash
feedback.

## App Review: nothing paid on iOS

Apple rejected 1.4.0 (18) on 2026-10-02 under guideline 3.1.1 (submission
`28911a02-6512-4188-a1c4-ce7ebafa0c75`, reviewed on an iPad Air 11-inch (M3)
and an iPhone 17 Pro Max): "the app accesses digital content purchased
outside the app, such as subscriptions, but that content isn't available to
purchase using In-App Purchase". The message opens with "The issues we
previously identified still need your attention", so this was not the first
time; the Membership and Funding sections had already left Settings on
2026-09-26 (`README.md`, "Recent changes").

ForkMesh memberships are sold on the web through Patreon
(`docs/app/payments-and-plans.md`). Under 3.1.1 and 3.1.3(b) an iOS app may
only show or unlock something bought elsewhere if the same thing is also sold
by In-App Purchase, and the app sells nothing. So the rule for the iOS app
is: it shows no membership, tier, price or plan, unlocks nothing a membership
pays for, and neither sends nor asks for money.

Apple did not name a screen. What the app still had, and 2026-10-04 removed:

- **Ads.** The screen showed "Membership: None", "Running 0 of 0" and
  "Needs a membership to run". Live ad slots are the one thing a membership
  buys (`www/public/business-model.json`, `advertising.allowances`), so
  managing your own ads was a paid feature reached from the app. The owner
  side is gone (`AdsAccount`, `OwnedAd.ownerActions`,
  `PackagesClient.adAction`). The screen is now the review queue only, the
  hub lists it as "Ad review" only for ad reviewers
  (`HubPackageState.adReviewer`, `PackageRoutes.hubRoutes(installed:adReviewer:)`),
  and the app reads `/api/ads/mine` for nothing but `isReviewer`.
- **Market.** The package page said "Free" or "Paid". Every package is free,
  but the label implied a paid kind; `MarketPackage.free` is gone.
- **Ledger.** "Ledger & tips" had Send (tip a member in SOL and pay
  contributor rewards through the desktop wallet), Treasury (the donation
  address with a QR code) and a payout-wallet balance. All three are gone
  with `WalletClient`, `WalletRequestModel`, `SystemWallet`, `WalletBalance`
  and the `wallet.` control family. The Ledger keeps the public records, a
  read-only rewards list and the personal income and expense records.
- **Patreon status.** `MembershipStatus`, `MembershipStatusRow` and
  `AccountDataModel.loadMembership` (`GET /api/accounts/patreon`) were no
  longer on any screen but still compiled in, with a link-out gate for the
  United States storefront. Removed, with `AccountProfile.solana` and
  `walletQRText`.

What stays, and why it is not a purchase:

- Sponsored messages in Activity: showing an ad is not selling one.
- Bounty panels on issues and Funds received in Activity: read-only public
  records; the funding buttons have been disabled since custodial wallets
  were retired.
- Agent usage ("API spend this month", "Extra usage") and fleet costs: the
  user's own accounts with other providers, reported by their desktop.
- The bounty and reward notification kinds: delivery preferences only.

1.4.0 (19) came back with the same 3.1.1 message ("We noticed that the app
still includes or accesses paid digital content…"), again without naming a
screen. On 2026-10-09 the rest went:

- **"Why this ad?"** on the sponsored card opened
  `forkmesh.com/docs/operations/ad-network/`, which lists the Patreon
  membership levels with their prices: a link from the app to buying
  something outside it. The button and `AdNetworkLinks` are gone; the card
  keeps its "Sponsored" label, the advertiser's link and Hide.
- **Credits and tips in Notifications.** The server's catalog still lists
  "Credits refilled" (the Claude Code usage window, issue #346) and "Tips"
  under "Money & rewards". `PingKindGroup.hiddenKinds` drops both kinds from
  whatever catalog the relay sends, the group is shown as "Rewards", and a
  `credits_refilled` ping is labelled "Claude Code" rather than "Credits".
- **Nodes link.** "Manage and revoke nodes from the web app" opened
  `/dashboard/settings`, one tab away from Account with its Membership and
  Plan usage cards. It now opens the Nodes tab, `/dashboard/settings/nodes`.
- **App Review notes.** `asc_appstore.py submit` now writes them on every
  submission (`set_review_notes`): it puts `REVIEW_NOTES` (the app is free,
  sells nothing, nothing bought elsewhere unlocks anything, the agent usage
  and server costs are the user's own accounts with Anthropic and Vultr)
  ahead of whatever notes the version already had, and leaves them alone if
  that text is already there.

If Apple names one of these in a later review, remove it the same way.

Before adding a screen to the iOS app, check that it shows no price, plan or
tier, does not depend on `role_grants` of scope `platform`, and has no button
that moves money. Android and the desktop are not bound by this.

Outside the code, the same rule covers the App Store listing: the
description, screenshots and What's New must not mention membership, Patreon
or tips. The App Review notes are written by the submit step (above).

## App Review: Apple Health is named in the app

Apple rejected the build reviewed on 2026-10-11 under guideline 2.5.1: "The
app uses the HealthKit or CareKit APIs but does not clearly identify the
HealthKit and CareKit functionality in the app's user interface." The app
uses HealthKit only (no CareKit), in seven optional tools: Pedometer, Water
Intake, Calorie Counter, Sleep, Sugar Break, Stretch Break and Food Guide
(`ForkMesh/Platform/HealthKitBridge.swift`).

Before this, each tool had one switch with "Apple Health" in its label and
nothing else, and the tools only exist after they are installed from Market,
so a reviewer who installed none saw no mention of Health at all. Since
2026-10-11:

- **Each tool's Apple Health card** ends with `AppleHealthDisclosure`
  (`ForkMesh/Features/Tools/AppleHealthViews.swift`): a "Works with Apple
  Health" mark, "Uses Apple's HealthKit", what that tool reads and writes, and
  a "How ForkMesh uses Apple Health" button that opens the guide as a sheet.
- **Settings > Legal > Apple Health** opens the same guide
  (`AppleHealthGuideScreen`) with no tool installed. It lists all seven tools
  with what is read, what is written and what is saved to the account, the
  promises (off until turned on, never used for advertising, how to turn it
  off), and a link to the privacy policy's Apple Health section
  (`LegalLinks.appleHealth`, `/privacy#apple-health`).
- **The text** is `AppleHealthGuide` in
  `ForkMeshCore/Sources/FMTools/AppleHealthGuide.swift`, one `AppleHealthUse`
  per `AppleHealthTool`. A tool that starts using HealthKit gets a case there
  and an `AppleHealthDisclosure` in its screen; `AppleHealthGuideTests` fails
  if a case has no entry. The same table is public at
  `www/docs/mobile/apple-health.md` (`/docs/mobile/apple-health/`) and in the
  privacy policy (`www/templates/pages/privacy.html`); change all three
  together, and the two usage strings in `Config/ForkMesh-Info.plist`.
- **App Review notes.** `asc_appstore.py submit` adds `HEALTH_REVIEW_NOTES`
  after `REVIEW_NOTES`, telling the reviewer both places.

The app links to the privacy policy and not to `/docs/mobile/apple-health/`:
the docs library's sidebar lists the business-model pages with their prices,
which is the kind of link 3.1.1 was raised for (above).

The mark is an SF Symbol heart on a white tile (`AppleHealthMark`), not
Apple's Health app icon, which is not in the repository. Apple's "Works with
Apple Health" badge can replace it if the artwork is added.

Apple's message asks for a screen recording on a physical device showing the
identification, attached in the reply and kept in the App Review notes for
later submissions: record Settings > Legal > Apple Health, then one installed
tool's Apple Health card.

## AutoFill

`ForkMeshAutoFill` (`com.forkmesh.forkmesh.AutoFill`) fills vault logins in
Safari and other apps, inserts a username or password into any field (iOS 18),
and is the importer Apple Passwords hands logins to (Credential Exchange,
iOS 26). It was written with the other Gate 2 extensions but left behind
`FORKMESH_NATIVE_EXTENSIONS` because its login cache lived in the App Group,
which no release can provision ("Widgets, Live Activities and the watch" below). Since
2026-10-02 the sealed cache, its key and the app links live in the keychain
group `$(AppIdentifierPrefix)com.forkmesh.forkmesh` that the app, the
notification extension and the share extension already share, so the
extension needs no App Group and ships by default.

- `tools/asc_bundle_ids.py ensure` registers the extension's id with
  `AUTOFILL_CREDENTIAL_PROVIDER` and turns the same capability on for the app:
  Apple asks for the entitlement on both the extension and its host app, and
  the app uses `ASCredentialIdentityStore` and `ASSettingsHelper`. Both are
  open to the App Store Connect API, so cloud signing at export provisions
  them with no browser step.
- `Config/ForkMesh-Info.plist` lists `ASCredentialExchangeActivityType` under
  `NSUserActivityTypes` and the extension's `ASCredentialProviderExtensionCapabilities`
  carries `SupportsCredentialExchange` and `SupportedCredentialExchangeVersions`
  (`1.0`); without them Passwords does not offer ForkMesh as a destination. The
  app receives the import token as an `NSUserActivity` (`VaultCredentialExchange`).
- Credential Exchange code sits under `#if compiler(>=6.2)` (Xcode 26) and the
  `.app` service identifiers under `#if compiler(>=6.3)` (an SDK with iOS
  26.2), so an older Xcode still builds the rest.
- `StoreConfigTests.testTheAutoFillExtensionShipsWithoutAnUnprovisionedAppGroup`
  fails if the group comes back or the target leaves the project.

## Widgets, Live Activities and the watch

Until 2026-10-09 no release carried the Home Screen widgets, the Dynamic
Island and Lock Screen Live Activities (running agents, timers, walks) or the
watch app: 1.4.0 (17) shipped phone-only. All three shared data through App
Groups (`group.com.forkmesh.forkmesh` for the widgets,
`group.com.forkmesh.forkmesh.watch` for the watch), and the public App Store
Connect API cannot create or assign an App Group (`/v1/appGroups` is 404;
Xcode automatic signing with the API key cannot either, tried 2026-09-24). So
the widgets were left out unless `FORKMESH_NATIVE_EXTENSIONS=1` and the watch
unless `FORKMESH_NATIVE_WATCH=1`, and both switches were off in the release
and ad hoc workflows. The Live Activities live in the widget extension, so
without it the app's `Activity.request` had nothing to draw them. Even the
app's own entitlements never listed the group, so a build with the widgets
would have shown them empty.

Since then they use keychain groups, which every profile allows with no
portal step, as the share extension and AutoFill already do:

- The app writes the widget snapshot and the widgets leave pending taps in
  `742ZANCB57.com.forkmesh.forkmesh` (service `com.forkmesh.forkmesh.surfaces`;
  `SurfaceContainer.sharedItems()`, `KeychainSharedItemStore` in FMBase),
  readable after first unlock so widgets render on a locked phone.
- The watch app leaves the complication summary in its own group
  `742ZANCB57.com.forkmesh.forkmesh.watchkitapp` (`WatchGroup.shared()`), which
  the watch app and its widgets already listed. The phone's group never
  reaches the watch.
- `ForkMeshWidgets` is in every build (`FORKMESH_NATIVE_WIDGETS=0` leaves it
  out) and `FORKMESH_NATIVE_WATCH` defaults to `1` in every workflow.
  `asc_bundle_ids.py ensure` no longer asks for `APP_GROUPS` on these ids.
- `*native_watch_check` fails the build if the exported widgets, watch app or
  watch widgets lack their keychain group.
  `StoreConfigTests.testTheWidgetsAndTheWatchAppShipWithoutAnUnprovisionedAppGroup`
  fails if an App Group comes back or a target leaves the project.

A watch installs the watch app from the iPhone's Watch app (App Store and
TestFlight builds), or, for an ad hoc build, only once the watch is a
registered device (below).

## `ios-native-adhoc`

The same archive and checks, exported `release-testing` for the team's
registered devices. It installs over a TestFlight or App Store copy of
ForkMesh, so it is also how to try the Flutter-to-native migration on a
phone before a release. The "Registered devices" step lists them and says
when no Apple Watch is registered (the watch app is then in the IPA but
installs on no watch).

Registering a device (an ad hoc profile covers the devices registered when it
is minted, so build again afterwards):

```sh
cd native/ios
ASC_KEY_ID=… ASC_ISSUER_ID=… python3 tools/asc_bundle_ids.py register-device "Name" <UDID>
python3 tools/asc_bundle_ids.py devices     # id, class, status, UDID, name
python3 tools/asc_bundle_ids.py has-watch   # exit 0 when an enabled watch is registered
```

A watch lists as platform IOS, class APPLE_WATCH; its UDID is in Xcode's
Devices window (or Finder / Apple Configurator) with the paired iPhone
connected.

## App icon

`tools/render_logo.py` (repository root) draws the app icon from `logo.svg` and
writes every file listed in the phone `AppIcon.appiconset/Contents.json`
(including the 1024 px App Store icon) and the 1024 px watch icon in
`ForkMeshWatch/Assets.xcassets`, along with every other logo file in the
repository (`docs/www/README.md`, "Logo files"). Run it with `python3` after
changing `logo.svg` (needs pycairo and Pillow); `--preview out.png` writes only
a 1024 px app-icon render. The PNGs are committed, so the Mac build does not
run the script.

- each size is rendered at its own pixel size, so the small icons are not
  downscaled from 1024;
- the hexagon mark sits at 78% of the tile on a charcoal gradient with a soft
  glow; its white ring keeps the outline on black home screens;
- no alpha channel (`FORMAT_RGB24`), which App Store Connect requires for the
  1024 px icon.

Until 2026-10-04 `native/ios/tools/app_icon.py` drew the older isometric cube
(three shaded faces) in pycairo; `render_logo.py` replaced it when the logo
became the white-ringed hexagon.

The in-app `Logo.imageset` holds `logo.svg` itself with
`preserves-vector-representation`, so the sign-in mark and the project switcher
draw it as a vector at any size; both scale it to fit with 11% padding on
black.

## Flutter-to-native migration

`FMAccount/FlutterMigration.swift`, run by
`ForkMesh/App/FlutterMigrationRunner.swift` from `AppServices.start` before
any store or service is built (the project router resolves the active
project from the preferences on first use). Tests:
`ForkMeshCore/Tests/FMAccountTests/FlutterMigrationTests.swift`.

What the Flutter build left on an upgraded phone:

- **Secrets:** flutter_secure_storage 10.3.4 with default iOS options:
  generic-password items, service `flutter_secure_storage_service`, account =
  the full key, UTF-8 value, `kSecAttrAccessibleWhenUnlocked`, in the default
  access group `742ZANCB57.com.forkmesh.forkmesh`, which the native
  entitlements list too.
- **Preferences:** shared_preferences 2.5.5: `UserDefaults.standard` keys
  `flutter.<key>` holding a Bool, an Int, a Double, a String or an array of
  strings.

The key names are byte-for-byte the native ones, WS1's project layout
included: production ("ForkMesh", `app.forkmesh.com`) owns the unprefixed
legacy namespace, every other project uses `p/<projectId>/`, and sessions
live in the shared `auth/session/<projectId>` slots. So the migration is a
copy that keeps each key, into the native Keychain service
`com.forkmesh.forkmesh.secure` and `fm.<key>` preferences (the native
`UserDefaultsBackend` takes the prefix as a parameter; the migration reads
Flutter's through a second backend with `flutter.`).

Rules:

- **Active project.** When the Flutter preferences name no active project,
  `relays/active` is set to `app.forkmesh.com`. Without it the build's own
  default (ForkMesh Next, `Config/Version.xcconfig`) would be active and the
  upgraded user would look signed out.
- **Idempotent.** `migration/flutter/done` stops every later run. Until it is
  set, each launch retries, and a Flutter value wins over a native one: a
  native value written before the copy finished came from a launch that could
  not see the Flutter data (a background launch on a locked phone, when the
  `WhenUnlocked` items read as `errSecInteractionNotAllowed`), for example a
  freshly minted node key.
- **Deferred, not guessed.** An unreadable Flutter Keychain, or a failed
  native write, returns `deferred` with nothing marked.
- **Verified.** Every copied value is read back before the migration counts
  as done. Counts, key lists and the time are kept under `migration/flutter/`.
- **Flutter data kept until the session is verified.** The copy deletes
  nothing. After launch, `FlutterMigrationRunner.retireWhenVerified` calls
  `GET /api/accounts/sessions` with the migrated session; only when the relay
  accepts it are the Flutter Keychain items and `flutter.*` preferences the
  migration copied deleted (these may include plaintext secrets an old
  Flutter build left in preferences). With no Flutter session at all
  (including the pre-projects `auth/session` slot) there is nothing to verify
  and the read-back suffices. A failed delete is retried on a later launch.
- **Files** need no move: Documents (`forkmesh/repos`, `projects/<slug>/…`,
  `ForkMesh from desktop`) and Application Support (`local_mesh_history.json`)
  are the same container paths in both apps, and the repository index's
  absolute paths are rebased on load.
- **Push:** the extension's credential items keep the Flutter names (group
  `742ZANCB57.com.forkmesh.forkmesh`, service `com.forkmesh.forkmesh.push`,
  items `session` and `session@<host>`), so it reads what the Flutter build
  left until the native app rewrites them. Native tokens register with
  `bundleId = com.forkmesh.forkmesh`, the APNs topic the server already uses.

## Open items

- **First release with widgets and the watch.** The 2026-10-09 change
  ("Widgets, Live Activities and the watch") has only been compiled in parts
  on Linux; the first `ios-native-adhoc` or `ios-native-release` run is the
  first full archive with the widget extension and the watch app embedded and
  signed. Check on a phone: an Overview widget fills in, a running agent shows
  in the Dynamic Island once the app has been opened, and the watch app
  installs from the Watch app.
- **Upgrade test on a phone.** Install the current App Store (Flutter) build,
  sign in, change a setting, then install the native build over it
  (TestFlight, or `ios-native-adhoc` on a registered device). Expect: still
  signed in, same project, same theme, same local repositories, pushes arrive.
- **App Store Connect metadata** before the first App Store submission of the
  native app: What's New for 1.4.0, watch screenshots and the 1024 px watch
  icon, and privacy nutrition labels for photos, microphone and speech (the
  composer).
- **External TestFlight.** The Account Holder must accept the pending
  agreement in App Store Connect (Business / Agreements); until then Beta App
  Review fails with `BETA_CONTRACT_MISSING`. Then run `ios-native-release`
  with `FORKMESH_IOS_TESTFLIGHT_EXTERNAL=1`.
- **Old Flutter builds.** Expire the remaining Flutter TestFlight builds
  (1.2.1 (12), 1.2.3 (14-16)) once testers are on native, so nobody installs
  one over it. App Store version 1.2.1 was rejected (seen 2026-09-28); the
  first tagged release renames it and submits the native build in its place,
  and no further Flutter version is submitted.
- **Old native bundle id.** Remove `com.forkmesh.nativeapp` from the server's
  APNs allowlist (`APNS_NATIVE_BUNDLE_ID` in `app/src/apns.py`) once no device
  token is registered under it, and delete the unused
  `com.forkmesh.nativeapp*` App IDs in the developer portal.
- **Upload history.** Write the App Store upload history, the build-train
  rules and the export-compliance notes into `native/ios/docs/`. The
  Flutter-era record was `mobile/docs/app-store-submission-checklist.md`, now
  only in git history.
