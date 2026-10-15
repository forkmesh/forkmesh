# FMCode / Repos (remote repositories)

The remote-repository half of the Code area: the network catalog, the
repository detail screen (every tab except Issues, Pulls and Discussions,
which are FMCode/Collab's), file previews, commits, mirrors, releases, actions,
agents, worktrees, mirror import and the code editor. Core in
`ForkMeshCore/Sources/FMCode/Repos` (`import FMCode`), views in
`ForkMesh/Features/Code` (everything but `Collab/`).

Syntax highlighting renders FMDiff's `CodeHighlighter` tokens.

## Conventions

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

## Core types

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
- `RepoOverviewModel(repo:client:)` (the repository screen, docs/native/NAVIGATION.md §3.4):
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

## App-target entry points (Features/Code)

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

## Surprises

- The Issues section's "New issue" button lives in RepoSectionScreen (pushes
  `NewIssueScreen(repo:)` after `AccountFeature.requireAccount`); the Pulls and Discussions pages
  carry their own. The repository screen's Issue action does the same.
- `RepoDetailTab` and `RepoDetailModel.tabs` outlive the tab strip: deep links still name a
  tab, which the repository screen maps to a `RepoSection` by raw value.
- A worktree chip with a sha shorter than 7 shows it whole.
- "Edit this file" on RepoFileScreen is not wired: QuickEditScreen has no seam yet.
- RepoFileScreen's lines sit in a both-way ScrollView, and SwiftUI centres content smaller than
  such a scroll view on both axes, so a file of a few lines floated mid-screen. The line column
  is at least the viewport's height and top-aligned, so line 1 sits right under the path header.
- The inline commit diff has no "Open in diff viewer" button until a full-screen diff seam
  (PatchFilesScreen) exists.
