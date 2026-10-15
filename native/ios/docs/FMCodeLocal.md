# FMCode / Local (on-device Git)

`ForkMeshCore/Sources/FMCode/Local`: repositories on the phone: create, import, clone, browse,
edit, commit, branch, publish, push/fetch/sync and pull requests. `import FMCode` (re-import
`FMGit` for `GitErrorCode`, `FMApi` for `ApiService`/`InboxService`/`Repository`, `FMAccount` for
`AuthSession`).

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

## GitWorkspace

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

## LocalRepoRecord / LocalRepoStore

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

### Files app access and a folder that outlives the app

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

## Remote, publish, pull requests

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

## Clone to this device: mirrors, failover, shallow

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

## Engine performance and safety

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

## View models and app target

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

## Projects and serving as a node

- `LocalRepoScope`: `projectId`, `namespace`, `key(_:)`, `directory(_:)`, `isLegacy`;
  `LegacyLocalRepoScope()` and `.project(id)`.
- `LocalNodeSyncAdapter.swift`: `LocalNodeSyncRepositories(store:)` and
  `LocalNodeSyncWorkspace(_:)` implement the node-sync contract in [FMChat](FMChat.md).
  `LocalNodeSyncApplier.forLocalRepos(store, prefs:scope:account:)` builds the applier with the
  account as commit author; `installOn(nodeSync)` lets the phone acknowledge rows for repositories
  it holds.

## Limits

Imports refuse ZIP64 and encrypted zips. A fetch to the relay that is answered with a redirect to a
mirror is not followed by the engine.
