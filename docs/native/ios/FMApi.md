# FMApi

Part of [iOS core modules](README.md).
The App's REST client and account-level services, the wire models, and
the view models of the account screens and the home shell, in
`ForkMeshCore/Sources/FMApi/` (`Services/`, `Models/`, `ViewModels/`).

## Services

The App's REST client and the account-level services, in `ForkMeshCore/Sources/FMApi/Services`. `import FMApi` (re-import `FMBase` for `JSONObject`,
`FMAccount` for `AuthService`/`AccountDevice`, `FMNet` for `HTTPClient`). Everything is
`@MainActor`; network waits happen off the main thread inside FMNet. Tests: `Tests/FMApiTests`.

### ApiService (`@MainActor @Observable final class`)

```swift
let api = ApiService(settings: settingsService,          // ApiSettings (SettingsService conforms)
                     http: URLSessionHTTPClient(),       // FakeHTTPClient in tests
                     performanceMonitor: monitor,         // optional
                     clock: SystemClock(),                // optional
                     sleep: { try await Task.sleep(...) })  // optional; tests pass { _ in }
api.sessionTokenProvider = { auth.session?.sessionToken ?? "" }
api.onSessionInvalid = { code in auth.handleUnauthorized(code: code) }
api.onUnauthorized = { /* route to login if needed */ }
api.onFeatureMissing = { feature in settings.relays.markFeatureMissing(settings.relays.activeId, feature) }
api.ownerCrypto = securityBootstrap.ownerKeys
Task { await api.enrollOwnerDeviceWhenSignedIn(auth) }
Services.put(api)
```

`StaticApiSettings(serverUrl:room:)` is a settable `ApiSettings` for tests and previews.

Plumbing: `relayEpoch`, `resetForRelaySwitch()` (drops every cache, including mirror-set,
federated-reply and commit-detail caches), `clearRepoCache`, `hasSession`, `url(path, query)`,
`gitOrigin`, `rawRequest(...)` (session, 20 s; a 401 signs out except a non-session 401 from
`/api/repo/`, see `endsSession(path, code)`). TTLs: `cacheTtl` 60 s, `catalogTtl` 30 s,
`offlineRepoTtl`, `sponsoredAdTtl`, `accountStatusTtl`; `maxAgentPromptChars` 8000.

Errors: HTTP failures throw `ApiException` (`message`); no answer throws FMNet's `HTTPError`.
Organization and account-package calls throw `OrgApiException(code, message)`, agent and merge
calls `AgentLaunchException`, `updateUserStatus` `UserStatusException`. `dartErrorText(error)`
gives the display text for any of them.

Responses of 16 KB or more, agent queue pages, account API calls, organization requests, runner
diffs and merge previews are parsed on a detached task (`ApiParse.parseAsync`, `modelAsync`).
Owner-sealed envelopes (ML-KEM) are also opened off the main actor.

#### Account (ApiService+Account.swift)

- Account events: `accountEventTicket()`, `accountEventsURL(ticket, node:)`
  (`wss://host/api/nodes/events?ticket=..&node=..`).
- Push: `registerPushDevice(deviceToken:environment:)`, `sendTestPush()`,
  `unregisterPushDevice(token)` (30 s timeout).
- `submitBugReport(...) -> BugReportResult`. Admins: `bugReportInbox(status:offset:limit:)`,
  `setBugReportStatus(id:status:)`, `bugReportAttachment(reportId:attachmentId:)`
  (`/api/bug-reports/inbox`). The signed-in reporter: `myBugReports(offset:) -> MyBugReports`
  (`GET /api/bug-reports/mine`, newest first, 50 a page) and `confirmBugReportFixed(id:) ->
  MyBugReportConfirmation` (`POST /api/bug-reports/mine` `{report, fixed: true}`, closes the report
  and stamps `confirmedFixedAt`; answers the updated `MyBugReport`).
- Profile and status: `accountAvatarPng`, `accountUserStatus`, `updateUserStatus(changes)`.
- Sessions and devices: `listDevices`, `listSessions`, `revokeSession(id)`.
- `roomChatPassphrase()`.
- Notifications: `notifications(node, limit:)`, `markNotificationsRead`, `deleteAllNotifications`,
  `deleteNotification`.
