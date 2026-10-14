# FMCode / Local (on-device Git)

`ForkMeshCore/Sources/FMCode/Local`: repositories on the phone — create, import, clone,
browse, edit, commit, branch, publish, push/fetch and pull requests. `import FMCode`
(re-import `FMGit` for `GitErrorCode`, `FMApi` for `ApiService`/`InboxService`/`Repository`,
`FMAccount` for `AuthSession`).

Files: GitWorkspace.swift, GitWorkspaceOperations.swift, LocalGitTypes.swift, GitOperationRunner.swift,
LocalRepoStore.swift, ForkMeshRemote.swift, ForkMeshSigningTransport.swift, the publish and pull
request services (LocalPatch in PullRequestService.swift), BlobId.swift, the view logic of the local
repository screens, clone to device and quick edit (LocalRepoScreenModel.swift,
LocalReposSectionModel.swift, QuickEdit.swift), and a zip reader for imports (LocalZipArchive.swift,
system zlib via CZlib).

**The engine layer** (GitRepository, remote
operations, RepositoryConfig, the transport protocol) is public API of FMGit, in
`FMGit/Engine/`. Only the HTTP transport stays here (`NetHttpTransport.swift`,
`GitNetHttpTransport`), because it runs over FMNet's `HTTPClient` (no redirects, bounded
bodies; TLS challenge/pinning hooks are not expressible there). The first `GitWorkspace`
registers it as `GitDefaultHttpTransport`, the transport an engine request without one uses;
with none registered FMGit fails every request as `networkUnavailable`.

## GitWorkspace (`public class`, `@unchecked Sendable`)

One per repository; share it between surfaces (the repository screen and the coding agent do).
Every call is `async throws`, runs off the main actor on a detached task, is serialized on the
workspace's `GitOperationRunner`, and throws `GitWorkspaceException` (`message` = sentence to
show; `code: GitErrorCode?`; `detail` = engine message, or the remote code such as
`push_too_large` for network operations).

```swift
let workspace = store.workspace(record)          // or GitWorkspace(worktree: url, performanceMonitor: monitor)
defer { workspace.dispose() }                    // the owner disposes; cancel() stops the running op
workspace.worktree / .path / .isBusy
try await workspace.initialize(files: ["README.md": "# x\n"], commitMessage: "Initial commit\n", author: author) -> String?
try await workspace.listFiles("dir") -> [LocalFileEntry]          // "" = root; directories first
try await workspace.readFile("a/b.txt") -> LocalFileContent        // isEditable / isBinary / isTooLarge (2 MiB)
try await workspace.writeFile("a/b.txt", text); deleteFile(path)   // `.git/...` refused
try await workspace.status() -> LocalStatus                        // staged / unstaged / untracked: [LocalChange]
try await workspace.stage([paths]); stageAll(); unstage([paths]); unstageAll(); discard([paths])
try await workspace.commit(message:, author:) -> String            // hex id
try await workspace.branches() -> [LocalBranch]                    // name, isCurrent, tipId (nil = unborn)
try await workspace.createBranch("feature/x", checkout: true); checkout("main")
try await workspace.log(limit: 200) -> [LocalCommit]               // newest first, max 500
try await workspace.summary() -> LocalRepoSummary                  // branch, headId, lastCommitSubject, lastCommitAt
try await workspace.importFromDirectory(url, author:, message:) / importFromZip(path, author:, message:)
try await workspace.push(credentials:, branch:, onProgress:) -> LocalPushResult   // never forces or deletes
try await workspace.fetch(credentials:, onProgress:) -> LocalFetchResult          // refs/remotes/origin/*
try await workspace.clone(credentials:, branch:, depth:, maxPackBytes:, onProgress:) -> LocalCloneResult
try await workspace.pull(credentials:, branch:, onProgress:) -> LocalPullResult   // the Sync button: fetch, then fast-forward
try await workspace.syncState(branch:) -> LocalSyncState                          // .label "2 to push · 1 to fetch"
GitWorkspace.describe(error) -> String; GitWorkspace.maxCloneBytes (1 GiB, the engine hard maximum); deviceLimits(maxPackBytes:); meshCloneDepth (1); maxLogEntries (500)
```
- `onProgress: @Sendable (GitTransferProgress) -> Void` is called OFF the main actor; hop with
  `Task { @MainActor in ... }`. `GitTransferProgress.label` is the busy-overlay sentence;
  `objects`/`total` count up while a pack is indexed ("Indexing 512 of 9743 objects") and while a
  clone or a sync writes files ("Checking out 64 of 3000 files").
