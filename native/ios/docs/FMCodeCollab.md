# FMCode / Collab

Issues, pull requests, discussions, the fediverse thread, and the diff viewer,
in `ForkMeshCore/Sources/FMCode/Collab` (logic, view models) and
`ForkMesh/Features/Code/Collab` (SwiftUI). `import FMCode` (plus `FMApi` for
the models, `FMDiff` for `DiffFile`). The collaboration half of the repository
screen, the new-issue screen, the branch diff and the diff viewer; after
native/android `collab/*.kt`.

## Views (app target, the seams)

- `RepoIssuesTab(repo:)`, `RepoPullsTab(repo:)`, `RepoDiscussionsTab(repo:)`: one page each of
  RepoDetailScreen. Pulls and Discussions carry their own "New PR" / "New discussion" floating
  buttons (sheets `NewPullSheet(repo:)`, `NewDiscussionSheet(repo:)`); the Issues "New issue"
  button is RepoDetailScreen's.
- `IssueDetailScreen(repo:number:)`, `PullDetailScreen(repo:number:)`,
  `DiscussionDetailScreen(repo:number:)`: read the published list and find the record; a
  number that is not there shows "Issue #N has not been published yet." (an issue number that
  is a pull request shows that pull request instead, `docs/native/REFERENCE_LINKS.md`; a failed read shows
  the error), never a crash. Extra inits `(repo:issue:)`, `(repo:pull:)`, `(repo:discussion:)`
  for callers already holding the record (no loading state); `(repo:pull:)` still reads the full
  record behind it, because the list rows carry no `changes.patch` or `commits.mbox`.
- The pull request page, top to bottom: the header (status chips, head → base branch labels,
  author and age, description), the changed files card (up to eight rows and "Review changes",
  which pushes `PullChangesScreen`), the phone merge card, the review card (Approve, Request
  changes and Comment as `FmToneButton`s, plus the thread subscription), the desktop requests
  ("On the desktop": only the requests that fit the pull's state, `PullShipCommands.offered`,
  as short tone chips with the full label as the accessibility label), the timeline and the
  federated replies. Pull to refresh re-reads the record; a merge from the phone refreshes it too.
  The buttons are the compact tone chips of the agent session's "On the desktop" card
  (`AgentChipRow` + `FmToneButton`), not the full-width outlined pills the page had before.
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
  inside a NavigationStack (it pushes). The seams.md `DiffViewer(diff: UnifiedDiff)` cannot exist:
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
  flow of each collaboration screen (under the header or tab picker), never a toast: the user asked on
  2026-10-09 for no toasts anywhere. The issue board shows its move result the same way under its
  column picker.

## Core (FMCode/Collab)

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
  `NewPullComposer`, `DiscussionCard`, `DiscussionDetail`, `NewDiscussionComposer`,
  `FederatedThread` (`canOpenBacklink`: https, host, no user info), `NewIssueComposer`,
  `BranchDiff`, `DiffFileListLogic`, `DiffFileRowText`. Free functions: `issueMatchesFilters`,
  `issueFilterValues/Options`, `normalizeIssueFilters`, `formatUsd`, `issueStatusText`,
  `parsePatchFiles`, `dartLines`, `changedFilesTitle`, `diffStatBar`, `applyDiffEmphasis`,
  `diffAge`, `collabBadgeTone`.
- `DiffFileLayout(file)`: prepared file, `unifiedRows`/`splitRows`, `hunkLabel` ("⋯ 9 unchanged
  lines"), `notices`, `runs(line, syntax:)` (highlighted pieces with intra-line marks, cached).

## Surprises

- `PatchPreviewLineKind` is the raw-line tint of the inline preview; FMDiff's `DiffLineKind` is
  the parsed one. `PatchChangeKind` stands in for Kit's `FmChangeKind` in the core.
- The composers send, then close.
- Diff canvas widths use Menlo's 0.602 em advance instead of measuring text.
- The unwrapped canvas is a both-way ScrollView, which centres content smaller than itself on
  both axes; the canvas is at least its viewport's height and top-aligned, so a diff of a few
  rows starts under the notices instead of mid-page.