- `webLink(href)`; `registerOwnerKey`, `enrollOwnerDeviceWhenSignedIn`.

#### Catalog and network (ApiService+Catalog.swift)

`repositories()` (mirrors collapsed; cached 30 s), `publishRepository(payload)`,
`createRepositoryImport(sourceUrl:targetOwner:mode:providerToken:)`, `networkStats()`,
`networkLeaderboards()`, `sponsoredAd()` (never throws; cached 30 minutes, 5 after a failure),
`sponsoredAdClicked(id)`.

#### Organizations and account packages (ApiService+Orgs.swift)

- Organizations: `myOrgs`, `createOrg`, `orgProfile`, `deleteOrg`, members, teams, team members,
  repositories, `linkOrgRepo`/`unlinkOrgRepo`.
- `accountApiCall(method, path, body)`: GET/POST/PUT/PATCH/DELETE, 20 s, bearer session; a 2xx
  without `"ok": false` answers the object, anything else throws `OrgApiException`. A `?query` in
  the path stays a query (not escaped to `%3F`).
- Thin wrappers: `vaultCall`, `caloriesOverview`, `caloriesApi`, `installCalories`, `notesApi`,
  `tasksApi`.

FMVault wiring:

```swift
let vault = VaultService(
    request: { method, path, body in
        do { return try await api.accountApiCall(method, path, body) }
        catch let error as OrgApiException { throw VaultError(error.code) }
    }, ...)
```

#### ForkMesh Apps (Models/Apps.swift, ViewModels/InstalledAppsModel.swift)

- `api.appsClient: AppsClient` over `accountApiCall`: `installed()` (`GET /api/apps/installed`),
  `revoke(installId:)`, `decide(org:requestId:approve:)`, `commands()` (`GET /api/apps/commands`),
  and non-throwing `run(text:channelId:)`, `interact(...)`, `forwardMentions(...)`; refusals come
  back as `AppReply(ok: false, error: code)`.
- `AppsChat`: the block kit parser (`section`, `context`, `divider`, `actions` with `button` and
  `static_select`; at most 20 blocks and 10 fields; text only), slash-command and mention parsing,
  suggestions, `/apps` text, error sentences.
- `ChatMessage.app: AppMessage?` carries a bot message's blocks and ids; `ephemeral` for replies
  only this phone shows. `InstalledAppsModel` lists, revokes and decides.

#### Agents, merges, actions (ApiService+Agents.swift)

- Sessions: `agentSessionPage` (opens owner-sealed rows with `ownerCrypto`), `agentSessions`,
  `agentTranscript`, `agentLaunchOptions` (owner route, then organization route on 403/404),
  `launchAgentSession`, `steerAgentSession`, `retryAgentSession`, `cancelAgentSession`.
- Merges: `canMergePulls`, `pullMergePlan`, `mergePull`. A started merge is polled 8 x 750 ms,
  then reported `pending`. Before a core accepts it, `mergePull` retries transient
  `503 merge_node_unavailable` or `mirror_unavailable` for up to 150 seconds with the same request
  id; `no_merge_executor` is not retried. The relay's reason for unavailability is shown as is.
  - A pull is branch-backed only when its `pull.md` says `derive: branch`
    (`PublishedPull.branchBacked`), the same test as the mirror's merge executor
    (`server/merge.go`) and the web dashboard. Any other pull is patch-backed: its plan pins the
    ledger commit as the head and the mirror replays `commits.mbox` / `changes.patch` onto the
    base; it is offered only when one of those files exists.
  - Merge calls go by `readOwner(owner, repo)`, the public owner, because the relay authorizes a
    merge through the organization linked to the catalog row.
  - Deploy approval: when `deployApprovalIdentity` is set, `mergePull` and runner merges add a
    signed `deployApproval` field (kind `pull` or `branch`, owner, number or session id, base and
    head, request id). The body is encoded once so polls resend the same signature. A missing
    identity or signing failure sends the merge without it; a merge is never blocked on it. The
    phone's key is shown as "Deploy approval key" for `approver_keys` under `[self_deploy]` in
    `deploy/fleet.toml`.
