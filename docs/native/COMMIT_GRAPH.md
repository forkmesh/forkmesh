# Commit graph (iOS Commits screen, Graph tab)

Requested 2026-10-02: a third tab on a repository's Commits screen, beside
History and Branches, that draws the git history as a lane graph like the
desktop's, and marks the commit each ForkMesh app is running: Desktop, Web and
Mobile.

## What it shows

- One row per commit from `GET /api/repo/<owner>/<name>/history` (the last 60
  commits of the default branch, or of the branch the screen was opened on).
  The server already sends `parents` as a space-separated string.
- The gutter is the desktop's `desktop/src/CommitGraph.h` painter ported to a
  SwiftUI `Canvas` (`RepoGraphGutter` in
  `native/ios/ForkMesh/Features/Code/RepoGraphView.swift`): the same six lane
  colours, lanes merging into a node curve in above it, new parent lanes curve
  out below it, a merge is a ring with a centre dot. The lanes come from
  `CommitGraph.layout` (`FMCode/Collab/CommitGraph.swift`, at most six lanes).
  Unlike the desktop, a merge's second parent that already has a lane also gets
  a curve from the node to that lane, so the merge reads as joined.
- A commit an app is running gets a ring in that app's colour and a chip under
  its subject (Desktop violet, Web green, Mobile orange).
- A "Where each app is" card at the top lists the three apps with the short
  commit and how far behind the tip it is. "Behind" counts the listed commits
  that are not ancestors of the marked commit, so commits on a merged branch
  count once and the number matches `git rev-list <mark>..<tip>` within the 60
  shown. Tapping a row scrolls to its commit. An app whose commit is older than
  the 60 commits says so; one that did not report a commit says "Not reported".
- The card only shows when at least one mark lands in the list, or when the
  repository is named `forkmesh`, so other repositories do not get a card of
  three "Older than the commits shown" lines.

## Where each commit comes from

| App | Source | Notes |
|---|---|---|
| Mobile | `FORKMESH_BUILD_COMMIT` in the app's Info.plist, read into `AppBuild.commit` | `Config/Version.xcconfig` holds an empty `FORKMESH_BUILD_COMMIT`; the shared Codemagic `native_project` step (used by `ios-native-build`, `ios-native-release` and `ios-native-adhoc`) writes `git rev-parse HEAD` into it. Builds from before this change, and local Xcode builds, report nothing. |
| Web | `rev` from `GET /api/version` on the account's server (`ApiService.serverVersion`) | `BUILD_REV`, 12 hex characters, with `-dirty-<ts>` on a dirty deploy; only the leading hex is matched. `dev` matches nothing. |
| Desktop | `commit` in the desktop's local-control `status` reply (`forkmesh::desktopBuildCommit()`) | Asked over the paired desktop's LAN or relay link with an 8 s timeout, only when the Graph tab opens or is refreshed. A desktop built before this change sends no `commit`. A dirty build reports `dirty`, which matches nothing. |

The desktop reads `FORKMESH_BUILD_COMMIT` through `DesktopBuildCommit.h`, defined
in `MainWindowHostInstall.cpp`, which already includes the generated
`ForkMeshBuildCommit.h`. `MainWindowLocalControl.cpp` therefore does not
recompile on every commit (see the rebuild-cost note in `desktop/CMakeLists.txt`).

A commit matches a row when either is a prefix of the other and both are at
least seven characters long (`RepoSurfaceMark.matches`).
