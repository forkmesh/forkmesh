# FMCode

Part of [iOS core modules](README.md).
The Code area's core module, `ForkMeshCore/Sources/FMCode`, in three
parts: Repos (the network catalog and the repository screen), Collab (issues,
pull requests, discussions and the diff viewer) and Local (repositories on the
phone, with FMGit's engine). `import FMCode`.

## Repos: remote repositories

The remote-repository half of the Code area: the network catalog, the
repository detail screen (every tab except Issues, Pulls and Discussions,
which are FMCode/Collab's), file previews, commits, mirrors, releases, actions,
agents, worktrees, mirror import and the code editor. Core in
`ForkMeshCore/Sources/FMCode/Repos` (`import FMCode`), views in
`ForkMesh/Features/Code` (everything but `Collab/`).

Syntax highlighting renders FMDiff's `CodeHighlighter` tokens.

### Conventions

- View models are `@MainActor @Observable public final class`es taking a
  `RepoClient` (the ApiService calls they need; `extension ApiService: RepoClient`).
  Tests fake the protocol. Views build them from `RepoServices` (Features/Code/CodeFeature.swift).
- `RepoFuture<Value>` is a load-once future with a snapshot: `snapshot` (`loading`, `data`, `error`), starts out
  waiting, `start { }` loads once, `load { }` is a fresh future (keeps old data/error while
  waiting, newest wins), `resolve(_)`. `error` is the error text via `dartErrorText`.
- `RepoException(message)` prints "Exception: message".
- Colours are named, not held: `RepoBadgeTone` (success/danger/warning/accent/secondary/tertiary);
  views map with `fm.repoTone(_)`. Icons are Material names for `FmIcon`.
- `RepoDetailTab` is FMApi's (Models/NotificationDeepLink.swift); FMCode adds `tabLabel`,
  `tabCaption`, `tabFloatingActionLabel`.

### Core types

- `RepoDisplay`: `humanSize`, `fileGlyph`, `sourceLabel`, `badgeTone`, `actionStatusTone`,
  `agentStatusTone`, `baseName`, `previewKind` (`RepoPreviewKind`), `decodedText(blob)`,
  `dartLines` (LineSplitter), commit map readers (`commitHash/Short/Subject/Author/Body`,
  `commitDateMs`, `relativeCommitTime`, `relativeAge`, `agoLabel`, `shortHash`),
  `parsePatchFiles` -> `[RepoPatchFile]` (`RepoChangeKind`), `changedFilesTitle`, `diffLineTone`.
- `ReposCatalog` (`RepoFilter`, `RepoSort`, `RepoMetaItem`, `filteredAndSorted`, `metaItems`,
  `isOnline`) and `ReposScreenModel` (`catalog`, `queryText`, `filter`, `sort`, `visibleRepos`,
  `stateKey`, `emptyTitle/Message`, `start()`, `reload()`).
- `RepoDetailModel(repo:initialTab:initialTarget:)`: `tab`, `tabs`,
  `takeInitialDestination() -> RepoDetailDestination?` (`.issue(n)`, `.pull(n)`,
  `.discussion(n)`, `.commit(JSONObject)`), once.
- `RepoOverviewModel(repo:client:)` (the repository screen, [NAVIGATION.md](../NAVIGATION.md) §3.4):
  `tiles(canEditSettings:) -> [RepoOverviewTile]` (section, title, SF Symbol, count) in
  `order`, `counts`, `load()` (open issues and pulls from the published ledgers, branches, releases, commits over
  `RepoOverviewClient`, which ApiService conforms to; a failed read leaves no count),
  `adopt(catalog)` (the catalog record for a repository opened from a bare `RepoRef`),
  static `title(of:)`.
- `CurrentRepoMemory(store:)`: the repository in use for the shell's Code button
  (`code/current-repo`), `remember(owner:name:)`, `ref`.
- `DesktopReposModel(channel:desktopName:fallback:)`: the paired desktop's repositories over
  `repos.list` (`DesktopControlFamilyName.repos`): `repos: [DesktopRepo]` (running agents
  first, then by name, deduplicated), `desktopName`, `problem`, `start()` (once; reloads on
  `repos.changed`), `load()`, `stop()`. On any error it falls back to `fallback()` (the
  `agents.list` repositories of an older desktop), else sets `problem`.
- Code tab: `RepoCodeTabModel` (`tree`, `branches`, `path`, `selectedRef`, `openDir(_:refresh:)`,
  `switchRef`, `up`, `crumbs: [RepoCrumb]`, `branchNames`, `branchPillLabel`, `entryMeta`, `entryGlyph`),
  `RepoGoToFileModel` (`query`, `search()`, `results: RepoFuture?`), `RepoFileModel`
  (`blob`, `rawURL`, `editable`, static `preview(for:) -> RepoFilePreview`),
  `RepoCodePreview` (lines, lazily cached `pieces(i)`, `gutterWidth`, `estimatedWidth`, `statusLine`).
- Commits: `RepoCommitsTabModel` (`view: RepoGitView`, `commits`, `branches`, `refresh()`,
  `commitRows()`, `branchRows`, `cloneURL`), `RepoCommitDetailModel` (`detail`, `summary`,
  `changedFiles`, `review`: the loaded diff as a `DiffReviewModel` for `DiffReviewStream`), `RepoChangedFiles.forCommit/forPatch` (rows, totals).
- Tabs: `RepoMirrorsTabModel` + `RepoSyncStatusCard`, `RepoReleasesTabModel` + `RepoReleaseAction.all`,
  `RepoWorktreesTabModel`, `RepoActionsTabModel` / `RepoActionRunModel`,
  `RepoAgentsTabModel` / `RepoAgentSessionModel`.
- Owner signing: `RepoOwnerSigning.actionsReadProof` (forkmesh-actions-read-v1) and
  `desktopCommandProof` (forkmesh-desktop-command-v1) -> `RepoOwnerProof`; `RepoSigner(session:identity:clock:)`;
  `RepoDesktopCommands.request(...) -> String` (the inline notice text, errors included).
- `MirrorImportModel(client:session:)`: `url`, `token`, `problem`, `status`, `busy`,
  `submit() async -> String?` (notice to close with), static `validateSourceUrl`, `explain`, `providers`.
- `CodeEditing`: `caret(in:utf16Offset:) -> CodeCaret`, `detectIndentUnit`,
  `formatNewline(old:new:indentUnit:colonOpensBlock:)` over `CodeEditValue` (UTF-16 offsets),
  `keyBarSymbols`.

### App-target entry points (Features/Code)

- `ReposScreen()` (the desktop's repositories, the device's, then the catalog);
  `RepoDetailScreen(repo:initialTab:initialTarget:)` and `RepoDetailScreen(ref:)`, the one
  repository screen (header, actions, section grid with counts, README), which pushes
  `RepoSectionScreen(repo:section:ref:full:)` per section. `CodeCurrentRepo.open(_:)` pushes it
  on the current tab's router path; `CodeCurrentRepo.remember(_:)` records the repository in use.
- `RepoFileScreen(repo:path:ref:)`, `RepoCommitDetailScreen(repo:commit:)`.
- `MirrorImportSheet(onDone: (String?) -> Void)`: present as a sheet; `onDone` gets the notice
  (nil on Cancel); the presenter dismisses and shows the notice.
- `CodeEditorView(text: Binding<String>, language: CodeLang, indentUnit: String = "  ",
  caret: Binding<CodeCaret>? = nil, focused: Binding<Bool>? = nil, showsKeyBar: Bool = true)`:
  a TextKit 1 UITextView (no wrap, both-way scroll, gutter, current-line band, syntax colours,
  auto-indent on Return, CodeKeyBar as the keyboard accessory; undo is UITextView's).
- `CodeFeature.register()`: no services of its own.
- Shared view bits: `RepoInfoCard`, `RepoChip`, `RepoChipWrap`, `RepoWrap` (Layout),
  `RepoMobileCard`, `RepoSmallBadge`, `RepoLoadingCard`, `RepoErrorCard`, `RepoEmptyCard`,
  `RepoSourceChip`, `RepoDividedRows`, `repoCopy(label, value) -> FmNotice` (shown inline by the screen that copied).

### Surprises

- The Issues section's "New issue" button lives in RepoSectionScreen (pushes
  `NewIssueScreen(repo:)` after `AccountFeature.requireAccount`); the Pulls and Discussions pages
  carry their own. The repository screen's Issue action does the same.
- `RepoDetailTab` and `RepoDetailModel.tabs` outlive the tab strip: deep links still name a
  tab, which the repository screen maps to a `RepoSection` by raw value.
- A worktree chip with a sha shorter than 7 shows it whole.
- "Edit this file" on RepoFileScreen is not wired: QuickEditScreen has no seam yet.
- RepoFileScreen's lines sit in a both-way ScrollView, which centres smaller content on both
  axes; the line column is at least the viewport's height and top-aligned, so line 1 sits right
  under the path header.
- The inline commit diff has no "Open in diff viewer" button until a full-screen diff seam
  (PatchFilesScreen) exists.

## Collab: issues, pull requests, discussions and diffs

Issues, pull requests, discussions, the fediverse thread, and the diff viewer,
in `ForkMeshCore/Sources/FMCode/Collab` (logic, view models) and
`ForkMesh/Features/Code/Collab` (SwiftUI). `import FMCode` (plus `FMApi` for
the models, `FMDiff` for `DiffFile`). The collaboration half of the repository
screen, the new-issue screen, the branch diff and the diff viewer; after
native/android `collab/*.kt`.

### Views (app target, the seams)

- `RepoIssuesTab(repo:)`, `RepoPullsTab(repo:)`, `RepoDiscussionsTab(repo:)`: one page each of
  RepoDetailScreen. Pulls and Discussions carry their own "New PR" / "New discussion" floating
  buttons (sheets `NewPullSheet(repo:)`, `NewDiscussionSheet(repo:)`); the Issues "New issue"
  button is RepoDetailScreen's.
- `IssueDetailScreen(repo:number:)`, `PullDetailScreen(repo:number:)`,
  `DiscussionDetailScreen(repo:number:)`: read the published list and find the record; a
  number that is not there shows "Issue #N has not been published yet." (an issue number that
  is a pull request shows that pull request instead, [docs/native/REFERENCE_LINKS.md](../REFERENCE_LINKS.md); a failed read shows
  the error), never a crash. Extra inits `(repo:issue:)`, `(repo:pull:)`, `(repo:discussion:)`
  for callers already holding the record (no loading state); `(repo:pull:)` still reads the full
  record behind it, because the list rows carry no `changes.patch` or `commits.mbox`.
- The pull request page follows GitHub's order, top to bottom: the header (the Open / Merged /
  Closed pill from `PullState`, `#N`, the "Ann wants to merge 3 commits into main from fix/a"
  line from `PullState.summary`, head → base branch labels), the changed files card (up to eight
  rows and "Review changes", which pushes `PullChangesScreen`), the Conversation card (the
  description as the opening comment, then `PullTimeline.events`), the federated replies, the
  merge card, the review card and the desktop requests ("On the desktop": only the requests that
  fit the pull's state, `PullShipCommands.offered`, as short tone chips with the full label as
  the accessibility label). Pull to refresh re-reads the record; a merge from the phone
  refreshes it too. The buttons are the compact tone chips of the agent session's "On the
  desktop" card (`AgentChipRow` + `FmToneButton`).
- The merge card of an open pull holds `Merge pull request` and `Merge & delete` (exact pinned
  commits; `Merge & delete` deletes the head branch in the same merge transaction). A merged
  pull shows "Pull request successfully merged and closed" there instead, and a closed one
  "Closed with unmerged commits" (`PullDoneCard`), so nothing on the page suggests the pull can
  still be merged. The review card then keeps only Comment and the thread subscription:
  `PullState.reviewable` is false once a pull is merged or closed.
- The timeline gets a "Merged" entry for a merged pull (`PullTimeline.mergedEvent`), built from
  `pull.md`'s `status`, `mergedAt` and `mergedBy`, placed by time among the events (signed events
  carry seconds or milliseconds, `PullTimeline.epochMs`), or last when `mergedAt` is missing. A
  mesh merge (`server/merge.go`) writes no `mergedBy`, so that entry names no one.