- Actions: `repoActions`, `actionLog`, `desktopCommand`. `issueBountyStatus`; bounty creation is
  retired (`ApiUnsupportedError`).

#### Repository reads (ApiService+Repo.swift)

Reads go by the repository's public route (organization alias or upstream, learned from
`repositories()`) and fall back to the serving node. `tree`/`blob` fail fast for a minute after a
503.

- `tree`, `blob` (cached 60 s), `blobs(paths)` (fresh cache entries first, the rest batched),
  `rawURL`, `rawBytes(maxBytes:)`, `cloneURL`.
- `commits`, `commitDetail`, `compare(base:head:)`, `branches`, `searchRepo`.
- `mirrors`, `mirrorSet`, `releases`.
- `issues`, `pulls` (served lists), `federatedReplies`.
- Ledger reads from `forkmesh/pulls` (`ApiService.pullLedgerRef`): `publishedIssues`,
  `publishedPulls` (with timelines), `publishedDiscussions`, and `publishedPullHeads` (no
  timelines, one batched `pull.md` read, cached 60 s) for callers that only need number, head and
  status. `pull.md` files are parsed off the main actor. The detail read also loads
  `changes.patch` and `commits.mbox`.
- `PublishedRecords.*`: pure record parsers (`issueFromMarkdown`, `pullFromMarkdown`,
  `pullHeadFromMarkdown`, `extractPatch`, `frontMatter`, ...); front matter and headings are read by
  walking lines and stopping early.

#### Branch operations

`POST /api/repo/<owner>/<repo>/branches/ops` runs merge, update and delete on the cores.

- `BranchOpRequest` pins the branch tip the person saw (`expectedOid`) and, for a merge, the base
  tip (`baseOid`); without a 40/64-hex tip nothing is sent.
- `ApiService.branchOp` re-posts the same body every 2 s while the core answers 202 (or 503
  `merge_status_pending`/`merge_node_unavailable` on a resumed request), up to 90 times, then
  answers `.pending` with ids kept for a retry. An answer without `executed` means the owner's
  desktop applies it later (`.queued`). `merge_conflict` keeps the conflicting files.
- `updateRunnerAgent` posts `{"action":"update","requestId"}` to the run's session.
- `completeTask(id, note:)` (`POST /api/tasks/<id>/complete`) marks the task an agent run came from
  as done after its merge; the task id is read from `taskKey` or a leading `[task:<32 hex>]`.

### InboxService (`@MainActor final class`)

Signed writes to a repository's relay inbox; canonical strings are byte-identical to the desktop
and the server.

```swift
let inbox = InboxService(settings: settingsService, identity: deviceIdentity, http: http,
                         performanceMonitor: monitor, clock: SystemClock())
```

`submitNewIssue`, `commentOnIssue`, `voteOnIssue`, `setIssueStatus`, `submitNewPull`,
`commentOnPull`, `reviewPull` (`approve`, `request-changes`, `comment`), `submitNewDiscussion`,
`commentOnDiscussion`, `setThreadSubscription`; all `async throws`. `issueContent` and
`pullCommentContent` are exposed for the contract tests.

### NotificationService (`@MainActor @Observable`)

`NotificationService(api:nodeName:)`: `notifications`, `unreadCount`, `unreadCountKnown`,
`inboxTotal`, `loading`, `error`, `hasLoaded`; `start()`, `appResumed()`,
`refresh(silent:catchUp:)` (dedupes concurrent reads), `markAllRead`, `deleteAll`, `delete`
(optimistic, restored on refusal), `markRead`.

### AccountEventChannel (`@MainActor @Observable`)

```swift
let events = AccountEventChannel(api: api, sessionToken: { auth.session?.sessionToken ?? "" },
                                 connector: URLSessionWebSocketConnector(), keepalive: 240, nodeName: "")
events.onTopic = { topic in ... }        // "pings", "issues", "direct-messages", "agents"...
events.onFrame = { frame in ... }
events.onConnected = { Task { await pings.refresh(silent: true, catchUp: true) } }
events.start()
```

`start`, `stop`, `restart` (after sign-in or account switch), `dispose`,
`appLifecycleChanged(_:)` (`.resumed` reconnects at once; the app must forward lifecycle changes),
`send(frame)`. Backoff 2 s doubling to 30 s; `{"type":"ping"}` every `keepalive` seconds.