- `LocalPullResult` (`branch`, `fetch`, `outcome`, `message`, `title`, `changedWorktree`,
  `needsAttention`) speaks of syncing: the button that runs `pull` is labelled Sync.
- Cancellation is cooperative (no isolate to kill): `cancel()` trips the engine's token and
  cancels the task (an HTTP exchange aborts); the call fails with `code == .cancelled`,
  message "Operation cancelled.". A disposed workspace refuses work the same way.
- The class is open for tests (override `push`/`fetch`/`clone` to script the network).
- `GitAuthor(name:email:)`, `GitAuthor.fromAccount(nodeName:email:)` (strips `<>` and controls;
  falls back to `forkmesh` / `<name>@mobile.forkmesh`). `LocalRepoText.author(session)` is the
  screens' `localRepoAuthor`.

## LocalRepoRecord / LocalRepoStore

`LocalRepoRecord` (`Identifiable` by `name`, `Hashable`): `name`, `path` (absolute worktree),
`worktree: URL`, `description`, `createdAt: Date`, `branch`, `lastCommitSubject`,
`lastCommitAt: Date?`, `publishedAs: String?` ("owner/name"), `origin: LocalRepoOrigin`
(`.authored` / `.cloned`), `isPublished`, `isCloned`, `syncLabel` ("Local only" / "Published" /
"Cloned"), `copyWith(...)`, `toJSON()` / `fromJSON(_:)` (the index.json shape the retired Flutter app wrote;
dates are UTC ISO-8601), `rebasedOn(root)`.

`LocalRepoStore` (`@MainActor @Observable`): `LocalRepoStore(root: LocalRepoStore.root(for: scope,
documents: documentsURL))` → `<Documents>/forkmesh/repos/<name>/` plus `index.json` for the legacy
project, `<Documents>/projects/<slug>/forkmesh/repos/` for any other (the retired Flutter app's paths, so
an update finds the repositories in place). `defaultRoot(in:)` is `<base>/forkmesh/repos`.
- State: `records` (most recent first), `isLoaded`, `loadError: String?`, `isBusy`, `revision`
  (bumped on every change), `meshIdentities` (lower-cased `owner/name` of published/cloned
  records; the iOS catalog badges them),
  `addListener { } -> UUID` / `removeListener`.
- `ensureLoaded() async` (never throws; failure in `loadError`), `loadIfNeeded() async throws`,
  `list()`, `find(name)`, `workspace(record) -> GitWorkspace` (caller disposes),
  `create(name:author:description:)`, `importDirectory(name:source:author:)`,
  `importZip(name:zipPath:author:)`, `cloneFromMesh(name:credentials:description:branch:onProgress:)`,
  `refresh(name)`, `updateSummary(name, summary)`, `markPublished(name, "owner/name")`,
  `delete(name)`, `cancelActive()`. A failed create/import/clone removes its folder.
- One copy per mesh repository: `cloneFromMesh` refuses an `owner/repo` that a record is already
  bound to (cloned or published from here) with `alreadyOnDevice(identity, as:)`, "owner/repo is
  already on this device as <name>. Open it and sync instead.". `record(meshIdentity:)` (trimmed,
  case-insensitive) and `record(for: repository)` (public `fullName`, then the route owner) find
  that record; `LocalRepoRecord.meshRoute` splits `publishedAs` into `(owner, repo)`.
- `sync(record, publisher:, onProgress:) -> LocalPullResult` runs `PublishService.pull` (mirror
  failover included) on the record's current branch in its own workspace, cancellable with
  `cancelActive()` and counted in `isBusy`, then refreshes the record's summary. An unpublished
  record, or one on a detached HEAD, is refused with a sentence.