- `NewPullSheet(repo:head:)` is the GitHub compare form: Base and Compare branch menus (from
  `branches`), a live comparison on the mirror (`compare`: commit count and the changed files
  card), Title (filled from the branch name when empty) and Description. "Paste a patch
  instead" keeps the pasted-diff form for this repository. When the repository has saved
  forks on other instances (`remote-forks`, [docs/app/remote-forks.md](../../app/remote-forks.md)), a **Send to** menu
  lists them: the pull is then signed for the target's owner and name, its head is
  `<this host>/<owner>/<repo>:<branch>`, and it is posted straight to the target's
  `/api/repo/<owner>/<repo>/pulls` with no session (`InboxService.openRemotePull`), the same
  request the website's Send to makes. Model: `NewPullModel` and `PullCompose` in
  `FMCode/Collab/PullCompose.swift`.
- `PullChangesScreen(repo:pull:files:commits:scope:)`: the pull's own patch in
  `DiffReviewStream` (viewed state per file is kept under `owner/name/pull/<n>`), with a Commits
  tab when the pull carries a `commits.mbox`; each commit opens `PatchCommitScreen`. An open
  branch-backed pull (`derive: branch`) also offers "Edit file" / "View file" on its head branch
  and "Compare branches", which opens `BranchDiffScreen` for inline line comments. A
  patch-backed pull has no head branch on the mirror, so it offers neither.