### UserStatusService (`@MainActor @Observable`)

`UserStatusService(api:relay:sessionToken:)`: `status`, `known`, `busy`, `signedIn`;
`syncSession`, `refresh`, `refreshIfStale(maxAge: 600)`, `setAway`,
`setCustom(emoji:text:expiresAt:)`, `clearCustom`. A lapsing status is dropped on time; the relay
(`OwnStatusAnnouncer`) announces changes.

### ChatRetentionService (`@MainActor @Observable`)

Per-account chat retention (`POST /api/accounts/chat-retention`: an empty body reads,
`{"defaultMs": n|null}` sets or clears the default, `{"chat": key, "retentionMs": n|null}` one
chat). `ChatRetention` clamps every value to [1 hour, 7 days] on read. The service reads on
sign-in, falls back to seven days on sign-out, applies changes optimistically with rollback, and
caches the last answer with the account name under `chat/retention`. `ChatInstaller` hands every
change to `ChatHub.applyRetention`.

### PushNotificationService (`@MainActor @Observable`)

The APNs side is a protocol the app implements (`PushPlatform`): permission status and request,
environment, token, pending open, badge, and shared-keychain credentials for the extension.
`registerForCalls` asks APNs for a token without asking permission: `start` uses it when the prompt
is declined or notifications are off in Settings, so the phone still registers its device and VoIP
tokens and calls ring through CallKit ([docs/app/chat-direct-calls.md](../../app/chat-direct-calls.md)).

