# FMApi services

The App's REST client and the account-level services, in `ForkMeshCore/Sources/FMApi/Services`
(models: [FMApiModels](FMApiModels.md)). `import FMApi` (re-import `FMBase` for `JSONObject`,
`FMAccount` for `AuthService`/`AccountDevice`, `FMNet` for `HTTPClient`). Everything is
`@MainActor`; network waits happen off the main thread inside FMNet. Tests: `Tests/FMApiTests`.

## ApiService (`@MainActor @Observable final class`)

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

### Account (ApiService+Account.swift)

- Account events: `accountEventTicket()`, `accountEventsURL(ticket, node:)`
  (`wss://host/api/nodes/events?ticket=..&node=..`).
- Push: `registerPushDevice(deviceToken:environment:)`, `sendTestPush()`,
  `unregisterPushDevice(token)` (30 s timeout).
- `submitBugReport(...) -> BugReportResult`.
- Profile and status: `accountAvatarPng`, `accountUserStatus`, `updateUserStatus(changes)`.
- Sessions and devices: `listDevices`, `listSessions`, `revokeSession(id)`.
- `roomChatPassphrase()`.
- Notifications: `notifications(node, limit:)`, `markNotificationsRead`, `deleteAllNotifications`,
  `deleteNotification`.
- `webLink(href)`; `registerOwnerKey`, `enrollOwnerDeviceWhenSignedIn`.

### Catalog and network (ApiService+Catalog.swift)

`repositories()` (mirrors collapsed; cached 30 s), `publishRepository(payload)`,
`createRepositoryImport(sourceUrl:targetOwner:mode:providerToken:)`, `networkStats()`,
`networkLeaderboards()`, `sponsoredAd()` (never throws; cached 30 minutes, 5 after a failure),
`sponsoredAdClicked(id)`.

### Organizations and account packages (ApiService+Orgs.swift)

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

### ForkMesh Apps (Models/Apps.swift, ViewModels/InstalledAppsModel.swift)

- `api.appsClient: AppsClient` over `accountApiCall`: `installed()` (`GET /api/apps/installed`),
  `revoke(installId:)`, `decide(org:requestId:approve:)`, `commands()` (`GET /api/apps/commands`),
  and non-throwing `run(text:channelId:)`, `interact(...)`, `forwardMentions(...)`; refusals come
  back as `AppReply(ok: false, error: code)`.
- `AppsChat`: the block kit parser (`section`, `context`, `divider`, `actions` with `button` and
  `static_select`; at most 20 blocks and 10 fields; text only), slash-command and mention parsing,
  suggestions, `/apps` text, error sentences.
- `ChatMessage.app: AppMessage?` carries a bot message's blocks and ids; `ephemeral` for replies
  only this phone shows. `InstalledAppsModel` lists, revokes and decides.

### Agents, merges, actions (ApiService+Agents.swift)

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

### Repository reads (ApiService+Repo.swift)

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

### Branch operations

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

## InboxService (`@MainActor final class`)

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

## NotificationService (`@MainActor @Observable`)

`NotificationService(api:nodeName:)`: `notifications`, `unreadCount`, `unreadCountKnown`,
`inboxTotal`, `loading`, `error`, `hasLoaded`; `start()`, `appResumed()`,
`refresh(silent:catchUp:)` (dedupes concurrent reads), `markAllRead`, `deleteAll`, `delete`
(optimistic, restored on refusal), `markRead`.

## AccountEventChannel (`@MainActor @Observable`)

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

## UserStatusService (`@MainActor @Observable`)

`UserStatusService(api:relay:sessionToken:)`: `status`, `known`, `busy`, `signedIn`;
`syncSession`, `refresh`, `refreshIfStale(maxAge: 600)`, `setAway`,
`setCustom(emoji:text:expiresAt:)`, `clearCustom`. A lapsing status is dropped on time; the relay
(`OwnStatusAnnouncer`) announces changes.

## ChatRetentionService (`@MainActor @Observable`)

Per-account chat retention (`POST /api/accounts/chat-retention`: an empty body reads,
`{"defaultMs": n|null}` sets or clears the default, `{"chat": key, "retentionMs": n|null}` one
chat). `ChatRetention` clamps every value to [1 hour, 7 days] on read. The service reads on
sign-in, falls back to seven days on sign-out, applies changes optimistically with rollback, and
caches the last answer with the account name under `chat/retention`. `ChatInstaller` hands every
change to `ChatHub.applyRetention`.

## PushNotificationService (`@MainActor @Observable`)

The APNs side is a protocol the app implements (`PushPlatform`): permission status and request,
environment, token, pending open, badge, and shared-keychain credentials for the extension.
`registerForCalls` asks APNs for a token without asking permission: `start` uses it when the prompt
is declined or notifications are off in Settings, so the phone still registers its device and VoIP
tokens and calls ring through CallKit (`docs/app/chat-direct-calls.md`).

`start` (and so sign-in, resume and `rebind`) never shows the system prompt: while permission is
`notDetermined` it registers for calls only. The prompt comes from `start(accountKey:ask: true)`,
which `enable` (Settings' "Enable notifications", now offered while permission is not asked yet)
and `askWhenUseful` call. The app calls `askWhenUseful` after the first chat message is sent; it
does nothing unless push has started and permission is still `notDetermined`
(`docs/native/PERMISSIONS.md`).

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

## AgentListSignal (`@MainActor @Observable`)

`ring()` bumps `rings`; observe it to reload the Agents tab.

## App wiring

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
