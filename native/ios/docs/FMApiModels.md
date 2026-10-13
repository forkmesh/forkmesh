# FMApi models

The wire models (repositories, chat, notifications, agents, orgs, user status,
agent launch, deep links) in `ForkMeshCore/Sources/FMApi/Models`. `import FMApi` (and `FMBase` for
`JSONObject`). Only FMBase is used; no ApiService here.

## Conventions

- Every model is a `public struct` (Equatable, Hashable, Sendable; Identifiable where it has an id) with `var` fields, a public memberwise `init` with defaults, and `init(json:)`.
- `init(json: JSONObject)` for an object; `init(json: JSONValue?)` where the input may be any JSON value (`RepoTree`, `RepoBlob`, `RepoCommitDetail`, `RepoComparison`, `RepoSearchResults`, `BountyWallet`, `IssueBounty`, `UserStatus`). `SponsoredAd.parse(_ JSONValue?) -> SponsoredAd?` (nil when it is not an ad).
- Parsing uses `ModelJSON`, not FMBase's lenient accessors: `ModelJSON.int` takes an integer as is, truncates any other number, else parses the text as an integer, else 0 ("0x1F" is 31, "1.5" is 0), `ModelJSON.double`, `ModelJSON.bool` ("yes"/"1"/"true"), `ModelJSON.string`, `ModelJSON.firstString(json, keys)` (trimmed), `ModelJSON.coalesce(a, b, ...)` (the first non-nil), `ModelJSON.epochMs`. `DartCore` holds `intTryParse`, `doubleTryParse`, `dateTimeTryParseMs(text, timeZone:)`, `toStringAsFixed` (exact, half away from zero), `encodeComponent`/`decodeComponent`, `toString` (null prints "null"). Use these in ApiService where a service parses by hand.
- All integers are `Int` (64-bit), timestamps are epoch-ms `Int`. `toJSON() -> JSONObject` exists only where a model is written back (`UserStatus`).
- Anything that read the local clock or zone takes it as a parameter with a default: `AgentSession(json:now:)`, `rateLimitCountdownLabel(now:)`, `createdDate(timeZone:)` (plus a `createdDate` property for `.current`), `RepoTreeEntry(json:timeZone:)`, all of UserStatus's time functions.

## Types (by Swift file)

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

## Surprises

- `Issue(json:)` reads a non-list `labels`/`assignees` as empty. List entries print as text (null is "null").
- A ten-digit numeric string such as "1700000000" in agent-launch timestamps parses as a compact ISO date (year 170000); numbers are fine.
- Status colours are not in the models (the screens pick them from the label text); they live with the views.
- Regexes use NSRegularExpression with ASCII `\w`/`\d`/`\b` spelled out; `issueMentionsOf` sorts stably.