`start` (and so sign-in, resume and `rebind`) never shows the system prompt: while permission is
`notDetermined` it registers for calls only. The prompt comes from `start(accountKey:ask: true)`,
which `enable` (Settings' "Enable notifications", offered while permission is not asked yet)
and `askWhenUseful` call. The app calls `askWhenUseful` after the first chat message is sent; it
does nothing unless push has started and permission is still `notDetermined`
([ONBOARDING.md](../ONBOARDING.md#the-permissions-intro)).

```swift
let push = PushNotificationService(api: api, platform: applePushBridge)   // platform nil = unsupported
push.accountKeyProvider = { auth.session?.nodeName ?? "" }
await push.didReceiveToken(hexToken, environment: "production")
push.didOpen(PushOpen(arguments: response.notification.request.content.userInfo))
```

State: `supported`, `token`, `environment`, `permission`, `registered`, `relayConfigured`,
`lastError`, `pendingOpen`. Actions: `start`, `rebind`, `enable`, `askWhenUseful`, `stop` (sign-out), `sendTest`,
`setBadge`, `collectLaunchOpen`, `takePendingOpen`. `PushOpen` carries `topic`, `alertId`, a
same-site `href`, `isChat`, `isDirectMessage` and `deepLink`.

### AgentListSignal (`@MainActor @Observable`)

`ring()` bumps `rings`; observe it to reload the Agents tab.

### App wiring

- On a session change: signed out stops events and push; signed in restarts events, rebinds push
  and syncs user status.
- `events.onTopic`: "pings"/"issues" refresh pings; "agents" rings the agent list.
  `onConnected` refreshes pings with catch-up and rings the agent list.
- Pings listener sets the badge when the unread count is known.
- Scene `.active`: `pings.appResumed()`, `events.appLifecycleChanged(.resumed)`, push rebind and
  `userStatus.refreshIfStale()`.
- Relay switch: stop events and push, `api.resetForRelaySwitch()`, then resume as on sign-in.
- Push taps: the shell consumes `push.takePendingOpen()` after `collectLaunchOpen()`.

Timers (`sleep:` parameters) are injectable on ApiService (5xx retry, merge poll),
AccountEventChannel (backoff, keepalive) and UserStatusService (status lapse).

## Models

The wire models (repositories, chat, notifications, agents, orgs, user status,
agent launch, deep links) in `ForkMeshCore/Sources/FMApi/Models`. `import FMApi` (and `FMBase` for
`JSONObject`). Only FMBase is used; no ApiService here.

### Conventions

- Every model is a `public struct` (Equatable, Hashable, Sendable; Identifiable where it has an id) with `var` fields, a public memberwise `init` with defaults, and `init(json:)`.
- `init(json: JSONObject)` for an object; `init(json: JSONValue?)` where the input may be any JSON value (`RepoTree`, `RepoBlob`, `RepoCommitDetail`, `RepoComparison`, `RepoSearchResults`, `BountyWallet`, `IssueBounty`, `UserStatus`). `SponsoredAd.parse(_ JSONValue?) -> SponsoredAd?` (nil when it is not an ad).
- Parsing uses `ModelJSON`, not FMBase's lenient accessors: `ModelJSON.int` takes an integer as is, truncates any other number, else parses the text as an integer, else 0 ("0x1F" is 31, "1.5" is 0), `ModelJSON.double`, `ModelJSON.bool` ("yes"/"1"/"true"), `ModelJSON.string`, `ModelJSON.firstString(json, keys)` (trimmed), `ModelJSON.coalesce(a, b, ...)` (the first non-nil), `ModelJSON.epochMs`. `DartCore` holds `intTryParse`, `doubleTryParse`, `dateTimeTryParseMs(text, timeZone:)`, `toStringAsFixed` (exact, half away from zero), `encodeComponent`/`decodeComponent`, `toString` (null prints "null"). Use these in ApiService where a service parses by hand.
- All integers are `Int` (64-bit), timestamps are epoch-ms `Int`. `toJSON() -> JSONObject` exists only where a model is written back (`UserStatus`).
- Anything that read the local clock or zone takes it as a parameter with a default: `AgentSession(json:now:)`, `rateLimitCountdownLabel(now:)`, `createdDate(timeZone:)` (plus a `createdDate` property for `.current`), `RepoTreeEntry(json:timeZone:)`, all of UserStatus's time functions.

### Types (by Swift file)

- `Chat.swift`: `Member` (the wire `self` is `isSelf`), `MemberGroup` (`online`, `userStatus`, `primary`, `id`, `nodeCount`), `ChatMessage` (`isSelf`, `timestamp: Date`).
- `Notifications.swift`: `NotificationPage` (`total` defaults to page length), `ForkNotification` (`kindLabel`, `filterGroup` string plus `filter: NotificationFilterGroup`, `isUnread`, `copyWith(readAt:)`, `meta: JSONObject`).
- `Actions.swift`: `RepoActions`, `ActionWorkflow`, `ActionRun` (`statusLabel`, `isActive`, `refLabel`, `shortCommit`, `durationLabel`), `ActionLog`, `DesktopCommandResult`.
- `Agents.swift`: `AgentTranscript`, `AgentSessionPage`, `AgentSession` (labels: `statusLabel`, `providerLabel`, `costLabel`, `diffLabel`, `latestStatusSummary()`, `rateLimitCountdownLabel(now:)`, `diffBase`...; `filesChanged/ahead/behind` are -1 when unknown).
- `Repository.swift`: `Repository` (`fullName`, `displayOwner`, `publicOwner`, `organization`), `collapseRepositoryMirrors(_:)`.
- `Bounty.swift`: `BountyWallet`, `IssueBounty`. `ModelJSON.swift` also has `solanaExplorerAddressUrl(_:)`, `solanaExplorerSignatureUrl(_:)`.
- `Issues.swift`: `Issue` (`bounty`, `copyWithBounty`), `IssueEvent`, `IssueMention`, `issueRelationNumbers`, `issueBodyWithoutRelationLines`, `issueReferenceNumbers`, `subIssueNumbersOf(issues, parent)`, `issueMentionsOf(issues, number)`.
- `Pulls.swift`: `PullRequest`, `PullEvent` (`displayTitle`), `PublishedPull`, `RepoDiscussion`, `DiscussionEvent`, `FederatedReply` (`nativeEvent` is always false from JSON; `displayBody`).
- `RepoContent.swift`: `RepoTreeEntry`, `RepoTree` (dirs first, then case-insensitive name), `RepoBlob`, `RepoCommitFile`, `RepoCommitDetail` (`commit: JSONObject`), `RepoComparison`, `RepoBranch`, `RepoCodeSearchMatch`, `RepoSearchResults`, `RepoMirror`, `RepoMirrorSet` (`syncStatus: JSONObject?`).
- `Releases.swift`: `RepoReleaseAsset` (`sizeLabel`, `platformLabel`), `RepoRelease(json:downloads: [sha: count])`.
- `Network.swift`: `NetworkStats`, `NetworkLeaderboards`, `FundsReceivedEntry`.
- `Orgs.swift`: `orgRoles`, `orgTeamPermissions`, `OrgSummary`, `OrgProfile`, `OrgMember`, `OrgTeam`, `OrgRepo`, `OrgApiException(code, message)` (an `Error`), `orgErrorMessage(_:)`.
- `SponsoredAd.swift`: `SponsoredAd.parse`, `isSafeUrl`, limits as statics, `uri: URL?`.
- `UserStatus.swift`: `UserStatus` (`.empty`, `textMax`, `isExpired(now)`, `visibleAt(now)`, `lapsesAt`, `untilLabel(now, timeZone:)`, `describe(now, timeZone:)`, `copyWith`, `toJSON()`), `ClearAfter` (CaseIterable, `label`, `expiresAt(now, customAt:, timeZone:)`), `StatusPreset`, `statusPresets`, `statusQuickEmojis`, `looksLikeSingleEmoji`, `formatStatusMoment(moment, now, timeZone:)` (en_US_POSIX `h:mm a` / `EEE` / `MMM d` / `MMM d, y`, the intl en_US output), `userStatusErrorMessage`.
- `AgentLaunch.swift`: `AgentLaunchTarget`, `AgentLaunchMessage`, `AgentLaunchSession(json:orgScope:)`, `AgentLaunchOptions(json:orgScope:)` (`preferredTarget`, `targetNamed`), `AgentStageState` (rawValue = the stage name), `AgentTaskStage`, `AgentTaskChain(session, pullStatus:, pullNumber:)` (`stages` computed, `summary`), `AgentQueueResult`, `AgentRetryResult`, `AgentLaunchException(message, code:, status:)` (an `Error`), `PullMergePlan`, `PullMergeOutcome`.
- `PromptAttachment.swift`: `PromptAttachment` (a composer photo: JPEG bytes, `wire` for a launch or steer body), `PromptAttachmentLimits` (5 photos, 5 MB each, 10 MB in all, 2048 px, JPEG q80), `PromptAttachmentError`, `promptOrPhotoNote` (a photos-only prompt becomes "See the attached photos."), `promptAttachmentsProblem`.
- `AgentAttachments.swift`: `AgentAttachmentMeta` (what the relay lists per transcript turn), `AgentAttachmentRef` (with where to fetch it from), `AgentAttachmentReconcile` (pairs the photos just sent with the relay's listing), `AgentSentPhotos` (the sent photos, kept per session). `AgentLaunchMessage.attachments` and `AgentLaunchSession.attachmentRefs` read them. The fetch and cache are `Services/AgentAttachmentStore.swift` (`AgentAttachmentStore`, `AgentAttachmentLoader`) and `ApiService+AgentAttachments.swift`; `launchAgentSession`/`steerAgentSession` take `attachments:` and send them as `attachments: [{name, mediaType, data}]`.
- `NotificationDeepLink.swift`: `RepoDetailTab` (String raw values = the tab names links use), `NotificationDeepLink.parse(_ href:) -> NotificationDeepLink?`.

### Surprises

- `Issue(json:)` reads a non-list `labels`/`assignees` as empty. List entries print as text (null is "null").
- A ten-digit numeric string such as "1700000000" in agent-launch timestamps parses as a compact ISO date (year 170000); numbers are fine.
- Status colours are not in the models (the screens pick them from the label text); they live with the views.
- Regexes use NSRegularExpression with ASCII `\w`/`\d`/`\b` spelled out; `issueMentionsOf` sorts stably.

## View models

The logic of the account screens and the home shell, in
`ForkMeshCore/Sources/FMApi/ViewModels`, tested on Linux in
`Tests/FMApiTests/*ViewModelTests.swift`. `import FMApi` (plus `FMAccount` for
`AuthService`/`AuthSession`). Covers sign-in, the guest gate, settings,
organizations, notifications, user status, the sponsored card and the home shell
(Kotlin: core/account/*Logic.kt). Every model is `@MainActor @Observable`; the
user-facing copy is kept verbatim.

### Pure logic (free functions and enums, Kotlin names)

- AuthFlowLogic: `AuthFlowStart`, `AuthCopy`, `AdNetworkLinks.whyThisAd`, `loginSubmit(...) -> LoginSubmit` (`.busy/.designPreview/.invalid/.send`), `LoginFormState` (`failed(error)`: `bad_totp` reveals the code field without error colour), `loginButtonLabel`, `passwordSubmits`, `authBackDismisses`, `gateCreatesPreviewSession`, `signInOverAppCreatesPreviewSession`, `loginRelaySubtitle`, `isGuestMode`, `requireAccount(guest:showSheet:openSignIn:isAuthenticated:) async -> Bool`.
- SettingsLogic: `sessionAccountName`, `hasRealSession`, `showsUserStatus`, `adoptedDisplayName`, `profileCardText`, `thisDeviceCard(session, nowMillis:) -> ThisDeviceCard`, `capabilitySummary`, `shortDeviceId`, `shortDeviceKey`, `countLabel`, `nodesSummary`, `nodesEmptyText`, `isCurrentDevice`, `deviceTone`, `deviceRowTitle/Detail/Timeline`, `webSettingsURL(serverUrl)`, sessions (`sessionsHeadline`, `sessionRowTitle/Meta/Icon`, `revokeOneConfirmation`, `revokeOthersConfirmation`), push (`showsPushCard`, `pushPermissionLabel`, `pushTokenLabel`, `pushRelayLabel`, `pushCanEnable`, `pushSendLabel`), account (`signedInAsName`, `accountCard`, `showsDeleteAccount`, `deleteAccountConfirmError`, `deleteAccountFailure`), `SettingsCopy.*` (every paragraph).
- OrgsLogic: `orgErrorText`, `orgRoleTone/Label`, `canUseOrgs`, `orgsListStateKey`, `newOrgName`, `orgMemberInput/orgTeamInput/orgRepoInput`, `defaultRepoNode`, menus (`memberMenuRoles`, `memberMenuRemove`, `teamMenuPermissions`, `teamMenuDelete`, `memberMenuAction`, `teamMenuAction`), `teamAddCandidates`, `teamAlreadyFull`, labels, `orgStats`, `OrgCopy.*`.
- NotificationsLogic: `notificationIconForKind` (Material names; `notificationLineIcon` is the MingCute bell), `notificationFilters`, `filterNotifications`, `notificationsBody(...) -> NotificationsBody` (`stateKey` for FmSwap), `NotificationsEmptyCopy(filter:)`, labels, `notificationTimeLabel` (12-hour, "2:40 AM"), `pingDetailWhen` ("Mon, Feb 2, 2026 · 2:40 AM", the Inbox ping page), `pingAgentPrompt`, `pingWhen` (24-hour, kept for the agent prompt so it matches the desktop's), `pingNamesLocalRepo(_:name:publishedAs:)`, `pingRepoChoice(...) -> PingRepoChoice` (`.copy/.open(i)/.pick(order)`), `notificationTapLink`.
- UserStatusLogic: `userStatusSaveAction(...) -> UserStatusSaveAction`, `initialClearAfter/CustomAt`, `userStatusCustomLabel`, `userStatusPickerStart`, `userStatusSummary`, `userStatusRepaintDelay` (UserStatusBuilder's timer).
- HomeShellLogic: `HomeShellCatalog` (`destinations`, `tools`, indices, `guestAction(forTool:)`), `shellStatusLabel`, `shellSubtitle`, `shellAccountLabel`, `shellAvatarLabel`, `shellPromptHint`, `pushChatConversation`, `pushDeepLink`, `ShellRelayState`, `ShellLocalDesktop`.

### Models

- `AuthFlowModel(auth:start:pushedOverApp:debugBuild:)`: `mode`, `show(_)`, `back() -> Bool` (true = dismiss), `identifier/password/totp`, `form`, `loading`, `revokedBanner` (consumed once on the login page), `relaySubtitle`, `signupURL`, `submit() async -> Bool` (true = run onAuthenticated), `finishAuthenticated()` (creates the preview session only on the empty-form path), `continueAsGuest()`.
- `SettingsFormModel(settings:)`: `setText(.displayName/.room/.passphrase/.github/.gitlab, value)` saves trimmed; `adoptAccountName(session)`.
- `AccountNodesModel(session:listDevices:)`, `AccountSessionsModel(listSessions:revokeSession:)` (`revoke(target) -> RevokeOutcome`), `DeleteAccountModel(auth:)`.
- `OrgClient` (`@MainActor` protocol; `ApiService` conforms), `OrgsListModel`, `CreateOrgModel`, `OrgDetailModel` (`load`, `memberMenu`, `teamMenu`, `addMember/addTeam/linkRepo/unlink`, `deleteOrg`, `actionError`: the refused action's sentence, shown inline in red until the next action), `TeamDetailModel` (same `actionError`).
- `NotificationsScreenModel(service:api:)`: `filter`, `body`, `rowKey` (changes after a refused swipe), `delete`, `deleteAll`, `markAllRead`, `open(n) -> NotificationTapTarget` (`.repo/.web/.context`), `notice` and `noticeIsError` (shown as an inline line above the list, not a toast).
- `UserStatusSheetModel(service:now:timeZone:)`: `emoji/text/clearAfter/customAt/problem`, `save() -> Bool` (close), `clear()`, `setAway`; `setAwayFromSettings(service, away) -> String?`.
- `SponsoredAdModel(settings:api:)`: `allowed`, `load()` (once), `visibleAd`, `follow(ad) -> URL?`, `hide()`. `PushCardModel(push:)`.
- `HomeShellModel(chat:notifications:targets:)`: `index`, `select(_)` (Chat clears the chat badge, `markBadgeSeen`, not the per-conversation unread dots), `prompt`, `promptTarget`, `promptHint`, `promptModeLabel`, `togglePromptMode()`, `promptBusy`, `submitPrompt() -> ShellPromptResult` (`.ignored`, `.startAgent`, `.handled(message)`, `.failed(message)`), `agentStarted()`, `agentsGeneration`, `routePush(isChat:isDirectMessage:alertId:href:) -> ShellPushRoute?`.

### Seams for other areas

- `ShellChatBridge` (`@MainActor` protocol): what the shell reads from the chat relay (`relayState`, `onlineCount`, `totalUnread`, `badgeUnread` (the Chat tab badge), `currentConversation`, `unreadConversations`, `connect`, `markBadgeSeen`, `switchConversation`). The Chat feature registers one: `Services.put(relay, as: ShellChatBridge.self)`. Without it the shell draws an offline relay. The prompt bar only starts agents (or feeds a page target); chat messages go through the Chat screen's own composer.
- `ShellPromptTargets` (`@MainActor @Observable`, one registered in Services by AppServices): a page takes over the prompt bar while it is on screen, as web pages do with the dock's `registerPageHandler` ([web-composer.md](../../app/web-composer.md)). `register(ShellPromptTarget(id:label:icon:title:placeholder:submit:)) -> UUID` on appear, `unregister(token)` on disappear; the newest registration wins and one with the same `id` replaces the old. While a target is active the prompt bar shows its placeholder, icon and label in the mode pill and sends on Return; Send calls `submit(trimmed text)`, clears the prompt on success and shows the returned sentence (or the `ShellPromptFailure` message, or an `OrgApiException` message) in the shell's inline notice line under the top bar (no toasts). Tapping the pill flips between the target and Agent (`bypassed`), so an agent can still be started from that page. With no target there is no pill and the prompt bar starts an agent. The Calories screen is the one user today.
- `ShellLocalDesktopBridge` (`localDesktop: ShellLocalDesktop`): the version footer's desktop light, green while `isConnected` (`.connected` on the LAN or `.relay`); `DesktopControlHub` registers as `ShellLocalDesktopBridge.self`. None = `.hidden` (red).
- App target (ForkMesh/): `AccountFeature.requireAccount(action:) async -> Bool`, `AccountFeature.openSignIn(_:)`, `AccountFeature.isGuest`; `AgentPromptHandoff` (Services): the prompt bar offers its text before presenting `AgentStartCard()`; the card calls `takePrompt()` and `started()`; `UserStatusEmoji(status:size:)`, `SponsoredAdCard(margin:)`.
