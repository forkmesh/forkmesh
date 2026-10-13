# FMApi services

The Worker REST client and the account-level services, in
`ForkMeshCore/Sources/FMApi/Services` (models: docs/FMApiModels.md). `import FMApi`
(re-import `FMBase` for `JSONObject`, `FMAccount` for `AuthService`/`AccountDevice`, `FMNet`
for `HTTPClient`). Everything is `@MainActor`; every `await` on the network happens off the
main thread inside FMNet.

Files: ApiService.swift + ApiService+Account/Catalog/Orgs/Agents/Repo.swift, ApiResults.swift,
PublishedRecords.swift, and one file per service below.

STATUS: complete. Every service below is implemented with these signatures and tested
(Tests/FMApiTests: ApiService*, Api*, InboxService, NotificationService, AccountEventChannel,
PushNotificationService, PushTestResult, UserStatusService tests; all pass on Linux).

## ApiService (`@MainActor @Observable final class`)

```swift
let api = ApiService(settings: settingsService,          // ApiSettings (SettingsService conforms)
                     http: URLSessionHTTPClient(),       // FakeHTTPClient in tests
                     performanceMonitor: monitor,         // optional
                     clock: SystemClock(),                // optional
                     sleep: { try await Task.sleep(...) })  // optional; tests pass { _ in }
api.sessionTokenProvider = { auth.session?.sessionToken ?? "" }       // @MainActor () -> String
api.onSessionInvalid = { code in auth.handleUnauthorized(code: code) }
api.onUnauthorized = { /* route to login if needed */ }
api.onFeatureMissing = { feature in settings.relays.markFeatureMissing(settings.relays.activeId, feature) }
api.ownerCrypto = securityBootstrap.ownerKeys             // OwnerKeys? (FMCrypto)
Task { await api.enrollOwnerDeviceWhenSignedIn(auth) }
Services.put(api)
```
`StaticApiSettings(serverUrl:room:)` is a settable `ApiSettings` for tests and previews.

Plumbing: `relayEpoch`, `resetForRelaySwitch()` (drops every cache; call between relays),
`clearRepoCache(owner, name)`, `hasSession`, `url(path, [(k, v)]) -> URL`,
`gitOrigin`, `rawRequest(method, url, headers:, body:) async throws -> HTTPResponse` (session,
20 s, a 401 still signs out, except a non-session 401 from `/api/repo/`, see
`endsSession(path, code)`). Static TTLs: `cacheTtl` 60 s, `catalogTtl` 30 s, `offlineRepoTtl`,
`sponsoredAdTtl`, `sponsoredAdFailureTtl`, `accountStatusTtl`, `maxAgentPromptChars` 8000.