- Static `isValidName`, `validateName(_) -> String?` (the sentence to show).
- Loading rebases stale absolute paths (container moves), drops vanished worktrees, adopts
  `<root>/<name>/.git` folders the index forgot, and rewrites the index.
- `relocate(to:) -> LocalRepoRelocation` (`moved`, `leftBehind`): move every worktree and the index to
  another folder and switch the store's `root` to it (below).
- Errors: `LocalRepoStoreException(message)`.

### In the Files app, and a folder that outlives the app

`Config/ForkMesh-Info.plist` sets `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`, so
the app's Documents folder shows in Files under On My iPhone › ForkMesh (the way DuckDuckGo shows its
Downloads). By default every repository created, imported or cloned on the phone is there as
`forkmesh/repos/<name>` (`projects/<slug>/forkmesh/repos/<name>` for a non-legacy project), next to
`ForkMesh from desktop`. Nothing else is kept in Documents: keys, preferences, chat history and caches
live in the keychain, `UserDefaults` and Application Support, which Files never shows.

iOS erases an app's own On My iPhone folder when the app is deleted. A folder the user makes elsewhere
in Files is not erased, so the "On this device" block has a row under its buttons saying where
repositories are saved, with **Change**: "Choose a folder in Files" or "Use ForkMesh's own folder".

- Choosing a folder (`LocalRepoFolderSetting.choose`, `ForkMesh/Features/LocalRepos/LocalRepoFolderRow.swift`)
  starts security-scoped access, refuses a folder inside the app's container (`NSHomeDirectory()`,
  deleted with the app) or in iCloud Drive (`LocalRepoFolder.problem`), saves a `.minimalBookmark`
  under the device-wide preference `localRepos.folderBookmark` (base64), and calls
  `LocalRepoStore.relocate(to: scope.directory(folder))`. Repositories then sit directly in the
  folder as `<name>/` with `index.json` beside them. Access stays open for the life of the process.
- `relocate(to:)` refuses while a create, import, clone or sync is running and refuses a folder
  inside the current root. It reads any `index.json` already in the new folder, writes it back first
  (so an unwritable folder fails before anything moves), renames each worktree across, and writes
  the merged index, so `publishedAs`, origin and descriptions travel with the repositories. A name
  the new folder already holds stays in the old root, listed in the old root's index, and the notice
  names it. The same store object changes root, so the views, the publisher and the node-sync
  applier keep working.
- At launch `LocalReposFeature.register()` resolves the bookmark before creating the store. A
  bookmark that no longer resolves, or a folder that is gone, leaves the store in the app's folder and
  shows `LocalRepoFolder.unreachable` in the row until the folder is chosen again.
- Delete and reinstall: the preference goes with the app, so the store starts empty in the app's
  folder. Choosing the same folder again moves nothing and loads its `index.json`, so every repository
  comes back with its sync link; worktrees missing from the index are adopted as usual.

Choosing "Use ForkMesh's own folder" moves the repositories back the same way and drops the bookmark.
Changes made in Files while the app is running show up the next time it starts.

## Remote, publish, pull requests

- `ForkMeshRemoteCredentials(origin:owner:repo:seed:pubkeyB64url:)` (`remoteUrl`, `canSign`,
  `anonymous`); the origin may be the ws/wss relay URL. `forkMeshPushCanonical(...)`,
  `forkMeshBodyDigest`, `describeRemoteFailure(error, push:, host:) -> ForkMeshRemoteFailure`
  (`code` "push_too_large"/"push_unauthorized"/..., `message`), `forkMeshRemoteHost(origin)`.
  Every engine request is signed (`x-forkmesh-pubkey/ts/sig/body-sha256`) when `canSign`.