- `NewIssueScreen(repo:parentNumber: = 0)`: a pushed page; submits to the signed inbox itself,
  closes, and posts its notice to the screen below. Gate it first with
  `AccountFeature.requireAccount(action: "open an issue")` (RepoDetailScreen does).
- `BranchDiffScreen(repo:base:head:title: = "")`: base "" = the repo's default branch (else "main").
- `DiffViewer(diff: String, title: String = "Diff")` (also `DiffViewer(diff: [DiffFile], title:)`):
  the inline diff preview (coloured lines, max 420pt) plus "Open in diff viewer". Must be
  inside a NavigationStack (it pushes). There is no `DiffViewer(diff: UnifiedDiff)`:
  `UnifiedDiff` is a namespace.
- `PatchFilesScreen(title:patch:)`: any unified diff in the full viewer (file list, viewed
  progress, pager). Also `DiffFilePagerScreen(files:initialIndex:viewed:subtitle:)`,
  `DiffFileBody(file:)`, `DiffFileList(files:viewed:subtitle:emptyMessage:header:)`,
  `PatchCommitScreen(repo:commit:files:)`, `DiffStatSummary(additions:deletions:showBar:)`,
  `DiffViewMenu(extra:)`, `PatchFilesCard(files: [PatchFile], commits: Int = 0, footer: String = "")`
  (a footer caps the card at eight rows and adds the "Review changes" row),
  `PullBranchRow(pull:)`, `FmToneLabel(label:icon:tone:)` (an `FmToneButton` face for a
  `NavigationLink` label), `DiffFileDestination(repo:route:)`.
