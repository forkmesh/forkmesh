# Desktop app (Qt)

The desktop app is the ForkMesh workspace: a Qt 6 Widgets program (`forkmesh`)
that keeps local Git data, signs collaboration records (issues, pulls, chat,
reviews), runs coding agents in worktrees, runs `.forkmesh` workflows, and
operates the user's mirror hosts over SSH. It runs on Linux, macOS and Windows.
It does not serve repositories to the network; managed mirrors run the Go
daemon in `server/` (see [../server/README.md](../server/README.md)). The same
binary runs on the deploy box as the "flagship" node, built from the primary
checkout.

User-facing guide: [www/docs/qt-client.md](../../www/docs/qt-client.md).
Topic pages in this folder:

- [build-and-test.md](build-and-test.md): CMake, `run.sh`, Rebuild & restart, ctest, CI
- [diagnostics.md](diagnostics.md): logs, stall watchdog, crash repair, inline notices, Health and Memory pages
- [networking.md](networking.md): relay transport, HTTP/3 client, local mesh, phone LAN control, hosts
- [agents-ui.md](agents-ui.md): Agents page, Sessions page, activity window, sounds and announcer
- [branch-diffs.md](branch-diffs.md): the branch/pull review pane renders wide diffs on a worker and streams them in
- [pull-merge-all.md](pull-merge-all.md): Merge all with AI on the Pull requests list: the no-branch Auto agent, its prompt, and the cleanup of merged pull requests and branches
- [tasks-page.md](tasks-page.md): Tasks page layout, top-search filtering, row and detail design, task work items and delete cleanup
- [saved-searches.md](saved-searches.md): saved searches, their pings, and the security-pertinent alarm
- [ui-redesign.md](ui-redesign.md): the interface redesign plan: design tokens, components, welcome and sign-in, expanded sidebar, phases and rules
- [windows.md](windows.md): Windows-only behaviour, installer, registry settings, Actions lock
- [git-locking.md](git-locking.md): per-repository queue for index-writing Git commands, `index.lock` recovery, merge timeout
- [package-modules.md](package-modules.md): optional packages built as shared modules, loaded only when installed
- [lifecycle.md](lifecycle.md): what a quit stops (agent and Actions scopes, escaped children), what it leaves running, and running without a window (including keeping the computer from sleeping)
- [genie.md](genie.md): the Genie spider pet, its health from code and marketing velocity, and the marketing coach
- [notes.md](notes.md): the Notes page: local and cloud notes, versions, signed requests
- [markdown-preview.md](markdown-preview.md): one render path for every Markdown preview, hard-break handling, the GitHub-style document styling and painted decorations
- [shortcuts.md](shortcuts.md): Shortcuts page, one-tap runs from every list, and the per-repository shortcut queue
- Agent runs across desktop, mirrors, web and phone: [../agents/README.md](../agents/README.md)

## Code map