Errors: HTTP failures throw `ApiException` (`message`; `description` is
"Exception: message"; sentences such as "HTTP 404 for /api/...", "Your session
expired. Please sign in again.", "Repo host is offline. ..."). No answer at all throws FMNet's
`HTTPError` (`.timedOut`, `.transport`). Org and account-package calls throw `OrgApiException`
(`code`, `message`), agent/merge calls `AgentLaunchException` (`message`, `code`, `status`),
`updateUserStatus` `UserStatusException`. `dartErrorText(error)` gives the text screens
show for any of them ("Exception: message" for an `ApiException`).

### Account (ApiService+Account.swift)
- `accountEventTicket() async -> EventTicket` (`status: .ok/.signedOut/.unavailable`, `ticket`)
- `accountEventsURL(_ ticket, node: "") -> URL` (wss://host/api/nodes/events?ticket=..&node=..)
- `registerPushDevice(deviceToken:environment:) async -> PushRegistration` (`ok`, `statusCode`, `relayConfigured: Bool?`)
- `sendTestPush() async -> PushTestResult` (`summary` is the sentence to show; `.unreachable`)
- `unregisterPushDevice(_ token) async -> Bool`
- `submitBugReport(message:contact:screen:severity:platform:appVersion:osVersion:device:) async -> BugReportResult` (`ok`, `id`, `message` = sentence to show)
- `accountAvatarPng(_ name) async -> String` (base64 PNG or ""), `accountUserStatus(_ name) async -> UserStatus`
- `updateUserStatus(_ changes: JSONObject = [:]) async throws -> UserStatus` (empty = read)
- `listDevices() async throws -> [AccountDevice]`, `listSessions() async throws -> [AccountSession]`, `revokeSession(_ id) async throws -> Bool` (true = this app's session was revoked)
- `roomChatPassphrase() async throws -> String`
- `notifications(_ node, limit: 40) async throws -> NotificationPage`, `markNotificationsRead(_ node, ids:, all:) async throws`, `deleteAllNotifications(_ node) async throws`, `deleteNotification(_ node, _ id) async throws`
- `webLink(_ href) -> URL?` (site path or http(s) link to open; nil for /api and non-web)
- `registerOwnerKey(_ owner: OwnerKeys) async -> String`, `enrollOwnerDeviceWhenSignedIn(_ auth: AuthService) async`

### Catalog and network (ApiService+Catalog.swift)
- `repositories() async throws -> [Repository]` (mirrors collapsed; cached 30 s)
- `publishRepository(_ payload: JSONObject) async throws -> ApiJsonResult` (`statusCode`, `body`, `ok`, `error`)
- `createRepositoryImport(sourceUrl:targetOwner:mode:providerToken:) async throws -> ApiJsonResult`
- `networkStats() async throws -> NetworkStats`, `networkLeaderboards() async throws -> NetworkLeaderboards` (shape-checked: `NetworkShape`)
- `sponsoredAd() async -> SponsoredAd?` (never throws; cached 30 min / 5 min after a failure), `sponsoredAdClicked(_ id) async`

### Organizations and account packages (ApiService+Orgs.swift)
- `myOrgs()`, `createOrg(name:displayName:description:) -> OrgSummary`, `orgProfile(_ org) -> OrgProfile`, `deleteOrg(_)`,
  `orgMembers(_)`, `setOrgMember(_ org, _ member, role:)`, `removeOrgMember(_, _)`, `orgTeams(_)`, `setOrgTeam(_, _, permission:)`,
  `deleteOrgTeam(_, _)`, `orgTeamMembers(_, _)`, `addOrgTeamMember(_, _, _)`, `removeOrgTeamMember(_, _, _)`, `orgRepos(_)`,
  `linkOrgRepo(_ org, _ repo, node:)`, `unlinkOrgRepo(_, _)` — all `async throws` (`OrgApiException`).
- `accountApiCall(_ method, _ path, _ body: JSONObject? = nil) async throws -> JSONObject` — GET/POST/PUT/PATCH/DELETE, 20 s,
  bearer session; a 2xx without `"ok": false` answers the object, anything else throws `OrgApiException(code, message)`.
- `vaultCall`, `caloriesOverview(_ day)`, `caloriesApi(_ method, _ suffix, _ body)`, `installCalories(tzOffsetMinutes:)`,
  `notesApi(_, _, _)`, `tasksApi(_, _, _)` — thin wrappers of `accountApiCall`.

**FMVault wiring** (the docs/FMVault.md snippet works as written):
```swift
let vault = VaultService(
    request: { method, path, body in
        do { return try await api.accountApiCall(method, path, body) }
        catch let error as OrgApiException { throw VaultError(error.code) }
    }, ...)
```
`api` is a `@MainActor` class, so capturing it in the `@Sendable` closure is fine; the call hops
to the main actor.

### ForkMesh Apps (Models/Apps.swift, ViewModels/InstalledAppsModel.swift; WS22)
- `api.appsClient: AppsClient`, a value over `accountApiCall`, so FMChat and tests can pass a closure instead:
  `installed() -> InstalledApps` (`GET /api/apps/installed`), `revoke(installId:)` (`DELETE /api/apps/installs/<id>`),
  `decide(org:requestId:approve:) -> String` (`POST /api/apps/orgs/<org>/requests/<id>`), `commands() -> AppCommandCatalog`
  (`GET /api/apps/commands`: commands and `mentionable` slugs), and three calls that never throw:
  `run(text:channelId:) -> AppReply`, `interact(appMessageId:channelId:actionId:value:) -> AppReply`,
  `forwardMentions(channelId:messageId:text:) -> Int`. A refusal comes back as `AppReply(ok: false, error: code)`.
- `AppsChat`: the block kit parser (`section`, `context`, `divider`, `actions` with `button` and `static_select`; at most
  20 blocks, 10 fields, unknown types skipped, text only), `parseSlash` and `mentionedSlugs` with the server's patterns,
  `suggestions`, `commandsText` (`/apps`), `errorText` (the web's sentences), `message(frame, channelId:)` for a bot frame.
- `ChatMessage.app: AppMessage?` carries a bot message's blocks, `appMessageId`, the channel id to send back, and
  `ephemeral` for a reply only this phone shows.
- `InstalledAppsModel(client:)`: `apps`, `loaded`, `loading`, `failure`, `busy`, `notice`; `reload()`, `revoke(_)`,
  `decide(_:org:approve:)`. Copy: `InstalledAppsCopy`.

### Agents, merges, actions (ApiService+Agents.swift)
- `agentSessionPage(_ owner, _ name, ownerAccount:) async throws -> AgentSessionPage` (opens owner-sealed rows with `ownerCrypto`; `sealedForOthers`), `agentSessions(...)`, `agentTranscript(_ owner, _ name, _ id: Int, ownerAccount:) -> AgentTranscript`
- `agentLaunchOptions(_ owner, _ repo) -> AgentLaunchOptions` (owner route, then org route on 403/404)
- `launchAgentSession(_ owner, _ repo, prompt:provider:model:targetNode:title:createPr:issueNumber:orgScope:) -> AgentQueueResult`
- `steerAgentSession(_ owner, _ repo, _ sessionId, prompt:orgScope:) -> AgentQueueResult`, `retryAgentSession(_ owner, _ repo, _ session: AgentLaunchSession) -> AgentRetryResult`, `cancelAgentSession(_ owner, _ repo, _ sessionId, orgScope:)`
- `canMergePulls(_ owner, _ repo) async -> Bool`, `pullMergePlan(_ owner, _ repo, _ pull: PublishedPull) async -> PullMergeOutcome` (polls a started merge 8 x 750 ms; `pending` after; clears the repo cache once merged). Before a core accepts the request, `mergePull` retries a transient `503 merge_node_unavailable` or `mirror_unavailable` for up to 150 seconds with the same request id. The exception is `no_merge_executor`, which cannot recover by waiting. The relay says whether the available cores are still agreeing on refs, cannot verify the signed repository state, lack the executor, or whether the already-selected job core is offline, and both phone clients show that reason instead of claiming no node exists.
  - A pull is branch-backed only when its `pull.md` says `derive: branch` (`PublishedPull.branchBacked`), the same test the mirror's merge executor (`server/merge.go`, `meta.derived`) and the web dashboard apply. Any other pull is patch-backed even when it names a `head`: a contributor's pull from another node names its own branch, which the mirror does not have, and carries its commits as `commits.mbox` / `changes.patch` in the pulls ledger. Its plan pins the ledger commit as the head (`expectedHeadOid == expectedPullsOid`) and the mirror replays the series onto the base tip; it is offered only when one of those two files is in the pull's ledger folder. Until 2026-10-06 the phone treated every pull with a `head` as branch-backed, asked `merge-preview` about a branch that did not exist, and showed "could not be pinned to exact commits" for every contributed pull (PR #263 on forkmesh/forkmesh).
  - `publishedPull` (the detail read) also loads the pull's `changes.patch` (when `pull.md` has no inline patch) and `commits.mbox` into `PublishedPull.patch` / `.commits`; the list reads do not.
  - All three go by `readOwner(owner, repo)`, the public owner. The relay authorizes a merge only through the organization linked to the catalog row (`_organization_owner_can_merge_repo`) and refuses the same request sent to the serving node's namespace (403), while `/api/orgs/<node>` answers 404. Before this, the phone never offered a merge for a mirror-served repository and would have been refused if it had. A repository with no organization keeps its own namespace and is not mergeable from the phone or the web.
  - Merge counts as deploy approval (2026-10-08): when `deployApprovalIdentity` is set (`AppServices` wires the node identity), `mergePull` adds `deployApproval` (kind `pull`, the `readOwner` path owner, the pull number, the plan's base and head, the body's `requestId`) and `runnerBranchAction` adds it for `action == "merge"` only (kind `branch`, the owner in the agent-sessions or agent-bots path, the session id, empty base and head). The pull body is encoded once, so the 202 polls resend the same requestId, ts and signature; the runner merge re-signs only when the requestId changes (an echoed id or the stale retry). No identity, or a signing failure, sends the merge without the field: a merge is never blocked on it. The relay used to refuse any extra key on the pull body; it now accepts `deployApproval` as optional and strips it before forwarding to the node. Update-branch and cleanup never carry it, nor does the desktop-agent merge over the control channel. The phone's key is shown and copied as "Deploy approval key" on Identity and recovery, for `approver_keys` under `[self_deploy]` in `deploy/fleet.toml`.
- `repoActions(_ owner, _ name, ownerAccount:ts:sig:) -> RepoActions`, `actionLog(_ owner, _ name, _ id, ownerAccount:ts:sig:) -> ActionLog`, `desktopCommand(_ owner, _ name, ownerAccount:command:target:ts:sig:payload:) -> DesktopCommandResult`
- `issueBountyStatus(_ owner, _ repo, _ number) -> IssueBounty`; `createIssueBounty`/`bountyWallet` always throw `ApiUnsupportedError` (retired).

### Repository reads (ApiService+Repo.swift)
Reads go by the repository's public route (org alias / upstream, learned from `repositories()`)
and fall back to the serving node. `tree`/`blob` fail fast for a minute after a 503.
- `tree(_ owner, _ name, path:ref:) -> RepoTree`, `blob(_ owner, _ name, _ path, ref:) -> RepoBlob` (cached 60 s)
- `rawURL(_ owner, _ name, _ path, ref:) -> URL`, `rawBytes(_ owner, _ name, _ path, ref:maxBytes:) async throws -> Data` (uncached, exact bytes)
- `cloneURL(_ owner, _ name) -> URL` (smart-HTTP clone URL), `gitOrigin`
- `commits(_, _) -> [JSONObject]`, `commitDetail(_, _, _ hash, fallbackCommit:) -> RepoCommitDetail`, `compare(_, _, base:head:) -> RepoComparison`,
  `branches(_, _) -> [RepoBranch]`, `searchRepo(_, _, _ query, ref:) -> RepoSearchResults`
- `mirrors(_, _) -> [RepoMirror]`, `mirrorSet(_, _) -> RepoMirrorSet`, `releases(_, _) -> [RepoRelease]`
- `issues(_, _) -> [Issue]`, `pulls(_, _) -> [PullRequest]` (served lists), `federatedReplies(_, _, kind:number:) -> [FederatedReply]`
- `publishedIssues(_, _) -> [Issue]`, `publishedPulls(_, _) -> [PublishedPull]` (from the `forkmesh/pulls` ledger, `ApiService.pullLedgerRef`), `publishedDiscussions(_, _) -> [RepoDiscussion]`
- `publishedPullHeads(_, _) -> [PublishedPull]`: the pulls without their timelines (`events` empty), for callers that only need number, head and status. It is the ledger listing, one batched `pull.md` read, and one `pull.md` read per branch-only pull, cached 60 s (`pullHeadsCache`). A fresh `publishedPulls` answer is reused. `publishedPulls` reads one folder listing and one batch per pull for the timeline, so a repository with 40 pulls costs about 80 requests. The Agents list used it on every ring for any repository with a branched run, which made most of the `tree` and `blobs` traffic in the 2026-10-01 performance report. Each head is parsed with `pullHeadFromMarkdown` on a detached task, so `body` and `patch` are empty: a `pull.md` carries the whole patch, and splitting every one into lines on the main actor froze the Agents tab for 500 ms or more (2026-10-02 report). The queued run screen (`LaunchedAgentSessionModel`) and the widgets' open-pull count read heads too. `publishedPulls` also parses each `pull.md` off the main actor.
- `blobs(_, _, _ paths, ref:) -> [String: RepoBlob]` takes fresh `blob` cache entries first and batches only the rest. Before 2026-10-01 it fetched every path again even when the cache held it.
- `PublishedRecords.*`: the pure record parsers (`issueFromRecordJson`, `issueFromMarkdown`, `pullFromMarkdown`, `extractPatch`, `pullHeadFromMarkdown`, `frontMatter`, `firstHeading`, `bodyWithoutFrontMatter`, `splitLines`, `eachLine`, `trimRight`...). `frontMatter` and `firstHeading` walk lines with `eachLine` and stop at the closing `---` or the first heading, instead of splitting the whole file.

## InboxService (`@MainActor final class`)

Signed writes to a repository's relay inbox; canonical strings byte-identical to the Qt client
and the Worker.
```swift
let inbox = InboxService(settings: settingsService, identity: deviceIdentity, http: http,
                         performanceMonitor: monitor, clock: SystemClock())
```
`submitNewIssue(_ owner, _ name, title:body:labels:milestone:priority:assignees:)`,
`commentOnIssue(_ owner, _ name, _ number, _ body)`, `voteOnIssue(_, _, _)`,
`setIssueStatus(_, _, _, _ status)`, `submitNewPull(_ owner, _ name, title:base:head:patch:commits:description:authorName:)`,
`commentOnPull(_, _, _, _ body)`, `reviewPull(_, _, _, _ state, _ body)` (state "approve" |
"request-changes" | "comment"), `submitNewDiscussion(_, _, title:body:category:)`,
`commentOnDiscussion(_, _, _, _ body)`, `setThreadSubscription(_ owner, _ name, node:source:number:subscribed:)`
— all `async throws` (`ApiException` with the Worker's error code or "HTTP n";
`ApiArgumentError` for bad subscription arguments). `issueContent(type, ev)` and
`pullCommentContent(type, ev)` are exposed for the contract tests.

## NotificationService (`@MainActor @Observable`)

```swift
let pings = NotificationService(api: api, nodeName: { auth.session?.nodeName ?? "" })
```
State: `notifications: [ForkNotification]`, `unreadCount`, `unreadCountKnown`, `inboxTotal`,
`loading`, `error` (String, via `dartErrorText`), `hasLoaded`. Actions: `start()` (shell up),
`appResumed()`, `refresh(silent:catchUp:) async` (dedupes concurrent reads), `markAllRead() async throws`,
`deleteAll() async throws`, `delete(_ n) async throws` (optimistic; restored on refusal),
`markRead(_ n) async throws`. `addListener`/`removeListener` for non-SwiftUI observers (badge).

## AccountEventChannel (`@MainActor @Observable`)

```swift
let events = AccountEventChannel(api: api,                    // AccountEventTicketSource (ApiService conforms)
                                 sessionToken: { auth.session?.sessionToken ?? "" },
                                 connector: URLSessionWebSocketConnector(),  // FMNet
                                 keepalive: 240, nodeName: "")
events.onTopic = { topic in ... }        // "pings", "issues", "direct-messages", "agents"...
events.onFrame = { frame in ... }        // hello / terminal frames (JSONObject)
events.onConnected = { Task { await pings.refresh(silent: true, catchUp: true) } }
events.onSignedOut = { ... }
events.start()
```
`connected`, `started` (observable); `start()`, `stop() async`, `restart() async` (after sign-in
or account switch), `dispose() async`, `appLifecycleChanged(_ state: AppLifecycleState)` (call on
scene phase changes; `.resumed` reconnects at once), `send(_ frame: JSONObject) -> Bool`. Backoff
2 s doubling to 30 s; `{"type":"ping"}` every `keepalive` seconds.

## UserStatusService (`@MainActor @Observable`)

```swift
let userStatus = UserStatusService(api: api, relay: nil, sessionToken: { auth.session?.sessionToken ?? "" })
userStatus.relay = relayService        // OwnStatusAnnouncer (FMChat's RelayService conforms), set once it exists
auth.addListener { Task { @MainActor in await userStatus.syncSession() } }
```
`status: UserStatus` (visible now), `known`, `busy`, `signedIn`; `syncSession() async`,
`refresh() async throws`, `refreshIfStale(maxAge: 600) async throws`, `setAway(_:) async throws`,
`setCustom(emoji:text:expiresAt:) async throws` (epoch ms, 0 = never), `clearCustom() async throws`
(all throw `UserStatusException`), `dispose()`. A lapsing status is dropped on time.

## ChatRetentionService (`@MainActor @Observable`)

How long chat messages stay before they disappear, per account (`POST /api/accounts/chat-retention`;
an empty body reads, `{"defaultMs": n|null}` sets or clears the default, `{"chat": key,
"retentionMs": n|null}` sets or clears one chat). The model is `ChatRetention` (`defaultMs`,
`chats`, `updatedAt`; `effectiveMs(for:)`, `isExpired(_:key:now:)`, `presets`, `label(_:)`,
`key(conversation:officeRoomId:)`). Every value is clamped to [1 hour, 7 days] on read as well as
by the server, so a bad cache or payload can never keep a message longer than seven days.

The service follows the session like `UserStatusService`: `syncSession()` reads on sign-in and
falls back to the seven-day standard on sign-out; `setDefault(_:)` and `setChat(_:_:)` apply at
once and roll back if the server refuses (`ChatRetentionError`). The last answer is cached in
`KeyValueStore` under `chat/retention` together with the account name, so an offline start of the
same account enforces its shorter times straight away and another account never inherits them.
The login payload's `chatRetention` is not read yet (`AuthSession` does not keep it);
`adoptAccountPayload(_:)` is there for when it is, and the sign-in read covers it meanwhile.
ChatInstaller (`AppServices+Chat.swift`) creates it, hands every change to
`ChatHub.applyRetention`, re-reads on resume and follows auth.

## PushNotificationService (`@MainActor @Observable`)

The APNs half is a protocol the app implements (`PushPlatform`, `@MainActor`
class-bound): `permissionStatus() async throws -> String?` ("authorized",
"provisional", "ephemeral", "denied", "notDetermined"), `requestPermission() async throws -> Bool?`
(also calls `registerForRemoteNotifications`), `environment() async throws -> String?`
("development" / "production"), `getToken() async throws -> String?` (hex token issued this
launch, ""/nil while pending), `takePendingOpen() async throws -> PushOpen?`, `setBadge(_ count) async throws`, `setCredentials(origin:account:session:)
async throws` (shared keychain for the extension), `clearCredentials() async throws`.
```swift
let push = PushNotificationService(api: api, platform: applePushBridge)   // platform nil = unsupported
push.accountKeyProvider = { auth.session?.nodeName ?? "" }
push.onOpened = { Task { await pings.refresh(silent: true) } }
// from the AppDelegate / UNUserNotificationCenter delegate:
await push.didReceiveToken(hexToken, environment: "production")
push.didFailToRegister(error.localizedDescription)
push.didOpen(PushOpen(arguments: response.notification.request.content.userInfo))
```
State: `supported`, `token`, `environment`, `permission`, `registered`, `relayConfigured`,
`lastError`, `pendingOpen`. Actions: `start(accountKey:) async`, `rebind(accountKey:) async`,
`enable() async`, `stop() async` (sign-out), `sendTest() async -> PushTestResult`,
`setBadge(_ count) async`, `collectLaunchOpen() async`, `takePendingOpen() -> PushOpen?`.
`PushOpen` (`topic`, `alertId`, `href` (same-site path only), `isChat`, `isDirectMessage`,
`deepLink: NotificationDeepLink?`).

## AgentListSignal (`@MainActor @Observable`)

`ring()` bumps `rings`; observe `rings` (or `addListener`) to reload the Agents tab.

## App wiring

- Auth listener (`auth.addListener`), on a change of `auth.session?.sessionToken`: signed out ->
  `await events.stop()`, `await push.stop()`; signed in -> `await events.restart()`,
  `await push.rebind(accountKey: auth.session?.nodeName ?? "")`. Also `userStatus.syncSession()`.
- `events.onTopic`: "pings" or "issues" -> `pings.refresh(silent: true)`; "agents" ->
  `agentList.ring()`. `events.onConnected`: `pings.refresh(silent: true, catchUp: true)` and
  `agentList.ring()`.
- `pings.addListener`: when `pings.unreadCountKnown`, `await push.setBadge(pings.unreadCount)`.
- Scene phase `.active`: `pings.appResumed()`, `events.appLifecycleChanged(.resumed)` (the channel
  does not observe the lifecycle itself; the app must forward it), and when signed in
  `push.rebind(accountKey:)` and `try? await userStatus.refreshIfStale()`.
- Relay switch: suspend = `await events.stop()`, `await push.stop()`; between relays
  `api.resetForRelaySwitch()`; resume = the auth-listener path above, then `pings.refresh(silent: true)`.
- Push taps: the shell consumes `push.takePendingOpen()` (open `deepLink`, else the inbox;
  `isChat` / `isDirectMessage` for conversations) after `await push.collectLaunchOpen()`.

## Surprises

- `accountApiCall` keeps a `?query` in the path argument a query (`tasksApi("GET", "?offset=0")`,
  `caloriesOverview(day)`), not an escaped `%3F`: `/api/tasks%3Foffset=0` matches no Worker
  route.
- `resetForRelaySwitch` also clears the mirror-set, federated-reply and commit-detail caches.
- The event ticket and push unregister requests use a 30 s timeout.
- Owner-sealed envelopes are opened on a detached task (ML-KEM), not on the main actor.
- A 2xx reply of `detachedParseBytes` (16 KB) or more from `getJson`, `postJson` or `deleteJson` is
  parsed on a detached task (`decodeResponseAsync`); smaller replies and errors use `decodeResponse`.
- The other request paths do the same through `ApiParse.parseAsync` / `jsonMapAsync`: the agent
  queue (`agentJson`), `accountApiCall` (calories, notes, calendar, steps and sleep sync, tasks,
  vault), `orgRequest`, the runner diff page and the merge preview. `agentLaunchOptions` and
  `accountAgentSessions` also build their models on a detached task (`ApiParse.modelAsync`): up to
  50 finished runs of 80 history turns each, plus 16 K of live text per running run.
- `AccountEventChannel.send` answers true once the frame is queued; a failed send closes the
  socket and schedules a reconnect.
- Timers (`sleep:` parameters) are injectable on ApiService (5xx retry, merge poll),
  AccountEventChannel (backoff, keepalive) and UserStatusService (status lapse).

## Branch operations and runner update (2026-10-04)

The phone manages branches the way the desktop does, through the App's
`POST /api/repo/<owner>/<repo>/branches/ops`, which runs them on the cores when they serve the
repository. The Branches screen offers Merge into <default>, Merge & delete, Update from <default>
and Delete (confirmed first) on each non-default branch.

- `BranchOpRequest` (`merge`/`update`/`delete` factories) pins the branch tip the person saw
  (`expectedOid`) and, for a merge, the base tip (`baseOid`); without a 40/64-hex tip nothing is
  sent (`BranchOpText.notPinned`). `body` adds `requestId`, `node`, `pinBase` and `pinOid` once the
  request is resuming.
- `ApiService.branchOp` re-POSTs the same body plus those ids every 2 s while the core answers 202
  (or 503 `merge_status_pending`/`merge_node_unavailable` on a resumed request), up to 90 times, then
  answers `.pending` with the ids kept so "Try again" checks on the same request. An answer without
  `executed` is the old queued shape: the owner's desktop applies it later (`.queued`). A 403 reads
  "Only the repository owner can manage branches."; a `merge_conflict` keeps the files
  (`BranchOpOutcome.conflicts`). `landedOnDefault` tells the screen to offer the Actions for the merge
  commit.
- `updateRunnerAgent` posts `{"action":"update","requestId"}` to the run's session, polling like the
  runner merge (`runnerBranchAction`); a 503 `merge_status_pending` is retried with the same id for
  merge and update alike.
- `completeTask(id, note:)` is `POST /api/tasks/<id>/complete` with `completionNote`, the call the
  desktop makes when an agent started from a task is merged. `RunnerAgentText.taskId` reads the task
  from `taskKey` (`task:<32 hex>`) or a leading `[task:<32 hex>]` in the run title or first prompt;
  `completionNote` is `Merged <branch> into <base> (<7>). <message>`.