- Shared bits other features may reuse: `CollabInfoCard`, `CollabChips`, `CollabRow`,
  `CollabEmptyCard`, `CollabWrap` (a wrapping `Layout`), `CollabIdentityBar(verb:)`
  (ComposeIdentityBar), `ThreadSubscriptionButton(repo:source:number:)`,
  `FederatedRepliesCard(repo:kind:number:)`.
- Services: `Services.require(ApiService.self)`, `Services.get(InboxService.self)` (nil → writes
  fail with "Failed: Exception: This device has no signing identity."), `AuthService`,
  `DeviceIdentity`, `AccountFeature.requireAccount/isGuest`. Result sentences go through
  `CollabMessenger.shared` (`notice: FmNotice?`) and show as `CollabNoticeLine()`, an inline line in the
  flow of each collaboration screen (under the header or tab picker), never a toast. The issue board
  shows its move result the same way under its column picker.

### Core (FMCode/Collab)

- Protocols `CollabAPI` (ApiService conforms: published lists, federatedReplies, issueBountyStatus,
  desktopCommand, compare, canMergePulls, pullMergePlan, mergePull) and `CollabInbox`
  (InboxService conforms: the ten signed writes). Tests drive fakes.
- View models (`@MainActor @Observable`): `RepoIssuesModel` (filters, `visible`, `countLabel`,
  `options/selection/pillLabel/isActive/pillEnabled`, `select/setStatus/clearMetadata`; filters
  kept per repo in `IssueFilterMemory` for the app's lifetime), `RepoPullsModel` (`status`, a
  `PullStatusFilter` that starts on Open; `visible`, `count(_:)`, `noMatch`. Closed covers merged
  and closed, so the default list holds only the pulls still waiting on someone),
  `RepoDiscussionsModel`, `IssueDetailModel` (parents/children/mentions/timeline across all
  issues, `refreshBounty`), `ThreadSubscriptionModel`, `PullDetailModel` (`request(command)` signs
  `forkmesh-desktop-command-v1\n{owner}\n{name}\n{command}\n{target}\n{ts}`), `PullMergeModel`,
  `PullReviewComposerModel`, `DiscussionDetailModel`, `FederatedRepliesModel`, `NewIssueModel`,
  `NewPullModel`, `NewDiscussionModel`, `BranchDiffModel`, `DiffPagerModel`, `DiffViewedSet`,
  `DiffViewSettings.shared` (split/wrap/syntax/fontSize 9...18, session-wide).