Everything is in `desktop/`.

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt` | The app target, test targets, optional modules, job pools, version stamping |
| `run.sh` | Incremental build and launch; `test`, `clean`, `rebuild` |
| `install.sh` | User-level XDG install to `~/.local/bin/forkmesh`, launcher, file-manager entries; `--uninstall [--purge]` |
| `cmake/`, `scripts/package-macos.sh` | Version header template, MinGW toolchain for the Windows cross-build, macOS `.app`/`.dmg` packaging |
| `src/main.cpp` | Startup: paths layout, single instance, CLI modes, crash handler, Actions recovery |
| `src/MainWindow.h`, `src/MainWindow*.cpp` | The main window: one class split by feature into many translation units |
| `src/MainWindowInternal.h`, `src/MainWindowInternal1-4.cpp` | Shared helper widgets and functions (namespace `forkmesh::ui`) |
| `src/MainWindowNavigation.cpp`, `src/Rail*.h` | The left rail: sections, folders, groups, sorting, shortcuts |
| `src/AppPaths.*`, `src/ProjectRegistry.*`, `src/SingleInstance.*` | Project layout, project list, per-project lock and socket |
| `src/BackgroundMode.*`, `src/BackgroundSettings.*`, `src/BackgroundRunnerBadge.*`, `src/TrayMode.*`, `src/KeepAwake.*` | Running without a window (`--headless`), start at boot, the top-bar badge, the tray, keeping the computer awake ([lifecycle](lifecycle.md#running-without-a-window)) |
| `src/PackageModules.*`, `cmake/ForkMeshPackageModules.cmake` | Optional packages built as lazily loaded shared modules ([package-modules](package-modules.md)) |
| `src/Theme.h`, `src/DesignTokens.*`, `src/UiKit.*`, `resources/theme/` | Classic style sheets, design tokens and components of the new interface ([ui-redesign](ui-redesign.md)) |
| `src/Agent*`, `src/ClaudeStreamSession.*`, `src/CodexAppServerSession.*` | Local agent runtimes, guardrails, worktrees, access gates |
| `src/ProjectAgentAccess.*`, `src/AgentRunRules.*`, `src/AgentNetworkGate.*` | Agent access policy, run/ask/deny decisions, external-call and destructive-command gate |
| `src/ControlNode.*` | Host and fleet operations: SSH commands, installers, agent CLI refresh |
| `src/ActionRunner.*`, `src/ActionStore.*`, `src/MirrorActionsConfiguration.*` | `.forkmesh` workflows, the run queue, the mirror Actions lock and journal |
| `src/ServerNode.*`, `src/NodeEventSocket.*`, `src/Http3Get.*` | Relay HTTP/WebSocket transport, account event topics, in-process HTTP/3 |
| `src/LocalMeshNode.*`, `src/LocalControl.*`, `src/MeshFirewall.*` | LAN mesh discovery and the phone control channel |
| `src/UpstreamCheckoutSync.*`, `src/PullStore.*`, `src/IssueStore.*` | Mesh converge of source checkouts, pull and issue records |
| `src/GitIndexQueue.*`, `src/SafeDelete.h` | Per-repository Git index lane; the guarded recursive delete |
| `src/StallWatchdog.*`, `src/CrashHandler.*`, `src/CrashLogScan.*` | UI stall sampling, crash records, crash-repair dispatch |
| `src/CodeHealth.*`, `src/ComponentHealthBoard.*`, `src/StatsDashboard.*`, `src/HomeBoard.*` | Health, Stats and Home pages |
| `src/NativePackageExtension.*`, `src/ForkMeshPackageRegistry.*`, `src/*HostFeed.*` | Market packages hosted in the desktop and the host sockets they use |
| `src/MarkdownRender.h`, `src/MarkdownDocument.h`, `src/MarkdownPreviewStyle.h` | Markdown rendering shared by README views, file previews, composers and packages ([markdown-preview](markdown-preview.md)) |
| `src/Tooltips.h` | The app-drawn tooltip window |
| `tests/` | Qt Test programs (`test_*.cpp`) and Python source validators (`validate_*.py`) |

## Architecture

- **One process per project.** `forkmesh --project <slug>` selects an
  app-level project; each has its own lock and single-instance socket. The
  default project is `forkmesh`. A second launch hands its target (a
  `forkmesh://` link, a folder path or a `file://` URL) to the running window
  over that socket; macOS delivers it as a `QFileOpenEvent`. If the running
  instance is ForkMesh running without a window, it answers `yield` and
  quits, and the new launch takes the lock and opens the window
  ([lifecycle.md](lifecycle.md#running-without-a-window)). Several projects
  run side by side, each with its own window or windowless instance, tray,
  and start-at-boot unit. The socket name carries the
  project and, for a state folder outside the default place, the first 16
  hex digits of the SHA-256 of that folder. Until 2026-10-15 it used
  `qHash`, which Qt seeds per process, so two launches of a sandboxed
  profile never found each other's socket.
- **Projects.** The project picker lists every slug-named folder in the
  projects root, hidden folders and symlinks excluded. Removing a project
  never deletes it: `project.json` is set aside so the folder is unlisted, and
  a folder ForkMesh created can be moved aside to `<slug>.deleted-<stamp>`. A
  folder that is itself a Git checkout is listed but will not open, because
  the project's `data/`, `ssh/` and identity key would land in its working
  tree. A new project starts offline with no instance and asks which instance
  to join when its window first opens; it never joins a default relay on its
  own. Turning offline mode off does not require an instance: the project goes
  online for everything else, while the relay connection, node presence and
  relay probe stay off until an instance is chosen. Choosing an instance for a
  project with no logo saves the instance's logo as `logo.png`: the `logo`
  path from `GET /api/instance`, else its favicon. Project settings is a page
  of the main window.
- **An instance belongs to one project (2026-10-21).** A project may hold
  several instances (rarely), but two projects never share one, so their
  accounts, chat and data cannot mix. Each project records the origins of its
  instances (`host[:port]`, production aliases folded to `app.forkmesh.com`)
  as `instances` in its `project.json` whenever its server list changes
  (`MainWindow::recordProjectInstances`); a project that has not recorded them
  yet is read from its settings file. Choosing an instance for a new project,
  "Add mainnode server", signing in to another instance, connecting a deployed
  relay and editing the relay address all refuse an instance another project
  uses (`ProjectRegistry::instanceOwner`) and offer to open that project. The
  new-project dialog no longer offers the current project's instances, which
  belong to it. Projects that already shared an instance before this rule are
  left as they are.
- **Project menu.** The instance, account and repository live as rows in the
  project picker's menu rather than in the top bar. When no account is signed
  in, the account row is the "Log in / Sign up" entry; the top bar has no
  separate sign-in button.
- **Top bar indicators.** Right-clicking the top bar (its empty space or the
  Agents, Nodes and Actions grids) opens a "Show in top bar" menu with a check
  for each grid. Unchecking one hides it even when it has something to show;
  checking it again brings it back. The menu belongs to the bar rather than
  to the grids so a hidden grid can still be restored. The hidden keys
  (`agents`, `nodes`, `actions`) are stored in `ui/hiddenTopBarIndicators`.
- **Main window.** One `MainWindow` class with a left rail of sections
  (indices are `k*SectionIndex` constants in `MainWindow.h`). Most sections are
  built lazily on first open. Settings is one scrolling page whose sections
  fold and are built on demand; a search builds every section so it can match
  their controls.
- **Rail.** Icons can be reordered by drag, grouped into folders that pop out
  beside the rail, split into labelled groups, and sorted (custom, by name, by
  use, or by badge count). Buttons can move between the rail, the repository
  tab bar and the Code toolbar, and any button that opens a page can be dropped
  into the rail as a shortcut. All of this is stored as JSON under `ui/*`
  settings keys (`ui/railOrder`, `ui/railFolders`, `ui/railGroups`,
  `ui/buttonPlacement`, `ui/railShortcuts`), keyed by stable rail keys.
- **Hiding a rail item, keeping it on Home.** Right-click any rail item (a
  folder member, shortcut or plugin too, but not Home) and pick "Hide from
  left rail (keep on Home)"; a repository tab or Code toolbar button offers
  "Hide from … (keep on Home)", which first moves it to the left rail and then
  hides it there. Its rail key goes into `ui/railHidden` (a string list).
  `applyRailOrder` takes hidden items out of the rail and its folders and parks
  their buttons in `railHiddenItems`, a hidden holder widget, instead of
  calling `hide()`: a button's own visibility stays what the feature that owns
  it set, so a package turned off still disappears from Home, and turning a
  feature on cannot make a hidden item reappear in the rail.
  `railItemWidgets()` lists the rail, its folders and the holder, so Home, the
  command palette, `railWidgetForKey` and a Home tile click
  (`QAbstractButton::click`) still reach the item, and `isInRail` counts it.
  A Home tile's right-click menu shows "Show in left rail" for a hidden item
  and "Hide from left rail (keep on Home)" for the rest
  (`addHomeTileRailActions`). The left rail's background menu has a "Hidden
  from the rail" list to show one or all again. Moving the button to another
  bar, dropping it on the rail, or "Reset button placement" also shows it again.
- **Pinned workflows.** Workflows (special agents among them) can be pinned to
  four strips around the window: the top bar beside the activity indicators,
  under the left rail, at the right edge and in the status bar
  (`ui/workflowPins`, `src/MainWindowRepoWorkflows.cpp`;
  [../app/workflows.md](../app/workflows.md#desktop-workflows-and-special-agents)).
  A strip with no pins is hidden.
- **Popped-out pages.** Dragging a rail icon out of the window, or "Open in new
  window", moves the section's current page into its own top-level window. A
  placeholder keeps the page's index in the section stack, because the rail's
  button ids and every `setCurrentIndex` caller address pages by index.
  Closing the window docks the page back. Package pages use their own window
  (see [../marketplace/README.md](../marketplace/README.md)).
- **Home.** A home-screen grid of every rail page, arranged into pages, with
  live counts read from the rail buttons themselves so they never disagree.
  The open repository's Code icon carries its open pull requests ("3 open
  pulls"), read from the repository's Pulls tab badge, so the two always
  match. Right-clicking a plugin's icon adds its settings and **Uninstall**
  (`HomeBoard::setTileMenu`, `MainWindow::addHomeTilePackageActions`): the
  tile's rail button carries the package entrypoint, so Home offers exactly
  the plugins the rail does.
- **Packages.** Many features are Market packages hosted in the desktop and
  can be turned off per project; hiding a package hides its rail button, tabs
  and search hits. Rules: [../marketplace/core-plugins.md](../marketplace/core-plugins.md).
- **Chat.** Relay rooms, server office channels and direct messages share one
  sidebar. Read marks, retention preferences and formatting sync with the
  account; see [../app/chat-read-sync.md](../app/chat-read-sync.md) and
  [../app/chat-formatting.md](../app/chat-formatting.md). Encryption, device
  approval, safety numbers and direct delivery: [chat.md](chat.md).
- **Notices.** There are no floating toasts. A message is shown inline on the
  page it was raised on, or in the open dialog's status line; unsolicited
  events are filed on the Pings page ([diagnostics.md](diagnostics.md)).
  A fresh celebrating ping plays `CelebrationOverlay` (confetti with a
  headline card); the card word-wraps its title and body onto as many lines
  as they need instead of eliding them, so "Email tracked in the CRM" and the
  email subject are read in full. The confetti stays click-through, so a ✕
  button (a sibling of the overlay, since a click-through widget passes clicks
  past its children too) sits on the card's top-right corner, or the window's
  when there is no card, and ends the show at once. It appears once the card
  has popped in and leaves when the fade-out starts.
- **Git work off the GUI thread.** Git runs through helpers in
  `MainWindowInternal.h` (`waitForGit`, `runOffThread`); a GUI-thread wait pumps
  the event loop, and Git on the GUI thread shows up in the stall log.
  Index-writing commands for one repository run one at a time through
  `GitIndexQueue`; see [git-locking.md](git-locking.md).
- **No display polling.** The desktop has no timers that re-read the relay
  for display. Updates arrive over the account event socket
  (`MainWindowRepoSync.cpp` handles topics such as `cloud-agents`, `tasks`,
  `chat-read`, `direct-messages`); catch-up reads are bounded by floors
  measured in minutes. See
  [www/docs/operations/polling-elimination.md](../../www/docs/operations/polling-elimination.md).
- **Backups run at background priority.** The hourly and "Back up now"
  `tar -czf` (`MainWindow::takeBackupNow`, `MainWindowData.cpp`) starts with
  nice 19 and, on Linux, the lowest best-effort I/O priority; `gzip` inherits
  both. Its first run lands right after login and packs the whole data
  folder, which held a core and the disk at full priority while calls and
  audio in other apps were starting (2026-10-19). Idle I/O class is not used
  because a disk kept busy by agents could starve the backup outright.
- **Branch lifecycle.** Deleting an agent, a branch row or a worktree deletes
  every copy of the branch (local, `origin/<b>`, `forkmesh-mesh/<b>`) and
  writes a tombstone so the mesh converge does not bring it back. Branches can
  be selected for one-confirmation bulk deletion; rows attached to a worktree,
  the default branch and active agent branches stay protected. Refresh fetches
  with `--prune`, so stale remote-tracking rows do not remain in the list. A
  branch the upstream deleted is deleted locally under strict conditions (see
  [Traps](#traps)).
- **Actions.** `.forkmesh` workflows run in disposable workspaces checked out
  from the bare mirror. Push triggers are matched over the whole pushed range.
  A run interrupted by a stop signal is re-queued, not failed.

## Protocols and interfaces

- **Relay HTTP API and account event socket:** `ServerNode.*`,
  `MainWindowRepoSync.cpp`. Signed requests use the node's Ed25519 key
  (`ForkMeshIdentity.*`). The desktop has no browser session token and sends
  no `Origin` header, so it uses key-signed routes, not cookie routes.
- **Website-queued agent jobs:** `GET /api/repo/<node>/<repo>/org-agent-jobs`
  and `POST .../org-agent-jobs/<id>/result` (`MainWindowAgentSync.cpp`).
- **Cloud agent rows:** `GET /api/account/agent-sessions`.
- **Cloud notes:** `/api/notes`, with a Bearer session or an account-key signed
  request ([notes.md](notes.md)).
- **Local mesh and phone control:** TCP/UDP 48732 (`LocalMeshNode.cpp`,
  `LocalControl.*`); see [networking.md](networking.md).
- **Editor bridge and MCP:** `ClaudeIdeBridge.*`, `McpConnector.*`. The MCP
  server (`tools/forkmesh_mcp_server.py`) reaches the desktop over the
  agent-intake socket (`AgentIntake.*`) to create agents, open agent windows
  and widgets, drive browser windows, suggest sessions, list and message
  agents and runners, and cut releases
  ([../tools/mcp-create-agent.md](../tools/mcp-create-agent.md),
  [../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md),
  [../tools/mcp-claude-code-desktop.md](../tools/mcp-claude-code-desktop.md),
  [../tools/mcp-releases.md](../tools/mcp-releases.md)).
- **Launch targets:** `forkmesh <target>` takes a `forkmesh://` link, an
  absolute folder path or a `file://` URL. A path after `--project` is a
  project name, never a folder.

### Open with ForkMesh

The file manager's right-click menu has **Open with ForkMesh** on folders. It
launches `forkmesh <folder>`; the desktop opens the repository whose
`localPath` is or contains the folder, or else imports it like Add repository
(the folder, its child checkouts, or the checkout around it). The visibility
question is always asked, so a right-click never publishes anything by
itself.

| Platform | Written by | Entry |
|---|---|---|
| GNOME Files (Nautilus) | `desktop/install.sh` | `~/.local/share/nautilus/scripts/Open with ForkMesh` |
| KDE Dolphin | `desktop/install.sh` | `~/.local/share/kio/servicemenus/forkmesh-open.desktop` |
| Cinnamon Nemo | `desktop/install.sh` | `~/.local/share/nemo/actions/forkmesh-open.nemo_action` |
| Windows Explorer | `tools/package/forkmesh.nsi` | `HKCU\Software\Classes\Directory\shell\ForkMesh` and `Directory\Background\shell\ForkMesh` |

Each Linux entry is written only when that file manager is installed and is
removed by every uninstall path. The launcher claims only
`x-scheme-handler/forkmesh`, never `inode/directory`, so `xdg-open <folder>`
keeps opening the file manager. macOS has no entry.

The app-menu and dock icon is the `forkmesh` icon that `desktop/install.sh`
puts in `~/.local/share/icons/hicolor`. In-app updates never re-run that
script, so a launcher installed before the logo changed kept showing the old
square icon with its black background. Five seconds after the window opens,
`src/DesktopLauncherIcon.cpp` rewrites those icons (16 to 512 px, the scalable
SVG and the unthemed `~/.local/share/icons/forkmesh.png` if present) from the
transparent `:/app/forkmesh.png` and `:/app/forkmesh.svg` built into the app,
then rebuilds the GTK icon cache. It only acts when
`~/.local/share/applications/forkmesh.desktop` exists, writes only files whose
bytes differ, and does nothing on Windows or macOS.

## Data layout and paths

`src/AppPaths.cpp` resolves `~/projects/<project>/` (override with
`FORKMESH_PROJECTS_DIR` or `FORKMESH_HOME`):

| Folder | Holds |
| --- | --- |
| `data/` | Node identity key, local stores, `data/secrets/` |
| `config/` | `QSettings` file (`config/ForkMesh/ForkMesh.conf`), `agent-rules.json` in the home project |
| `cache/`, `state/`, `logs/` | Caches, runtime state, logs |
| `ssh/` | Fleet SSH key (`forkmesh_shared_ed25519`) |
| `toolchain/` | Bundled build tools |
| `src/` | The source checkout (primary checkout in the home project) |

Each project has a `project.json` (name, `created`, `agentAccess`). Legacy
locations (`~/.local/share/ForkMesh`, `~/.config/ForkMesh`, `~/.forkmesh`) are
moved by rename with compatibility symlinks
(`forkmesh --migrate-layout --dry-run` previews). `tools/forkmesh_paths.py` and
`tools/forkmesh-paths.sh` implement the same rules and must agree. At startup
the app exports `FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR` to every child
process, agents included; tests that spawn installers must drop them (see
`AGENTS.md`).

## Configuration

- **Settings storage.** `QSettings` lives in `config/ForkMesh/ForkMesh.conf` on
  Linux and macOS. On Windows the home project's settings are the registry
  (`HKEY_CURRENT_USER\Software\ForkMesh\ForkMesh`); see [windows.md](windows.md).
- **Environment:** `FORKMESH_HOME`, `FORKMESH_PROJECTS_DIR`, `FORKMESH_PROJECT`,
  `FORKMESH_START_DIRECTORY` (set by `run.sh`), `FORKMESH_SONIFY_BACKEND`
  (`silent`, `pipe`, `none`), `QT_QPA_PLATFORM=offscreen` for tests.
- **CMake cache options:** `FORKMESH_BUILD_TESTS`, `FORKMESH_VERSION_OVERRIDE`,
  `FORKMESH_BUILD_COMMIT_OVERRIDE`, `FORKMESH_HEAVY_JOBS_OVERRIDE`,
  `FORKMESH_BUNDLED_OPENSSL`.
- **Notable settings keys:** `diagnostics/autoAgentOnErrorLog` (crash and error
  repair dispatch), `diagnostics/reportErrors` (send errors to the server, off
  by default), `ui/interfaceSounds`, `agents/listenToNewRuns`, the `ui/rail*`
  and `ui/buttonPlacement` layout keys, `settings/sectionsExpanded`,
  `chat/retentionPreferences`.
- **Per-project agent access:** `project.json` key `agentAccess`; global rules
  in the home project's `config/agent-rules.json`.

## Build, run, test

```sh
./desktop/run.sh            # incremental Release build, then launch
./desktop/run.sh test       # build and run the "critical" ctest label
```

Qt 6.4 is the floor. OpenSSL 3.0 builds; 3.5 is needed at run time for
ML-KEM-768 and Argon2id (`-DFORKMESH_BUNDLED_OPENSSL=ON` builds a pinned 3.5).
Details, manual CMake, test targets and known failures:
[build-and-test.md](build-and-test.md).

## Operations

- The deploy box runs the flagship from `~/projects/forkmesh/src` (binary
  `desktop/build/forkmesh`) as a systemd user service. **Restart** on the rail
  (Rebuild & restart) rebuilds that checkout and restarts the unit; it waits
  for running agents.
- **Update path.** The desktop update pulls the primary checkout with
  `git pull --ff-only`. It must never delete or re-clone the primary source
  checkout: its `.git` holds every local branch and every agent worktree, and
  an earlier update that fell back to `rm -rf` and a fresh clone after one
  failed pull erased all of them. A failed pull stops the update, reports why
  and changes nothing (or moves the checkout aside); a fresh clone runs only
  when there is no checkout, into a missing or empty folder.
  `app/tests/test_desktop_update_keeps_checkout.py` covers it.
- **Hosts.** Network → Hosts installs, updates and removes ForkMesh on SSH
  hosts; every host update also refreshes the host's agent CLIs
  ([../agents/README.md](../agents/README.md#agent-cli-refresh-on-hosts)).
- Logs and diagnosis: [diagnostics.md](diagnostics.md).

## Desktop agent sandboxing

Agents run in Git worktrees of the open repository, inside layered controls.
The full description is in
[../agents/README.md](../agents/README.md#desktop-agent-sandboxing) and
[../agents/access-rules.md](../agents/access-rules.md).

- **Folder lock (Linux, default on).** Landlock restricts an agent to its
  worktree, the project's top-level folders except `data/`, `config/`,
  `cache/`, `state/`, `logs/` and `ssh/`, and a few shared tool and scratch
  paths.
- **External-call gate.** `AgentNetworkGate.*` classifies Bash, WebFetch and
  WebSearch calls. For Claude Code a PreToolUse hook asks the desktop over a
  local socket; an unreachable socket denies. Legacy script providers ask the
  same gate. The gate works per tool call; it is not a network sandbox.
- **Access policy.** A project's `agentAccess` merges with the global rules
  (`effectiveAgentAccess`); `agentrules::decide` turns the result into run,
  ask or deny for every provider. The global rules file lives where no
  agent's folder lock reaches, so an agent cannot loosen its own rules.
- **KVM workspace (optional).** Agents can run in a QEMU/KVM guest, which is
  then the boundary.
- **Memory containment.** Each agent runs in its own systemd scope inside a
  capped slice ([../agents/memory.md](../agents/memory.md)). `AgentJail.h` is
  a resource fence, not a security sandbox.

### Guardrails and why

- **Destructive commands** (recursive deletes outside temp folders, force
  pushes, remote branch deletes, `git reset --hard`, `git clean -f`,
  `git branch -D`, stash drops, history rewrites, disk wipes) are classified
  by `classifyCommandRisk` and, by default, wait for the user's approval. A
  single mistaken command can erase unrecoverable data.
- **Tool allowlist.** An optional list of tool families; tools outside it are
  not offered to the model, so a tool a newer agent CLI adds stays off.
- **Access failures** (a refused folder or host) prompt the user instead of
  failing silently.
- **Recursive deletes in the desktop itself** go through
  `forkmesh::safedelete::removeTree`, which refuses `HOME`, checkouts, other
  worktrees and protected project folders
  ([../engineering/destructive-actions.md](../engineering/destructive-actions.md)).

## Security

- The node identity key lives under `data/`; losing it loses the account. Never
  delete `~/projects/forkmesh/{data,config,ssh,state,logs}` or the primary
  checkout (see `AGENTS.md`).
- Bearer tokens are held in memory and never written to `QSettings`
  ([www/docs/qt-client.md](../../www/docs/qt-client.md#multiple-relay-instances)).
- Cloud credentials (Settings → Cloud) are sealed with AES-256-GCM under a
  0600 key file in `data/secrets/`; the settings file holds only ciphertext
  ([networking.md](networking.md#aws-credentials)).
- The fleet SSH key is `ssh/forkmesh_shared_ed25519` (legacy
  `ssh/vultr_mirror_ed25519` when it is the one present), resolved by
  `sharedHostKeyPath()` in `ControlNode.cpp`. Passwords are only a fallback
  and never go on a command line.
- Errors are not sent off the machine unless the user opts in
  (`diagnostics/reportErrors`). Diagnostics and run logs mask workflow
  variables that are marked secret.
- Package extensions get settings and data scoped per package
  (`NativePackageExtension.*`).
- Signed records and requests use the node key; the desktop never relies on
  cookies or a browser origin.
- Agents are sandboxed as described in
  [Desktop agent sandboxing](#desktop-agent-sandboxing).
- Actions steps run in a bubblewrap sandbox unless a workflow says
  `sandbox: false` ([below](#actions-outside-the-sandbox)).

### Actions outside the sandbox

A sandboxed step runs as uid 65534 with a fresh home and `/tmp`, its own
`/proc`, and no view of the user's files (`ActionRunner::sandboxArguments`).
A workflow that has to work on this computer itself sets a top-level
`sandbox: false` (`no`, `off` and `0` work too). The disk cleanup that first
used it moved to the Workflows page on 2026-10-18
([../tools/free-disk-space.md](../tools/free-disk-space.md)).

```yaml
name: Host step
on: [workflow_dispatch]
sandbox: false
```

- **Manual runs only.** `ActionFile::parse` marks the workflow invalid unless
  `on` is exactly `workflow_dispatch`, so a push, pull request, release or
  any other event can never start it; only the node's owner can, with
  **Run workflow** on the desktop or from a paired phone. Approval is
  unchanged: it is bound to the workflow text and the repository tree, and
  the repository's auto-approve setting applies as for any other run. That is
  the same trust as running a shortcut from that checkout, plus the approval
  record. A manual run on another branch runs that branch's code, so only
  start one on a branch whose author you trust.
- **What changes.** The steps run as the desktop user with plain `bash -se`
  (no bwrap) in the run's verified writable clone, still inside the
  `forkmesh-action-*.scope` systemd scope with the same memory, task, CPU
  and time limits and the same rlimits. The environment is still built from
  scratch: the real `HOME`, `USER`, `LOGNAME`, `TMPDIR=/tmp`,
  `PATH=$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin`, the session bus, the
  usual `FORKMESH_COMMIT`/`FORKMESH_REF`/`FORKMESH_RUN_ID`, and
  `FORKMESH_LOCAL_CHECKOUT`, the repository's local checkout on this desktop
  (absent when it has none). A workflow's `env:` and node variables cannot
  set `FORKMESH_LOCAL_CHECKOUT`. There is no `/cache` build cache and no
  network namespace.
- **Disk check.** A sandboxed run needs the full workspace quota (2 GiB) free
  before its checkout; a `sandbox: false` run needs 1 GiB, so a disk cleanup
  can still start on a nearly full disk.
- **Where it runs.** Only the desktop runner honours the key, on Linux. The
  mirror and runner-tier executors take push events only, which such a
  workflow never has; an older desktop that does not know the key runs it
  sandboxed, where it fails instead of touching anything.
- **Phones and web.** Nothing new is needed: the phones' Actions screens list
  it under **Run by hand**, start it and show its steps live, as for any
  workflow. The web canvas shows the key as **Where the steps run** and
  refuses to save `sandbox: false` with any event besides Manual run
  (`sandbox_needs_manual_run`).

Tests: `desktop/tests/test_action_security.cpp` (parse rules, and a run that
checks the uid, the real home, the local checkout and that `env:` cannot
redirect it), `app/tests/test_workflow_files.py`.

### Approval ping for a contributor's pull request

A pull request's checks wait for approval while its author has no merged pull
request in the repository (`PullAuthorTrust::isEstablished`). Not merged is
not the same as new: the ping used to say "their first contribution" for an
author's fifth unmerged pull request too. The ping now counts the author's pull
requests in the repository up to this one, merged, closed and open alike
(`PullAuthorTrust::pullOrdinal`), and says "their 3rd pull request here and
none merged yet"; only a real first one reads "First pull request needs your
approval". Test: `desktop/tests/test_crypto.cpp`.

One ping per pull request, not per workflow: `applyWorkflowScan` used to ping
inside its workflow loop, so a held pull request in a repository with two pull
request workflows ("CI tests", "Desktop client CI tests") filed two identical
rows. The held runs are now collected and `notifyHeldActionRuns` files one row
naming every held workflow, linked to the first held run.

The Actions page's approval pane says why a run is held. It used to show only
the diff of the workflow file against the last approved copy, so a held pull
request whose workflow was unchanged read "No changes from the approved
workflow" above an Approve button, with no hint of what needed approving.
`MainWindow::showRun` now opens the banner with the reason: the pull request
number and author for a first-time contributor, a workflow that is new or
changed, or repository files that changed since the last approval (and whether
automatic approval is off). When the workflow is unchanged, the diff pane
points at the pull request's code (or the commit) as the thing to review. The
button reads "Approve && run" in source: a single `&` is a Qt mnemonic and
showed as "Approve_run".

### New pull request pings count new pull requests only

`applyPullsInboxEntries` counted every inbox entry it accepted. Two kinds of
entry are not new pull requests: review and lifecycle events, and a replayed
delivery that `PullStore::applyRemotePull` recognises as already stored (it
returns true so the entry is acknowledged). Inbox drains overlap when a GET
lands before the previous drain's acknowledging DELETE, so on 2026-10-21 three
new pull requests filed five pings ("2 new pull requests", "3 new pull
requests" and three single ones). `applyRemotePull` now reports through
`opened` whether it wrote a pull request, `PullsInboxOutcome::opened` counts
those, and only they ping and name the author and title.

### Pull list shows whether a pull's code is on this machine

Some open pull requests have no local branch holding their code, so Fix,
Resolve and agent checkouts cannot start from them. The branch-state worker
(`MainWindow::refreshPullBranchState`) now looks for the code under the same
names `localPullAgentBranch` uses (`pullLocalBranchCandidates`: the signed head
first for a branch-backed pull, `pr/<n>` first for a stored patch) and records
the one it found in `PullBranchState::codeBranch`. Ahead/behind and the
worktree mark are measured on that branch, so a relay pull held as `pr/<n>` is
no longer shown as unmeasured. When no candidate exists the row paints a faded
`cloud` icon in place of the branch icon and the hover text says "Code not on
this machine"; otherwise it names the branch. Closed and merged pulls are not
measured and keep the plain branch icon.

## Change process

- Source files hold code only; the reason for a change goes in the matching
  page under `docs/` (this page for desktop-wide rules, a topic page
  otherwise).
- Build the app target before calling a change done; unit targets do not
  compile `MainWindow*.cpp` or `main.cpp`. Run the relevant ctest targets and
  the window tests for UI changes ([build-and-test.md](build-and-test.md)).
- New `Q_OBJECT` headers must be registered for moc in `CMakeLists.txt`.
- Destructive operations use the guarded helpers; a failed pull, fetch,
  clone or HTTP call never falls back to deleting or re-cloning.
- Record a new invariant or pitfall under [Traps](#traps), not as a dated
  note.

## Traps

- **Mirror status reports what exists, not what was asked for.** The repo
  detail's MIRROR STATUS menu and status line say "Managed mirror enabled"
  only when `publishToNetwork` is set and the mirror folder exists; before the
  first publish they say "Not published yet", and "Delete mirror..." is only
  offered when there is a mirror to delete.
- **Pull timestamp units.** Legacy pull and event records can store Unix
  seconds while current records use milliseconds. Normalize them with
  `issueDisplayTimestampMs()` before ordering, formatting, or storing a
  display timestamp; compact PR list labels otherwise render current records
  as decades old.
- **Qt never draws a tooltip on screen.** `Tooltips::install()`
  (`src/Tooltips.h`) marks Qt's `QTipLabel` `WA_DontShowOnScreen` and draws its
  text in its own `Tooltips::Window`, because Qt's tip inherited transparent
  style-sheet backgrounds, did not wrap plain text, and hid on unrelated
  focus and leave events. Keep using `setToolTip`, `Qt::ToolTipRole` and
  `QToolTip::showText`/`hideText`. Do not restyle tips through `QToolTip { }`
  rules: the window paints `QPalette::ToolTipBase`/`ToolTipText` itself.
  `tests/test_tooltips.cpp` covers it.
- **Widget fonts are sized in pixels.** The global style sheet and the font
  size setting set `font-size` in `px`, so `font().pointSizeF()` is `-1` and
  `setPointSizeF(font.pointSizeF() * 1.25)` logs `QFont::setPointSizeF: Point
  size <= 0 (-1.250000)` and leaves the font unchanged. Size a derived font
  with `forkmesh::scaledFont`, `forkmesh::resizedFont` or
  `forkmesh::fontSizedLike` (`src/FontSizing.h`), which keep whichever unit the
  font carries. `tests/test_font_sizing.cpp` covers the helpers and
  `tests/validate_error_log_regressions.py` rejects a raw
  `setPointSizeF(x.pointSizeF() ...)`.
- **`MainWindowInternal.h` is in the precompiled header.** Editing it, or
  `MainWindow.h`, recompiles every `MainWindow*.cpp`. New helper bodies go in
  `MainWindowInternal1-4.cpp` with only a declaration in the header. New shell
  behaviour for the opt-in interface goes through `forkmesh::ui::NextAccess`
  and the hooks in `NextShellHooks.h` ([ui-redesign.md](ui-redesign.md)).
- **Namespace `forkmesh::ui`.** A bare `class Foo;` in `MainWindow.h`
  declares a different class; forward-declare inside the namespace.
- **Only the `forkmesh` target compiles `main.cpp`.** `MainWindow*.cpp` are
  compiled by the app and the window/perf test targets, not by the small unit
  targets. A `ninja ... | tail` pipeline hides the build's exit status.
- **New `Q_OBJECT` headers** must be listed for moc. Several classes avoid
  `Q_OBJECT` on purpose and use `std::function` callbacks.
- **External and cloud agent rows have negative ids** (`<= -1000`, cloud rows
  `<= -1000000`). A guard like `sessionId < 0` silently excludes them; use
  `isExternalSession()`/`isCloudSession()`.
- **`AgentSession::name` is the repository name**, not the task title (that is
  `issueTitle`). It also seeds the agent's colour and voice.
- **Show a chat row as soon as it is inserted.** `addMessageRow` calls
  `row->show()` right after `insertWidget`; otherwise the layout counts new
  rows as empty for one pass and paints squeezed rows.
- **Every item view needs a painted selection band.** The theme's
  `QAbstractItemView::item { border: 0px; }` hands item painting to the style
  sheet, which then paints no selection background without an
  `::item:selected` rule while text still switches to the highlight colour. A
  view-specific `::item:selected` rule must set `color` too.
- **Registry-backed settings on Windows.** `QSettings::fileName()` is a
  registry path; never build a file path from it (`settingsFilePathFor`
  returns empty for registry names).
- **A relaunch under systemd must not start a child.** A child started by the
  flagship lives in the unit's cgroup and dies when the unit stops. Relaunch
  paths call `requestSupervisingUserServiceRestart(path)` so the unit is
  restarted instead
  ([../tools/source-publisher.md](../tools/source-publisher.md#flagship-guard)).
- **Qt 6.4 is the floor.** Guard newer Qt APIs with `QT_VERSION_CHECK`;
  `tools/qt64-build-check.sh` builds against the oldest supported Qt.
- **OpenSSL 3.5 features fail closed on 3.0** with a message
  ([build-and-test.md](build-and-test.md#openssl)).
- **A leftover `.git/rebase-apply` blocks every later `git am`.** `PullStore`
  tracks live conflict sessions; `mergePull` and `beginPullBranch` treat any
  other `rebase-apply`, or a `MERGE_HEAD` equal to the PR head being merged, as
  stale and clear it first, keeping the user's tracked edits.
- **A branch to merge may not be local.** `git merge <name>` only resolves
  local refs. The merge worker looks for local and remote-tracking refs,
  fetches the branch from the mirror, or replays a patch-based pull from its
  mbox. A missing ref is created; an existing branch is never moved.
- **One `commitResolvedMerge`.** Every merge-conclusion path goes through it,
  so all report `gitFailureDetail` ([diagnostics.md](diagnostics.md)).
- **`forkmesh-mesh/*` is not a remote.** The mesh converge fetches mirrors by
  URL into `refs/remotes/forkmesh-mesh/*`. Deleting such a row drops the
  tracking ref and runs a forced converge whose pruning removes it from the
  mirrors; a mesh copy whose local branch still exists is refused.
- **A deleted branch leaves its tip in `refs/forkmesh/trash/deleted/<b>`.** The
  converge re-creates a local branch from any mirror copy not in main, so a
  delete writes this tombstone first and the converge skips copies it
  contains. In the other direction, a local branch is deleted because the
  upstream deleted it only when the upstream's trash refs (or the signed
  branch-trash listing from the app) name its tip, no worktree has it checked
  out, and it carries no unpublished work. A failed or malformed listing
  deletes nothing.
- **One bad ref must not fail a whole sync.** A mirror ref whose tip object is
  unreadable is dropped from the mirror (only when `git cat-file` cannot read
  it, with its value as the old-value guard) and the fetch retried; a bad
  local copy is excluded with a negative refspec for a while.
- **A stop signal reaches the Actions runner's children.** Restarting the
  systemd unit sends `SIGTERM` to every process in it. A phase process that
  ends unsuccessfully after a stop signal is an interruption
  (`ActionRunner::interruptForShutdown`): the run is re-queued, not failed, and
  a push whose snapshot could not be bound is written back to the spool.
- **A checkout that loses a race on the mirror's worktree list is retried.**
  Concurrent `git worktree prune` can delete an admin folder mid-`worktree
  add`; `ActionRunner::checkoutLostWorktreeRace` recognises the errors and
  retries into a fresh folder, at most three attempts.
- **`on.push.paths` is matched over the whole pushed range.**
  `forkmesh::actions::pushRangeChangedPaths` diffs `<old> <new>` when the old
  value is an ancestor, and otherwise the union of the tip's diffs against
  every parent. Every push source must carry the ref's old value. Each
  pattern is compiled once per match, not once per changed path
  ([diagnostics.md](diagnostics.md#keeping-the-gui-thread-free)).
- **A push to an old branch runs that branch's copy of a workflow.** When the
  default branch's copy of the same workflow has no `push` trigger, a push to
  any other branch skips it (`ActionFile::defaultBranchRetiresPush`).
- **Workspaces clone `--single-branch`.** A torn loose ref in the mirror is
  advertised as all zeros and breaks a full clone.
- **A desktop that cannot sign for its own private repository has nothing to
  download.** `syncPrivateRepository()` falls back to
  `downloadPrivateReplica()` when the owner signing capability is missing,
  which is right for a collaborator's copy but wrong for a repository this
  account created on this computer: a guest creating a Private repository got
  "Private repository is unavailable or this account is not authorized". A
  record owned by `accountOwner()` with a local source now stays local and
  says it needs a signed-in account to publish.

## Related docs

- [www/docs/qt-client.md](../../www/docs/qt-client.md): user guide
- [www/docs/operations/desktop-control-node.md](../../www/docs/operations/desktop-control-node.md): Control page
- [www/docs/engineering/definition-of-done.md](../../www/docs/engineering/definition-of-done.md)
- [../engineering/destructive-actions.md](../engineering/destructive-actions.md): guarded deletes
- [../native/DESKTOP_PARITY.md](../native/DESKTOP_PARITY.md): native app parity with the desktop