- `PublishService(api:identity:prefs:clock:scope:)` (`@MainActor`; `identity: DeviceIdentity`,
  prefs key `node/catalogNodeId` in the scope's namespace): `credentials(owner:repo:signed:)`,
  `push(workspace:owner:repo:branch:onProgress:)`, `fetch(...)`,
  `publish(session:workspace:repo:branch:isPublic:description:head:forkedFrom:onProgress:) async throws -> PublishOutcome`
  (a push failure throws; the catalog answer is an outcome), `publishCatalogRecord(...)`,
  static `describeSession(session) -> String?`, `validatePublishName(_:)`. `PublishOutcome`
  (`status: PublishStatus`, `message`, `publishedAs`, `canonicalNodeId`, `isSuccess`),
  `PublishHead`, `buildCatalogRecord(...) -> JSONObject`, `normalizeForkedFrom(...)`.
- `PullRequestService(pushBranch:submitPull:)` or `.live(publisher:inbox:)`:
  `prepare(workspace:record:headBranch:baseBranch:) async throws -> PullRequestPreparation`
  (`isReady`, `blocker: PullRequestBlocker?`, `message`, `summary`, `patch`, `changedPaths`,
  `binaryPaths`; diffs from the merge base), `submit(workspace:preparation:title:onProgress:)`
  (`@MainActor`; pushes first; throws `PullRequestSubmitError`).
- `GitBlobId.of(bytes)`, `GitBlobId.servedAgrees(served, id)`,
  `LocalPatch.forFile(path:beforeBytes:afterText:oldId:newId:) -> PatchBuildResult` (mode-less
  `index` line; refuses binary / non-UTF-8).

## View models (what the LocalRepos views bind to)

- `LocalRepoScreenModel(record:store:publisher:session:workspace:)` — tabs, files, status,
  branches, log, busy label, mesh banner, sync, commit message, the inline `notice`; actions for every button
  (`refreshAll`, `openDirectory`, `newFile`, `stage`..., `commit`, `checkout`, `createBranch`,
  `beginPublish`/`publish`, `push`, `fetch`, `pull` (Sync), `deleteRepository`). Disposes its
  workspace on deinit.
- `LocalReposSectionModel(store:session:prefs:publisher:)`: besides create and import,
  `syncableRecords` (published or cloned), `canSync`, `cloneCount`, `sync(record)` and
  `syncAll()` (one repository after another behind the busy card; `cancel()` stops the run) with
  a summary notice such as "Synced 2 of 3 repositories · 1 updated · check notes · failed: web"
  (in the error colour when a repository failed), shown inline in the section.
- `CloneToDeviceModel(store:publisher:session:mirrors:)`: `existing(repository)`, `clone(...)`
  (refuses a repository already on the device before any mirror lookup), `sync(record) ->
  LocalPullResult?`, `wasCancelled`.
- `LocalFileEditorModel(workspace:path:)`, `QuickEditModel(repo:path:ref:servedBlobSha:api:inbox:)`,
  `QuickEdit.blocker/head/submitFailure`, `LocalRepoText` (row labels), `LocalNotice` (message and `isError`; the views show it
  as an inline line, never a toast, since the user asked on 2026-10-09 for no toasts anywhere).

## App target (ForkMesh/Features/LocalRepos)

- `LocalReposFeature.register()` (`@MainActor`, no arguments): creates `LocalRepoStore` under
  Application Support and `PublishService` (from the registered `ApiService`, `DeviceIdentity`,
  `KeyValueStore`), and puts both in `Services`. Call it before the Agents feature.
- `LocalRepoServices`: `store`, `publisher`, `folder` (`LocalRepoFolderSetting`, where repositories are saved), `session`, `canClone`, `pullRequests`
  (`PullRequestService.live` when an `InboxService` is registered — what the agent screen needs).
- Views: `LocalReposSection()` (a "Sync clones" / "Sync all" button once a repository is published
  or cloned, and a long-press menu with Open and Sync on each row),
  `LocalRepoScreen(record:agentPrompt:agentAutoStart:)` (Sync and Push in the bottom bar, Fetch in
  the ⋯ menu, the sync state beside the mesh banner), `CloneToDeviceButton(repo:)` (toolbar-safe:
  the busy card, the result and the repository open in one full-screen cover; for a repository
  already on the device it becomes a menu with "Open on this device" and "Sync from ForkMesh"
  instead of cloning again), `QuickEditScreen(repo:path:ref:servedBlobSha:)`,
  `LocalFileEditorScreen(workspace:path:onClose:)`, `PublishRepositorySheet`,
  `LocalGitBusyCard/Overlay`.
- LocalRepoScreen opens `AgentScreen(repoName:workspace:record:initialPrompt:autoStart:)` (Agents
  feature) with its own workspace.
- The network catalog (`ReposScreen`) keeps every row and badges the ones on this device "Cloned"
  (or "On device") through `store.record(for:)`.
- The `.localRepos` route (the "On this device" search and navigation entry) opens
  `LocalReposRouteScreen` (`ForkMesh/Navigation/RouteFallbacks.swift`): the same section on its own
  screen. Until 2026-10-14 nothing was registered for it, so the phone showed the "Coming soon"
  placeholder ("This screen (localRepos) is being built by WS5").

## Not verified / left out

The SwiftUI views were only parsed (no Mac). Imports are refused for ZIP64 and encrypted zips.

## Clone to this device: mirrors, failover, shallow

`CloneToDeviceModel.clone(repo:localName:)` clones by the repository's public name
(`repo.displayOwner`, e.g. `forkmesh/forkmesh`), never by the serving node in `repo.owner`
(`mirror35/forkmesh`). The relay answers `/<node>/<repo>/info/refs` for a mirror node with 503
`mirror_unavailable`, which the old clone reported as "This repository is offline on ForkMesh" —
that was the whole bug for the flagship: it has four healthy HTTPS mirrors.

- Discovery is the one the web and desktop use: `GET /api/repo/<owner>/<repo>/mirrors`
  (`ApiService.freshMirrorSet`, bypassing the cache). `CloneEndpoints.plan` keeps rows with a
  credential-free `https://` `mirrorUrl` (or `endpoint` + `/<owner>/<repo>.git`) that are not
  abuse-blocked and are online or clone-available; healthy rows first (`RepoMirror.isHealthy`: the
  desktop's `mirrorCatalogEntryIsHealthy` — online, integrity ok, lastSync > 0, cloneAvailable,
  endpointHealthy, endpointFresh), then not-behind, lower latency, newer sync. The relay route
  `{origin}/{owner}/{repo}` is always the last source.
- `CloneEndpoints.run` tries the sources in order. Cancellation, a size refusal
  (`resourceLimitExceeded`) and a local store refusal stop at once; anything else moves to the next
  source. When all fail, `CloneFailure.message` lists every source tried with its reason (or says
  the mirror list could not be read). The busy card shows the source and live download bytes.
- A mirror source gets anonymous credentials (`ForkMeshRemoteCredentials.at(endpoint:)`): the
  device key signs only requests to the relay. After a mirror clone, `origin` is set back to the
  relay URL (with the clone's one-branch refspec) so push and fetch go where they always went.
- Mesh clones are shallow and single-branch: `depth 1` of the default branch, no tags. Every
  workspace repository (clone, fetch, push proof) opens with `GitWorkspace.deviceLimits`: the
  engine hard maxima for pack (1 GiB), inflated (1 GiB) and object (256 MiB) bytes, 30 s connect,
  60 s idle read and the 20 min deadline. On 2026-10-04 the flagship at depth 25 had grown to a
  158 MB pack, 1.3 GB inflated with a 77.5 MB object, so the old 96 MiB guard (and the 768 MiB
  inflated and 64 MiB object defaults) refused it on every phone. Depth 1 is a 72.5 MB pack,
  159 MB inflated, largest object 4.5 MB: half the download and an eighth of the inflate and
  hash work. History is fetched only when a push needs it (shallow push proof). The engine still
  holds a pack in memory while it checks it, which is what the 1 GiB ceilings bound. FMGit records the boundary in
  `.git/shallow` (`deepen`, `shallow-info`), walks treat boundary commits as parentless, later
  fetches send `shallow` lines and skip tags. A relay that cannot do shallow gets a full clone.
- Untrusted packs are screened with SHA-1DC (`GitSha1DCDetector`, the vendored
  `Sources/CSha1DC`, cr-marcstevens/sha1collisiondetection a06f1adb, MIT). Before it, no detector
  was injected and every clone failed closed with `hash_screening_unavailable` — the Android app
  has the same gap.
- Checked live from Linux: a shallow clone of forkmesh/forkmesh from mirror35 (70.8 MB, 9,743
  objects) landed with `origin` at the relay and `fetch = +refs/heads/main:refs/remotes/origin/main`.
- Left as it was: a later fetch still goes to the relay, which answers the flagship with a 307 to
  a mirror that the engine does not follow.

## Engine speed

- `GitPackObjectStore` (FMGit) reads one object at a time: the `.idx` gives its offset, the pack
  is memory-mapped, ofs and ref delta chains (up to 4096 deep) resolve against a 32 MiB
  delta-base cache, and every returned object is hashed (and screened, when the repository was
  opened with a detector) and compared with the id asked for. Before, each `GitRepository` — one
  per workspace call — re-read and fully re-parsed the pack on its first packed read, SHA-1DC
  screening included, so opening a cloned repository (summary, status, log, then sync state)
  paid for indexing the whole pack three or four times; on the 70.8 MB forkmesh/forkmesh pack
  that is what made "Indexing" and "Reading repository" take so long.
- Checkout, clone and sync write only the paths whose blob or mode changed, without an fsync per
  worktree file (the index and refs are still flushed), delete a removed path even when it is a
  symlink, and prune the folders a removal empties.
- Checkout, clone and fast-forward refuse a commit whose tree has an entry named `.git` in any
  letter case, at any depth (`pathEscapesWorktree`, "tree entry names repository metadata"), as
  `git` does. Before 2026-10-12 a tree holding `.git/config` or `.git/HEAD` was written straight
  into the repository's own `.git` directory (and `.GIT` reached it too on the case-insensitive
  iOS file system), so a cloned or pulled repository could rewrite its own config, HEAD and refs.
  The check is in `collectTreeEntries` of both engines (FMGit and the Kotlin
  `GitRepository.kt`); the tests are `testCheckoutRefusesATreeThatWritesIntoTheGitDirectory`
  in `LocalEngineWorkflowTests` and `GitCheckoutTest`.
- Measured with this pack store and checkout on `main`'s copy of the engine (the same FMGit code),
  on a 3,000-file repository with 20 commits (12k objects, 2.3 MB pack), full clone, debug build on
  an M-series Mac, before → after: clone 55.6 s → 9.9 s; summary 27.7 s → 0.28 s; status 32 s →
  11 s; log 34 s → 0.3 s. Release build after: clone 1-2 s, status 0.3-0.6 s, summary, log and an
  up-to-date sync 5-10 ms.
- `ForkMeshSha1` runs on the vendored sha1dc core with collision detection and safe-hash off
  (plain SHA-1, same digests). The pure-Swift compression ran at about 5 MB/s in the Debug builds
  the app is run from, so `status` — which reads and hashes every tracked file — spent 32.7 of its
  36.5 s on the forkmesh/forkmesh clone (7,104 files, 148 MB) hashing; it now takes 4.05 s there.
- On the simulator, the app's own stall log for that clone: before, `git.summary` 78-136 s,
  `git.status` 109-154 s and `git.log` 77-161 s each time the repository opened (and `git.clone`
  183 s); after the pack store change only `git.status` still crossed the slow-operation line
  (31.9 s), before the SHA-1 change.
- `status` still reads every tracked file (no stat cache: that needs racy-git smudging and an
  index refresh during status).

## Projects and serving as a node

- `LocalRepoScope` (`LocalRepoScope.swift`): the part of `ProjectScope` this area uses,
  `projectId` and `namespace`, with `key(_:)`, `directory(_:)` and `isLegacy` as
  defaults. `LegacyLocalRepoScope()` is the legacy namespace, `.project(id)` a pinned one,
  `slugFor(_:)` the project slug. WS1's `ProjectScope` (FMAccount) conforms with an empty
  extension; until then the app registers nothing and everything uses the legacy scope.
- `LocalNodeSyncAdapter.swift`: `LocalNodeSyncRepositories(store:)` (a
  `NodeSyncRepositorySource`) and `LocalNodeSyncWorkspace(_ workspace:)` (a `NodeSyncWorkspace`)
  over the store and `GitWorkspace`, meeting the contract in docs/FMChat.md.
  `LocalNodeSyncApplier.forLocalRepos(store, prefs:scope:account:)` builds the applier with the
  account as commit author and the applied ledger at `scope.key("nodeSync/applied")`;
  `installOn(nodeSync)` lets the phone acknowledge inbox rows for repositories it holds.