- `CollabWriter(repo:inbox:)`: vote, comment, toggleStatus, submitIssue, commentOnPull, review,
  submitPull, submitDiscussion, reply, each answering a `CollabNotice` ("Submitted to the
  inbox - pending the repo owner applying it." / "Failed: $e"); blank text sends nothing (nil).
- Copy and rules as namespaces: `CollabCopy`, `ThreadSubscription`, `IssueCard`, `IssueDetail`,
  `BountyFunding`, `SubIssues`, `IssueTimeline`, `IssueActions`, `PullCard` (the compact Pulls
  row: `meta` = number, author, age, comment count and patch `diffStat`; `branches`, `snippet`,
  latest `review` verdict), `PullDetail`,
  `PullShipCommands`, `DesktopCommand`, `PullTimeline`, `PullReview`, `PullMerge`,
  `NewPullComposer`, `PullState`, `PullCompose`, `NewPullModel`, `DiscussionCard`, `DiscussionDetail`, `NewDiscussionComposer`,
  `FederatedThread` (`canOpenBacklink`: https, host, no user info), `NewIssueComposer`,
  `BranchDiff`, `DiffFileListLogic`, `DiffFileRowText`. Free functions: `issueMatchesFilters`,
  `issueFilterValues/Options`, `normalizeIssueFilters`, `formatUsd`, `issueStatusText`,
  `parsePatchFiles`, `dartLines`, `changedFilesTitle`, `diffStatBar`, `applyDiffEmphasis`,
  `diffAge`, `collabBadgeTone`.
- `IssueTaskMove`: `taskBody` and `move` (create the task, send the signed issue delete, trash the
  task again if the delete fails), behind **Move to task** in `IssueToolsMenu`; see
  [docs/app/issue-to-task.md](../../app/issue-to-task.md).
- `DiffFileLayout(file)`: prepared file, `unifiedRows`/`splitRows`, `hunkLabel` ("⋯ 9 unchanged
  lines"), `notices`, `runs(line, syntax:)` (highlighted pieces with intra-line marks, cached).

### Surprises

- `PatchPreviewLineKind` is the raw-line tint of the inline preview; FMDiff's `DiffLineKind` is
  the parsed one. `PatchChangeKind` stands in for Kit's `FmChangeKind` in the core.
- The composers send, then close.
- Diff canvas widths use Menlo's 0.602 em advance instead of measuring text.
- The unwrapped canvas is a both-way ScrollView, which centres content smaller than itself on
  both axes; the canvas is at least its viewport's height and top-aligned, so a diff of a few
  rows starts under the notices instead of mid-page.

## Local: on-device Git

`ForkMeshCore/Sources/FMCode/Local`: repositories on the phone: create, import, clone, browse,
edit, commit, branch, publish, push/fetch/sync and pull requests. `import FMCode` (re-import
`FMGit` for `GitErrorCode`, `FMApi` for `ApiService`/`InboxService`/`Repository`, `FMAccount` for
`AuthSession`). Mirrors and clone
failover follow [docs/mirroring.md](../../mirroring.md).

Files: GitWorkspace.swift, GitWorkspaceOperations.swift, LocalGitTypes.swift,
GitOperationRunner.swift, LocalRepoStore.swift, ForkMeshRemote.swift,
ForkMeshSigningTransport.swift, the publish and pull request services, BlobId.swift, the screen
view models (LocalRepoScreenModel.swift, LocalReposSectionModel.swift, QuickEdit.swift), and a zip
reader for imports (LocalZipArchive.swift, system zlib via CZlib).

**The engine layer** (GitRepository, remote operations, RepositoryConfig, the transport protocol)
is public API of FMGit (`FMGit/Engine/`). Only the HTTP transport lives here
(`NetHttpTransport.swift`, `GitNetHttpTransport`), because it runs over FMNet's `HTTPClient` (no
redirects, bounded bodies). The first `GitWorkspace` registers it as `GitDefaultHttpTransport`;
with none registered FMGit fails every request as `networkUnavailable`.

### GitWorkspace

One per repository, shared between surfaces (the repository screen and the coding agent). Every
call is `async throws`, runs off the main actor, is serialized on the workspace's
`GitOperationRunner`, and throws `GitWorkspaceException` (`message` to show, `code:
GitErrorCode?`, `detail`).

```swift
let workspace = store.workspace(record)          // or GitWorkspace(worktree: url, performanceMonitor: monitor)
defer { workspace.dispose() }                    // the owner disposes; cancel() stops the running op
try await workspace.initialize(files: ["README.md": "# x\n"], commitMessage: "Initial commit\n", author: author) -> String?
try await workspace.listFiles("dir") -> [LocalFileEntry]          // "" = root; directories first
try await workspace.readFile("a/b.txt") -> LocalFileContent        // isEditable / isBinary / isTooLarge (2 MiB)
try await workspace.writeFile("a/b.txt", text); deleteFile(path)   // `.git/...` refused
try await workspace.status() -> LocalStatus                        // staged / unstaged / untracked
try await workspace.stage([paths]); stageAll(); unstage([paths]); unstageAll(); discard([paths])
try await workspace.commit(message:, author:) -> String            // hex id
try await workspace.branches() -> [LocalBranch]
try await workspace.createBranch("feature/x", checkout: true); checkout("main")
try await workspace.log(limit: 200) -> [LocalCommit]               // newest first, max 500
try await workspace.summary() -> LocalRepoSummary
try await workspace.importFromDirectory(url, author:, message:) / importFromZip(path, author:, message:)
try await workspace.push(credentials:, branch:, onProgress:) -> LocalPushResult   // never forces or deletes
try await workspace.fetch(credentials:, onProgress:) -> LocalFetchResult          // refs/remotes/origin/*
try await workspace.clone(credentials:, branch:, depth:, maxPackBytes:, onProgress:) -> LocalCloneResult
try await workspace.pull(credentials:, branch:, onProgress:) -> LocalPullResult   // Sync: fetch, then fast-forward
try await workspace.syncState(branch:) -> LocalSyncState                          // "2 to push · 1 to fetch"
```

- `onProgress` is called off the main actor; `GitTransferProgress.label` is the busy-overlay
  sentence, with object and file counts during indexing and checkout.
- Cancellation is cooperative: `cancel()` trips the engine's token and cancels the task; the call
  fails with `code == .cancelled`. A disposed workspace refuses work the same way.
- The class is open for tests (override `push`/`fetch`/`clone` to script the network).
- `GitAuthor.fromAccount(nodeName:email:)` strips `<>` and control characters, falling back to
  `forkmesh` / `<name>@mobile.forkmesh`.

### LocalRepoRecord / LocalRepoStore

`LocalRepoRecord`: `name`, `path`, `description`, `createdAt`, `branch`, `lastCommitSubject`,
`lastCommitAt`, `publishedAs` ("owner/name"), `origin` (`.authored` / `.cloned`), `syncLabel`;
JSON in the `index.json` shape (UTC ISO-8601 dates).

`LocalRepoStore` (`@MainActor @Observable`), rooted at `<Documents>/forkmesh/repos/` for the legacy
project and `<Documents>/projects/<slug>/forkmesh/repos/` for others:

- State: `records` (most recent first), `isLoaded`, `loadError`, `isBusy`, `revision`,
  `meshIdentities` (lower-cased `owner/name` of published or cloned records).
- `ensureLoaded`, `list`, `find`, `workspace(record)`, `create`, `importDirectory`, `importZip`,
  `cloneFromMesh`, `refresh`, `updateSummary`, `markPublished`, `delete`, `cancelActive`. A failed
  create, import or clone removes its folder.
- One copy per mesh repository: `cloneFromMesh` refuses an `owner/repo` already bound to a record
  ("… is already on this device as <name>. Open it and sync instead.").
- `sync(record, publisher:)` runs `PublishService.pull` (with mirror failover) on the record's
  current branch; unpublished records and detached HEADs are refused.
- Loading rebases stale absolute paths, drops vanished worktrees, adopts `<root>/<name>/.git`
  folders the index forgot, and rewrites the index.
- `relocate(to:)` moves every worktree and the index to another folder (below).

#### Files app access and a folder that outlives the app

`UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` make the app's Documents folder
visible in Files; only repositories (and "ForkMesh from desktop") are kept there. Keys,
preferences, chat history and caches live in the keychain, UserDefaults and Application Support.

Because iOS erases the app's own folder on delete, the user can choose another folder
(`LocalRepoFolderSetting.choose`): folders inside the app container or in iCloud Drive are refused,
a `.minimalBookmark` is saved under `localRepos.folderBookmark`, and the store relocates.
`relocate(to:)` refuses while an operation runs or when the target is inside the current root,
writes the merged `index.json` first (so an unwritable folder fails before anything moves), renames
each worktree across, and leaves name collisions in the old root. At launch an unresolvable
bookmark leaves the store in the app's folder and shows the folder as unreachable. After a
reinstall, choosing the same folder reloads its `index.json`.

### Remote, publish, pull requests

- `ForkMeshRemoteCredentials(origin:owner:repo:seed:pubkeyB64url:)`: every engine request to the
  relay is signed (`x-forkmesh-pubkey/ts/sig/body-sha256`) when `canSign`;
  `describeRemoteFailure` maps errors to codes such as `push_too_large` and `push_unauthorized`.
- `PublishService(api:identity:prefs:clock:scope:)`: credentials, push, fetch, `publish(...)` (a
  push failure throws; the catalog answer is a `PublishOutcome`), catalog record building, name
  validation.
- `PullRequestService`: `prepare(workspace:record:headBranch:baseBranch:)` diffs from the merge
  base and reports blockers, changed and binary paths; `submit(...)` pushes first.
- `GitBlobId.of(bytes)`, `LocalPatch.forFile(...)` (mode-less `index` line; refuses binary and
  non-UTF-8).
- Quick edits use the raw file response as the patch pre-image. A tree listing can lag that
  response, so its blob id never prevents a person from preparing a federated pull request.

### Clone to this device: mirrors, failover, shallow

- `CloneToDeviceModel.clone` clones by the repository's public name, never by the serving node.
  Mirrors come from `GET /api/repo/<owner>/<repo>/mirrors` (`ApiService.freshMirrorSet`).
  `CloneEndpoints.plan` keeps credential-free `https://` mirrors that are not abuse-blocked and
  are online or clone-available, healthy first (online, integrity ok, synced, endpoint healthy and
  fresh), then lower latency and newer sync; the relay route `{origin}/{owner}/{repo}` is last.
- `CloneEndpoints.run` tries sources in order; cancellation, a size refusal and a local store
  refusal stop at once, anything else moves on. `CloneFailure.message` lists every source and
  reason.
- Mirror sources use anonymous credentials (the device key signs only relay requests). After a
  mirror clone, `origin` is set to the relay URL so push and fetch go there.
- Mesh clones are shallow and single-branch (`depth 1` of the default branch, no tags).
  `GitWorkspace.deviceLimits` uses the engine hard maxima: 1 GiB pack, 1 GiB inflated, 256 MiB
  per object, 30 s connect, 60 s idle read, 20 minute deadline. FMGit records the boundary in
  `.git/shallow`; later fetches send `shallow` lines. History is fetched only when a push needs it.
- Untrusted packs are screened with SHA-1DC (`GitSha1DCDetector`, vendored `Sources/CSha1DC`, MIT).

### Engine performance and safety

- `GitPackObjectStore` (FMGit) reads objects on demand: the `.idx` gives the offset, the pack is
  memory-mapped, delta chains (up to 4096 deep) resolve against a 32 MiB delta-base cache, and
  each object is hashed (and screened when a detector is set) and compared with the requested id.
- Checkout, clone and sync write only changed paths (index and refs are flushed), delete removed
  paths including symlinks, and prune emptied folders.
- Checkout, clone and fast-forward refuse any tree entry named `.git` in any letter case at any
  depth (`pathEscapesWorktree`), as `git` does, so a repository cannot write into its own `.git`
  directory. The check is in `collectTreeEntries` of both engines (FMGit and the Kotlin
  `GitRepository.kt`); tests: `testCheckoutRefusesATreeThatWritesIntoTheGitDirectory`
  (`LocalEngineWorkflowTests`) and `GitCheckoutTest`.
- `ForkMeshSha1` runs on the vendored sha1dc core with collision detection off for plain hashing.
- `status` still reads every tracked file (no stat cache).

### View models and app target

- `LocalRepoScreenModel`: tabs, files, status, branches, log, busy label, mesh banner, sync,
  commit, publish, push, fetch, Sync, delete; disposes its workspace on deinit.
- `LocalReposSectionModel`: create, import, `syncAll()` with a summary notice.
- `CloneToDeviceModel`, `LocalFileEditorModel`, `QuickEditModel`, `LocalRepoText`, `LocalNotice`
  (inline messages, never toasts).
- `LocalReposFeature.register()` creates `LocalRepoStore` and `PublishService` and registers them
  in `Services`; call it before the Agents feature. Views: `LocalReposSection`, `LocalRepoScreen`
  (opens `AgentScreen` with its own workspace), `CloneToDeviceButton`, `QuickEditScreen`,
  `LocalFileEditorScreen`, `PublishRepositorySheet`. The `.localRepos` route opens
  `LocalReposRouteScreen`. The network catalog badges repositories present on the device.

### Projects and serving as a node

- `LocalRepoScope`: `projectId`, `namespace`, `key(_:)`, `directory(_:)`, `isLegacy`;
  `LegacyLocalRepoScope()` and `.project(id)`.
- `LocalNodeSyncAdapter.swift`: `LocalNodeSyncRepositories(store:)` and
  `LocalNodeSyncWorkspace(_:)` implement the node-sync contract in [FMChat](FMChat.md).
  `LocalNodeSyncApplier.forLocalRepos(store, prefs:scope:account:)` builds the applier with the
  account as commit author; `installOn(nodeSync)` lets the phone acknowledge rows for repositories
  it holds.

### Limits

Imports refuse ZIP64 and encrypted zips. A fetch to the relay that is answered with a redirect to a
mirror is not followed by the engine.
