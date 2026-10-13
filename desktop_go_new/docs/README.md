# desktop_go_new: forkmesh-core, the Go background engine

Issue 1336 (2026-10-02) started splitting the desktop into a responsive Qt UI and
a lightweight Go engine, `forkmesh-core`, that does the processing. The desktop
and the phones (iOS, Android, and the web over the relay) reach the same engine.
The work moves in stages: the transport and the families with no UI state moved
first, and the checklist at the end tracks the rest. A method family keeps
working while it waits, because the core hands every method it has not taken
over back to the desktop's existing C++ handler.


## Where everything lives (2026-10-04)

Every change in this work stays inside `desktop_go_new/`, and nothing outside it
differs from `main`. The existing desktop in `desktop/` builds and behaves
exactly as before.

### Layout

| Path | What it is |
| --- | --- |
| `desktop_go_new/cmd`, `desktop_go_new/internal` | The Go engine, `forkmesh-core` |
| `desktop_go_new/desktop-overlay/files/` | New Qt sources, at the paths they take in the desktop tree (`desktop/src/CoreClient.cpp`, the facades, `MainWindowCore*.cpp`) |
| `desktop_go_new/desktop-overlay/patches/10-desktop.patch` | The hooks in existing desktop files: `CMakeLists.txt`, `install.sh`, the classes that get a core-backed facade |
| `desktop_go_new/desktop-overlay/patches/20-marketplace.patch` | The Disk Cleanup package's core facade and its version bump |
| `desktop_go_new/desktop-overlay/patches/upstream-*.patch` | Changes to tools, tests, the site and the docs. They are kept for when this work merges into `main` and are not applied by the build. The release-host test in `upstream-tools-and-tests.patch` (`test_release_password_reads_plain_files_and_reports_an_unreadable_vault`) still fails and needs fixing before that patch is proposed. |
| `desktop_go_new/docs/README.md` | This page |
| `desktop_go_new/tools/forkmesh_desktop_vault.py` | The read-only Python vault reader for the release host |

### Building the integrated desktop

1. `desktop_go_new/desktop-overlay/build.sh` runs `stage.sh`, which builds the
   stage `~/projects/forkmesh/builds/desktop-go-new/stage`:
   - copies `desktop/`, `marketplace/` and `www/` from the checkout;
   - adds the overlay files;
   - applies the numbered patches;
   - symlinks every other top-level folder (`www` is copied because the
     marketplace generator refuses resources that resolve outside the tree).
2. It then configures and builds `forkmesh` and `forkmesh-core` in
   `~/projects/forkmesh/builds/desktop-go-new/build`.
3. Override the locations with `FORKMESH_GO_NEW_BUILD_ROOT` and
   `FORKMESH_GO_NEW_BUILD_DIR`, and the job count with `FORKMESH_GO_NEW_JOBS`.
4. If a patch stops applying because `main` changed a hooked file, fix it in the
   stage and refresh the overlay (see Developing).
5. Rebased 2026-10-06 onto `main` 468977bb0d: `main` added agent folders to
   `agents.list` (the desktop now pushes `folders` in the agent context and the
   core's `agents.list` returns them, defaulting to `[]`), and "running a
   shortcut again restarts it" (the core's `started` event carries the path in
   `hooks().shortcutPath`, so `shortcutRunning()` and the restart after
   `core.insights.shortcut.finished` cover core-run shortcuts too). The other
   conflicts were adjacent additions (`GenieSpiderWidget`, `AgentScratch.h`).
   The rebase replays the patches at their base commit and cherry-picks them,
   so git's three-way merge does the work; `save.py` then rewrites them.

### Running it

`desktop_go_new/run.sh` stages, builds and launches the integrated desktop in
one step, with `FORKMESH_CORE_BIN` pointing at the `forkmesh-core` built beside
it. It honours the same `FORKMESH_GO_NEW_*` variables as `build.sh`.

| Command | What it does |
| --- | --- |
| `run.sh` or `run.sh run` | `build.sh`, then launch `forkmesh` |
| `run.sh build` | `build.sh` only (extra arguments go to the first `cmake` configure) |
| `run.sh start` | Launch the last build without staging or building |
| `run.sh test` | `go test ./...` with the ForkMesh path variables dropped |

Like `desktop/run.sh`, it keeps the caller's directory in
`FORKMESH_START_DIRECTORY`. Staging overwrites the stage's `desktop/` and
`marketplace/` from the checkout, so run `save.py` first when the stage holds
unsaved edits, or use `run.sh start`.

The stage is a plain copy, not a git checkout. Its build commit reads
`unknown`, and Rebuild & restart in that binary rebuilds the stage, not the
primary checkout.

### Developing

- Edit the stage's `desktop/` or `marketplace/`, then run
  `desktop_go_new/desktop-overlay/save.py`. It copies files that are new
  compared with the checkout into `files/`, and rewrites the two numbered
  patches from the stage's differences.
- Go code is edited in place in `desktop_go_new/`. The stage links to it.

### Secrets: copy until finalized

The unchanged desktop and the release tools still read the Actions variables,
the chat device key, the genie token and the release password files in
plaintext. So the vault copies them in and keeps the plaintext:

- **Migration** is sealed and verified as before, and it re-copies newer
  plaintext every minute.
- **Finalizing** (`core/vaultFinalize=true` in the settings, or
  `FORKMESH_VAULT_FINALIZE=1`) replaces the plaintext with markers. This is the
  earlier behaviour, and it only makes sense once the tools in
  `upstream-tools-and-tests.patch` have landed.
- **Desktop facade:** while the vault is not finalized, the facade writes both
  the vault and the plaintext. Refusing to write plaintext without a core
  applies only after finalizing.

## Code map

| Path | Responsibility |
| --- | --- |
| `desktop_go_new/cmd/forkmesh-core` | The binary: `serve`, `call <method> [json]`, `status`, `stop`, `version`; `modules.go` lists the modules |
| `desktop_go_new/internal/daemon` | `Core`: layout, identity, registry, hub, settings cache, built-ins (`core.ping`, `core.status`, `core.routes`, `core.owners`, `core.shutdown`), single-instance lock, linger |
| `desktop_go_new/internal/ipc` | Local transport for the desktop and CLI: newline-delimited JSON, requests both ways, `control.adopt` socket hand-off |
| `desktop_go_new/internal/rpc` | Method registry (exact names and `prefix.` families), `ErrDelegate`, peers, hub broadcast |
| `desktop_go_new/internal/paths` | The `~/projects/<project>` layout, rule for rule the same as `tools/forkmesh_paths.py` (a test runs both) |
| `desktop_go_new/internal/identity` | The Ed25519 node key from `data/identity/ed25519.pem` |
| `desktop_go_new/internal/qsettings` | Read-only reader for `ForkMesh.conf` (the desktop stays its only writer) |
| `desktop_go_new/internal/control` | The phone control protocol: handshake, AES-GCM frames, pairing, account checks, relay chunking and pacing, file transfer, proxy to the desktop |
| `desktop_go_new/internal/files` | `files.browse.*` |
| `desktop_go_new/internal/diag` | `diag.watch`, `diag.unwatch`, `diag.log`, `diag.buildMac`, `diag.health`, `diag.processes`, `diag.stalls`, `diag.stall`, `core.vitals` (with `detail`), `core.diag.stall/stalls` |
| `desktop/src/CoreVitals.{h,cpp}`, `CoreDiag.{h,cpp}`, `MainWindowCoreDiag.cpp` | Footer and Stats page vitals from `core.vitals.sample`; stall records pushed to the core; `ui.diag.state` |
| `desktop_go_new/internal/repos`, `internal/git` | `repos.commits`, `repos.branches`, `core.git.*` (including `core.git.run` and `core.git.agentDiffStats`); git runner with the per-repository index queue |
| `desktop_go_new/internal/stats` | `stats.repo`, `stats.ratchet.prepare/run`, `stats.codemagic`, `insights.shortcut`, `insights.shortcuts` |
| `desktop_go_new/internal/secrets` | `secrets.list/prepare/set/delete` (writes go through `ui.secrets.apply`) |
| `desktop_go_new/internal/vault` | Encrypted secrets at rest: `data/vault/secrets.json`, `core.vault.*`, the Actions variables migration, `vault.Variables(core)` for modules ([Secrets at rest](#secrets-at-rest-the-vault)) |
| `desktop/src/CoreVault.{h,cpp}`, `ActionVariablesBackend.h`, `VaultFile.{h,cpp}` | Qt facade behind `ActionStore::variables()/setVariables()`, and the read-only C++ vault reader used without the core |
| `desktop/src/SettingsSecretBackend.h`, `tools/forkmesh_desktop_vault.py` | The facade for secrets kept under a `ForkMesh.conf` key (chat device key, Genie token); the read-only Python vault reader the release host uses |
| `desktop_go_new/internal/teammates` | `teammates.options` |
| `desktop_go_new/internal/insights` | Tool-efficiency, improvement and guardrail ledgers (`tool-efficiency.json`, `tool-improvements.jsonl`, `guardrails.json`), the shortcut script runner, the agent socket sampler: `core.insights.*`, `insights.tools/guardrails/network/shortcutRun/prepare/run` ([Insights ledgers and the shortcut runner](#insights-ledgers-and-the-shortcut-runner)) |
| `desktop/src/CoreInsightsHook.h`, `MainWindowCoreInsights.cpp` | Facade: the three ledger classes forward writes to the core and mirror its snapshots; Shortcuts page runs through the core; `ui.insights.*` answers |
| `desktop_go_new/internal/phonefamily` | Shared owner gate, confirmation tokens (the C++ `ConfirmationStore`), parameter reading and device labels for phone families |
| `desktop_go_new/internal/qvariant` | Decoder (and test encoder) for `@Variant(...)` values in `ForkMesh.conf` (`QDataStream` `Qt_4_0`) |
| `desktop/src/CoreSecrets.{h,cpp}` | `ui.secrets.apply`: the desktop writes the Actions variables the core approved |
| `desktop_go_new/internal/terminal` | PTY sessions for the desktop: `core.terminal.*` |
| `desktop_go_new/internal/agents` | Read-only agent store: `core.agents.sessions/transcript/events/search` |
| `desktop_go_new/internal/agents` (writer) | The single writer of `data/agents/**` (`core.agentstore.*`): meta records, transcript and event lanes, tombstones, recycle bin, model outcomes |
| `desktop_go_new/internal/qtjson` | JSON that parses and prints exactly as `QJsonDocument` does (number forms, key order, escapes, `Indented`); golden fixtures come from the real Qt |
| `desktop_go_new/internal/agentrun` | Agent runs parsed in the core: Claude stream-json, the Codex app-server JSON-RPC and the `AgentRunner` log, transient dialog runs, durable stderr, normalised events, transcript writes, redaction (`core.agentrun.*`); native `agents.transcript`, `agentops.bin` |
| `desktop/src/CoreAgentStore.{h,cpp}`, `CoreAgentRun.{h,cpp}`, `MainWindowCoreAgentRunner.cpp` | `AgentStore` writes routed to the core (Unix socket or loopback TCP); the run event hub behind `ClaudeStreamSession`, `CodexAppServerSession` and `AgentRunner` core mode; the `AgentRunner` re-attach |
| `desktop_go_new/internal/agentmodel` | The agent session model for phones and the desktop: session list, pushed desktop context, `agents.list/queue/follow/transcript`, `agentops.*`, `diag.prepare/run`, background-work completion checks ([Agent session model](#agent-session-model)) |
| `desktop/src/MainWindowCoreAgentModel.cpp`, `CoreAgentModel.h` | Pushes the desktop's agent context, answers `ui.agents.*`, applies core completion checks, core-confirmed agentops |
| `desktop_go_new/internal/agentproc` | Agent processes behind a per-process shim: spool, replay, acks, containment (`core.agentproc.*`); `desktop/src/AgentProcess`, `CoreProcess`, `MainWindowCoreAgents.cpp` |
| `desktop_go_new/internal/coretest` | Test harness: a core in a temp sandbox, fake peers |
| `desktop_go_new/internal/relay` | The node-role WebSocket to the relay (`/api/nodes/events`): `core.relay.*`, `core.relay.state`/`core.relay.event` events |
| `desktop/src/NodeEventSocketCore.cpp` | `NodeEventSocket` backed by the core's relay socket |
| `desktop_go_new/internal/hosts` | Saved hosts (read, and written through `WriteSettings`), the host SSH runner, probes, mirror service actions, phone log streams, host jobs (installer, uninstall, deploy-all, agent CLIs, Actions configuration, link codes, Vultr provisioning and DNS, destroy, node delete), the SSH leg of the host log feeds, the mirror push engine (SSH remotes, managed-host fan-out, release gates, artifact CAS), git-remote unwiring, agent logins and credential storage: `hosts.*`, `fleet.*`, `core.hosts.*`, `core.mirror.*` |
| `desktop/src/MainWindowCoreHosts.cpp` | Hosts facade: probes and mirror-node SSH through the core, log lines to the core, `ui.hosts.context` |
| `desktop/src/MainWindowCoreHostJobs.cpp` | Host jobs facade: the Hosts page's installers, Vultr provisioning, destroy and node delete run as core jobs; transcripts, statuses and snapshots rendered; `ui.hosts.*` answers |
| `desktop/src/MainWindowCoreMirrorPush.cpp` | Mirror push facade: `pushToSshMirrorRemotes`, `pushRefsToManagedHosts` and `removeSshMirrorRemotesForNode` handed to `core.mirror.*`; completion callbacks, the repository table |
| `desktop_go_new/internal/mesh` | The `#mesh` room (`LocalMeshNode` ported): UDP beacons, TCP links, `RoomCrypto`/`RoomSeal`, history, queue, wake tokens, diagnostics: `core.mesh.*` |
| `desktop/src/CoreMesh.{h,cpp}` | `LocalMeshNode` backend over `core.mesh.*`; bridges the desktop's Bluetooth links to the core |
| `desktop/src/CoreClient.{h,cpp}` | Qt client: finds or launches the core, connects as the UI, requests both ways, hands phone sockets over |
| `desktop/src/MainWindowCore.cpp` | Wiring: attaches `LocalControlServer` to the core, answers `ui.*` requests |
| `desktop/src/CoreGit.{h,cpp}` | Blocking core client for worker threads; routes index-writing git commands to `core.git.run` |
| `desktop/src/CoreAgentPages.{h,cpp}` | Agents page: diff and worktree stats (`core.git.agentDiffStats`) and transcript search (`core.agents.search`) |
| `desktop/src/CoreTerminal.{h,cpp}`, `TerminalBackend.h` | `TerminalWidget` backed by `core.terminal.*`; the workspace terminal survives a desktop restart |
| `desktop_go_new/internal/update` | Rebuild & restart in the core: pull, configure, build passes, install, relaunch; `update.*` native, `core.update.*` |
| `desktop_go_new/internal/backup` | Hourly snapshots, restore unpack, export, `diag.backups`, `core.backups.*` |
| `desktop_go_new/internal/cleanup` | Cleanup host removals: `core.cleanup.removeWorktree`, `core.cleanup.removeSandbox`; the Disk Cleanup scanner and its deletes: `core.cleanup.scan/deleteFiles` |
| `desktop_go_new/internal/pulls` | `PullStore` in the core: pull list, signed comments, reviews and threads, branch reviews, create, status, delete, intake, sessions, `core.pulls.*` ([Pull store](#pull-store)) |
| `desktop/src/PullStoreCore.{h,cpp}`, `CorePulls.{h,cpp}` | `PullStore` facade glue and its core backend; `ui.pulls.rewrite` in `MainWindowCoreMergeQueue.cpp` |
| `desktop_go_new/internal/mergequeue` | `mergequeue.*` native and the merge executor: `core.mergequeue.*` |
| `desktop/src/MainWindowCoreMergeQueue.cpp` | Merge queue facade: `ui.mergequeue.view/apply/purge`, the executor handed to the core |
| `desktop_go_new/internal/actions` | `.forkmesh` workflows: parser, run store, sandboxed runner, run queue, pull-request checks, served release landing and auto-fix, mirror Actions configuration (variables sealed in the vault), `core.actions.*`, `repos.list`, `repos.actions*`, `repos.prepare/run` |
| `desktop/src/CoreActions.{h,cpp}`, `MainWindowCoreActions.cpp`, `MainWindowCoreActionQueue.cpp`, `ActionSandboxClaims.h` | Actions facade: runs execute in the core, followed from core events and re-attached after a restart, pull checks and auto-fix handed to the core, `ui.actions.repo/perform/autoFix`, cross-process sandbox claims |
| `desktop/src/CoreMirrorActions.{h,cpp}` | `--configure-mirror-actions-stdin` delegate: the mirror Actions transaction through `core.actions.mirrorConfig.apply` |
| `desktop_go_new/internal/safedelete` | Guarded tree delete (port of `server/safedelete.go`) and the linked-worktree guard |
| `desktop_go_new/internal/ownergate` | Owner gate, desktop-only guard and device labels for the update and backup modules |
| `desktop/src/MainWindowCoreOps.cpp`, `CoreOps.h` | Facades: Rebuild & restart, backups and restore, `ui.update.*` |
| `desktop_go_new/internal/netlog` | System log digest: badge classifier, `network_log.txt` and its archive read incrementally, `stats.timeline`, `core.log.append` |
| `desktop_go_new/internal/quality` | `RepoQuality::scan` port, issue status reader, `stats.quality` |
| `desktop/src/CoreStats.{h,cpp}` | Sends each system-log line to the core (`core.log.append`, batched) |

## Architecture

- **One core per project.** `forkmesh-core serve --project <slug> --state-dir
  <state>/core` holds `core.lock` (flock) and writes `endpoint.json` (pid,
  version, socket path, loopback TCP port, token; mode 0600) in
  `state/core/`.
- **Launch.** `CoreClient` reads `endpoint.json` and connects. When nothing is
  listening, it starts the binary through `processreaper::startReleased()`.
  That launch drops the instance marker and double-forks, so a desktop quit or
  Rebuild & restart does not reap the core
  ([lifecycle.md](../../docs/desktop/lifecycle.md#what-a-quit-deliberately-leaves-running)).
- **Finding the binary.** `FORKMESH_CORE_BIN`, then `forkmesh-core` beside the
  app (also `../libexec/forkmesh/` and `../Resources/`).
- **Linger.** The core outlives the desktop by `--linger` (2 minutes by
  default) and stays up while a module reports `Busy()` (an open phone
  connection, a PTY session, a running fetch). Phone channels and terminals
  therefore survive a desktop restart.
- **Upgrades.** When the binary on disk is newer than the running core's start
  time, the desktop asks it to `core.shutdown` and starts the new one.
- **Switching it off.** Set `FORKMESH_CORE=0`, or `core/enabled=false` in the
  settings, and the desktop runs as before, with no core.
- **Who answers a method.** A module registers exact method names on
  `rpc.Registry`. For a phone request the control module:
  1. looks the method up;
  2. runs the native handler if there is one;
  3. otherwise sends the request to the desktop as `ui.control.request`, and the
     desktop answers through its unchanged C++ handler.

  A native handler can return `rpc.ErrDelegate` to pass a single request on (the
  files module does this for an owner it cannot vouch for). The desktop learns
  the native routes from the hello reply (`CoreClient::nativeRoutes()`).
- **Owner checks.** The owner-only families (`files.browse.*`, `diag.*`,
  `stats.*`, `repos.*`, and the others in `MainWindowLocalControlFamilies.cpp`)
  answer `not_owner` unless the phone's user matches the desktop's node owner or
  account. The desktop pushes both names with `core.owners` whenever it sends
  `control.info`. `repos.*` asks the desktop instead (`ui.repos.locate`), which
  runs the exact C++ owner, slug and checkout checks and returns the directory.

## Protocols and interfaces

### Local IPC (desktop and CLI ↔ core)

- **Transport.** A Unix socket `state/core/core.sock` (0600, in a 0700 folder)
  on Linux and macOS. Loopback TCP everywhere, which needs the endpoint token
  (Windows uses this).
- **Hello.** The first line is
  `{"id":N,"method":"core.hello","params":{"role":"ui"|"cli","token":…,"pid":…}}`.
  The reply carries the version and the route table. A newer `ui` hello takes
  over from the previous one.
- **Messages after the hello.** Requests `{id,method,params}`, responses
  `{id,ok,result}` or `{id,ok:false,error}`, events `{event,data}` and
  notifications (a request with no id). Both sides may send requests.
- **Ordering.** Notifications from one connection are handled in order;
  requests run concurrently.

### Phone sockets

- **LAN.** `LocalMeshNode` still owns the mesh TCP port. On a control hello it
  calls `CoreClient::adoptControlSocket()`, which opens a new IPC connection
  and sends one `control.adopt` line. On Unix the TCP descriptor rides that
  `sendmsg` as `SCM_RIGHTS`, so the desktop is out of the path from then on.
  Elsewhere the connection becomes a byte pipe, and the desktop drops the
  core's one-line reply before relaying.
- **Relay.** When the core owns the relay socket ([Relay socket](#relay-socket)),
  phone frames go straight from `internal/relay` to the control module and
  back, and the desktop is not in the path. Otherwise `onNodeControlFrame`
  forwards frames as `control.relay.in`, the core sends with `ui.relay.send`,
  and the desktop writes to its own `NodeEventSocket`.
- **Desktop-side state.** `LocalControlServer` runs in core mode. Connections
  the core reports (`ui.control.opened`) get ids from
  `LocalControlServer::kCoreConnectionBase`, and their replies go back as
  `control.reply`. The desktop stops writing `paired-devices.json`: the core
  owns it and pushes `ui.control.devices`. Approve, deny, forget and send-file
  become core requests. If the core goes away, `detachCore()` closes those
  connections and reloads the device file, so new phones are handled natively
  again.
- **Full contract.** Every message name is in
  `desktop_go_new/internal/control` (see its tests).

## Phone families: secrets, teammates, insights, merge queue, wallet, Discord, stats

Every method of these families was checked against its C++ handler. A method
moved only where the core can build the same reply; the others still reach the
desktop's C++ handler through the proxy, for the reason in the table. Native
handlers keep the family's owner gate (`not_owner` unless the device has a key
and its user is the node owner or the account) and its confirmation rules
(`internal/phonefamily`, the same as `ConfirmationStore`: a token is bound to
the device, the action and the target, lasts 60 s, is spent by any attempt,
and at most 32 are outstanding).

### Native

- **`secrets.*`** (`internal/secrets`).
  - Same checks and codes as the C++: `bad_request`, `invalid_name` (a name is
    `[A-Za-z_][A-Za-z0-9_]*`, at most 128 characters), `value_too_large` (more
    than 16 KiB of UTF-8), `not_found`, and the three confirmation errors.
  - `secrets.list` returns names only (up to 500), read through
    `vault.Variables` (the vault plus any not yet migrated plaintext; see
    [Secrets at rest](#secrets-at-rest-the-vault)).
  - Writes: the desktop stays the only writer of the settings file. Once the
    token is spent, the core asks the desktop `ui.secrets.apply` (`action`,
    `name`, `value`). The desktop checks again, answers `not_found` for a name
    that is gone, writes through the vault facade (`core.vault.set`; a refused
    write answers `vault_unavailable`), syncs `QSettings`
    and reloads the Variables page. The core then writes the "Set the Actions
    variable …, requested by …" line through `ui.log`, replies `{ok: true}` and
    sends `secrets.changed` to the phones. Without a desktop the answer is
    `desktop_unavailable`.
  - The core never logs a value and never returns one.
- **`teammates.options`** (`internal/teammates`): the static lists (providers,
  modes, schedule modes, task types with labels, editable fields), byte for
  byte the C++ reply.
- **`insights.shortcuts`** (`internal/stats/insights_shortcuts.go`).
  - The core finds the four scope folders (`shortcut`, `repository`, `app`,
    `organization`) from the saved repositories and reads up to 200 files of up
    to 64 KiB each. It parses their metadata and adds the use counts and run
    results from `shortcuts/usageCounts` and `shortcuts/runStats`. Those are
    `QVariantMap` values, decoded by `internal/qvariant`.
  - File rules follow `QDir`: regular, readable, not hidden, sorted by name
    case-sensitively. The skill filter (`*.skill`, `*.md`, `*.prompt`, `*.txt`)
    ignores case. A file without a suffix is a `script` when the user may run
    it (`access(X_OK)`).
  - Shortcut scripts run in the core (`internal/insights`), so `running` and
    `lastOutput` come from the core's runner. Only when the core runs nothing
    does one `ui.insights.shortcutState` call ask the desktop (a run it
    started before it attached). Without a desktop nothing else is running.
- **`insights.tools`, `insights.guardrails`, `insights.network`,
  `insights.shortcutRun`, `insights.prepare`, `insights.run`**
  (`internal/insights`). See
  [Insights ledgers and the shortcut runner](#insights-ledgers-and-the-shortcut-runner).

- **`mergequeue.list`, `mergequeue.prepare`, `mergequeue.run`**
  (`internal/mergequeue`). See [Merge queue and pull store](#merge-queue-and-pull-store).
- **`stats.timeline`, `stats.quality`** (`internal/netlog`, `internal/quality`).
  See [System log digest and repository quality](#system-log-digest-and-repository-quality).

### Still with the desktop

| Methods | Why |
| --- | --- |
| `teammates.list`, `teammates.get`, `teammates.briefing`, `teammates.stats` | Re-checked in round 4 (see [Teammates](#teammates-what-can-move)). The teammate files and the runtime overlay in `ForkMesh.conf` can be read by the core, but every reply is scoped to the Team page's selected repository (`m_teamRepoIndex`, else the open repository), status and grades come from `m_agentSessions` (the session model, moving into `internal/agentmodel` now, with no API for this yet), and `Running…` for the contributor-payout teammate is the desktop's in-memory payout run. |
| `teammates.room` | The `#team` history is the desktop's chat model (`m_history`). `saveChatHistory()` writes it to the chat history file only on some events (close, edits, room switches), keeps the last 1,000 messages, and the file name follows the active server, so a reader of the file would lag and could miss messages. |
| `teammates.selectRepo` | Changes the Team page's selected repository. |
| `teammates.prepare`, `teammates.run` | Run, stop, prompt, continue and grade agents, or save a teammate and reload the Team page: agent runtime. `prepare` stays with `run` so its tokens are on the same side. |
| `insights.usage` | Token, cost and turn totals of `m_agentSessions` (including sessions whose totals are not saved yet). Moves with the session model (`internal/agentmodel`). |
| `wallet.status` | `busy` and `queued` are the desktop's tip queue (`m_tipInFlight`, `m_tipTransferQueue`). Re-checked in round 3: the queue cannot move on its own. Each queued entry holds the passphrase the owner typed into the desktop's review dialog (wiped with `secureErase` when it is used), and every step of a transfer writes Ledger page state (`m_ledgerPayingRecordId`, the row's "processing" mark, `m_ledgerPaymentLine`, the CFO payout run). Moving it means moving `TipSigner`, the three-phase blockhash, sign and broadcast flow, the payout receipts and the Ledger state machine together. |
| `wallet.rewards` | Returns the Ledger page's cached `/api/rewards/estimates` answer (`m_ledgerPayable`). A core-side fetch would change when the list refreshes. |
| `wallet.prepare`, `wallet.request` | The request opens the tip or payment dialog on the desktop, where the owner reviews it and types the wallet passphrase. The pending request and its tokens live with that dialog, and `wallet.status` reports that request, so the whole family stays on one side. |
| `discord.status`, `discord.list`, `discord.messages`, `discord.prepare`, `discord.send` | The gateway client and message history are the Discord package's `DiscordClient` inside the desktop (`m_history`, `m_reactions`). Re-checked in round 3: a Go gateway client is possible (`internal/relay` already speaks WebSocket), but the client is 2,600 lines of package code (REST, gateway resume, rate limits, attachments, avatars, typing, reactions) that feed the desktop's chat model. The phone reads that chat model. A core client would make a second owner of the history, so the client should move only together with the chat model. |
| `stats.sentry` | The snapshot is `remoteSnapshot()` of the Sentry package's desktop extension, an in-memory object the package's own client fills. Confirmed in round 3: it moves only if that package's client moves to the core. |
| `pings.fixes`, `pings.fix` | Pings are the desktop's notification list, and fixes run desktop actions. Re-checked in round 3: the ping journal on disk cannot stand in for the list. It is saved on a debounce, and `localId` is renumbered from `m_nextNotificationId` on every start. The fix context also reads `m_actionRuns` and `disabledWorkflows`. The fixes (turn a workflow off or on, re-run, approve, reject, start or prompt an agent) run the desktop's Actions queue and agent runtime, and the desktop writes the fix ledger (`notePingForFixes` on every new ping). |

These names are not methods: `credentials.json` (a file the files browser
hides), `process.execute`, `network.connect`, `secret.read`, `tool.invoke` and
`device.control` (agentops capability names), `ratchet.on` and `ratchet.off`
(the action in the `stats.ratchet.prepare` reply).

The Variables page writes through the vault facade. The Shortcuts page builds
its own cards on the main thread; it could later read `insights.shortcuts`
from the core. Its script runs go through the core (see below).

## Insights ledgers and the shortcut runner

`desktop_go_new/internal/insights` is a port of `AgentToolEfficiencyLedger`,
`AgentToolImprovementLog`, `AgentGuardrailLedger`, `AgentToolCatalog`,
`AgentToolPolicy` (the parts the ledgers and the phone use) and the policy
half of `LocalControlInsights`. The core is the only writer of the three files
while a desktop is attached; without a core the desktop keeps them as before.

### Files and save cadence

| File (`data/agents/`) | Written |
| --- | --- |
| `tool-efficiency.json` | 5 s after the first change of a burst (ledger or improvement log), as the desktop's save timer did; at core stop when it has samples or exists; at once after a reset. Compact `QJsonDocument` form, written atomically. |
| `tool-improvements.jsonl` | One line per occurrence the moment it is found; rotated to `.1` at 2 MiB; moved to `.sent` when the signals are handed to an agent. |
| `guardrails.json` | 5 s after the first change, only when it has events; at core stop when a save is pending (the desktop dropped that last save on quit). |
| `state/core/insights-offsets.json` | With every ledger save: the last stdout offset taken from each core-parsed run, so a restarted core replays only what the saved ledger has not seen. |

### Feeding the ledger

- **Core-parsed runs (in-process).** Once a second the module lists the runs of
  `internal/agentrun` (`core.agentrun.list`) and attaches to each new one with
  an in-process peer (`core.agentrun.attach`, `after` = the saved offset). It
  receives exactly the normalised events the desktop receives, replayed ones
  included, and skips anything at or before the offset it already took. The
  session's provider and model come from its store record, the working
  directory from the agent process (`agentproc`), both cached for 30 s. Runs
  keep being counted while no desktop is attached.
- **Every other run (desktop).** `AgentToolEfficiencyLedger::ingest` and
  `abandonSession` in the desktop forward to `core.insights.ingest` and
  `core.insights.abandon` (notifications, in order), with the desktop's own
  session context, exactly the inputs the C++ ledger used. Events the core
  delivered (`coreagentrun::deliveringAnyStored()`) are not forwarded, so
  nothing is counted twice. Only the `tool_use` and `tool_result` blocks are
  sent.
- `result` and `_codex_agent_complete` abandon the session's open calls and
  reset its repeat window, as `recordAgentToolEfficiency` does.
- Guardrail events stay where they are detected (the desktop's guardrail
  audit and its Approve/Deny dialog) and go to `core.insights.guardrail`.
- Switching a tool family off updates the core's set (`core.insights.disabled`);
  the core reads `agents/disabledToolFamilies` at start.

### Desktop facade

- `CoreInsightsHook.h` holds the hooks the three ledger classes check. A ledger
  is core-owned when the core is attached, has answered `core.insights.state`,
  and its file is one of the core's three files (a window test's private agent
  root therefore stays local).
- Core-owned objects forward every write (`clear`, `archiveSent` and the record
  calls included), skip `save()`/`saveLater()`, and become mirrors: the core
  pushes `core.insights.changed` (150 ms debounce, only the sections that
  changed) and `MainWindowCoreInsights.cpp` loads it into the same objects
  (`fromJson`, which now also reads an `inFlight` count, and
  `AgentToolImprovementLog::loadSnapshot`), then calls their `changed`
  callbacks. Boards, tables, the Stats page and the digest prompt read the
  objects unchanged.
- On attach, a desktop that changed a ledger locally saves it first and asks
  `core.insights.reload`; the core reloads the files only when it has no
  unsaved changes and no open calls. On detach the desktop reloads the files.

### Shortcut runner

- `core.insights.shortcut.start/stop/state`: `bash <script>` in the repository
  checkout, stdout and stderr merged, 96 KiB of output kept, the first line
  `$ bash <path>`, then `done (exit 0)`, `failed (exit N)` (the signal number
  for a killed script, as `QProcess` reports it) or `failed to start bash`.
  Stop sends SIGTERM, then kills after 3 s.
- Events: `core.insights.shortcut.started/output/finished/stats` to the
  desktop (the Shortcuts page panel, status line and stop button) and
  `insights.shortcutOutput` and `insights.changed` to phones.
  `runShortcut` and `stopShortcut` on the desktop go through the core when it
  has the route; the core runs one shortcut at a time for both.
- Run stats stay in `ForkMesh.conf` (`shortcuts/usageCounts`,
  `shortcuts/runStats`). The core keeps operations (a start counts a use and
  stamps `lastRunAtMs`; a finish records the outcome, or only the last output
  line when the run was stopped), and applies them to the current values when
  the desktop is attached: read, apply, write through `core.WriteSettings`
  with the new `variant` type (base64 `QDataStream` `Qt_4_0` bytes of a
  `QVariant`). `internal/qvariant.Encode` writes the same bytes as Qt (golden
  test), including null strings for an empty `lastOutput`. Operations queue
  while no desktop is attached. `ui.settings.write` clears the Shortcuts
  page's in-memory counters whenever a `shortcuts/` key is written.
- Prompt and skill shortcuts start agents, which is the agent runtime:
  `insights.run` asks the desktop (`ui.insights.runShortcutAgent`, the same
  checks and error codes as before).

### Phone methods

- `insights.tools` and `insights.guardrails` are built from the core's
  ledgers; the disabled families and guardrail policies are read from
  `ForkMesh.conf` (`normalizeDirectories` ported).
- `insights.network` samples in the core: `sock_diag` over netlink for every
  TCP socket and its byte counters, `/proc/<pid>/fd` for the sockets of each
  agent's process tree. The roots (session, pid, label) come from the desktop
  (`ui.insights.networkRoots`, because its sessions may run outside
  `agentproc`); without a desktop, from the running `agentproc` processes and
  their store records. Linux only, as before.
- `insights.prepare/run` tokens are issued and spent in the core.
  `toggleFamily` runs `setAgentToolFamilyEnabled` on the desktop
  (`ui.insights.toggleFamily`: the setting, the board, and the notices to
  running agents), so without a desktop it answers `desktop_unavailable`.
- `insights.usage` stays with the desktop (session totals in memory).

### Tests

`golden_test.go` replays a scenario through the Go port and compares it with
`testdata/golden.json`, written by `testdata/golden.cpp` linked against the
real C++ classes (`testdata/gen.sh`): the ledger file, totals, family, usage
and model rows, every improvement line in the JSONL file, the digests, loads
of hand-made and broken files, the guardrail ledger, command shapes, family
classification, enforcement notes, the catalog, `normalizeDirectories`, the
shortcut output helpers, confirmation subjects and summaries, network rows,
and the `QVariant` bytes of the shortcut counters. `module_test.go` covers the
desktop feed, the in-process feed with offsets across a restart, the phone
gates, the shortcut runner with its settings writes, and the network method.

### Teammates: what can move

- `teammates.list`, `get`, `briefing` and `stats` read three things: the
  teammate files under `<checkout>/.forkmesh/teammates` and the runtime overlay
  under `teammates/runtime/<sha>/<id>/` in `ForkMesh.conf` (both readable by the
  core today), the agent sessions (status, grade, cost, latest output: the
  session model), and two pieces of desktop state, the Team page's selected
  repository and the contributor-payout run. They can move once
  `internal/agentmodel` exposes sessions to other modules and the desktop
  context it receives carries the Team repository and the payout run.
- `teammates.prepare/run` start, stop, prompt and grade agents and edit the
  Team page; they move with the agent runtime.
- `teammates.room` cannot be read from disk with parity (see the table above);
  it stays with the desktop's chat model.

## Relay socket

The node-role WebSocket to the relay (`wss://<relay>/api/nodes/events`) runs in
the core (`desktop_go_new/internal/relay`). It is a port of
`desktop/src/NodeEventSocket.cpp`, and the desktop keeps that class as a facade,
so the MainWindow code that uses its signals and send methods is unchanged.
The point of the move: a phone away from home keeps its control channel while
the desktop restarts, because the socket and the phone protocol both live in
the core.

- **Methods (desktop only, `forbidden` for phone peers).**
  - `core.relay.configure {url, account, node, identity?}`. `url` is the URL
    the desktop's URL factory built, with its signed `owner`, `ts` and `sig`
    query. The core keeps the endpoint and the owner. It checks that `sig` was
    made by the key in `data/identity/ed25519.pem` (and that the key matches
    `identity` when given): if the desktop imported another identity since the
    core loaded it, the core reloads the file, and if the key still differs it
    answers `identity_mismatch`. `invalid_url` (not `ws`/`wss`, no host) and
    `invalid_owner` are the other refusals. The same endpoint and owner as the
    running socket keep the connection; anything else closes it (close 1000)
    and connects again. The reply is the state below.
  - `core.relay.stop`, `core.relay.reconnect` (`{reconnecting}`; skips the
    backoff wait like `reconnectNow()`), `core.relay.state`.
  - `core.relay.send {kind, frame}` → `{sent}`. `kind` is `signed`
    (`sendSignedFrame`: only a frame type the relay listed in `accepts`),
    `terminal` (`sendTerminalFrame`) or `control` (`sendOwnerControlFrame`,
    paced); anything else is `invalid_kind`.
- **Events to the desktop.** `core.relay.state {running, connected, accepts,
  relays, controlBacklog, budgetWindowMs, budgetLimit, attempts, seq}` on every
  connect, link loss, hello, wait for a reconnect, and when the control queue
  starts or stops waiting on the budget. `seq` only grows within one core
  process; the facade drops a state older than the one it holds (a configure
  reply can arrive after the event that followed it). `core.relay.event
  {frame}` carries every `event`, `terminal`, `agent-status-result` and
  `heartbeat-result` frame unchanged, and `control` frames when no control
  module is loaded. `hello` frames and unknown types are not forwarded.
- **Authentication.** The same as the C++: each attempt mints a fresh
  `forkmesh-issues-pull-v1\n<owner>\n<ts>` signature with the node key (query
  order `owner`, `ts`, `sig`). The desktop never sends its account session
  token, and the socket does not need one. The User-Agent is
  `ForkMeshDesktop/<core version> (account=<a>; node=<n>)`, each label trimmed,
  cut to 64 UTF-16 units and percent-encoded like `QUrl::toPercentEncoding`.
- **Same constants as `NodeEventSocket.cpp`.** Connect, TLS and upgrade within
  30 s. Any `HTTP/1.x 101` is accepted (the C++ does not check
  `Sec-WebSocket-Accept` either); any other status logs "Relay event socket
  upgrade refused (<status line>); falling back to the sync poll until it
  reconnects." through `ui.log`. Reconnect backoff 2 s doubling, capped at
  2 minutes, plus a random 0 to delay/4 + 250 ms; a successful upgrade resets
  it. No URL (not signed in) waits the same backoff silently. A protocol ping
  every 25 s, an application `{"type":"ping"}` when no text frame went out for
  4 minutes, and the link is dropped when nothing arrived for 70 s. Inbound
  frames above 64 KiB drop the link; outbound frames above 60 KiB are refused.
  A server ping is answered with a pong, a server close is echoed and the link
  reconnects. `TCP_NODELAY` is set on the socket. Client frames are masked.
- **Control frames and pacing.** A `{"type":"control","from":"browser"}` frame
  goes to the control module (`RelayIn`), and a frame from another node is
  dropped, as in the C++. Each successful upgrade calls `RelayRestored`, so
  closes that could not be sent go out at once (`relay_reconnected`); the
  desktop's `onNodeControlRelayRestored` does nothing while the core owns the
  socket. The control module sends through the socket (`SetRelaySender`)
  instead of `ui.relay.send`. Those frames pass the same pacer as the C++
  (`control.SendBudget` at 90% of the hello's `controlBudget`, 200 per 10 s
  when absent) and wait in a queue of at most 16 MB. A link loss empties the
  queue.
- **Hook in `internal/control` (additive).** `control.Module` publishes itself
  as `core.Get(control.RelaySinkKey)` and has `SetRelaySender(func)`,
  `RelayOwned()`, `RelayIn(frame)` and `RelayRestored()`. With no sender set,
  `sendRelay` uses `ui.relay.send` as before.
- **Desktop facade.** `MainWindow::startNodeEventSocket` passes
  `m_coreClient` to `NodeEventSocket::setCoreClient`, and `startCoreClient`
  hands a late core client to a socket that already exists.
  - `start()` with a connected core runs in core mode. The facade calls the URL
    factory and the firewall authorizer itself, as before, and sends
    `core.relay.configure`. Its state (`isConnected`, `accepts`,
    `relaysTerminal`, `controlBacklog`) mirrors `core.relay.state`.
    `core.relay.event` frames go through the unchanged `processFrame`, so the
    same signals fire. The send methods answer from the mirrored state and
    pass the frame on with a `core.relay.send` notification.
  - A core client that is not connected yet gets 5 s before the facade opens
    its own socket. When the core connects later, the facade closes its own
    socket and hands over.
  - A failed configure (an older core answers `unsupported`, or
    `identity_mismatch`) logs one line and the facade opens its own socket.
  - When the core connection drops, the facade reports disconnected at once
    and opens its own socket after 15 s unless the core is back.
  - `stop()` (sign-out, offline mode, a data operation) sends
    `core.relay.stop`. Destroying the facade (a desktop quit) sends nothing,
    so the core keeps the socket.
- **Desktop restarts.** The socket does not depend on the desktop. A restarted
  desktop sends the same configure, gets the live state back, and
  `connectedChanged(true)` runs MainWindow's usual catch-up (relay sync, task
  board, desktop commands). Events that arrive while no desktop is attached are
  dropped; that catch-up covers them. If a newly attached desktop does not send
  `core.relay.configure` within 2 minutes (it started offline or signed out),
  the core closes the socket. The relay module never reports `Busy()`: the core
  outlives a closed desktop by the linger time, or longer while a phone channel
  is open (the control module is busy then).
- **Traps.**
  - The relay connection now comes from the `forkmesh-core` executable. A
    path-keyed firewall such as OpenSnitch needs its own rule for that path,
    or the socket never connects while the desktop's own HTTPS calls still
    work.
  - If the IPC link drops while the core keeps running, after 15 s both the
    core and the desktop hold a node socket, and both answer relayed phone
    frames. This ends when the core client reconnects and the facade closes
    its own socket.
- **Tests.** `internal/relay/relay_test.go` runs a loopback relay (plain and
  TLS with a test CA): the signed upgrade and User-Agent, a refused upgrade,
  frames both ways, the event forwarding and filters, control frames to a fake
  sink and to the real control module (a `channel_gone` close goes back out
  through the core's socket), reconnects after a server close and a stale
  link, `reconnectNow`, the backoff curve, budget pacing (`windowMs` 1000,
  `max` 10: nine frames at once, the tenth after the window), oversize frames,
  reconfigure and stop, a desktop detach and reattach, the reclaim window, and
  the refusals.

## Hosts and fleet

`internal/hosts` runs the work behind Network → Hosts and answers the phone's
`hosts.*` and `fleet.*` families. Since round 3 every host job runs in the
core: the installer, uninstall, deploy-all, the agent-CLI install, the mirror
Actions configuration, link codes, the Vultr provisioning machine (with its
Cloudflare DNS calls), Vultr destroy and the admin node delete. The Qt Hosts
page renders what the core streams. The jobs run on the core's context, so a
desktop quit or Rebuild & restart does not stop them, and the next desktop
re-attaches to their output.

### Saved hosts

- **Source.** The core reads the `hosts/list` setting, the JSON array that
  `forkmesh::control::loadSavedHosts()` reads.
- **Passwords.** The core drops the legacy password fields (`pass`,
  `password`, `sshPassword`, `adminPassword`) in memory, as
  `scrubSavedHostPasswords()` does. A password it finds there, or one the
  desktop sends with a probe or a job, lives only in the core's memory, like
  `m_hostSessionPasswords`. No write ever carries one: the writer strips the
  four fields again before encoding.
- **Writes.** `rememberHost`, `forgetHostNamed` and `forgetHostAt` in
  `savedwrite.go` are ports of `MainWindow::rememberHost()`,
  `forgetSavedHostNamed()` and `forgetHostAtRow()`, including
  `mergeSavedHostRecord()`, the `statusChangedAt` stamp and the two
  tombstone ledgers. They go through `Core.WriteSettings` with the C++ keys
  and types:
  - `hosts/list`: type `string`, the compact JSON array (`saveSavedHosts()`
    stores a `QString`, not a `QByteArray`);
  - `hosts/decommissionedNodes` and `nodes/deletedNodes`: type `string`,
    compact JSON objects (30-day TTL, 200 entries, newest first).
- **Without a desktop.** A write that meets `desktop_unavailable` is queued
  (`settingsq.go`, last value per key) and retried every 2 s and on the next
  attach. Reads see the queued value, so a job that saves a host and then reads
  it back works while the desktop is away.
- **Refresh.** After a write of one of those keys the core sends
  `core.hosts.saved`. The desktop drops its cached ledgers and calls
  `refreshHostsTable()`.
- **Fleet instances.** `rememberHost` refuses `fmnext-*` names and the
  addresses the desktop lists from `corelogs::loadFleetNodes()`
  (`isFleetInstanceHost`), like the C++. `fmnext-*` names are also refused
  with `protected_node` in `hosts.prepare` `install` and `fleet.prepare`
  `add` (a deliberate difference: the C++ accepts the request and then
  silently drops the write).

### SSH runner

`buildHostSSHCommand` reproduces `forkmesh::control::buildHostSshCommand`:

- **Binary.** `ssh` from `PATH`. With a session password it runs `sshpass -e
  ssh` and passes the password in `SSHPASS`; an inherited `SSHPASS` is always
  removed first.
- **Options.** `StrictHostKeyChecking=accept-new`, `HashKnownHosts=yes`,
  `UpdateHostKeys=yes`, `ConnectTimeout=30`.
  - Without a password: `BatchMode=yes` and `PreferredAuthentications=publickey`.
  - With one: `PreferredAuthentications=publickey,password`.
  - No `ControlMaster`; the C++ uses none either.
- **Trust store.** `known_hosts` is in `Layout.SSH`. The folder is made 0700
  and the file is created 0600 if missing.
- **Keys.** The host's own `identityFile` if it has one. Otherwise the shared
  key `ssh/forkmesh_shared_ed25519`, or the legacy `ssh/vultr_mirror_ed25519`
  when only that one exists. A key is passed with `-i` and
  `IdentitiesOnly=yes`.
- **Validation.** Hosts and users are checked as `isSafeSshEndpoint()` checks
  them, and the error strings are the C++ ones.
- **Timeouts and output.**
  - Probe: 35 s, then SIGTERM and a kill; the last 4 KiB of output is kept.
  - Mirror-node commands (`runMirrorNodeSsh`): killed at their deadline. That
    is 45 s for a service verb, 60 s for `du`, and the caller's `timeoutMs`
    for `core.hosts.exec`.
  - The reported output is the last 2 KiB, with ANSI codes stripped, bearer
    tokens and named credentials redacted (`redactProcessOutput`), and
    trimmed.

### Jobs, transcripts and statuses (`jobs.go`)

- **Jobs.** Every long-running host operation is a job: `install`,
  `uninstall`, `deploy`, `agentClis`, `actions`, `destroy`, `provision`,
  `deleteNode`. A job has an id, kind, host, origin (`desktop` or the phone's
  device label), start and end times, `ok`, a final `message` and a `result`
  object. While any job runs, `Busy()` is true, so the core neither lingers
  out nor upgrades under it.
- **Transcripts.** Output goes to named channels with absolute byte offsets
  (1 MiB kept per channel). `install` is the Hosts page's install log; each
  deploy-all session has `deploy:<job>:<node>`. A clear (the C++
  `m_hostInstallLog->clear()`) starts a new epoch.
- **Statuses.** The labels the C++ sets are statuses with a target: `install`
  (`m_hostInstallStatus`), `vultr` (`m_vultrStatus`) and `nodes`
  (`m_nodesStatus`).
- **Events.** `core.hosts.job` (every change), `core.hosts.output {channel,
  offset, epoch, text, clear}`, `core.hosts.status {target, text}`,
  `core.hosts.deploy` (sessions, headers, remaining, failed),
  `core.hosts.provision` (the provisioning snapshot), `core.hosts.saved`,
  `core.hosts.linkPrompt {code}`, `core.hosts.loginTerminal {node, ip, user}`,
  `core.hosts.probeRequested`, `core.hosts.notify {title, body, error}`,
  `core.hosts.flash {text, error}`, `core.hosts.nodeDeleted {node, ok,
  message, deletedNames, alreadyAbsent}`, `core.hosts.feedLines`,
  `core.hosts.feedEnded`.
- **Re-attach.** `core.hosts.jobs` returns the jobs, the channel ranges, the
  statuses, the deploy snapshot and the provisioning snapshot;
  `core.hosts.transcript {channel, from}` returns a channel's text from an
  offset. `core.hosts.cancel {id}` cancels a job.

### The installers (`install.go`, `installscripts.go`, `deploy.go`)

- **Command builders.** `buildHostInstallCommand` and
  `buildVultrMirrorNodeInstallCommand` are ports of the C++ functions of the
  same names: the plain installer, the verified published-binary install, the
  native mirror-node update for a signed release, the binary upload, the
  direct-HTTPS variant and the Go mirror package for a new Vultr server. The
  long shell scripts in `installscripts.go` were generated from the C++
  string literals byte for byte. `qtArg` substitutes `%1`…`%99` the way
  `QString::arg` does; `withAgentCliServiceRefresh` wraps every install.
- **Inputs from the desktop.** What only the build knows comes from
  `ui.hosts.environment` (the desktop pushes it as `core.hosts.environment`
  on connect and before each job; the core caches the last one): installer
  and uninstaller URLs, version and build commit, source dir, the app binary,
  kernel and CPU, catalog, accounts-link and node-delete URLs, the profile
  public key, link signer and link user, account owner and signing ability,
  the node-delete session token, admin flag, linked nodes, fleet addresses,
  the mirror root, the release-manifest override, the resolved mirror
  package files, the provisioning log path, whether this device has agent
  logins, the Hosts form's Vultr key and upload checkbox.
- **Install / update.** The `runHostInstall` flow: busy refusal, saved host
  `installing`, the transcript header lines, stdin (sudo password, Cloudflare
  token, upload marker and bytes), runner-token redaction, link-code
  detection (`FORKMESH LINK CODE`), the finish lines, `sshFailureSummary`,
  `sshConnectionFailureHint`, the saved status (`installed` or `install
  failed`).
- **Link codes.** The core signs `forkmesh-link-v1\n<signer>\n<code>\n<ts>`
  with the node key (`Core.RequireIdentity`, the same `ed25519.pem` the
  desktop's `ForkMeshIdentity` uses) and posts it to `accounts/link-node`. When
  the desktop cannot sign, `core.hosts.linkPrompt` asks it to show the
  link-code dialog.
- **Deploy-all.** Parallel sessions, one transcript each, with the C++
  headers, statuses and per-session saved statuses. For a signed release the
  core logs the rollout lines and asks the desktop to clear the rollout
  (`ui.hosts.clearReleaseRollout`).

### Vultr, DNS, destroy and node delete (`provision.go`, `provisionhelpers.go`, `vultrdestroy.go`, `nodedelete.go`)

- **Provisioning.** The six-stage machine of `MainWindowVultrMirrors.cpp`:
  credentials and the shared key (`ssh-keygen`), the Vultr SSH key, plan and
  Debian image, an existing instance or a new one, boot polling, SSH knocks,
  the install (the Go mirror package), optional agent CLIs, the Cloudflare A
  record, the seed push (`git` with the shared key) and the signed health
  proof. The checkpoint is the same `hosts/vultrProvision/v1` string the C++
  writes, through `WriteSettings`. At start the core restores it and, like
  `restoreVultrProvision()`, resumes an `active` deployment itself once a
  desktop is attached (the install needs the desktop's environment). A
  phone-started create has no form checkbox, so it reuses the last
  "install agent CLIs" choice. `ensureVultrMirrorDns()` is not ported: it has
  no caller in the C++ (the mirror sets up its own DNS with the Cloudflare
  token sent during the install).
- **Log file.** While provisioning, the core writes the install transcript to
  `data/deployments/vultr-mirror-latest.log` (ANSI stripped, last 1 MiB,
  atomic replace), which `fleet.provision` tails.
- **Credentials.** The Hosts form's key wins, else the stored Actions
  variable (read through the vault). See [Accepted keys](#accepted-keys-credstorego)
  for where a key Vultr accepts, and the Cloudflare token, are kept.
- **Destroy and node delete.** The C++ flows. The core unwires the dead
  node's git remotes itself ([Unwiring](#unwiring-a-dead-node)). The relay delete uses the desktop's session token when it has one,
  otherwise a signature by the node key over the C++ canonical string. On
  success the core forgets the saved host and writes both ledgers, then the
  desktop clears its roster and directory caches
  (`applyRelayConfirmedNodeDeletion`).

### Agent CLIs, Actions configuration and the SSH log feeds (`agentcli.go`, `actionscfg.go`, `logfeed.go`, `logsources.go`)

- **Agent CLIs.** `runAgentCliInstall` with the byte-exact bootstrap command.
  The core reads the logins to copy itself (`agentcreds.go`, a port of
  `localAgentCliCredentials()` and `buildAgentCliBootstrapPayload()`):
  - the active account's config folder per provider
    (`agents/activeAccount/<claude|codex>`, `agents/accountProfiles/<provider>/<id>/configDir`,
    else `CLAUDE_CONFIG_DIR`/`CODEX_HOME`, else `~/.claude`/`~/.codex`);
  - `.credentials.json` and `auth.json` from there (read with a 4 MiB cap);
    `agents/claudeApiKey`/`agents/codexApiKey` as `ANTHROPIC_API_KEY`/`OPENAI_API_KEY`
    only when that provider has no login file;
  - the same checks (a JSON object of at most 256 KiB), the same
    `<section> <base64>` payload and the same error and summary strings.

  The bytes go only to the child's stdin and are wiped after the write; no
  log line, transcript or IPC message carries them. The environment's
  `agentCredentials`/`agentAccess` fields are recomputed by the core, so a
  stale desktop answer cannot claim logins the core cannot read.
- **Actions configuration.** `runHostActionsConfiguration` with the C++
  request validation, stdin payload, 64 KiB cap, 45 s timeout and result
  parsing. Remote output is never rendered.
- **Host log feeds.** The SSH leg runs in the core: `core.hosts.feed.start
  {key, node, ip, user, password, identityFile, sshUntilMs}`, `feed.stop`,
  `feeds`. The core runs the feed script, parses lines with the
  `HostLogSources` rules, pushes them into phone streams itself (so phones
  keep getting lines while the desktop is away) and sends them to the desktop
  (`core.hosts.feedLines`) for the main log. It redials with the C++ backoff.
  A feed started because QUIC failed is recycled at `sshUntilMs` while a
  desktop is attached (`core.hosts.feedEnded {recycling: true}`), so the
  desktop can try QUIC again. Lines the core already pushed are skipped when
  the desktop's tap (`core.hosts.lines`) repeats them.
- **QUIC stays in Qt.** The QUIC leg uses `forkmesh::net::Http3Get` (OpenSSL
  QUIC and `libnghttp3`). The Go standard library has no HTTP/3 or QUIC
  client, and the core is standard-library only.

### Other work in the core

- **Probes.** The same remote probe script and the same labels as
  `probeSavedHost()` ("● Online · Mirror node", "◌ Provisioning · waiting
  for SSH", …), plus the column colours and the tooltip. Results are kept per
  credential key and sent as `core.hosts.probed`. With no desktop attached, a
  phone's `hosts.list` probes every saved host at most once every five
  minutes. When both sides have a label, the newer one wins.
- **Service verbs.** `restart`, `stop` and `start`, with the same commands as
  `MainWindowLocalControlHosts.cpp` (a restart skips a host that is still
  cloning), as entries in the operations table (`core.hosts.operations`,
  `core.hosts.operation`).
- **Phone log streams.** `hosts.logs.*` follows the C++ `LogStream`: a window
  of 4 unacknowledged batches, at most 2000 buffered lines (the oldest dropped
  and counted), at most 200 lines per batch, one batch per stream every
  400 ms and a flush right after an ack, 200 recent lines per host of which
  120 come back as the backlog.

### Mirror push (`mirrorpush.go`, `mirrorhealth.go`, `mirrorproc.go`, `mirrormethods.go`)

`MainWindowMirrorPush.cpp` runs in the core: the push to the ssh:// remotes,
the direct ref fan-out to managed hosts, the release gates and the artifact
CAS. The desktop keeps the triggers (commits, mirror syncs, the safety pass,
releases) and the release follow-up steps; each trigger calls the facade,
which hands the work to `core.mirror.*`.

- **SSH remotes (`pushRemotes`).** The same idle reasons, word for word, for
  the caller to show. `git remote -v` from the working copy (each `(push)`
  ssh:// URL once). Gateways that are cooling down are skipped unless a
  person asked. The slot is per repository and URL, and an overlapping
  request queues one replay of both halves. The command is `git -C <checkout>
  push --porcelain --no-force --(no-)prune <url> refs/heads/*:refs/heads/*
  refs/tags/*:refs/tags/* [+]refs/pr/*:refs/pr/*`, with `GIT_TERMINAL_PROMPT=0`
  and the C++ default `GIT_SSH_COMMAND` when none is set. Results are
  classified by `classifyMirrorPush` (held-back rewinds are not failures).
  `hint:` lines are dropped from the reason. The log lines and the
  flash messages of a user-initiated push are the C++ ones.
- **Gateway health (`MirrorGatewayHealth`, `NetworkBackoff`).** Only transport
  failures count (`mirrorGatewayUnreachable`). The backoff starts at 10 s and
  stops at 5 min; after a day without an answer a gateway is parked at 6 h,
  with one retirement notice and an hourly announcement until then. The
  streaks are stored in `mirrors/sshGatewayHealth` (same record format,
  `stringList`, through the settings queue, so a write waits for the
  desktop). The streaks are restored at the first use, never the backoff.
  The retry timer fires when the earliest skipped gateway is eligible again,
  250 ms late, between 1 s and 1 h, and only when that is sooner than the
  periodic pass (`repos/mirrorSyncIntervalMinutes`). `fleet.summary` reads
  `gateways` and the push counts from the core.
- **Managed hosts (`pushManaged`).** Only `forkmesh/forkmesh`, only saved
  hosts that may be active, only `root`, one per address. For each host the
  core builds the transport, the wake (`curl … /v1/control/sync?local=1`),
  the inventory and one seed command per artifact; one failure counts the
  host as unavailable with the C++ log line. `GIT_SSH_COMMAND` is the
  transport without its destination and command, plus
  `ServerAliveInterval=15`/`ServerAliveCountMax=2`, quoted like
  `hostSshCommandLine`. The push adds `--receive-pack=runuser -u forkmesh-node
  -- git-receive-pack` and stops at 75 s; the wake stops at 45 s and runs even
  after a partial push. An all-current safety pass wakes nobody. The ref gate
  (`MirrorRefPushGate`) lets one writer per repository and host run and
  replays the newest state once after the batch drains.
- **Release gates.** `releases/pendingMirrorPublications` is read from the
  settings the desktop writes (the token is the compact JSON
  `[owner, name, tag]`, lower-cased like `releaseRolloutToken`). An ordinary
  push is held while any publication of the repository is pending; a release
  push stops when a newer one superseded it, and waits 250 ms at a time while
  an ordinary ref push to the same hosts is in flight.
- **Artifact CAS.** A release carries its signed digests. Each must be a
  64-hex digest and a regular non-empty file (not a symlink) at
  `<mirror>/forkmesh-releases/sha256/<2>/<hash>/data`, else the refs are held
  back with the C++ message. The core seeds every artifact on every host over
  the SSH channel (stdin from the file, 5 min cap per object) and only then
  pushes the refs. One failed host fails the release, and the refs stay
  where they were. An ordinary push also reconciles the retained CAS (largest first): it
  asks each host which objects are missing (`for hash in …`, at most 512
  digests), streams only those, then wakes the daemon. This runs beside the
  ref push, never in front of it, one reconciliation per repository at a
  time. The seed and inventory commands are the C++ strings; a test reads
  them out of `ControlNode.cpp` and compares.
- **Callbacks.** `pushRefsToManagedHosts`' `onFinished(succeeded, failed)`
  becomes a token: the core sends `core.mirror.finished {token, succeeded,
  failed}` and the facade calls the stored callback. If the core goes away,
  waiting callbacks get `(0, 1)`, which leaves a release pending.
- **Repository table.** The core needs `owner`, `name`, `localPath`,
  `mirrorPath`, `previewOnly` and `private`. Every request carries its
  record. The facade also sends the whole list (`core.mirror.repos`) on
  connect and whenever it changes, and the core keeps it in
  `state/core/mirror-repos.json` (0600) for phone pushes and unwiring while
  the desktop is away.
- **Deliberate differences.** A managed push whose hosts are all taken by an
  in-flight writer calls its callback with `(0, 0)` (the C++ never called it,
  which left a phone's `fleet.action` waiting forever). Seed and inventory
  processes are not listed in the desktop's process activity view.

### Unwiring a dead node

`removeSshMirrorRemotesForNode` in Go (`unwire`): the node's address comes
from the saved host, then every repository folder (checkout and bare mirror)
loses the ssh:// remotes whose host is that address or belongs to the node
(`mirrorGatewayHostBelongsToNode`, whole dash/dot segments). Each removal is
a `git remote remove <name>` (config only; no history is touched) with the
C++ log line. The node's gateways are dropped from the health streaks, and
`core.mirror.unwired` tells the desktop to reload its remotes table. Destroy,
node delete and the desktop's own callers (`deleteMeshNodeCompletely`, the
Hosts page) all use it.

### Accepted keys (`credstore.go`)

A Vultr key that the API accepted, and the Cloudflare token a deployment
uses, are kept by the core:

- **Vault, not plaintext.** The values go into the vault (`actions`
  namespace) through the vault module's `Apply`. A Vultr key updates every
  alias that is present (`VULTR_API_KEY`, `VULTR_API_TOKEN`, `VULTR_TOKEN`,
  `VULTR_KEY`) and adds `VULTR_API_KEY` if missing, as
  `rememberVultrApiKey()` does. `actions/variables` in `ForkMesh.conf` gets
  the marker for those names (other entries are written back as they were).
  The value is never written there. Without a usable vault the core reports
  "The credential was not saved: the encrypted vault is unavailable (code)"
  and writes nothing in plaintext.
- **The deploy `.env`.** `app/.env.production`, found like
  `siteDeployEnvFilePath()` (the desktop also sends the path it resolved as
  `deployEnvPath`). It is rewritten like `writeEnvAssignments()`:
  - no symlinks, at most 512 KiB;
  - `updatedEnvAssignment()` line rules;
  - temp file and rename, mode 0600;
  - the same error strings.
- **Once per key.** A Vultr key is kept once per core run, unless a step
  failed and should be retried. The transcript says where it went ("Saved
  your Vultr API key to …"). The desktop reloads its variables page on
  `core.hosts.variablesChanged {names}` (names only).

### Desktop-only state: `ui.hosts.context`

Some of what `hosts.list` and `fleet.summary` show still lives in the
desktop's memory: the home roster, the probe labels, the mirror client and
push state, the gateway health records and the repository rows for
`fleet.repos`. The core asks for it with `ui.hosts.context` (3 s timeout) and
keeps the last answer for when the desktop is away. Busy, provisioning,
deploying, node delete, the running fleet actions, the last result and the
provisioning status now come from the core's own state (merged with the
desktop's for work it still started natively).

### Phone methods

All of `hosts.*` and `fleet.*` are native. The owner gate (`not_owner`) is the
shared `phonefamily.Gate`; neither family has a LAN-only method.

- **`hosts.prepare/run`.** All six actions. `install` accepts an empty name
  (`bad_host` for an invalid one), `destroy` needs the typed name
  (`typeToConfirm`, `name_mismatch`), `update` loads the saved host and runs
  the installer, `install` starts the provisioning machine with the phone's
  plan and region, `destroy` runs the Vultr destroy. Results arrive as
  `hosts.action`.
- **`fleet.prepare/run`.** All twelve actions with the `LocalControlFleet`
  subjects, summaries and typed confirmations (`fleetactions.go`), a separate
  token store, `already_running`, `busy`, `not_started`, `cannot_sign`,
  `nothing_to_resume`, `not_admin`, `hosts_page_unavailable` (`fleetTarget`,
  `reconcile`). Results arrive as `fleet.action`.
- **`mirrorPush`.** Runs entirely in the core, with or without a desktop: the
  subject is looked up in the repository table (`unknown_repo`), both halves
  start, `no_mirror_targets` or the idle reason when neither took it, and
  `fleet.action` with "Managed hosts: N updated, M failed." when the managed
  half ends. A repository the desktop never reported is `unknown_repo` until
  a desktop has attached once (the table is then kept on disk).
- **`hosts.options`.** Native for the stored key and the form key; a key
  Vultr accepts is kept by the core ([Accepted keys](#accepted-keys-credstorego)).
- **`fleet.repos`.** Native once the desktop has answered `ui.hosts.context`
  (repository rows are desktop state).
- **Logging.** The "requested by", refusal and result lines reach the system
  log through `ui.log` with the C++ wording.

### Desktop methods (`core.hosts.*`, refused for phones with `forbidden`)

- Probes and SSH: `state`, `probe`, `probeAll`, `exec`, `operations`, `lines`,
  `context`, `environment`.
- Jobs: `jobs`, `transcript`, `cancel`, `install`, `uninstall`, `deployAll`,
  `linkCode`, `agentClis`, `actionsConfig`, `destroy`, `deleteNode`,
  `provision.create`, `provision.resume`, `provision.end`, `provision.state`,
  `fleetFinished`.
- Feeds: `feed.start`, `feed.stop`, `feeds`.
- Mirror push (`core.mirror.*`, same refusal): `repos {repos}` → `{count}`;
  `push {repo, userInitiated, releaseTag, pruneRefs}` → `{targets,
  idleReason}`; `managed {repo, wakeIfUnchanged, releaseTag, artifactHashes,
  reconcileArtifacts, pruneRefs, token}` → `{targets}`; `release {repo,
  releaseTag, artifactHashes, token}` (`pushReleaseToManagedHosts`);
  `unwire {node, repos?}` → `{removed}`; `state` → `{pushing, pending,
  gateways, repos}`. A bad record is `bad_repo`. Events: `core.mirror.finished
  {token, succeeded, failed}`, `core.mirror.unwired {node, removed}`,
  `core.hosts.variablesChanged {names}`.

### Qt facade (`MainWindowCoreHosts.cpp`, `MainWindowCoreHostJobs.cpp`, `MainWindowCoreMirrorPush.cpp`)

- **When it is used.** `coreHostJobsAttached()` checks that the core is
  connected and has the `core.hosts.install` route. Each entry point decides
  when it is called; without the core everything runs natively, as before.
- **Entry points.** `runHostInstall`, `runHostUninstall`,
  `runHostDeployAllParallel`, `runAgentCliInstall`,
  `runHostActionsConfiguration`, `submitHostLinkCode`, `destroyVultrHostNamed`,
  `createVultrMirrorFromForm`, `resumeVultrProvision`, `endVultrProvision`
  (after its confirmation), `deleteMeshNodeCompletely` (after its typed
  confirmation) and the SSH leg of `launchHostLogFeed` each start with a
  `…ThroughCore` call. They read the form and dialogs as before, send the
  request, and keep the caller's callback until the job's `core.hosts.job`
  event ends it. Two cases stay native: a runner setup install (it needs the
  host-key dialog) and an install while a native provisioning run is active.
- **Rendering.** Output events append to `m_hostInstallLog` through
  `appendHostInstallLog` (the ANSI renderer) or to the deploy panes; a gap in
  the offsets is filled from `core.hosts.transcript`. Status events set the
  three labels. The provisioning snapshot is copied into the `m_vultr*`
  members and rendered with `renderVultrProvisionProgress`; the deploy
  snapshot builds the panes and mirrors `m_hostDeployRemaining`.
  `coreHostInstallBusy()` joins the busy checks of the network lamp and the
  release rollout.
- **Desktop answers.** `ui.hosts.environment`, `ui.hosts.clearReleaseRollout`,
  `ui.hosts.context`. (`ui.hosts.rememberVultrKey`,
  `ui.hosts.rememberCloudflareToken`, `ui.hosts.unwire`,
  `ui.hosts.agentCredentials` and `ui.hosts.mirrorPush` are gone: the core
  does that work itself.)
- **Mirror push.** `pushToSshMirrorRemotes(index, userInitiated, …)`,
  `pushRefsToManagedHosts(…)` and `removeSshMirrorRemotesForNode(node)` start
  with a `…ThroughCore` call when the core has the `core.mirror.push` route.
  These calls are blocking on the worker connection (`coregit::callCore`,
  5 s, 20 s for unwiring), because their callers use the returned count and
  idle reason right away. The core only plans and starts the work in that
  time; the git and SSH processes run in the core. If the request never
  reached the core, the native C++ path runs. If it reached the core but got
  no answer, the facade reports zero targets and does not push a second time.
- **Re-attach.** On connect the facade pushes the environment, lists
  `core.hosts.jobs`, restores the statuses, refills the install log from the
  core's transcript, rebuilds the deploy panes and applies the provisioning
  snapshot. At start-up, `resumeVultrProvision` waits up to 20 s for the core
  before falling back to the native resume, so a restarted desktop never runs
  a second provisioning beside the core's. On disconnect it releases waiting
  callbacks, clears the mirrored busy state and moves core SSH feeds back to
  native ones.

### Traps

- **Settings lag.** The core reads `hosts/list` from the file. The desktop's
  writes sync right away, so the core trails it only while a write is in
  flight; the core's own pending writes are overlaid on its reads.
- **Two writers of the provisioning log.** While provisioning, the desktop's
  `appendHostInstallLog` still schedules `saveVultrProvisionLog()` from the
  widget. Both write the same text with an atomic replace.
- **A QProcess that never starts.** The C++ install never calls its callback
  when SSH fails to start (`FailedToStart` emits no `finished`). The core
  finishes the job as failed instead.

### Tests

No test reaches a real host, Vultr or Cloudflare: fake `ssh`, `sshpass`,
`ssh-keygen` and `git` scripts go first on `PATH`, Vultr and Cloudflare are
loopback `httptest` servers, the health proof is signed with a test key and
a fake desktop records `WriteSettings` and answers `ui.hosts.*`.

- `hosts_test.go`: SSH argument parity, key resolution, probe labels, service
  verbs and tokens, owner gate, `forbidden`, phone log streams, `fleet.disk`,
  summaries, `hosts.options` (stored and form keys).
- `jobs_test.go`: command builders (plain, published, upload, native
  release, Vultr package), install streaming, link-code prompt, failure hint,
  busy refusal, uninstall, deploy-all, the settings queue while the desktop is
  away, ledgers, fleet-instance refusals, signed link codes, `forbidden`.
- `phonejobs_test.go`: fleet subjects and refusals, typed confirmations,
  add/forget, deploy-all, phone update through the installer, destroy typed
  name, link/resume refusals, mirror push and `fleet.action`.
- `provision_test.go`, `agentcli_test.go`, `actionscfg_test.go`,
  `logfeed_test.go`: the provisioning machine, destroy and node delete (with
  the core's own unwiring through a fake `git`); agent CLI install with login
  files the core reads; Actions configuration; SSH feeds.
- `mirrorpush_test.go` (fake `git` and `ssh`):
  - SSH remotes: arguments, environment, the cooldown and its stored
    streak, the "answering again" reset, held-back branches, prune, idle
    reasons, `bad_repo`, `forbidden`;
  - managed hosts: target filtering, `GIT_SSH_COMMAND`, receive-pack, wake,
    no wake for an unchanged safety pass;
  - releases: artifacts seeded before refs, superseded and invalid sets,
    missing CAS objects, seed failures;
  - CAS reconciliation and an invalid inventory; unwiring by address and by
    node name (not by a partial segment); the phone's native `mirrorPush`;
  - classifier, health, backoff, gate and URL helpers, and the seed and
    inventory commands compared with the literals in `ControlNode.cpp`.
- `credstore_test.go`: `.env` line rules and file safety, the worker bundle
  lookup, a Vultr key and Cloudflare token going to the vault (markers only
  in the settings) and the `.env`, no plaintext without a vault, the active
  agent account's login files, the API-key fallback and payload checks.

## Local mesh

`internal/mesh` runs the `#mesh` room in the core, so the core owns UDP and
TCP port 48732 and the room keeps running (with its queued messages) while the
desktop restarts. It is `desktop/src/LocalMeshNode.cpp` rule for rule:

- **Same behaviour.** Beacon contents and timing, the multicast, broadcast,
  unicast and paced subnet sweeps, reply beacons, the lower-id-dials rule, the
  duplicate-link rule, keepalive, 8 s dials, the 1.5 s redial, links blocked,
  history replay to joiners, relaying, the queue, wake tokens, typing, `bye`.
  The constants are the same, and so are the drop reasons and the diagnostics
  line and detail.
- **Wire.** `RoomCrypto` (PBKDF2-SHA256, 210 000 rounds, AES-256-GCM) and
  `RoomSeal` (X25519, HKDF-SHA256, per-frame content keys) are byte compatible
  with the desktop and the phones. JSON goes out in Qt's compact sorted-key
  form, and lengths count UTF-16 units like `QString`.
- **Keys and files.** The desktop passes its X25519 key
  (`chat/e2ee/x25519PrivateV2`) in `core.mesh.start`; without it the core
  reads `ForkMesh.conf`. History is the desktop's own file
  (`data/local_mesh/history-<sha16>.json`), and only one side writes it at a
  time. Seal keys learned in the core go to `data/local_mesh/seal-peers-<sha16>.json`,
  seeded once from the `chat/e2ee/peersV2/…` setting.
- **Phone control.** A `fm-control-hello` on an inbound link goes straight to
  `control.Adopt` (found with `Core.Module("control")`, an additive daemon
  hook), so the Qt descriptor hand-off is no longer used while the core runs
  the mesh. The beacon and the hello advertise `control` once the desktop
  has set a control handler.
- **Lifetime.** The mesh is not `Busy()`: it lives as long as the core, so it
  stays up through a restart within the linger window (2 minutes) and stops
  with a `bye` when the core exits.

### Methods and events (desktop only; phones get `forbidden`)

- Requests: `core.mesh.start` (returns `attached`, `state`, `history` and, on
  re-attach, `note`), `core.mesh.stop`, `core.mesh.state`, `core.mesh.history`,
  `core.mesh.diagnostics {bluetooth}` (`line`, `detail`), `core.mesh.connect`.
- Notifications, which stay in order: `core.mesh.identity`, `sendChat`
  `{id,text}` and `sendFile` `{id,fileName,mimeType,data}` (the desktop picks
  the id, so `sendChat()` still returns it at once), `typing`, `control`,
  `announce`, `link.open`, `link.data`, `link.closed`.
- Events: `core.mesh.message`, `queued`, `peers`, `blocked` (each with the full
  `state`), `typing`, `wake`, `system`, `inbound`, `link.write`, `link.close`,
  `link.dropped`.

### Qt facade

`MainWindow::startLocalMesh` calls `CoreMesh::attach()` before `start()`.
`LocalMeshNode` keeps its API and signals, and each public method asks the
backend first (`LocalMeshBackend` in `LocalMeshNode.h`, so the test targets
that compile `LocalMeshNode.cpp` need nothing new).

- **Choosing a side.** With the core connected and offering
  `core.mesh.start`, the room runs there. If the core is still connecting, the
  facade waits up to 4 s and queues sends, then runs the native room. A core
  that connects later takes over (the native room says `bye` first). When the
  core goes away, the facade waits 8 s for it, then runs natively, reloading
  history from the file.
- **Bluetooth stays in Qt.** RFCOMM and `BleMeshLink` sockets are still found
  and dialled by `LocalMeshNode`. In core mode each one becomes a byte pipe
  (`link.open/data/closed` up, `link.write/close` down), so framing, crypto and
  the room logic run in the core. The 10-minute `duplicate link` hold-off and
  the 5 s rescan after a settled drop follow `link.close` and
  `link.dropped`.
- **Synchronous getters.** `members()`, `peerCount()`, `isQueued()`,
  `linksBlocked()`, `wakeTargets()` and `listenPort()` read the last `state`.
  The debug bar's `diagnosticsLine()` asks `core.mesh.diagnostics` at most once
  a second, and only while the bar is visible.
- `MeshFirewall` stays in Qt; it reacts to `linksBlockedDetected` and
  `inboundLinkAccepted` as before.

### Tests

`go test ./internal/mesh/` never binds 48732 or a non-loopback address. The
interface list is a fake loopback one, and the "multicast" group is
127.0.0.1.

- **Vectors.** The phones' `RoomSeal` envelope (`RoomSealTests.swift`,
  `RoomSealCryptoTest.kt`) and its mutations. A `RoomCrypto` beacon envelope
  generated with Python's PBKDF2 and AES-GCM, matched byte for byte. The mesh
  room key, the peer cap and its eviction, and the settings key escaping.
- **Wire.** Address policy, sweeps (including a tether /28), framing, beacon
  bounds and wake tokens.
- **Rooms on loopback.** Three nodes link, chat, type, backfill a late joiner
  and relay. Also: the queue across a restart, wake targets, a returning
  device replacing its stale link, a crossing dial, discovery by beacon, the
  unicast reply and higher-id dial, links blocked, the control hand-off (with
  the bytes after the hello), and a Bluetooth bridge giving way to TCP.
- **Two cores.** Two `coretest` cores with mesh modules: chat, a file and
  typing across, re-attach with history, the control hello reaching the
  control module, the diagnostics, the bridge, and the seal-peer file. The
  key comes from the request on one core and from `ForkMesh.conf` on the
  other.

### Gaps

- Not yet run with the built desktop and a phone on a real LAN.
- This box has no Qt Bluetooth headers, so the `FORKMESH_HAS_BLUETOOTH` hunks
  in `LocalMeshNode.cpp` were not compiled.
- macOS and Windows tell Ethernet and Wi-Fi apart by a 6-byte hardware address
  and name prefixes (Qt reads the link type), so the sweep interfaces can
  differ there.
- If only the IPC connection breaks while the core keeps the room, the desktop
  starts a second room after 8 s under the same id, on a fallback port.

## Desktop pages

Issue 1336, round 2. What moved was picked from the desktop's own records,
`stalls.log` and `actions.jsonl` for 2026-10-01 to 2026-10-02 (1,222 stall
records, about 27,000 finished background tasks):

- **Agents page diff refresh.** "refresh diff and worktree status"
  (`startAgentDiffStatsRefresh`) ran 152 times for 7,014 s of worker time,
  46 s per pass, holding a `QThreadPool` slot all the while. Its git calls were
  the largest git cost in the app: the `git log --cherry-pick` for owned paths
  (3,707 calls, 3,716 s), `rev-list --left-right --count` (3,250 calls,
  2,258 s) and the `diff --numstat` reads (about 1,000 s). Every active or
  selected session was re-read on every pass, even when neither its branch nor
  its base had moved.
- **Index writes.** Merges, commits, `worktree` and `checkout` commands ran
  under `GitIndexQueue` in the desktop and under the core's lane in the core, so
  the two processes could collide on `index.lock`.
- **Not moved, and why.** GUI-thread stalls in those two days were layout,
  paint, rich text and `RoomSeal` sealing, not git, transcripts or vitals. The
  footer's once-a-second sampling (`/proc/self/stat`, `/proc/meminfo`,
  `statvfs`) appeared once (503 ms, `SystemStats::totalSwapBytes`, under
  memory pressure), and `diag.vitals` carries percentages only, while the
  footer tooltips need byte totals. Source-control status and branch lists
  already run on workers and cost seconds a day in total. Transcript search
  ran 4 times for 162 ms; it moved anyway because it was a one-call change.

### Index writes: `core.git.run`

- **Method.** `core.git.run {path, args, write, timeoutMs, input |
  inputBase64}` returns `{code, ok, timedOut, stdout | stdoutBase64, stderr,
  error, truncated, attempts, write, bypassed, queuedMs}`. `path` is any
  existing absolute directory, bare repositories included. `write` is ORed
  with the runner's own classification, so `write:true` queues a command the
  runner would not (for example `worktree add`). Without it the command runs
  outside the lane. Timeouts and the lock retry rules are the runner's: 8 s for
  reads, 60 s for writes, 120 s for merges, unless `timeoutMs` says otherwise.
  `stdout` is text when it is valid UTF-8, otherwise `stdoutBase64`.
- **Desktop side.** `routeGitIndexWrite()` in `GitIndexQueue` is the hook;
  `CoreGit.cpp` installs it while the core is connected.
  `runGitCaptureDirect` and PullStore's `runGit` offer every index-writing
  command to it first. The hook takes a command only on a worker thread that
  holds no lane, when the desktop's own lane for that repository is idle, and
  when the command is not `git pull` or a `filter-branch` rewrite. Otherwise
  the command takes the old path.
- **Transport.** A worker cannot use `CoreClient`, which lives on the GUI
  thread. `CoreGit` opens its own short connection: the socket (or TCP with
  the token), a `cli` hello, one request, then it waits with a deadline of the
  git timeout, plus the 300 s queue wait, plus 30 s. It needs neither the GUI
  thread nor an event loop. The hello's route table tells it whether the core
  has the method; a core without it is remembered by pid and skipped.
- **Failure rules.** If the request never left (no core, refused hello, an
  older core), or the core rejected it before running git (`bad_request`,
  `no_directory`), the desktop runs the command natively. Once the request is
  sent, a dropped connection or a missed deadline is reported as a failed
  attempt ("forkmesh-core did not finish git merge (core_gone)"). It is never
  re-run, because the core may already have run it.
- **Diagnostics.** Each routed command is a `git` background ticket with the
  caller's source and " [core queue]" after the command line, so the footer
  and `actions.jsonl` still show it.

### Agents page: `core.git.agentDiffStats`

- **Method.** `core.git.agentDiffStats {path, configuredBase, checkedOut,
  selected, token, sessions: [{id, status, branch, baseBranch, merged,
  finishedAtMs, prNumber, patchPath, known, signature}]}` is a port of the
  worker body of `startAgentDiffStatsRefresh`: `backgroundDefaultBranch`,
  `backgroundWorktrees`, `backgroundBranchTips`, the per-session signature and
  skip rule, and `readAgentDiffStat`. That covers the patch summary, worktree
  status, owned paths, the numstat with dirty and untracked files, ahead and
  behind, and the merge-tree conflict probe. It sends one
  `core.git.agentDiffStat {token, id, stat, signature}` event per session read,
  then replies `{base, stats, signatures, liveIds}`. Four sessions are read at
  once.
- **Cache.** The owned-path log with its numstat, the ahead/behind counts and
  the merge-tree result depend only on the two commits. When both branch tips
  are known, the core runs those commands on the tip hashes and caches the
  answers under (repository, base tip, branch tip), up to 8,192 entries.
  Worktree status and the working-tree diff run on every pass. An unchanged
  active agent therefore costs one `git status` and one `rev-parse`, not five
  history walks. A tip missing from the `for-each-ref` short names (an
  ambiguous name) skips the cache and uses the names, as before.
- **Desktop side.** Same worker, same result handlers. The worker calls the
  core through `CoreAgentPages::diffStats`, posts each event through
  `applyAgentDiffStatResult`, and fills the batch from the reply. If the call
  fails, it runs the old loop.

### Transcript search

`runAgentTranscriptSearch` flushes the transcript write lanes on its worker,
then asks `core.agents.search {needle, ids}` (eight files at once in the core).
It runs the old loop when the core is absent or the agent store is not the
core's (`FORKMESH_WINDOW_TEST_AGENT_ROOT`).

### Terminals

- **Backend.** `TerminalWidget` gets a backend through `TerminalBackend::hooks()`
  (`TerminalBackend.h` is header-only, so test targets that build
  `TerminalWidget.cpp` link as before). `CoreTerminal.cpp` installs the hooks
  when `CoreClient` is created. While the core is attached (not on Windows),
  `runCommand` starts `bash -lc '<command>; ec=$?; ...'` with
  `core.terminal.start`, exactly the native wrapper. The environment is the
  desktop's own, without `TERM`/`COLORTERM`, which the core sets. The core
  cannot unset a variable, so the `extraEnv` names without `=` (the provider
  credentials) become `unset NAME;` at the front of the command. A name that is
  not a shell identifier sends the terminal down the native path.
- **I/O.** Keystrokes go as `core.terminal.input` notifications (handled in
  order, 12 KB chunks). Resizes are coalesced into one `core.terminal.resize`
  per event-loop turn. Output arrives as `core.terminal.output` events; a
  `reset` frame resets the screen (`ESC c`) before the replay. `core.terminal.exit`
  becomes `finished(code)`.
- **Fallback.** If the start fails (no core, `cli_missing`, `terminal_failed`,
  `unsupported`), the widget clears itself and runs the same command with
  `forkpty`.
- **Surviving a restart.** The workspace terminal (`setPersistKey("workspace")`)
  saves its terminal id under `core/terminals/workspace` once it starts. When
  the window closes it leaves the shell running in the core instead of closing
  it. Showing the terminal pane calls `resumeOr()`, which sends
  `core.terminal.attach`, draws the replay (128 KB) and then the output that
  came while attaching. An unknown id clears the setting and starts a new
  shell. The "+" button still ends the shell and starts a new one.
- **Orphans.** Terminals are started with `mode` `desktop` or
  `desktop:<key>` and `device` `forkmesh-desktop:<pid>`. On every connect the
  desktop lists the core's terminals and closes the `desktop` ones it does not
  own, and the `desktop:<key>` ones whose id is not the saved one. A shell left
  by a crashed desktop therefore does not keep the core alive.
- **Linger.** A kept workspace shell keeps the core `Busy()`, so the core stays
  up until that shell exits, even when no desktop comes back.

### Tests

- `go test ./internal/repos/`:
  - `core.git.run`: validation, reads, writes, stdin, binary output, and the
    shared lane. A held lane blocks `write:true` and index writers but not
    reads, and the module reports `Busy()` while writes wait.
  - `core.git.agentDiffStats` against a real repository with a linked
    worktree: dirty, untracked, conflicting, missing, pull-metadata and
    unchanged sessions, the events, the signature format and `forbidden` for
    phones.
  - The cache, with a counting git wrapper: a second pass runs no `log`,
    `rev-list` or `merge-tree`, and a moved base reads again.
- Qt: `qt-check.sh` (syntax only) on every changed file. The checks ran without
  the build's stale precompiled header, because `GitIndexQueue.h` and
  `TerminalWidget.h` are in `MainWindowInternal.h`. The app was not built or
  run in this round.

## Desktop pages, part 2: diagnostics and git on the core

Issue 1336, round 3. Measured on the desktop's `stalls.log` and `stalls.log.1`
(2026-10-01 01:04 to 2026-10-03 15:16, 825 stall records):

- **Vitals on the GUI thread.** 4 stalls (503, 526, 537 and 600 ms) had
  `SystemStats::totalSwapBytes`, `SystemStats::diskTotalBytes` or
  `MainWindow::sampleHostVitals` frames: the once-a-second `/proc/meminfo`,
  `statvfs` and `/proc/self` reads, under memory pressure.
- **Source control.** 8 stalls named source-control or branch functions; none
  was a git process. They were GUI-thread file reads (`refTipFromFiles`),
  table layout and diff rendering. The git reads already ran on workers.
- **After.** Not measured: the app was not built or run in this round (shared
  build directory). With the core attached, the footer, the Stats page tiles
  and the descriptor check read a cached sample instead of `/proc` and
  `statvfs`, so those 4 stall shapes cannot recur on the GUI thread.

### Vitals: `core.vitals {detail:true}` and `core.vitals.sample`

- **Request.** On every connect `CoreVitals.cpp` sends `core.vitals {pid,
  watch:true, detail:true, disks:[QDir::homePath(), dataRoot]}`. The reply
  carries `detail`; afterwards the core sends `core.vitals.sample` to that
  connection once a second (the phone event `diag.vitals` is unchanged, four
  percentages).
- **Sample.** `memory`, `cpu`, `disk`, `swap` (percent, as `diag.vitals`),
  `memoryTotal`, `memoryAvailable`, `swapTotal`, `swapFree`, `diskTotal`,
  `diskFree` (data root), `cpuCount`, `load1`, `disks: [{path, total, free}]`
  and `app: {pid, cpuPercent, resident, swapped, threads, openFiles,
  openFileLimit}` for the desktop's pid, plus `sampledAtMs`.
- **Same numbers as the C++.** Memory and swap come from `/proc/meminfo`
  (`MemTotal` equals `_SC_PHYS_PAGES` times the page size), disks from
  `statvfs` (`f_bavail`, `f_frsize`), resident memory from `VmRSS` (equal to
  `statm` resident pages), threads from `/proc/<pid>/task`, the soft limit from
  `/proc/<pid>/limits` (unlimited is 0, as `openFileSoftLimit`). The desktop's
  CPU is the footer's formula: utime plus stime over wall time, no upper
  clamp, unknown on the first sample. Open files: the core counts
  `/proc/<pid>/fd` from outside, while the desktop's own count includes the
  descriptor of the directory it is reading, so the facade adds 1.
- **Facade.** `forkmesh::corevitals` mirrors the `SystemStats` calls. Each
  returns the core's value while a sample is under 3 s old and the value is
  known, and the native call otherwise (core absent, older core, macOS process
  stats, a disk path the core does not sample). Callers: the footer
  (`updateFooterDiagnostics`, CPU, memory, swap and home-disk tooltips),
  `checkFileDescriptorPressure`, the high-memory auto-kill check, and the
  Stats page (`sampleStatsHistory`, the cpu, memory, host-memory, swap,
  open-files and threads tiles). The disk tile reads the repository's disk,
  which the core samples since part 3.
- **Not moved.** `MainWindowMemory.cpp` measures `residentBytes()` immediately
  before and after a heap trim, which a 1 s sample cannot do. The footer still
  ticks once a second for its activity counters; it paints from the cached
  sample.

### Stall records: `core.diag.stall`, `core.diag.stalls`

The watchdog stays in Qt (it samples the GUI thread). `onUiStall` notifies
`core.diag.stall {entry}` with the same entry it appends to `m_stallLog`;
`clearStallLog` sends `core.diag.stall {clear:true}`; on every connect the
desktop replaces the core's list with `core.diag.stalls {entries}`. The core
keeps the last 100, as the C++ does, so indexes match `m_stallLog` and the
`diag.prepare/run` resolve the same record. The list outlives a
desktop quit until the next desktop syncs its own (empty) list.

### Phone methods now native

- **`diag.stalls`, `diag.stall`.** From the core's list: newest first,
  `stallRow`/`stallDetail` ported (stamp, 64-unit `when`, 400-unit summary,
  line and byte counts, 2,000-unit line cap, 64,000-unit detail cap,
  `unknown_stall`). `canSend` needs a non-empty list and the desktop's stall
  repository (`stallReportRepoIndex() >= 0`, from `ui.diag.state`); without a
  desktop it is false.
- **`diag.health`.** Pressure, memory, swap, disk (data root), host CPU and
  load from the core's 1 s sampler; the desktop's resident memory, open files,
  threads and the "this process" part of `pressureLine` from `/proc/<pid>` of
  the UI connection's pid; `stalls` from the core's list; `agents` and
  `desktop` from `ui.diag.state` (one O(sessions) loop on the GUI thread, no
  I/O).
- **`diag.processes`.** The core runs `ps -eo pid=,ppid=,user=,rss=,%mem=,args=
  --sort=-rss` (4 s, `ps_unavailable`, `ps_failed`, `timeout`), parses it as
  `parseProcessTable` (30 rows, whole-table parent map) and redacts as
  `commandSummary`. Agent roots (`pid`, `session`, `label`, `stoppable`) come
  from `ui.diag.state {roots:true}` (`agentSessionProcessId`,
  `isStoppableAgentSession`), so Codex and `AgentRunner` children are
  attributed too.
- **Without a desktop.** `diag.health` counts the agent store's sessions
  (`internal/agents`, the desktop's last save) and names the desktop from
  `control.info`; `diag.processes` takes roots from
  `agentproc.Processes()` (a new additive export) for keys
  `agent-<session>-<ms>`, labelled from the store. The C++ answered nothing at
  all in that case.
- **Still proxied: `diag.prepare`, `diag.run`.** `stopAgent` goes through
  `stopAgentSessionById`, which also moves the session to Stopped, writes the
  transcript and pumps the queue; a core-side `core.agentproc.stop` would kill
  the process without any of that, so it is not exact. `sendStall` and
  `sendStalls` start an agent (`startAdHocAgentForRepo`). Both actions are the
  agent runtime's, and `prepare` stays with `run` so their tokens are on one
  side. `diag.backups` is the backup module's.

### Git on the core queue

- **Environment.** `core.git.run` takes `env` (`NAME=VALUE` strings, at most
  1,024 and 1 MB; a bad entry is `bad_request`), applied over the core's own
  environment and its fixed `LC_ALL=C`, `GIT_OPTIONAL_LOCKS=0`,
  `GIT_TERMINAL_PROMPT=0`, `GIT_PAGER=cat`. The desktop sends its whole
  environment except `FORKMESH_DESKTOP_INSTANCE`, with `LC_ALL=` and
  `LANGUAGE=` when it has none, so localized output (`%ar`, pull messages)
  matches a native run. `SSH_AUTH_SOCK`, `GIT_SSH_COMMAND`, askpass and
  credential helpers are the current desktop's, not the ones the core started
  with.
- **`git pull` and the other detached commands.** `runGitDetached`
  (`startDetachedGit`) is the Commits page pull, the source-control status
  reads, the change badge and other detached reads. Its worker now calls
  `coregit::runDetached` first. Index writers (`pull` included) go to
  `core.git.run {write:true}` and wait on the core's per-repository lane, which
  they skipped entirely before (no desktop lane either). Reads go to
  `core.git.run` outside the lane. Output is stdout, plus stderr when the
  request merged channels. A read falls back to the native `QProcess` when
  the core is absent, rejects it, drops it or truncates it (32 MB); a write
  that was sent is reported as failed and never re-run. The ticket says
  `[core]` or `[core queue]`. `core/gitReads=false` keeps reads native.
- **GUI-thread writes.** `routeGitIndexWrite` no longer refuses the GUI thread.
  `PullStore::runGit` on the GUI thread (merge, update from base, merge
  queue, foreign-pull adoption) sends `core.git.run` from a `std::async`
  thread and pumps events in 40 ms slices (12 ms of events each), the same
  keep-alive as `waitForFinishedKeepAlive`. Commands issued while the GUI
  thread holds a desktop lane, and `filter-branch` rewrites, still run
  natively.
- **Not routed in part 2.** The phone `files.*` pull and the `runOffThread`
  reads that call `runGitCapture` directly moved in part 3 (see "Desktop
  pages, part 3"). The Rebuild & restart pulls run in `internal/update`.
- **Why not `core.git.status/branches/log`.** Those return summaries (counts,
  a fixed branch format, a fixed log format). The pages parse porcelain v1
  per-file records, custom `for-each-ref` formats and `log --format` with
  `%ar`, so they read the same commands' output through `core.git.run`.

### Tests

- `go test ./internal/diag/`: stall row and detail parity (stamp, redaction,
  truncation), the core-owned list (push, sync, clear, 100 cap, `forbidden`
  for phones, `unknown_stall`, `canSend` with and without a desktop),
  `diag.health` keys and values from the desktop and from the fallback,
  `diag.processes` tree attribution from desktop roots and from agentproc
  keys, the `ps` errors, `parseProcessTable`/`commandSummary`, detail vitals
  (reply, `core.vitals.sample` events, disks, phone events unchanged) and the
  CPU meter.
- `go test ./internal/repos/`: `env` validation and a per-call environment
  (`GIT_CONFIG_*`) on reads and writes.
- Qt: `qt-check.sh` on `CoreVitals.cpp`, `CoreDiag.cpp`,
  `MainWindowCoreDiag.cpp`, `MainWindowDiagnostics.cpp`, `MainWindowStats.cpp`,
  `MainWindowCore.cpp`, `CoreGit.cpp`, `GitIndexQueue.cpp`,
  `MainWindowBranches.cpp`, `PullStore.cpp`, `MainWindowInternal4.cpp`. Not
  built or run.

## Desktop pages, part 3: worker git reads, the phone pull and the disk tile

Issue 1336, round 4.

### Worker git reads: `runGitCaptureDirect` → `core.git.run`

- **What moved.** Every git read that reaches `runGitCaptureDirect` on a
  worker thread: the `runOffThread` bodies (branch panel, commit history,
  outgoing count, catalog and mirror passes) and the GUI-thread
  `runGitCapture` calls, which `runGitCaptureBackgrounded` already hands to a
  worker. Index writers keep their route (`routeGitIndexWrite`).
- **How.** `coregit::runRead` sends `core.git.run {write:false, env,
  timeoutMs, inputBase64?}` with the desktop's whole environment (as the
  detached reads do), so localized output and credentials match a native
  run. The timeout is the native one: `gitTimeoutMsForArgs(args,
  gitWaitTimeoutMs(args))`. The core runs the read outside the repository
  lane, with its own lock-retry policy.
- **Same answer.** Success gives stdout (text or `stdoutBase64`); a failure
  gives `error` = stderr, as `runGitIndexSerialized` did, and the stdout
  becomes `failureOutput`; a timeout reads "git timed out after Ns: …".
- **Falls back to the native `QProcess`** when the core is absent, lacks the
  route, refuses the request, drops it or truncates the output (32 MB), on
  the GUI thread, and with `core/gitReads=false`. A read is safe to run
  twice, so a dropped connection is retried natively.
- **Diagnostics.** Each routed read is a `git` ticket ending in " [core]",
  with the caller's source and routine.
- **Cost.** One short local connection (socket, hello, request) per read,
  about a millisecond; the fork happens in the small Go process instead of
  the multi-gigabyte desktop.

### The phone `files.*` pull

- **In the core (`internal/files`).** `files.browse.run` for `pull` (an
  index writer) now holds the core's shared repository lane
  (`git.SharedRunnerKey`, the runner `core.git.run` uses, published by the
  repos module) before it starts git, so a phone pull never races a merge or
  commit the desktop routed to the core. `fetch` does not take the lane. The
  reply, events, output (merged channels, last 1,500 characters), the 120 s
  limit and the texts are unchanged; the limit starts once the lane is held.
  `Stop` also ends a pull still waiting for the lane.
- **In the desktop.** The C++ handler still answers when the core delegates
  (an owner it cannot vouch for). With the core attached, its pull and fetch
  go to `core.git.run` from a pool thread (`coregit::runCommand`, with
  `GIT_TERMINAL_PROMPT=0`, `GIT_OPTIONAL_LOCKS=0`, `LC_ALL=C`), so they wait
  on the same lane. The message is stdout then stderr (the core returns them
  separately, so the interleaving of `QProcess::MergedChannels` is lost). A
  request that never reached the core runs the old `QProcess`.

### The Stats page disk tile

`corevitals::diskTotalBytes/diskFreeBytes` now add a path the sample lacks
(the tile asks for the repository's git folder) to the desktop's
`core.vitals {watch, detail, disks}` request, keeping the last six such paths
besides home and the data root (the core samples at most 8). The first call
answers natively; from the next sample on the tile reads the core's numbers.
The core now reads each watcher's options under its lock, since a second
`core.vitals` from the same desktop replaces them.

### Tests

- `internal/files/lane_test.go`: a pull waits for a held lane (and is busy
  meanwhile) while a fetch does not; `Stop` ends a pull waiting for the lane.
- `internal/diag`: a second `core.vitals` adds the repository disk to the
  reply and to the following samples, with one watcher (`-race`).
- Qt: `qt-check.sh` on `CoreGit.cpp`, `MainWindowInternal4.cpp`,
  `MainWindowLocalControlFiles.cpp`, `CoreVitals.cpp`. Not built or run.

## Rebuild & restart, backups and cleanup

Rebuild & restart, hourly backups and the Cleanup page's removals run in the
core when the desktop is attached. A desktop quit, crash or restart in the
middle no longer stops them, and the phone's `update.*` and `diag.backups`
are answered by the core.

### Rebuild & restart (`internal/update`)

- **Who starts it.** The desktop decides *when* for the Restart buttons,
  the queue behind running agents, the profile panel's Update and the phone;
  the core decides for the hourly auto-update and runs Quick update (see
  "Auto-update and Quick update" below).
  `MainWindow::buildAndRelaunch` and `updateRebuildRestart` hand the job to
  `core.update.start` instead of running `QProcess` steps. The run as another
  user, the install-mirror lookup, the prebuilt auto-update and the clean
  rebuild go to the core too, but only when the core lists them in the
  `capabilities` of its `core.update.config` reply (`installSource`,
  `asUser`, `prebuilt`, `clean`). An older core gets the native path, so it
  never runs a sudo job as the wrong user.
- **What the core runs.** Everything `buildAndRelaunch` did, with the same
  status lines, percentages and phases:
  1. for `kind: "update"`: `git remote set-url` and `git pull --ff-only`, or
     `git clone --depth 1` into a missing or empty folder;
  2. the free-space check;
  3. clearing the scratch folder through `safedelete`;
  4. configure, unless the cache already matches the source, build type and
     generator (stale generator entries are dropped through `safedelete`);
  5. up to three `cmake --build` passes while the sources keep changing
     (`sourceChangedSince`, same skip rules as `SourceTreeWatch.h`);
  6. the Go mirror-node companion;
  7. the `--version` start check, the install with `.bak-update`, and the
     companion copy.
- **Process groups.** Every step runs in its own session. `core.update.cancel`
  sends SIGTERM to the group, then SIGKILL 1.5 s later.
- **A failed pull stops the run.** It reports "Nothing in <src> was changed or
  removed". The core never deletes or re-clones a checkout (`AGENTS.md`). The
  test `TestFailedPullStopsWithoutTouchingTheCheckout` checks this with a fake
  `git` that returns 503.
- **Relaunch.** It follows `processreaper::startReleased`: the core starts the
  new binary with `setsid` and without `FORKMESH_DESKTOP_INSTANCE`.
  - **Desktop attached.** The core asks `ui.update.relaunch {mode: "release"}`
    (the desktop releases its single-instance lock), starts the binary, then
    sends `ui.update.quit`. That is the same order as the C++: release,
    start, quit. If the desktop cannot release the lock, the core asks it to
    quit, waits up to 30 s for it to exit, then starts the binary.
  - **Under a systemd user service** (the unit and path the desktop reported
    match): the core asks `ui.update.relaunch {mode: "supervised"}`, and the
    desktop sets the restart flag and quits. With no desktop attached, the
    core runs `systemctl --user restart --no-block <unit>`.
  - **No desktop attached** (it quit mid-build): the core starts the desktop
    itself.
- **After the relaunch.** The run counts as running until a desktop with a
  new `startedAtMs`/pid sends `core.update.config`, or for at most 20
  minutes. The phone's "Restarted on <version>" check (same process vs. new
  process) works unchanged, because `update.status` reports the desktop's
  `version` and `startedAtMs`, not the core's.
- **Linger.** `Busy()` is true while a job runs or a relaunch is pending.
- **Logs.** Restart-log lines (`logRestart`) reach the desktop as
  `core.update.log` events, and the desktop writes them to its settings log.
  They also go to `forkmesh-core.log`.
- **Install-mirror lookup** (`source.go`). `updateRebuildRestart` sends
  `kind: "update"` with `catalogUrl` (the desktop's `catalogApiUrl()`, which
  is settings logic) and no `installUrl`. The core then emits the "Finding an
  online ForkMesh mirror..." status (with "(installing for <user>)" on a sudo
  run). It sends `GET <catalogUrl>/api/install-source` with a 30 s timeout
  (the C++ blocked on a nested `QEventLoop` with none) and turns `{node, repo}`
  into `<catalogUrl>/forkmesh/<repo>`, exactly like `resolveInstallCloneUrl`:
  `repo` defaults to `forkmesh` when it is not a string, the status code is
  ignored, and query and fragment are dropped. When nothing is found, the run
  stops with "No online ForkMesh mirror is available right now. Try again
  shortly." before any `git` call. `core.update.finished` carries the short
  `flash` text (this one, or a stopped pull's reason), and the facade shows
  it with `flashMessage`, as the C++ did.
- **Run as another user** (`asuser.go`, `asUser` in the request). When the
  desktop runs as root under `sudo`, every step the C++ ran through
  `runUpdateStepUser` runs as `sudo -u <user> -H [env K=V ...] <program>
  <args>` with the same argv and log lines: `git remote set-url`, `pull
  --ff-only`, `mkdir -p` of the clone parent, `clone`, `mkdir -p` of the
  scratch folder, configure, `cmake --build` (`env TMPDIR=...`) and the
  companion `go build`. After that come the `sudo -u <user> -H <binary>
  --version` start check, "Installing for <user>...", the same `sh -c`
  install script (copy to `.bak-update`, `cp -f`, `chmod 0755`, the
  companion), and the relaunch: release, then `sudo -u <user> -H <app>
  <args>`, then quit. There is no systemd check, as in the C++. One
  deliberate difference: the scratch folder and stale generator entries are
  cleared in-process through `safedelete` with `buildDir` as the expected
  parent, not with `sudo -u <user> rm -rf`. The core runs as root here, and a
  raw `rm -rf` is banned. The desktop keeps `m_updateAsUser` set while the
  core runs the job, so `recordRestartDuration` still files the time as a
  handoff to another user.
- **Prebuilt auto-update** (`prebuilt.go`, `core.update.prebuilt`).
  `tryPrebuiltAutoUpdate` hands over `tag`, `tagCommit`, `os`, `arch`
  (`QSysInfo::currentCpuArchitecture()`), `catalogUrl`, `tempDir`, `appPath`
  (`runningClientExecutable()`) and `mirrors` (`{identity, casRoot}` for each
  repository with a mirror). Repository records are desktop state, so the
  desktop computes the identity with `repoSegment` and the CAS root with
  `mirrorReleaseCasRoot`. The reply decides synchronously:
  - `{handled: false}`: the desktop runs `updateRebuildRestart`.
  - `{handled: true, started: false}`: waiting for the release's signed
    artifacts.
  - `{handled: true, started: true, run}`: a job runs.

  The checks and the `logSystem` lines (through `Core.LogUI`) match the C++:
  - `git show <rev>:.forkmesh/releases/latest/release.json` at the tag commit,
    then at `@{upstream}`;
  - the `tag` must match, and `tag_commit` must equal the tag commit or be
    its ancestor (`merge-base --is-ancestor`);
  - "waiting" is logged once per tag;
  - asset selection by `os`/`arch` with a 64-hex `blob_sha256`, the
    `forkmesh-mirror-node*` companion, and `repo` as `owner/name`;
  - the node's own CAS blob, matched case-insensitively by identity and
    verified by SHA-256;
  - otherwise a download of `/api/repo/<owner>/<name>/releases/blob/sha256/<hash>`
    to `<tempDir>/forkmesh-update-<hash12>.part`, hashed while it streams and
    renamed only when the SHA-256 matches;
  - the companion is refreshed from the CAS or a `.part` download, and its
    `.part` is removed afterwards;
  - staging is `forkmesh-update-staged` (`chmod 0755`), then "prebuilt update
    to <tag> staged", then the normal install (`--version` check,
    `.bak-update`) and relaunch.

  A failed download or checksum mismatch removes only the `.part` file, logs
  "falling back to a source build", and leaves the install untouched.
  `core.update.finished` then carries `fallback: "source"`, and the facade
  calls `updateRebuildRestart`. With no desktop attached, the core runs that
  source update itself. The facade starts the caution flash when the job
  reaches "installing", as `installPrebuiltAndRelaunch` did. There is one
  visible difference: the core counts the download as a running update, so
  phones see `running: true` during it, where the C++ showed nothing until
  the install.
- **Clean rebuild** (`clean: true`). `rebuildAndRelaunch` checks
  `CMakeLists.txt` and hands over. The core runs "clearing the build cache",
  then "Clearing build cache...", then `safedelete.Refusal(clientDir,
  clientDir/build)`. A refusal stops the run with "Could not clear the build
  cache: <reason>" and nothing removed. Otherwise
  `safedelete.RemoveTreeUnder(clientDir, clientDir/build)` (a symlink inside
  is unlinked, not followed), then the Release rebuild.

### Auto-update and Quick update (`autoupdate.go`, `quick.go`)

- **Auto-update decides in the core.** `internal/update` owns the timer: the
  first check 5 to 15 minutes after the core starts, then every hour ±10
  minutes (one interval drawn at start, as the C++ `QTimer`). Unlike the
  desktop's timer it does not restart with every desktop restart. A check
  runs the same gates in the same order as `maybeAutoUpdate`:
  1. the desktop delegated (`core.update.config` carries `autoUpdateDir`,
     the desktop's `runningClientDir()`) and is attached;
  2. `update/autoUpdate` (default off) and `update/autoUpdateRestart`
     (default on) from `ForkMesh.conf`;
  3. no check in flight, no rebuild running (a core job, or the desktop's
     reported run), and `ui.update.agents` reports neither `anyRunning`
     (`anyAgentRunning()`) nor `rebuildBusy` (the Rebuild button disabled);
  4. `CMakeLists.txt` in the folder;
  5. `git rev-parse --abbrev-ref --symbolic-full-name @{u}` (3 s), `git fetch
     --quiet --tags` (45 s, killed after), the newest tag by
     `for-each-ref --sort=-creatordate`, `rev-list -n1 <tag>` and
     `merge-base --is-ancestor <commit> HEAD` (3 s each; a timed-out ancestry
     check counts as "not contained", as in the C++);
  6. the rebuild and agent gates again, then `ui.update.auto {clientDir, tag,
     commit}`.
- **The desktop starts it.** `ui.update.auto` re-checks the Rebuild button
  and `anyAgentRunning()` (replying `started:false` if either changed), logs
  "Auto-update: release <tag> is available; updating in the background.",
  then runs `tryPrebuiltAutoUpdate` (which hands over to
  `core.update.prebuilt`) or `updateRebuildRestart` (`core.update.start`).
- **No desktop, no auto-update.** As before: the C++ timer only ran while the
  desktop ran, and the core needs the desktop's repository records.
- **Switch-over.** The core lists `autoUpdate` in its capabilities; while it
  is attached with that capability, `maybeAutoUpdate` returns at once. An
  older core (no capability) leaves the desktop's timer in charge, and an
  older desktop (no `autoUpdateDir`) never starts the core's.
- **Quick update** is `core.update.start {kind:"quick", label:"update",
  repoUrl, buildType:"Release", appPath: runningClientExecutable()}` when the
  core lists `quick`. The core runs `runQuickUpdate`'s steps: with
  `CMakeLists.txt` present, "Pulling the latest version..." and `git pull
  --ff-only` with `quickUpdatePullArguments`' fallbacks (`origin <branch>`,
  then `origin HEAD`); otherwise `mkdir -p` of the parent and "Downloading the
  latest version..." with `git clone --depth 1 <repoUrl> <src>`. A failed
  step stops with "Update failed: <summary>" before any build (nothing is
  deleted or re-cloned), then the normal build, install and relaunch follow.
  The desktop keeps the caution flash, the restart log, the profile save and
  the button state; a refused job runs the old steps.

### The `update.` phone family

- **Native.** `update.status`, `update.unwatch`, `update.prepare` and
  `update.run` are native with exact JSON parity: the run state, 300-line
  backlog, `update.progress` batching (150 lines, 7000 characters, `skipped`),
  confirmation tokens and the owner gate.
- **Delegated until configured.** They return `rpc.ErrDelegate` until the
  desktop has sent `core.update.config`, so an older desktop keeps answering
  through its C++ handler.
- **Running agents.** `update.prepare` asks `ui.update.agents` for the
  running-agent count (0 when no desktop is attached).
- **Starting a run.** `update.run` asks `ui.update.start`, so the desktop
  shows the spinner and runs its own pre-checks. With no desktop attached, the
  core runs the rebuild from the saved config.
- **Runs the desktop drives itself.** For the native fallback (no core, or a
  core without the capability), the desktop forwards its log, status and
  spinner state as notifications:
  `core.update.reset`, `core.update.append`, `core.update.note` and
  `core.update.desktop` (the `activity` countdown text comes from the
  desktop's restart timing history). The phone therefore sees every run.

### Backups (`internal/backup`)

- **Snapshots.** The hourly timer, the 15 s first snapshot when one is due,
  `tar` to `.part` then rename, pruning to the kept count, and the same status
  texts and `noteBackupResult` codes. The desktop pushes its settings with
  `core.backups.config` (`arm` restarts the timer, as `startAutoBackups` did),
  and stops its own timer while attached. `core.backups.now` and `export`
  carry the config, because IPC requests run concurrently.
- **Restore.** `core.backups.unpack` unpacks to the desktop's
  `tmp-forkmesh-import-<stamp>` folder and reports `core.backups.unpacked`.
  The desktop then runs `finishConfigRestore` (renames and relaunch). A failed
  unpack, or one that finishes with no desktop attached, removes the work
  folder through `safedelete`.
- **Export and cancel.** `core.backups.export` writes the configuration
  archive. `core.backups.cancel` stops a running snapshot before a restore or
  data wipe, and the desktop disarms the core's timer at the same time.
- **`diag.backups`.** Native once configured (it delegates before that). The
  answer is the same JSON as `diag::backupStatus`.
- **The `backups` package.** It keeps using `backupfeed::Host`. Its callbacks
  run through the same `MainWindow` methods, which are now facades.

### Cleanup (`internal/cleanup`)

- **`core.cleanup.removeWorktree`.** A port of the Cleanup host's worktree
  removal:
  - the linked-worktree check;
  - "has uncommitted changes; kept";
  - the `stash create` and trash-ref backups (`refs/forkmesh/trash/cleanup/<date>/...`);
  - `branchRefusal`;
  - removal, and the branch delete by `update-ref -d` against the saved tip.

  A dirty worktree is force-removed only through
  `safedelete.ForceRemoveWorktree`, which refuses `HOME`, the repository
  itself, checkouts with a `.git` folder and protected ForkMesh folders. The
  git commands go through `internal/git` (the per-repository index queue).
- **`core.cleanup.removeSandbox`.** Refuses a sandbox the desktop's
  `ActionRunner` claims (its claimed names come with the request). Everything
  else goes to `actions.RemoveUnclaimedSandbox`, which checks the core's own
  Action claims and lock files and removes through `safedelete`.
- **Threading.** The Cleanup package calls the sandbox remover on a worker
  thread. The facade sends the request on the GUI thread and waits on a
  semaphore, so the GUI never blocks.

### Cleanup scanner (`core.cleanup.scan`)

The Disk Cleanup page's scan and its file deletions run in the core when the
desktop is attached. A scan walks tens of gigabytes and a delete can take
minutes; in the core neither competes with the GUI thread, and a desktop quit
or Rebuild & restart in the middle no longer cuts a delete off half-way.

- **What was ported.** `scan.go` is `CleanupScanner.cpp` rule for rule: the
  five categories, the four statuses, item ids (`<category>:<title>:<path>`),
  titles, every reason text, the `/proc` snapshot (`ReadProcesses`, with a
  `ProcRoot` for tests), `Measure` (disk blocks, hard links once, links never
  followed; `measure_windows.go` uses sizes), live Claude sessions, the git
  reads with `GIT_OPTIONAL_LOCKS=0` (through `internal/git` with `NoLane`, so
  the scan never waits on the index queue) and `ccache -k cache_dir`.
  `plan.go` is `CleanupPlan.cpp`: `PlanRefusal`, `BuildPlan`, `Describe`,
  `MemberRefusal`, `WorktreeRefusal`, `DeleteFiles`, `DeleteSandboxes`,
  `FreedBytes`, `LogLine`, `AppendLog`.
- **Methods** (desktop only; phones and relay peers get `forbidden`):

  | Method | Params | Reply, then event |
  | --- | --- | --- |
  | `core.cleanup.scan` | `token`, `context` | `{id, attached}`; `core.cleanup.progress {id, tokens, stage}` per stage, then `core.cleanup.scanned {id, tokens, items, notes, cancelled, scannedAtMs}` |
  | `core.cleanup.cancel` | | `{cancelled, id}`; the scan ends with `cancelled: true` |
  | `core.cleanup.state` | | `{scanning, scanId, deleting, deleteId, lastScan, lastDelete}` |
  | `core.cleanup.deleteFiles` | `token`, `files`, `sandboxes`, `worktrees`, `skipped`, `claimed`, `context`, `logPath` | `{id}`; `core.cleanup.deleted {id, tokens, outcomes, worktreeRefusals, logged, freed, finishedAtMs}` |
  | `core.cleanup.appendLog` | `outcomes`, `logPath`, `nowMs` | `{logged}` |

- **The context.** The desktop sends what only it knows, exactly as
  `CleanupFeature::desktopContext()` builds it: home, temp roots, uid, the
  project, data, builds, worktrees and Actions sandbox folders, the `claude`
  binary, repositories, agent sessions, task work items and claimed
  sandboxes. The core reads `/proc` and the clock itself.
- **Wire format.** An item is the C++ `Item` with the same field names;
  `category`, `kind` and `status` are the C++ enum values (Claude 0 to
  OtherCaches 4; Files, Worktree, ActionSandbox, Report; InUse, Attached,
  Orphaned, Protected). A step is `{item, choice}`, an outcome
  `{path, bytes, reason, removed, error}`.
- **Deleting.** The core applies `PlanRefusal` again to every step it is
  given, then `DeleteFiles` removes each member with
  `safedelete.RemoveTreeUnder(<item folder>, <member>)` after the same
  delete-time checks as the C++, and `DeleteSandboxes` goes through
  `RemoveUnclaimedSandbox` (the desktop's claims from the request, then the
  core's own Actions claims and locks). One check is stricter than the C++:
  no member inside the core's own `data`, `config`, `ssh`, `state` or `logs`
  folder is removed, even when `safedelete` would allow a nested path there.
  Worktrees are only re-checked: `worktreeRefusals` carries the delete-time
  refusal per item id, and the page then removes each worktree through the
  host's `removeWorktree` (`core.cleanup.removeWorktree`), because the
  desktop must first release the sessions that track the path.
- **The log.** At the end of a delete the core appends the plan's skipped
  items and its file and sandbox outcomes to `<logs>/cleanup.log` (the
  desktop's `logPath()`). The worktree outcomes come later through
  `core.cleanup.appendLog`. The format is unchanged; the two batches can
  carry different times, where the C++ wrote one time for the whole run.
- **Restarts.** `Busy()` is true while a scan or a delete runs, so the core
  lingers and defers an upgrade. A scan requested while one runs attaches to
  it (both tokens are in its events), and a scan requested during a delete
  starts when the delete ends. `core.cleanup.state` keeps the last scan and
  delete results.
- **Qt facade (`CoreCleanup.{h,cpp}`).** `forkmesh::corecleanup::install`
  fills four optional `cleanupfeed::Host` hooks (`coreScan`,
  `coreCancelScan`, `coreDeleteFiles`, `coreAppendLog`) from
  `installCleanupHost`. Each returns false unless the core is connected and
  lists the route, and `CleanupFeature` then keeps its `QtConcurrent` path
  (`FORKMESH_CORE=0`, an older core). Events are matched by a token the
  facade makes per request; a disconnect settles the request with an error.
  A scan that fails that way runs natively; a delete reports "the background
  engine could not finish it" for its items and the page scans again.
- **Small differences from the C++.** "N kept: why" reasons are listed in
  first-seen order (the C++ iterated a `QHash`); `SizeText` uses the C locale
  (the page still formats sizes itself); `\d` in the temp-file patterns is
  ASCII.
- **Tests.** `scan_test.go` ports every case of `test_cleanup_scanner.cpp`
  (helpers, a fake `/proc`, Claude, Codex, worktrees with real `git`, builds
  and caches, deletion and the log) and adds protected items, refusals that
  come from `safedelete`, the core's own data folder, link checks, and a
  source check that the package has no delete call of its own. `jobs_test.go`
  covers progress events, attach, cancel, busy, a delete with its log,
  worktree re-checks, a scan waiting for a delete, and `forbidden` for phones.

### Guarded deletes (`internal/safedelete`)

- **Origin.** A port of `server/safedelete.go`. `parity_test.go` runs the
  server's whole refusal table against it.
- **Worktree guard.** `worktree.go` adds the linked-worktree guard for
  `git worktree remove --force`.
- **Destructive-delete test.** Both files are helpers in
  `app/tests/test_no_unguarded_destructive_deletes.py`, which now also scans
  `desktop_go_new` and checks the core's disposable-folder list against the
  other three helpers.

### Qt facade (`MainWindowCoreOps.cpp`, `CoreOps.h`)

- **Per-call switch.** Each hook checks `coreOpsAttached()` (connected, and
  the core lists the route), so an older core or `FORKMESH_CORE=0` keeps the
  native path. A start the core refuses (other than `already_running`) runs
  natively.
- **Following the run.** Events drive the existing UI:
  - `core.update.output` goes to `appendUpdateLog` and
    `noteRestartStepOutput`;
  - `core.update.state` goes to `setRestartPhase`, `setRestartSpinProgress`
    and the status label;
  - `core.update.finished` stops the spinner on a failure.
- **Re-attach.** On connect, the desktop sends `core.update.config`, asks
  `core.update.state` and re-attaches to a running job: spinner, log backlog
  and status.
- **Hooks in existing files.** One line each:
  - `buildAndRelaunch`, `updateRebuildRestart`, `restartInProgress`,
    `cancelRestart`, `appendUpdateLog`, `setUpdateStatus` and
    `showUpdateLog` (`MainWindowSetup.cpp`);
  - `updateRestartCountdown` and `stopRestartSpin`
    (`MainWindowRepoDetail.cpp`);
  - `startAutoBackups`, `takeBackupNow`, `pruneOldBackups`,
    `restoreConfigArchive`, `exportConfigData`,
    `stopLiveServicesForDataOp` and `host.running` (`MainWindowData.cpp`);
  - the two Cleanup host callbacks (`MainWindowCleanup.cpp`).

### Still in the desktop

- **Phone `diag.*` families.** `diag.health`, `diag.processes`,
  `diag.stalls` and `diag.stall` are native now (see "Desktop pages, part 2");
  `diag.prepare/run` take their tokens in the core (`internal/agentmodel`); the
  stall-fix agents still start on the desktop.
- **Rebuild steps.** Only the native fallbacks, used when the core is absent
  or does not list the capability: `resolveInstallCloneUrl` (still a nested
  event loop there), `runUpdateStepUser`, `tryPrebuiltAutoUpdate`, the clean
  wipe in `rebuildAndRelaunch`, `maybeAutoUpdate` and the `runQuickUpdate`
  steps.
- **Auto-update parameters.** The repository records (mirror identities and
  CAS roots for the prebuilt path) and `catalogApiUrl()` are desktop state,
  so `ui.update.auto` starts the update through the desktop's own (core-backed)
  `tryPrebuiltAutoUpdate`/`updateRebuildRestart`.
- **Backup table.** Listing snapshots for the table (a directory read) and
  deleting a single snapshot.
- **Cleanup scanner.** Only the native fallback (the package's own
  `QtConcurrent` scan and deletes), used when the core is absent or does not
  list `core.cleanup.scan`. Worktree removals keep going through the Cleanup
  host's `removeWorktree` callback, as before.

### Tests

These need no network or real build. They use temp sandboxes with fake
`cmake`, `git`, `ninja` and `tar`, and real `git` only in temp repositories:

- `internal/update`: feed and batching, a full rebuild with install and
  relaunch, a build failure, a failed pull, a second build pass, cancel, low
  disk, systemd relaunch, release-then-quit order, and the phone family with
  owner gate, tokens, progress and delegation; `leftovers_test.go` covers the
  install-source lookup against a loopback server (parse rules, timeout,
  closed port, no mirror), sudo runs against a fake `sudo` that records argv
  (every step, start-check failure, a failed pull that neither deletes nor
  clones), the prebuilt path with a real temporary git repository and a
  loopback blob server (download, CAS install, checksum mismatch, fallback
  with and without a desktop, `@{upstream}`, waiting, no artifact), and the
  clean wipe (symlink not followed, a checkout or `HOME` refused);
  `autoupdate_test.go` runs the auto-update check against real temporary
  repositories (an upstream with a new tag, busy agents, a busy rebuild, the
  desktop refusing at the last moment, the setting off, no upstream, no
  checkout, no desktop) and Quick update with the fake tools (pull then
  build, a failed pull that stops before the build, a clone when there is no
  checkout);
- `internal/backup`: names, rotation, `tar` arguments, status, a real snapshot
  through `diag.backups`, a failing `tar`, cancel, unpack clean-up;
- `internal/cleanup`: clean and dirty worktrees, trash refs, protected
  branches, sandbox rules;
- `internal/safedelete`: the server parity table and the worktree guard.

## Agent processes

The desktop used to run Claude Code, Codex app-server and the legacy
`AgentRunner` agents as `QProcess` children of the UI. Their pipes were read
on the UI thread and every desktop restart killed them; the startup pass then
marked each Running session Queued ("Resuming after ForkMesh restart") and
relaunched it with `--resume`. `internal/agentproc` moves the processes into
the core, and the Claude transcript sessions now survive a desktop restart.

### Shape: one shim per agent

The core does not hold the agent's pipes itself. `core.agentproc.start` runs
`forkmesh-core agentproc-shim <dir>` (the core's own binary), and the shim owns
the agent:

- **Process tree.** The shim is a session leader (`setsid`); the agent is its
  child in its own process group. When the desktop passes a usable
  `systemd-run`, the shim runs inside a transient scope, so the shim and the
  agent leave both the desktop's and the core's cgroup and survive
  `systemctl --user restart` of a supervising desktop service.
- **Spool.** The shim writes stdout and stderr to
  `state/core/agentproc/<key>/out.<offset>` and `err.<offset>`: 4 MB
  segments named by their starting byte offset. It notifies the connected core
  of the new lengths over `<dir>/s` (on Windows, loopback TCP; see
  [Shim transport on Windows](#shim-transport-on-windows)), a socket with 5-byte binary frames
  (`frame.go`). Socket paths over 100 bytes move to `$TMPDIR/fmap-<uid>/`.
- **Commands.** The socket carries stdin writes, close stdin, a signal to the
  pid or the group, kill (SIGKILL the group) and stop (close stdin, SIGTERM,
  SIGKILL of the group after the grace).
- **Records.** `run.json` holds the shim pid, agent pid, start time and boot
  id. `exit.json` holds code, signal, crashed and the final lengths; the shim
  writes it before it reports the exit. The core writes `spec.json` (program,
  arguments, directory, unit; never the environment, which reaches the shim
  over its stdin), `meta.json` (opaque, from the desktop) and `ack.json`.
- **Core restarts.** Because the agent belongs to the shim, an upgrade's
  `core.shutdown`, a core crash or the linger exit leaves agents running. A new
  core scans `state/core/agentproc` at start: it connects to every live shim,
  marks a dead shim without `exit.json` as `lost`, and drops exited entries
  after 7 days. Only `core.shutdown {"hard":true}` (a new, additive parameter;
  the daemon records it as `core.shutdown.hard`) stops the agents on the way
  out.

### Methods (desktop only; phones and relay peers get `forbidden`)

| Method | Params → result |
| --- | --- |
| `core.agentproc.start` | `key`, `program`, `args`, `env` (the full list), `cwd`, `label`, `detachable`, `merge` (merged channels), `scope`, `meta`, `attach` (default true: the caller is attached from byte 0 atomically), `window`, `skipStdout`/`skipStderr` (honoured only when the core parses that stream), `secrets` (redaction values for the parser, held in memory, never written) → `{key, pid, shimPid, unit, contained, parsed, parsedStderr}` |
| `core.agentproc.write` | `key`, `data` (base64) |
| `core.agentproc.closeStdin`, `.kill` | `key` |
| `core.agentproc.signal` | `key`, `signal` (name or number, default TERM), `group` |
| `core.agentproc.stop` | `key`, `graceMs` (default 1500), `forget` (forget once it exits) |
| `core.agentproc.list` | → `processes`: key, pids, running, exit, stdout/stderr lengths, ack, meta, unit, detachable |
| `core.agentproc.attach` | `key`, `stdoutOffset`, `stderrOffset` (-1 = the persisted ack), `window` → the list entry; replays from the offsets, then streams |
| `core.agentproc.detach` | `key` (empty: all) |
| `core.agentproc.ack` | `key`, `stdoutOffset`, `stderrOffset`: persisted to `ack.json`; segments wholly below it are deleted |
| `core.agentproc.flow` | `key`, offsets the desktop has read; with `window` set, the core never has more than `window` bytes unread in flight (the facade uses 4 MB) |
| `core.agentproc.setMeta` | `key`, `meta` (merged; null removes) |
| `core.agentproc.forget` | `key`; `running` while it runs; removes only the files the module creates, then the empty folder |

Events go only to attached peers. `core.agentproc.stdout` and `.stderr` carry
`{key, data, offset, end}`, where `offset` is where `data` starts and `gap`
counts bytes the spool no longer had. `core.agentproc.exit` carries
`{key, code, crashed, signal, lost, stdout, stderr}`, sent after the last
byte. A signal death reports the signal number as `code` with `crashed`, the
way `QProcess` does.

- **Ownership.** `detachable:false` processes belong to the connection that
  started them. When it closes, the core stops them (SIGTERM, 1.5 s, SIGKILL),
  which matches a `QProcess` dying with the desktop. A process a restarted
  core adopts goes to the first desktop that attaches, and is stopped after
  60 s if none does. Detachable processes have no owner.
- **Busy.** `Busy()` is true while any process runs, so the core lingers.
- **Spool bound.** Segments are trimmed only once acknowledged, so everything
  not yet acked is kept. A runaway agent past 512 MB per stream loses its
  oldest segments, and replay reports them as `gap`.
- **Containment.** The rules follow `agentresources::contain()`. When the C++
  would contain (memory governor available), `scope` carries `systemd-run`,
  the slice and the agent `MemoryHigh`/`MemoryMax`/`OOMPolicy`, and the core
  builds the same `--user --scope --quiet --collect --slice= --unit=
  --description= --property=…` line. Otherwise, on Linux with a user manager,
  an escape scope without limits is used. The unit is
  `forkmesh-agent-core-<label>-<core pid>-<serial>.scope`:
  - the desktop never registers it, so `processreaper` does not stop it on a
    quit;
  - it keeps the `forkmesh-agent-` prefix that `AgentResourceGovernor::freezable`
    refuses to freeze.

  If `systemd-run` fails, the start is retried without the scope.
  `FORKMESH_DESKTOP_INSTANCE` is stripped from the agent's environment
  (`processreaper::releasedEnvironment` semantics), and the shim itself starts
  with the core's released environment.

### Qt facade (`AgentProcess.{h,cpp}`, `CoreProcess.{h,cpp}`)

- **`AgentProcess`.** The QProcess-shaped interface the three session classes
  now hold: `start`, `startContained(program, args, label)`, `write`,
  `closeWriteChannel`, `readAll*`, `state`, `processId`, `terminate`/`kill`,
  `waitFor*` and the five `QProcess` signals. `LocalAgentProcess` wraps a real
  `QProcess`; its `startContained` is the old `contain()` + `start`.
  `AgentProcess::create()` asks a factory, and with none installed it returns
  the local one (tests, `FORKMESH_CORE=0`).
- **`CoreProcess`.** `MainWindowCore.cpp` calls `installCoreAgentProcesses()`,
  which installs the factory. A process is core-backed when, at creation time,
  the core is connected, `core/agentProcesses` (default true) is on and the
  core has `core.agentproc.start`.
  - Writes made before the start reply, or while the core is away, queue and
    flush in order. Every operation is a notification, because notifications
    are the only ordered path.
  - If the core is unavailable at start, it starts a local `QProcess`.
  - It re-attaches by itself from its current offsets when the core comes back
    after a restart.
  - `waitForFinished(ms)` cannot block. It hands the stop to the core
    (`stop` with that grace, `forget`) and returns true.
  - A detachable process deleted after `aboutToQuit` is left running; any
    other deleted live process is stopped by the core.
- **Session classes.** `ClaudeStreamSession`, `CodexAppServerSession` and
  `AgentRunner` changed only their process type, connect targets and the
  contain/start call.
  - The dialog-owned sessions (Calendar, Market) and the `AgentRunner`
    worktree phase are non-detachable: their I/O leaves the UI thread, and they
    still die with the desktop as before. Codex turns (round 3) and the
    `AgentRunner` agent phase (round 4) are detachable.
  - The Agents-page Claude runs (`launchCliRuntime`) pass
    `options.processKey = agent-<sid>` and `processMeta` (account, worktree,
    repository). They become detachable processes keyed
    `agent-<sid>-<launch ms>`.

### Re-attach after a desktop restart

- **Offsets.** In core mode `ClaudeStreamSession` frames stdout one line at a
  time, so each event knows the byte offset where its line ends. The event is
  emitted with `_so` (that offset) and `_sk` (the process key), and the
  transcript store persists them with the event. After the desktop's
  transcript-write barrier (`whenTranscriptWritesDrained`, the same one
  `finished()` waits on) the session acks the delivered offset, throttled to
  one ack in flight every 250 ms.
- **Meta.** Parser state is kept in the process meta: permission mode, launch
  mode and bypass flag, resume id, model, effort, cwd and `conversationOpened`.
  `setPermissionMode` and the control response update it.
- **Startup.** `initAgents` calls `adoptCoreAgentAtStartup(session)` for every
  session. It reads `state/core/agentproc` synchronously (the core is not
  connected yet); a shim counts as live by pid and boot id.
  - A session with a live or recently exited Claude process that is not
    merged, Queued, Stopped or Cleared keeps its status instead of going to
    Queued. Its in-flight steer message counts as delivered, because the CLI
    already read it from stdin.
  - Other processes found there are stopped once the core connects.
  - With the core or `core/agentProcesses` off, the shims get SIGTERM and the
    old resume path runs.
- **On connect.** `reattachCoreAgentSessions()` lists the core's processes,
  loads the session's stored events (`ensureStreamEventsLoadedAsync`, which now
  calls `coreAgentEventsLoaded`) and wires a new `ClaudeStreamSession` exactly
  as `startCliTranscript` does. It then calls `attachProcess(key, meta, offset,
  stderrAck, pid)`, where offset = max(ack, the largest `_so` stored under that
  key), so the stream resumes on the first byte the transcript does not have.
- **Output that went on while the desktop was down.** It replays in order,
  including a `result` and the exit, which then run the normal
  `finishCliTranscriptRun`. A Running session whose last stored event is a
  `result` re-arms the background-subprocess completion check.
- **Fallback.** If the core does not hand a session back within 30 s, the
  session is resumed the old way: its shim gets SIGTERM, then the session is
  Queued and the queue pumps quietly.

### Restart scenarios

Codex turns joined the first column in round 3 and the `AgentRunner` agent
phase in round 4; see
[The remaining runs](#the-remaining-runs-agentrunner-and-dialog-sessions).

| Scenario | Claude runs, Codex turns, `AgentRunner` agent phase | Idle Codex, `AgentRunner` worktree phase, dialog sessions |
| --- | --- | --- |
| Rebuild & restart, Update, quit and relaunch | keep running; re-attached | stopped with the desktop's connection; resumed as before (a dialog is gone with its window) |
| Desktop crash or SIGKILL | keep running; re-attached | stopped by the core when the connection drops; resumed |
| Core upgrade (`core.shutdown`), core crash, linger exit | keep running (shims); the next core adopts them and the open desktop re-attaches | keep running; re-claimed by the desktop within 60 s |
| `systemctl --user restart` of a desktop service | keep running only when a scope was used (systemd user manager present) | resumed |
| `core.shutdown {"hard":true}`, logout, reboot | stopped; resumed as before | stopped; resumed |
| Core off, `core/agentProcesses=false`, core never answers (30 s) | shim stopped; resumed as before | local `QProcess`, as before |

### Gaps

Round 3 closed the Codex and per-run resource gaps; see
[Agent engine](#agent-engine) for what changed and what is still open.

- **No resume fallback in attached mode.** A re-attached process that exits
  before its conversation opens cannot relaunch with the saved prompt; the
  session ends through the normal exit path.
- **In-memory run state is rebuilt only in part.** Not restored: the last
  assistant text and the Listen and announcer state. The background-work check
  itself now runs in the core and is adopted by the restarted desktop.
- **Not exercised.** KVM-isolated launches (they go through the shim
  unchanged) and Windows (it builds; the shim uses loopback TCP there, tested
  on Linux only).
- **Phones.** See [Agent session model](#agent-session-model) for the agent
  phone methods the core answers.

### Tests

`internal/agentproc` uses the test binary as both the shim and a fake agent,
and covers:

- start, write, stdout, stderr, merged channels and exit;
- environment stripping and the working directory;
- replay from offsets across detach and attach;
- ack persistence and segment trimming, with a gap reported below a trim;
- the spool cap;
- flow-window pacing;
- signal, kill, and stop with grace and forget;
- owner-bound processes stopping with their connection;
- an orphan claimed or stopped after a core restart;
- a soft core stop leaving the agent running and a second core adopting it
  and resuming at the ack;
- an exit that happened with no core, replayed by the next one;
- a SIGKILLed shim reported as `lost`;
- start failures and the scope fallback;
- phone refusal and hard stop;
- the exact `systemd-run` argument list.

`internal/daemon` tests the hard-shutdown flag. A manual run of the real
binary confirmed that the shim and the agent sit in
`forkmesh.slice/forkmesh-agent-core-*.scope` without the instance marker,
survive `core.shutdown`, and are adopted by the next core.

## Agent engine

Issue 1336, round 3. Round 2 moved the agent processes into the core, but the
desktop still parsed every stdout line on its UI thread, wrote `meta.json`,
`transcript.txt` and `events.jsonl` itself, and held Codex's JSON-RPC session in
memory. Now the core is the only writer of `data/agents/**`, parses Claude and
Codex output itself, and hands the desktop finished events.

### The agent store writer (`internal/agents`, `core.agentstore.*`)

- **One writer.** `agents.Writer` is a port of `AgentStore.cpp`, and the golden
  test checks it byte for byte. It has:
  - two ordered lanes (events, transcript) like `AgentTranscriptWriter`;
  - tombstones for deleted or recycled session folders;
  - `meta.json` replaced through a temporary file (mode 0600, as
    `QTemporaryFile` creates it) with an asynchronous `fsync`;
  - the written-bytes cache that skips identical saves;
  - `nextId` over the live tree and the bin;
  - recycle with `deleted-at.json`, restore (a binned Running, Queued or
    Waiting record comes back Stopped), purge and the expiry sweep, all
    through `safedelete.RemoveTreeUnder`;
  - `model-outcomes.json`.
- **Exact JSON.** `internal/qtjson` parses and prints the way `QJsonDocument`
  does. Things it reproduces:
  - integers that fit `qint64`, and doubles that are exactly integral, are
    stored as integers (Qt's `convertDoubleTo`);
  - other doubles print as integers when integral and at most 2^53, otherwise
    in the shorter of decimal and `e±XX` form (ties go to decimal);
  - keys are sorted by code point and the last duplicate wins;
  - `/` is not escaped, control characters are `\u00XX`, and a lone surrogate
    from a `\u` escape survives as `\udXXX` (and becomes `?` in transcript
    text, as `QString::toUtf8` does);
  - invalid UTF-8 rejects the document, unknown escapes are taken literally;
  - the `Indented` layout, including empty `{\n    }`.

  Desktop-sent records keep their number tokens (`ParseKeepNumbers`), so
  re-indenting the compact bytes the desktop sends yields exactly the
  `meta.json` the C++ would write.

| Method (desktop and CLI only; phones get `forbidden`) | Params → result |
| --- | --- |
| `core.agentstore.root` | → `{root}` |
| `core.agentstore.create` | `session` (the record) → `{session, id, createdAtMs}` |
| `core.agentstore.save` | `session` → `{saved}` (false for a tombstoned folder or a newer schema) |
| `core.agentstore.delete`, `.recycle` | `session` → `{ok}` |
| `core.agentstore.restore` | `id` → `{ok, path}` |
| `core.agentstore.purge` | `id` → `{ok}` |
| `core.agentstore.purgeExpired` | `retentionMs`, `nowMs` → `{purged}` |
| `core.agentstore.appendLog`, `.appendEvent`, `.clearEvents` | `path` (`sessions/<key>/<id>/<file>`) or `key`+`id`; `text` / `event`. Sent as notifications, so they are applied in order |
| `core.agentstore.writePatch` | `path` or `key`+`id`, `patch` → `{ok}` |
| `core.agentstore.sync` | waits until both lanes have written everything queued before it |
| `core.agentstore.outcomes` | → `{outcomes}` |

The read handlers (`core.agents.transcript`, `.events`, `.search`) flush the
lanes first.

**Qt facade.** `AgentStore` keeps its API.
- **Attaching.** On every core connect, `CoreAgentStore` probes the core
  (`core.agentstore.root`) and routes a store only when its canonical root is
  the core's.
- **Appends.** Each `AgentTranscriptWriter` lane thread keeps its own
  blocking Unix-socket connection (`forkmesh::coreagentstore::Lane`) and sends
  appends as notifications, so the order holds per lane. `flush()`,
  `flushPath()` and the drain barrier send `core.agentstore.sync` on that lane
  first, so a read after a write sees it.
- **Records.** Saves, creates, deletes, recycle, restore and purge are
  synchronous requests on a shared, mutex-guarded channel. A save keeps the
  desktop-side tombstone and dedupe checks.
- **Fallback.** A transport failure falls back to the native write for that
  operation; `FORKMESH_CORE=0` and a different root stay native.
- **Transport.** The lane and record channels are blocking sockets: the Unix
  socket when the endpoint has one, otherwise the core's loopback TCP port
  with the endpoint token in the hello (Windows always, and any platform
  where the Unix socket cannot be reached). TCP channels set `TCP_NODELAY`
  and are not inherited by child processes.
- **Two writers.** While attached, the desktop never writes those files
  itself. The core writes only for desktops that route to it and for the runs
  it parses (below).

### Runs parsed in the core (`internal/agentrun`, `core.agentrun.*`)

- **Who parses.** `agentproc` accepts in-process consumers
  (`SetConsumerFactory`). A process whose meta has `parse: claude` or
  `parse: codex` is consumed by `agentrun` from its spool.
- **The desktop's attachment.** It skips stdout (`skipStdout`, honoured only
  when a consumer exists). The start and attach replies say `parsed`.
- **Acks.** The core acknowledges the spool once its lanes have written the
  events (250 ms throttle).
- **After a core restart.** The consumer resumes at
  `max(ack, the last stored _so for that key)`, so no event is stored twice.
- **Credentials.** The desktop pushes its provider credential values
  (`core.agentrun.credentials`, a notification sent before every start). They
  are held in memory only. Until a desktop has pushed them, the core does not
  parse (or store) anything, so nothing is written unredacted.

**Claude.** The stream-json handling of `ClaudeStreamSession` and
`applyTranscriptEvent`, in the same order:
1. the `AgentJsonlFramer` rules (8 MB frames, CR stripped, whitespace lines
   skipped, `_runtime_protocol_error` events);
2. `_so` (as a double) and `_sk`;
3. `redactProviderCredentials` (values, bearer headers, assignments, `sk-`
   tokens, sensitive keys);
4. `_forkmesh_account` when the event names a session or thread;
5. `_ts` on assistant and Codex text events;
6. the event line, then `AgentLogText` transcript text.

The run also tracks the conversation-opened flag and permission-mode
responses into the process meta, plus the last Claude session id (resume id),
the idle and waiting state, and usage (turns, duration, cost, output tokens),
all listed by `core.agentrun.list`.

**Codex.** `CodexAppServerSession`'s protocol now lives in the core (the
`codex` runtime):
- what it does: `initialize`, rate limits, paged `model/list`,
  `thread/start` and `thread/resume` with the fresh-thread fallback,
  `turn/start`, `turn/steer`, the queue that holds a message while the mode
  changes, every notification and item mapping, the synthetic plan tools,
  server requests (approvals, user input, elicitation, permissions,
  unsupported), `permission.requested` for command approvals;
- each emitted event gets `_so` = line start + 1 + its index in the line,
  which keeps replays exact;
- the desktop drives it with `core.agentrun.codex {key, op: send | respond |
  options | mode | interrupt}`;
- `permission.requested` is not stored (as before) and is re-sent to a
  desktop that attaches while it is still pending;
- the protocol state is snapshotted into the process meta (`codexState`), so
  even a core restart continues the session;
- the model list reaches the desktop as `{models}` (`modelsListed`).

| Method | Params → result |
| --- | --- |
| `core.agentrun.credentials` | `values` (notification or request) |
| `core.agentrun.attach` | `key`, `after` → run state. Replays every stored event of that key with `_so > after` from `events.jsonl`, then streams |
| `core.agentrun.detach` | `key` |
| `core.agentrun.list` | → `{runs, credentials}` |
| `core.agentrun.codex` | `key`, `op`, `text` / `token`+`answer` / `model`+`mode`+`effort` → run state with `ok` |
| `core.agentrun.salt` | → `{salt}` (hex; the attachment-id salt) |

Events: `core.agentrun.event` carries `{key, so, line}`, the normalised event
as compact JSON text that the desktop parses on its worker pool. A raw-only
line carries `raw`, Codex protocol lines carry `raw`, and some events carry
`stderr`, `models` or `transient`. `core.agentrun.exit` comes after the last
event has been written.

**Qt facade.**
- **Claude.** `ClaudeStreamSession` requests `parse` for Agents-page runs
  when `forkmesh::coreagentrun::available()`. Stored events are emitted inside
  a `StoredDelivery` scope, and `applyTranscriptEvent` then skips its own
  redaction and store writes (it still notifies the follow observer). It
  finishes on `core.agentrun.exit`; a native exit that arrives before the
  attach reply is held. It does not ack. When the core refuses, stdout flows
  to the desktop as in round 2. After a core reconnect it re-attaches from the
  last `so`.
- **Codex.** `CodexAppServerSession` does the same for Codex. Its processes
  are now detachable while a turn runs (idle after `result`). `send`,
  `respondToRequest`, `setTurnOptions`, `interrupt` and the mode command
  become core ops. A process the core does not parse is bound to the desktop
  (idle) and runs natively.
- **Re-attach.** `adoptCoreAgentAtStartup` also adopts parsed Codex processes,
  and `finishCoreCodexReattach` wires a `CodexAppServerSession::attachProcess`
  the way the Claude path does. The live test's guarantee (the full transcript
  exactly once after a restart) now comes from the core's single write plus the
  `_so` replay.

**Event order.** Events the desktop makes up itself (guardrail notices, the
synthesized permission question, `_local_user`) are still written through
`AgentStore`, so they land in arrival order next to the core's events. They
can sit a few events later than before.

### Terminal, IDE bridge and external-call gate after a restart

- **Agent terminal.** The agent terminal keeps a core PTY under the persist
  key `agent`, with the session it runs in `core/agentTerminalSession`. At
  startup that session keeps its status, and `resumeCoreAgentTerminal()`
  re-attaches the widget (or re-queues the session when the PTY is gone).
  `startClaudeCodeTerminal` currently has no caller, so this path is ready but
  idle.
- **External-call gate.** `agentnet::Server::listen` takes a stable path,
  `$TMPDIR/forkmesh-agent-gate-<user>-<hash of the core state folder>`. A
  re-attached run's hook reaches the restarted desktop: its session token
  carries the session id and is not tied to the old process. While no desktop
  is up the hook still fails closed.
- **IDE bridge.** `ClaudeIdeBridge` reuses its last port and auth token
  (`core/ideBridge/port`, `token`), so a re-attached Claude that reconnects
  finds the same lockfile and token.

### The remaining runs: `AgentRunner` and dialog sessions

Round 4 moved the last agent output that the desktop parsed on its UI thread.

- **Dialog sessions (Calendar "Fill event", Market package text).** A
  `ClaudeStreamSession` or `CodexAppServerSession` started without a
  `processKey` now asks for a *transient* parse when the core can parse: the
  key is `dialog-<claude|codex>-<pid>-<ms>-<n>`, the meta carries `parse` and
  `transient: true`, and the process is not detachable.
  - The core parses it exactly like an Agents-page run, but writes nothing to
    the agent store (there is no session). It keeps the last 4,096 normalised
    events in memory so `core.agentrun.attach {after}` can replay them.
  - They do not survive a desktop restart on purpose: the dialog that waits
    for the answer is gone with the window, so the core stops the process when
    its desktop connection closes, as before.
- **`AgentRunner` (Cloudflare AI, local Llama, OpenAI API and other configured
  commands).** The agent phase starts as `runner-<sid>-<ms>`, detachable, with
  meta `{runtime: forkmesh-managed-runner, parse: log, sessionId, storeKey,
  provider, repoPath, worktree, promptFile, program}`. The worktree phase
  (`git worktree add`) is unchanged.
  - The core's `log` runtime reads the merged output, keeps a split UTF-8
    sequence until it is complete (the desktop used to decode each chunk on
    its own), redacts it the way `AgentRunner::redact` does (the run's API key,
    `sk-` tokens, bearer headers, credential assignments, plus the pushed
    provider credentials) and appends each chunk to `transcript.txt` through
    the store writer, adding a newline when the chunk lacks one, as
    `AgentStore::appendLog` does.
  - The run's API key reaches the core as `core.agentproc.start {secrets}`
    (`CoreProcess` takes it out of the meta key `_secrets`). It is held in
    memory only; after a core restart the run is redacted with the pushed
    credentials and the patterns alone.
  - The desktop receives `core.agentrun.event {key, text, so}`, emits
    `logLine` and runs the auth-issue checks on the redacted text; it no longer
    writes the chunks itself. Its own `==> …` lines still go through
    `AgentStore`.
  - It finishes on `core.agentrun.exit`, after the writer has drained, and
    then runs the unchanged post-processing (provenance trailer, patch
    capture, worktree cleanup).
  - **Restart.** `adoptCoreAgentAtStartup` adopts a live `log` process like a
    Claude one, and `finishCoreRunnerReattach` (`MainWindowCoreAgentRunner.cpp`)
    rebuilds the runner: the session from the store, the configuration from
    `agentConfigForProvider` with the session's model, mode and strength, the
    worktree, repository and prompt file from the meta. The run then goes on
    to its normal completion. Output written while no desktop was up is in the
    transcript; the live `logLine` stream resumes from the re-attach.
  - The ack window: the core acknowledges the spool once the writer has
    drained, so a core crash between a write and its ack can repeat one chunk
    in the transcript (the `log` runtime has no per-line offsets to dedupe
    against).

### Durable stderr

Stderr of every core-parsed run (Claude, Codex, transient, `log`) is now read
by the core instead of the desktop.

- `agentproc` hands stderr to a consumer that implements `Stderr(data,
  offset)`; the start and attach replies say `parsedStderr`. A desktop
  attachment that sends `skipStderr` then gets no `core.agentproc.stderr`
  events.
- `agentrun` decodes it (split UTF-8 kept), redacts it and sends
  `core.agentrun.event {key, stderr, eo}` (`eo`: the stderr byte offset the
  text ends at) only to subscribers that attached with `stderr: true`. Older
  desktops do not ask and keep reading stderr from the process attachment, so
  nothing is shown twice in either direction.
- `core.agentrun.attach {stderr: true, stderrAfter}` replays the stderr since
  `stderrAfter`, at most the last 256 KB, in one `replay` event. The desktop
  (`CoreAgentRun.cpp`) remembers the last `eo` per key, so a core reconnect
  resumes without repeats, and a restarted desktop (empty raw view) gets the
  tail.
- The core acknowledges stderr itself and keeps the last 1 MB in the spool
  for replays; nothing depends on the desktop's ack any more.

### Shim transport on Windows

The core talks to each shim over `<dir>/s`, a Unix socket. Windows builds
(`agentproc.defaultUseTCP`) use loopback TCP instead:

- the shim listens on `127.0.0.1:0` and writes `tcp.json` (`addr`, a 192-bit
  random `token`; 0600, written through a temporary file) into its folder
  before it answers the launch handshake;
- the core dials that address and sends an `a` frame with the token first.
  The shim drops any connection whose first frame is not the right token
  (5 s limit) before it is treated as the client;
- `spec.json` records `transport: tcp`, so a restarted core reconnects the
  same way; the shim removes `tcp.json` when it exits.

The tests run the TCP transport on Linux (start, output, a soft core stop, a
second core adopting the agent, wrong or missing tokens refused).

### Phone methods

| Method | Where | Why |
| --- | --- | --- |
| `agents.transcript` (id > 0) | **native** | Same text window, `since`, `reset`, UTF-16 offsets (CR stripped, invalid bytes as U+FFFD), redaction, `active` and `status` from `meta.json`, and attachments with the core-owned salt (the desktop adopts it through `core.agentrun.salt`, so its proxied `agents.attachment` ids agree). Id 0 answers `unknown_session`. It delegates negative ids (outside sessions), records the store does not have (cloud rows), and anything before a desktop has pushed credentials |
| `agentops.bin` | **native** | Owner gate, recycled sessions from the store, `binTitle`, retention from `agents/recycleRetentionHours` |

Round 4 made the rest of the agent family native or core-confirmed; see
[Agent session model](#agent-session-model) for the full table.

### Tests

- **`internal/qtjson`.** 3,000+ doubles, the parser edge cases and the
  `Indented` form, against fixtures from `testdata/golden.cpp` built with the
  real Qt (`testdata/gen.sh`).
- **`internal/agents`.**
  - The C++ store scenario (create, save, append, patch, clear, recycle, late
    append, restore, delete, a late save, id reuse, recycle) is replayed, and
    every file, file mode and folder is compared with what `AgentStore.cpp`
    produced.
  - `SessionFromQt`/`ToQt` are checked against C++ `fromJson`/`toJson`.
  - The `core.agentstore.*` methods end to end, including phone refusal, path
    escapes, tombstones and outcomes.
- **`internal/agentrun`.**
  - `AgentLogText` and redaction against C++ fixtures.
  - A Claude stream fed in 1-, 3-, 17- and 64-byte chunks writes exactly the
    C++ `events.jsonl` and `transcript.txt`.
  - A resumed consumer does not store twice.
  - The Codex protocol, replayed against the real `CodexAppServerSession`
    driven by a fake process (`testdata/codex_golden.cpp`, `gen_codex.sh`):
    every stdin write and every event match, in two scenarios with 68 checks.
  - Process tests with fake `claude` and `codex` binaries: a Claude run
    survives a core restart with every event and transcript line once, and a
    Codex run keeps its thread across a detach and re-attach with replay once.
  - The phone `agents.transcript` and `agentops.bin` rules.
  - Round 4: durable stderr (redacted, split UTF-8 kept, `eo` offsets, replay
    from `stderrAfter` and of the tail, nothing for subscribers that did not
    ask, no `core.agentproc.stderr` with `skipStderr`); transient Claude and
    Codex runs (parsed, nothing in the store, in-memory replay from `after`,
    no parse without the flag); the `log` runtime (run secrets redacted and
    kept out of `meta.json`, `transcript.txt` written by the core, no
    `events.jsonl` lines, split UTF-8 across chunks).
- **`internal/agentproc`.** Consumers and `skipStdout` are covered through
  `agentrun`, and the existing tests still pass. The TCP shim transport has
  its own tests (`transport_test.go`).

### Gaps

- **Not exercised.** The round-4 Qt side (`AgentRunner` core mode and
  re-attach, dialog transient parse, durable stderr, the TCP store link) has
  been syntax-checked only. The Codex live restart recipe is in
  [Verification](#codex-restart-test-recipe); the `AgentRunner` re-attach has
  no live run yet (it needs a configured runner provider in the sandbox).
- **Event order.** Desktop-originated events can land a few lines later than
  before (see above).
- **Windows.** The Go side builds for Windows and the TCP paths are tested on
  Linux; the Windows-only C++ branches (winsock in `CoreAgentStore.cpp`,
  `OpenProcess` in `CoreProcess.cpp`) cannot be compiled on the Linux build
  box (no MinGW or Windows SDK headers) and have not been built. Without the
  shim's process group, `signal` and `stop` on Windows kill the agent process
  only (`TerminateProcess`), not its children. `terminateShim` (core off at
  startup) does nothing on Windows; the core stops leftover shims instead.
- **Parsing still on the desktop.** Only when the core is absent or refuses to
  parse (no credentials pushed yet, a different store root): those runs use
  the round-2 path.
- **Launching is still the desktop's.** The read model, the phone protocol
  and the completion checks moved in round 4 ([Agent session
  model](#agent-session-model)); starting and resuming runs did not.
- **Agent terminal.** Its launch path has no caller today.

## Agent session model

Issue 1336, round 4. Rounds 2 and 3 moved the agent processes, the stream
parsing and the store writer into the core, but the phones still asked the
desktop for everything about the agents themselves: the list, the queue, the
follows, the diffs, the recycle bin and the confirmations. The background-work
poll after a finished turn (a `/proc` walk every 250 ms) also ran from the UI
thread's timers. `internal/agentmodel` now owns the agent read model and the
phone protocol for agents, runs the completion checks, and keeps answering
while the desktop is closed or restarting. The launch path (prompt building,
worktrees, CLI arguments, accounts) is still the desktop's, so starting,
continuing and steering an agent are still executed there.

### What the core holds (`internal/agentmodel`)

- **Sessions.** The records come from the store the core already writes
  (`agents.Writer`). One cached list serves every read; any record change
  reported by the writer's new observers (`Writer.Observe`: record, removed,
  restored, bin, log, event) drops it, and it is never older than 2 s. A saved
  record also drops the store's per-file meta cache, because a same-size
  rewrite within one mtime tick used to read back stale.
- **The desktop's context.** What only the desktop knows arrives as
  `core.agents.model.push` (a request; the reply carries the confirmation
  secret, below). The desktop sends it from the agents bell
  (`notifyLocalControlAgentsChanged`, at most every 300 ms, now also with no
  phone connected) and, debounced and only when its fingerprint changed, from
  every `refreshAgentTable`. It carries:
  - per session: unread, the phone-terminal id, diff stats and worktree, the
    known prompt images, and whether work is in flight
    (`agentSessionWorkInFlight`);
  - the outside rows exactly as `localControlAgentsPayload` builds them
    (now `localControlOutsideRows()`), and for each one the facts the
    `outsideAgent` lambda and `readLocalControlOutsideTranscript` use (label,
    folder, other user, phone terminal, transcript path and kind);
  - the repository records (owner, name, checkout, mirror, preview-only, and
    whether it belongs to the active relay, the `repoIndexFor` rule);
  - providers, default provider and mode, `runningAgentCount()`,
    `m_agentQueue` in order, `maxRunningAgents()`, the open repository, the
    pending approvals with their index, and the saved permission grants;
  - a sequence number; an older push is ignored.
- **Fresh facts on demand.** Approvals, grants and `agentops.prepare` for
  approve, deny and revoke ask `ui.agents.context` first, as the C++ computed
  them per request. Usage is never pushed (it reads account files):
  `agentops.usage` asks `ui.agents.usage` for the inputs and builds the summary
  in the core (`usageSummary`); a desktop that went away leaves the last
  answer.
- **Phone bell.** `agents.changed` is rung after every applied push and, while
  no desktop is attached, on record changes, with the desktop's 300 ms
  spacing. The desktop no longer broadcasts it while the core takes pushes.
- **Follows.** The follow pump (`agents.follow`, `agents.unfollow`,
  `agents.output`) is a port of `handleLocalControlFollowRequest` and
  `pumpLocalControlFollows`: per-connection limit of 8 (the oldest idle follow
  goes), `LogTail` growth reads with the same `from`/`next`/`reset` window,
  attachments when the read starts at 0 or the new text names one, 15 ms (LAN)
  and 80 ms (relay) spacing, relayed reads held back while the relay socket
  has a backlog (`relay.Client.ControlBacklog`), outside transcripts watched
  every 200 ms, and a failed send drops the connection's follows. Log growth
  comes from the writer's observer instead of `AgentStore::setLogObserver`.
- **Outside transcripts.** `ClaudeSessionScan::readEvents`/`tailStartOffset`
  and `AgentLogText::outsideEntry` (Claude and Codex) are ported; the tail kept
  per outside row is shared by `agents.transcript` and follows, as the C++
  shares `m_localControlOutsideTails`.
- **Queue view.** `agentQueueOrder` (queued, not held, not deleted, stable by
  priority) and `agentQueuePayload` are computed from the pushed `m_agentQueue`
  and the store's records.
- **Completion checks.** See [Background work](#background-work-after-a-turn).
- **Confirmations.** The tokens of `agentops.prepare` (holder = device key and
  connection) and `diag.prepare` (holder = device key) are issued and consumed
  in the core.

### Background work after a turn

When a Claude or Codex turn ends with `result` while processes it started are
still alive, the session stays Running until they exit
(`completeAgentSessionWhenSubprocessesExit`). The core now runs that check:

- `core.agents.background.watch {id, pid, announced}` starts it; the core
  walks `/proc` (the `SystemStats::processTable`/`descendantProcesses` port,
  zombies skipped), 20 polls at 250 ms and then every second;
- the state machine is the C++ one: CPU ticks decide busy, 8 s without CPU or
  15 minutes overall stop the stragglers (SIGTERM, SIGKILL after 4 s), and
  10 s after that the session completes anyway, with the same three notices;
- each poll is sent as `core.agents.background {id, count, busy, summary,
  changed, notice}` and the end as `{id, done, notice}`; the desktop applies
  them with its existing code (the drain marker, the notice in the transcript,
  the status cell, Success and `notifyAgentDone`);
- a watch for a check already running on the same pid is adopted (a restarted
  desktop's re-arm), `announced` stops a second "The turn is over" notice
  after a hand-off, a new turn or a Waiting status cancels it
  (`core.agents.background.cancel`), and the core drops it by itself once the
  stored status is no longer Running;
- with no desktop attached the core appends the notices to `transcript.txt`
  and makes the Success transition itself, so a background build that drains
  while the desktop is closed ends the run;
- `Busy()` is true while a check runs: the core lingers, and an upgrade waits
  for it;
- if the core goes away, the desktop resumes the checks locally.

The process walk exists on Linux only, as in the C++; elsewhere the table is
empty and a turn completes at once.

### While no desktop is attached

The core answers from the store and the last pushed context:

- `agents.list` and `agents.queue`, with `running` counted from the store's
  Running sessions;
- follows and transcripts (own and outside), diffs, files, worktrees, merge
  statistics, the recycle bin (`bin.retention` needs the desktop to write the
  setting and answers `desktop_unavailable` without it);
- `agents.stop`, `diag.run stopAgent`: the core stops the session's agent
  processes (`agent-<sid>-…`, or meta `sessionId`) through `agentproc` and
  saves the record Stopped with `finishedAtMs`; a row running in a phone
  terminal answers `desktop_unavailable`;
- `agents.queue.move`: the priority arithmetic of `moveQueuedAgentSession`,
  saved to the record, so the order holds when the desktop rebuilds its queue
  from the priorities at startup.

Starting, prompting, continuing, taking over and handing off answer
`desktop_unavailable`, as the proxy did.

### Methods

| Method (desktop and CLI only; phones get `forbidden`) | Params → result |
| --- | --- |
| `core.agents.model.push` | the context above → `{applied, confirm}` |
| `core.agents.model.state` | → `{pushed, seq, queue, running, limit, follows, sessions, confirm}` |
| `core.agents.background.watch` | `id`, `pid`, `announced` → the check's state with `adopted` |
| `core.agents.background.cancel` | `id` → `{cancelled}` |
| `core.agents.background.list` | → `{checks}` |

The core asks the desktop `ui.agents.context` (the context, now), `ui.agents.usage`
(usage inputs), and notifies `ui.agents.reload {sessions, bin}` (after a bin
restore or purge) and `ui.agents.opsArm` (the first `agentops.*` request on a
desktop connection, so the desktop's approvals bell and approval pings run as
before). Event: `core.agents.background`.

### Core-confirmed requests

Five `agentops` actions and `diag.run` still execute desktop code (merge,
delete, answer, bulk stop/continue, revoke; the stall agents). The core checks
and spends the token, then hands the request to the desktop with
`rpc.DelegateWith` (new, additive: a delegation that carries replacement
parameters; `control` proxies those instead of the phone's). The parameters
gain `_coreConfirmed: <secret>`. The desktop skips its own token check only
when that equals the secret the core gave it (push reply or
`ui.agents.context`) and the connection is a core connection
(`LocalControlServer::kCoreConnectionBase`); the core removes any
`_coreConfirmed` a phone sends. Everything after the token check is the
unchanged C++.

### Phone methods

| Method | Where | Why |
| --- | --- | --- |
| `agents.list`, `agents.queue` | **native** | Rows from the store (filters, active-first order, 200 rows, prompt and error cut at 600 UTF-16 units, held, stoppable, redaction) plus the pushed context; delegated until the first push and credentials |
| `agents.follow`, `agents.unfollow`, `agents.output` | **native** | The follow pump above, for own and outside sessions |
| `agents.transcript` | **native** | Outside sessions (negative ids) now too; own sessions also list the desktop's known prompt images |
| `agents.attachment` | **native** for the raw image on the LAN; **proxied** for relay and `preview` | The preview path decodes with `QImageReader`, scales and re-encodes with `QImage::save` (JPEG at falling quality, alpha flattened to white); Go's `image/jpeg` produces different bytes and has no HEIC or WebP decoder. Errors decided before decoding (`unknown_attachment`, `not_an_image`, `unreadable`, `too_large`, `too_large_over_relay`) are native |
| `agents.stop`, `agents.queue.move` | **native** while no desktop is attached; desktop otherwise | Attached, stopping tears down the desktop's session objects and the queue lives in `m_agentQueue` |
| `agents.start`, `.prompt`, `.continue`, `.takeover`, `.handoff`, `.queue.limit`, `.markRead` | desktop (`desktop_unavailable` without one) | The launch path; the limit parks running transports and writes the setting; the read state is the desktop's settings |
| `agents.image.begin`, `.chunk`, `.cancel` | proxied | The uploads are consumed by `agents.start`/`agents.prompt`, which run on the desktop |
| `agentops.prepare`, `.approvals`, `.grants`, `.usage` | **native** | Tokens in the core; approvals and grants fresh from the desktop; usage summarised in the core |
| `agentops.diff`, `.file`, `.mergestats`, `.worktrees` | **native** | `readAgentDiff`, `readAgentDiffImage`, `readLandedMergeStats` (with `mergedBranchFromMergeSubject` and `mergeVelocityForTimestamps`) and the worktree scan ported, run with `internal/git` |
| `agentops.bin` (agentrun), `.bin.restore`, `.bin.purge`, `.bin.retention` | **native** | Core store; the desktop reloads its list and bin strip on `ui.agents.reload` |
| `agentops.answer`, `.merge`, `.delete`, `.bulk`, `.revoke` | **native token**, desktop executes | Answering reaches the desktop's permission controller; merge, delete and continue are desktop code paths; grants live in the desktop's capability broker |
| `agentops.accounts.select`, `.rename`, `.add` | desktop | Account profiles and their login folders are the desktop's (settings and `AgentAccounts`) |
| `diag.prepare`, `diag.run` | **native token**; `stopAgent` native while no desktop is attached | Stall agents start on the desktop |

### Qt facade (`MainWindowCoreAgentModel.cpp`, `CoreAgentModel.h`)

- `coreAgentModelAttached()` (the core has `core.agents.model.push`) switches
  the bell to a push; `localControlAgentsContext()` and
  `localControlAgentOpsContext()` build the context next to the code that
  builds the C++ payloads; `localControlAgentOpsUsageInputs()` shares
  `agentOpsUsageProviders()` with `localControlAgentOpsUsage()`.
- `ui.agents.context`, `ui.agents.usage`, `ui.agents.reload`,
  `ui.agents.opsArm` are answered there; `core.agents.background` events go to
  `applyCoreBackgroundWork`.
- `completeAgentSessionWhenSubprocessesExit` hands the check to the core
  (`watchCoreBackgroundWork`); `markAgentSessionRunning` and
  `notifyAgentWaiting` cancel it.
- The agentops and diagnostics handlers accept a core-confirmed request
  (`forkmesh::coreagentmodel::confirmedByCore`).
- Without the core (or with an older one) every path is the one before.

### Deliberate differences

- `agents.list` sorts with a stable sort (the C++ `std::sort` leaves equal
  rows in an unspecified order).
- The context is pushed on change, so a phone can read a value up to 300 ms
  old; `running` depends on the clock (`kSlotStaleMs`) and is only re-sent
  with the next push.
- Outside rows and phone-terminal flags stay as last pushed while no desktop
  is attached.
- Merge statistics run git with `LC_ALL=C`, so the `--shortstat` line is
  parsed in English on any locale.

### Tests

- `testdata/gen.sh` builds `testdata/ops_golden.cpp` against the real
  `LocalControlAgentOps.cpp`, `AgentLogText.cpp` and the merge-velocity
  functions extracted from `MainWindowInternal3.cpp`; 150 cases (summaries,
  numstat, diff caps, untracked paths, image paths, worktree porcelain,
  left-right counts, shortstat logs, merged-branch subjects, merge statistics
  with velocity windows, bin payloads, usage summaries, grants, canonical ids,
  targets, outside Claude and Codex entries) match byte for byte.
- `agents.list` rows byte for byte for a fixture; the queue view; delegation
  before the first push or credentials; stale pushes ignored; the 300 ms bell.
- Follows: growth, redaction, status pushes, the limit of 8, unfollow, peer
  close, outside transcripts, relay backlog pacing, delegation without a
  context.
- agentops: owner gate and bell arming, token issue and spend (holder,
  mismatch, replay), the `_coreConfirmed` marker set by the core and never
  passed through from a phone, fresh approvals and grants, diff, image, worktree
  and merge statistics on a real git repository with a worktree, the recycle
  bin (restore brings a running session back stopped, purge, retention through
  `ui.settings.write`), usage.
- Detached stop and queue move; raw attachments on the LAN and delegation of
  previews; `diag.prepare`/`diag.run`.
- Background work: busy and idle polls, adoption, completion with a desktop,
  straggler stop and the forced completion without one (notices in
  `transcript.txt`, Success saved), cancellation when the session leaves
  Running.
- `rpc.DelegateWith` in `internal/rpc` and through `internal/control` to the
  desktop; the writer observers in `internal/agents`.

### Gaps

- **Launching stays on the desktop.** `processAgentQueue`, the queue pump,
  parking over the limit, usage-limit holds and their timers, resume,
  continue, takeover and handoff are desktop code because the launch path is
  (`startCliTranscript`, `AgentRunner::start`). The core only shows the queue.
- **The desktop still reads its own list from disk.** `reloadAgents` loads
  `m_agentSessions` through `AgentStore` (stat-cached); it does not subscribe
  to a core session stream.
- **Not exercised.** The Qt side is syntax-checked only; a live run with a
  phone (list, follow across a desktop restart, merge through the
  core-confirmed path, a background build draining with the desktop closed) is
  still to do.

## Actions

The `.forkmesh` workflow runtime. The package is `internal/actions`
(`actions.New()`); the desktop side is `CoreActions.{h,cpp}` (the runner
facade), `MainWindowCoreActions.cpp` (the `ui.actions.*` handlers and the
restart reconcile) and `ActionSandboxClaims.h`.

### What runs in the core

- **Workflow parsing** (`workflow.go`): `ActionFile` rule for rule. The same
  indentation parser and its quirks (comments dropped inside block scalars,
  `>` kept literal, `QMap` key order for `on:` events and `jobs:`), the
  `on.push.paths` wildcard as Qt 6.8 converts it (`*` and `**` stop at `/`),
  labels, pins, `needs`, `substitute`, `defaultBranchRetiresPush`, and the
  directory scan (text-mode read, so `\r` is dropped before hashing).
- **Run store** (`store.go`): the same `actions/runs/<v2-key>/<id>/meta.json`
  and `log.txt`. `meta.json` is written byte for byte like
  `QJsonDocument::Indented` (sorted keys, 4-space indent, Qt's escaping),
  so history written by either side reads the same. Legacy-key migration,
  `needs` resolution, approvals and variables (read-only, from
  `ForkMesh.conf`) and the spool format are ported. The core caches parsed
  records by mtime and size, because the desktop still writes queue states.
- **Runner** (`runner.go`, `sandbox.go`, `release.go`): the `ActionRunner`
  state machine (checkout worktree, disposable clone, detached checkout,
  approved-snapshot verification, steps, quota check, artifact and release
  landing, cleanup). Steps run as
  `systemd-run --user --scope … -- bwrap … /bin/bash -se` with the same
  arguments, environment policy, limits, unit name
  (`forkmesh-action-<core pid>-<run>-<step>.scope`) and log handling (16 KiB
  chunks, 1 MiB cap, 128 KiB tail, 4096-column wrap, secret redaction). The
  step script goes in on stdin.
- **Child setup.** Go cannot run code between `fork` and `exec`, so the core
  starts `/proc/self/exe __forkmesh-actions-exec …`. That helper sets the
  umask, `RLIMIT_CORE/AS/FSIZE/NOFILE/CPU` (the C++ child modifier's values)
  and `exec`s the real program. `setsid` comes from `SysProcAttr`, and stop
  signals go to the process group as before.
- **Repository digest** (`gitops.go`): `repositoryStateDigest` with its memo,
  and the push-range helpers (`ActionPushRange`).
- **Sandbox sweep.** Trees are claimed in memory and by an `flock` on
  `<tree>/.forkmesh-claim`. The desktop's native runner takes the same lock,
  and both sweepers skip a tree whose lock another process holds. Without it,
  the desktop's startup sweep would delete a run the core is still building.
  Deletes go through `internal/safedelete`.

### Desktop methods (`core.*`, `forbidden` for phones)

| Method | Does |
| --- | --- |
| `core.actions.start` | `{run, workflow:{path,content}, mirror, worktree, variables, slice, root}`; refuses `already_running`, `invalid_workflow`, `store_mismatch` (the desktop's store root differs), `core_stopping` |
| `core.actions.stop` | `{run}` → `{stopping}` |
| `core.actions.active` | Active runs (record, workflow, mirror, worktree), finished-but-unacknowledged runs, claimed sandbox names |
| `core.actions.ack` | `{runs:[id]}` drops finished records the desktop has handled |
| `core.actions.log`, `core.actions.runs` | Log slice and run records from the store |
| `core.actions.parse`, `core.actions.workflows`, `core.actions.digest`, `core.actions.sandbox` | Parser, workflow listing (mirror `HEAD`, else checkout), state digest, sandbox probe |

Events to the desktop: `actions.log`, `actions.step`, `actions.status`,
`actions.release` (`prepared` or `landed`), `actions.finished`,
`actions.interrupted`, `actions.sandbox` (claim on or off). Each carries the
run id and, where state changed, the full `record`.

### Phone methods

- **Native:** `repos.actions`, `repos.actions.log`, `repos.prepare` and
  `repos.run`, with one confirmation store shared by prepare and run (48-hex
  tokens, 60 s, 32 outstanding, holder-bound, spent on any outcome).
- **Repository facts.** Once the desktop has attached the run queue (see
  [The run queue](#the-run-queue)), the repository record and this node's
  labels come from the core's repository snapshot. The checks keep the C++
  order: the shared owner gate (`not_owner`), the slug (`bad_request`), then
  the `repoIndexFor` rule (`unknown_repo`: not a preview, same owner and name,
  in the active relay). Before that, and with an older desktop, they still
  come from `ui.actions.repo`.
- **Computed in the core:** runs (from disk), workflows, branches, step names,
  `here`/`runsOn`, summaries and the UTF-16 log slices.
- **The actions themselves.** The `workflow.run`, `workflow.enable/disable`
  and `run.approve/reject/rerun/stop/skip` intents are `repos.prepare`
  actions. After the core spends the token it applies the intent to its own
  queue, with the C++ replies (`Started run #N.`, `Approved run #N.`, …), the
  same errors (`unknown_repo`, `unknown_run`, `wrong_state`, the approval
  sentence, `not_started`), the "requested by" line in the system log and a
  coalesced `repos.changed` to the phones (750 ms). Only
  `workflow.enable/disable` asks the desktop (`ui.actions.workflowDisabled`),
  because `disabledWorkflows` is part of the repository record the desktop
  owns and saves; without a desktop it fails with `desktop_unavailable`.
  `ui.actions.perform` is used only when the queue is not attached (an older
  desktop).
- **`repos.list`** is native. Rows come from the repository snapshot
  (`writableRecordFor` is sent as `writablePath`, so `checkout` and `branch`
  read the same working copy); the `actions` column is `ActionsSummary` over
  the store; agent counts come from the core's agent store
  (`agents.Module.Store().LoadAllSessions()`, skipping deleted and
  association-only sessions and those with no owner). The C++ also counted
  in-memory sessions not yet written and skipped cloud rows (which are never
  on disk). Before the queue is attached the request is delegated to the
  desktop; with no desktop at all the rows come from `repositories/items/*`
  in `ForkMesh.conf` (`SettingsSnapshot`).

### Qt facade

- **Choosing the core.** `ActionRunner` gets an `ActionRunnerCoreLink`
  (`CoreActions.cpp`). `start()` hands the run to the core when the client is
  connected and the route exists. A definite refusal falls back to the native
  runner. A timeout or lost connection reconciles instead, so a run is never
  started twice.
- **Signals.** The link turns events back into the runner's signals in native
  order (`statusChanged` and then `finished` once `busy()` is false, then
  `releaseMetadataLanded`). It updates the desktop's run cache through
  `ActionStore::adoptRun`, without writing to disk: the core is the only
  writer while a run executes.
- **Restarts.** The core outlives the UI (an active run makes it `Busy()`). On
  start, `restoreActionRuns` hands `running` records to
  `deferActionRunToCore`. `reconcileCoreActionRuns` then asks
  `core.actions.active` and does three things:
  - re-attaches each run that is still active: a run the core's queue
    started is followed directly from its events (see
    [Reactions to runs](#reactions-to-runs)); with an older core it goes to
    an idle runner as before;
  - replays `onRunFinished` (and `onReleaseMetadataLanded`) for runs that
    finished while no desktop was attached, then acknowledges them;
  - requeues the rest as before (`RESTARTING`).

  It waits 20 s for the core before falling back. A connection loss in the
  middle of a run gets the same treatment from the link.
- **Shutdown.** `core.shutdown` (an upgrade) stops runs with `INTERRUPTED`
  and leaves them `running`, so the desktop requeues them from the approved
  snapshot, which is the native behaviour on exit.

### The run queue

`queue.go`, `queuescan.go`, `queueops.go`, `queueapi.go`, `approvals.go`,
`repofacts.go`. The queue of `MainWindowActions.cpp` ported decision for
decision; every log line keeps its C++ text and goes to the desktop's system
log through `Core.LogUI`.

- **Arming.** The queue runs once a desktop sends `core.actions.queue.attach`
  with a repository snapshot, and stays armed for the life of the core (it
  keeps scanning and starting runs while no desktop is attached). The
  snapshot is the desktop's `m_repositories` reduced to what Actions needs:
  runtime `mirrorPath` (encrypted mirrors live in temporary
  materializations), `relay` (the `repositoryBelongsToActiveRelay` answer),
  `pinned`, enable and auto-approve flags, external source and ref, disabled
  workflows, node pins, plus `nodeLabels`, `machine`, the systemd `slice`,
  the mirror configuration generation and suspension flag, and `native` (runs
  the desktop's own runners still execute). The desktop resends it with
  `core.actions.queue.repos` when its fingerprint changes; it checks on every
  spool pass (4 s) and every `processActionQueue` call.
- **Restore at attach.** A `running` run that no core runner and no native
  desktop runner owns is requeued with the `RESTARTING` log line; `queued`
  runs go back on the queue; awaiting runs of auto-approving repositories are
  approved (first attach only). This replaces the desktop's restore pass.
- **Scans.** Every 4 s (and on `core.actions.queue.kick`): the external
  source scan (3.5 s throttle), the `*.push` spool files (the desktop keeps
  the `*.commit` files, which drive `propagateRepoUpdate`), the served-head
  scan, the queue pass and the mirror runtime state.
- **Creating runs.** Push, release, pull-request and manual triggers, rerun,
  `on.push.paths`, `defaultBranchRetiresPush`, the base branch's
  `pull_request` answer, node pins, disabled workflows, snapshot binding, the
  stop-time spool, superseding (a running core run is stopped, a native one
  gets `actions.stop`), first-time pull-request authors held.
- **Starting runs.** `needs` (waiting runs are announced once, blocked runs
  skipped), the runner pool (`clamp(NumCPU, 2, 4)` like the desktop, counting
  core runs and native ones), runners set aside while a snapshot is
  reverified (2 min timeout), the pinned materialization (`ui.actions.pin`,
  released with `ui.actions.unpin` or by the desktop's `onRunFinished`),
  stale-record detection, and the approval check against the reverified
  tree and digest. Runs start in the core's own runner; `actions.started`
  tells the desktop, which follows the run from its events without an
  `ActionRunner` ([Reactions to runs](#reactions-to-runs)).
- **Approvals.** `actions/approved/<v2 key>` is written through
  `core.WriteSettings` as `json` (the desktop's `QJsonDocument` compact bytes,
  exactly what `ActionStore::approve` wrote). An approval stays in an
  in-memory overlay (read-your-writes, like the C++ queued approvals) until
  the settings file shows it, and is retried at the next attach when no
  desktop could write it. `actions/servedHeads/*` and
  `actions/externalHeads/*` are written the same way; a head whose write
  fails is not queued, so the record-before-run rule holds.
- **Housekeeping.** Cancelled runs are deleted (`actions/autoDeleteCancelledRuns`,
  default on) through `internal/safedelete`, with `actions.deleted`; the
  mirror runtime state and summary files (`MirrorActionsSummary` ported:
  `BuildSummary`, `WriteSummaryFile`, `WriteStateFile`, the ownership policy
  and the root hand-off) and the `forkmesh-mirror-renew.service` kick.
- **Desktop methods** (`forbidden` for phones): `core.actions.queue.attach`,
  `.repos`, `.kick`, `.state`, `.push`, `.commit` (`trigger`: `push`,
  `release`, `pull_request`, `pull_request_manual`; `explicit` claims the
  commit for 60 s against the push pipeline), `.manual`, `.approve`,
  `.reject`, `.rerun`, `.stop`, `.skip`, `.autoApprove`, `.pullChecks`
  ([Pull-request checks](#pull-request-checks)). Errors carry the C++
  sentences. Events: `actions.run` (a record the queue changed; `created`,
  and `ping` for an approval request), `actions.started`, `actions.deleted`,
  `actions.queue` (the queue, verifying runs and the per-run reason text the
  run pane shows), `actions.push` (the "Push received" alert),
  `actions.pushed` (fan out to SSH mirror remotes), `actions.stop`.
- **Qt facade** (`MainWindowCoreActionQueue.cpp`). `coreActionQueueActive()`
  is true once the attach succeeded and the client is connected. Then the
  queue entry points in `MainWindowActions.cpp` forward instead of running:
  `enqueuePushEvent`, `queueWorkflowsForCommit` (all triggers, pull context,
  the explicit claim), `startManualWorkflowRun`, `autoApproveAwaitingRuns`,
  `stopActionRun`, `skipActionRun` (asynchronous), and `approveActionRun`,
  `rejectActionRun`, `rerunActionRun` (a blocking `callCore`, because their
  callers use the result; the C++ blocked on the same git work).
  `processActionQueue`, `finishActionQueueVerification`,
  `scanExternalActionsSources`, the `*.push` half of `scanActionSpool` and the
  mirror state and summary writers stand down; `actionRunQueueReason` shows
  the core's reasons. When the core goes away the flag drops and the native
  queue picks the queued runs up again (`requeueOrphanedActionRuns`).
- **Desktop requests:** `ui.actions.pin`, `ui.actions.unpin`,
  `ui.actions.workflowDisabled`, `ui.actions.autoFix`.

### Pull-request checks

`pullchecks.go` ports `queueChecksForOpenPulls`, `pullCheckCandidate`,
`resolvePullCheckHeads` and `applyPullCheckHeads` decision for decision.

- **Method.** `core.actions.queue.pullChecks {owner, name, trigger, force,
  numbers, trusted, identity}`. The desktop calls it from every place that
  used to resolve heads: the pull reload of the open repository and the
  relay-sync sweep (automatic, `pull_request`), and "Run checks against this
  PR" (`pull_request_manual`, `force`, the pull's number). `trusted` is the
  node's public key, the account name and the user name (the core adds the
  repository owner), `identity` whether the node identity loads (the
  `PullStore::canWrite` rule). The reply is immediate (`{accepted}`, or
  `unknown_repo`); the pass runs in the background and `Busy()` holds the
  core up while it does.
- **Pull records come from disk.** The core loads the ledger itself with
  `internal/pulls` (`LoadAll(true)`, the derived patches included), so the
  desktop sends no pull list. Only open pulls count; the automatic pass keeps
  the C++ fingerprint, the 60 s rescan floor, the in-flight flag, the
  "did not replay" set (cleared when the open pulls change), newest first,
  at most 25 resolved and 4 queued per pass, at most 2 signed-patch replays
  (`pulls.Store.MaterializePullRef`, the C++ `ensurePullHeadRef`).
- **Publishing.** The same ref order (the branch of a branch-backed pull,
  `refs/pr/<n>/head`, `refs/heads/pr/<n>`), the bridged-head rule (same tree,
  or the published head descends from the requested one), the
  `--force-with-lease` push into the served mirror, one retry after
  dropping refs that name a bad object (`dropUnreadableRefs` ported), and
  the explicit claim so the mirror's post-receive hook does not also queue
  `on: push` workflows. Git runs in the core with a 20 s timeout per command.
- **Trust.** `PullAuthorEstablished` is `PullAuthorTrust::isEstablished`:
  the signer key, a trusted entry, a name match only for a pull with no
  key, or an earlier merged pull by the same identity. A first-time author's
  run is held for approval as before.
- **Log lines** keep their C++ text ("could not publish pull request #N's
  head …", "N more open pull request(s) … still need checks").
- **Difference.** A pull the desktop lists only from the code branch
  (`archivedOnCodeBranch`) is not in the core's ledger read, so "Run checks"
  on such a pull queues nothing until it is restored into the ledger.

### Reactions to runs

- **Followed from events, not by a runner.** A run the core's queue starts
  is followed directly: `actions.started` puts its id in a set
  (`forkmesh::coreactions::followDirect`), and `actions.log`, `.step`,
  `.status`, `.release`, `.finished` and `.interrupted` for that id call the
  same `MainWindow` slots the runner signals called, in the same order
  (`onRunStatusChanged` then `onRunFinished`, then
  `onReleaseMetadataLanded`). These events are handled even before the
  queue attach completes. Notifications and pings ("Action started",
  "Action failed", approval requests) stay desktop subscribers to these
  events. The finish is acknowledged at once; a finish already handled is
  never replayed by the reconcile. A lost core gets the link's 20 s grace,
  then `onRunInterrupted`. Runs a native desktop runner started through
  `core.actions.start` (an older core without the queue) keep the runner
  link.
- **Release landing in the core.** When a run lands release metadata, the
  core fetches the working copy into the served mirror
  (`+refs/heads/*`, `+refs/tags/*`, the C++ conditions: a mirror path that
  exists and differs from the working copy) before it reports the finish.
  `actions.finished` and the finished record carry `served: {ok, error}`
  (`skipped` when the conditions do not hold), and `onReleaseMetadataLanded`
  uses that answer instead of its own `git fetch` on the GUI thread. The
  CAS seeding, managed-host fan-out, rollout and restart that follow still
  run in the desktop; they depend on the mirror push engine (see Hosts).
- **Agent auto-fix.** For a failed run the queue started, the core decides
  (`agents/autoFixFailures`, a branch ref), builds the prompt (the run log's
  last 12 000 characters and its path, the C++ text) and asks
  `ui.actions.autoFix {run, owner, name, branch, workflow, prompt}`. The
  desktop picks the session the C++ picked (the last one on that branch, not
  external), skips it while it runs, queues or waits, sends the prompt with
  `sendPromptToAgentSession` and flashes the same message. The core has no
  API yet to prompt a session (the session model is still in the desktop),
  hence the `ui.*` request. Without a desktop the request waits in the core
  (up to 32) and goes out at the next queue attach. `actions.finished`
  carries `autoFix: true`, so `maybeAutoFixFailedRun` does not send it a
  second time.

### Mirror Actions configuration

`mirrorconfig.go` ports `MirrorActionsConfiguration`: request validation, the
node check, the gateway file (`FORKMESH_MIRROR_REFRESH_CONFIG`, then
`/var/lib/forkmesh-mirror/gateway/mirror-refresh.json`), the action mirror
(`git clone --mirror` or `fetch --prune`), the repository merge, the journal
(same file name, schema 2, the `sha256` over Qt compact JSON, so either side
recovers the other's journal), a `QLockFile`-compatible lock (pid, name and
host; stale after 30 s or when the holder is gone), rollback and the exact
result codes. Settings are written through `core.WriteSettings` and checked by
re-reading `ForkMesh.conf`, comparing values as the INI file stores them.
Methods: `core.actions.mirrorConfig.apply` (`{request}` or the request
itself), `.recover`, `.reset`. At the first attach the core rolls back an
interrupted journal and suspends external Actions if it cannot. `apply` and
`recover` answer `desktop_unavailable` at once when no desktop is attached
(the core writes settings only through it), before taking the lock.

- **The helper on mirror hosts.** `forkmesh --configure-mirror-actions-stdin`
  (what `ControlNode` runs over SSH) installs a delegate
  (`CoreMirrorActions.cpp`) before it reads the request. When a core runs on
  the host and is not switched off, the request goes to
  `core.actions.mirrorConfig.apply` and the core's result is printed in the
  same `FORKMESH_ACTIONS_RESULT=` form. The C++ transaction runs as before
  when the core is absent, has no such route, or answers
  `desktop_unavailable`. A request the core took but did not answer reports
  `configuration_core_failed` rather than running a second transaction.
- **Variables sealed in the vault** (`mirrorvault.go`). With the vault module
  present, pushed variables go to the vault's `actions` namespace and
  `actions/variables` in `ForkMesh.conf` gets only the marker map. The
  transaction stays atomic across both stores:
  1. under the lock, the vault write copies the previous effective values
     (vault values with newer plaintext overlaid) to `mirror-rollback/*`,
     replaces `actions/*` and records `mirror-txn/<requestId>` with the
     previous generation, all in one vault write;
  2. the journal is written with `actions/variables` as the marker map of
     the previous names, so it never holds a value;
  3. the settings are written; on success the journal goes, then the
     rollback copy.
  A failed write rolls back both. A transaction found with no journal is
  settled by the generation in `ForkMesh.conf`: equal to its request id
  means it committed (the copy is dropped), equal to the previous generation
  means the settings were rolled back (the vault is restored), anything else
  keeps the vault as it is. The core settles that at start, at every attach
  and before every apply, so it also cleans up after the C++ startup
  recovery. A vault that cannot be opened refuses pushed variables with
  `variables_vault_unavailable` and changes nothing; requests without
  variables need no vault. `vault.changed` tells the desktop to drop its
  cache.
- **Without the core.** The C++ helper refuses pushed variables with
  `variables_vault_unavailable` when a vault file exists on the host
  (the desktop's rule: no plaintext once the vault exists); with no vault
  yet it writes them as before and the first core run migrates them.

### Still in the desktop

- **Desktop reactions:** the notifications and pings themselves, the release
  CAS seeding, managed-host fan-out, rollout and restart after the served
  landing, `propagateRepoUpdate` for `*.commit` spool files, push hooks,
  `syncMirrorActionsConfiguration` (it reloads the repository list; the
  generation reaches the core in the snapshot).
- **The settings half of startup recovery** in `main.cpp`. It has to roll
  back an interrupted journal before `MainWindow` reads the repository list,
  and at that point no desktop is attached, so a core could not write the
  settings. The core settles the vault half itself (see above) and rolls back
  a journal it finds at the first attach.
- **`ActionTelemetry`.** This is the desktop's general background-work
  ledger, not part of the runner. The core logs runner failures to its own log
  instead.

### Deliberate differences

- **Failed start.** A child that fails to start ends its phase at once
  (`!! ⚠️  Process failed to start: …`). The C++ waited for the step or job
  timeout.
- **Ungraceful upgrade.** `CoreClient` restarts an older core even while it
  is busy, so a rebuild that changes the core binary interrupts running
  Actions, and they requeue. Deferring that upgrade while `core.status` lists
  `actions` as busy would keep them running.

### Tests

- **Parser and store:** `workflow_test.go`, `store` cases inside the parity
  test.
- **Runner** (`runner_test.go`): real temp git repositories and fake
  `systemd-run`, `bwrap` and `systemctl` on `PATH`. The fake `bwrap` maps the
  bind mounts to host paths, so nothing touches `/home/forkmesh` or user data.
  Covered: success with artifacts, secrets and env policy; step failure; stop;
  step timeout; digest mismatch; missing mirror; `checkout: false`; shutdown
  interrupt; the output cap; the build cache lock; release landing and its
  commit; cross-process sandbox claims.
- **Module** (`module_test.go`): `core.actions.*`, events, finished
  bookkeeping, `Stop()`, and the four phone methods with their errors.
- **Queue** (`queue_test.go`): real temp repositories, the fake sandbox, and a
  fake settings writer that rewrites a temp `ForkMesh.conf`. A spooled push
  runs an auto-approved workflow and writes its approval; a manual run waits
  for approval, a second one supersedes and deletes it, approve and rerun
  run; attach requeues an interrupted run (`RESTARTING`) and leaves a native
  one alone; `needs` orders Deploy after CI and a pinned workflow is skipped;
  phone intents, the owner gate and `repos.list` (snapshot and settings
  fallback) are native.
- **Mirror configuration** (`mirrorconfig_test.go`): apply through the
  writer, rollback after a failed write, journal recovery, tamper detection,
  a busy lock, reset; the summary and state files and their ownership policy.
  `mirrorvault_test.go`: variables sealed (no value in `ForkMesh.conf`, the
  journal or the vault file), the vault rolled back with the settings, a
  transaction settled by the generation either way, a locked vault refusing
  variables but not an apply without them, `desktop_unavailable` without a
  desktop and `forbidden` for phones.
- **Pull-request checks** (`pullchecks_test.go`): a real ledger checkout with
  a branch-backed pull from a stranger, a signed-patch pull from a trusted
  key and a closed pull; both heads published into the mirror (the patch
  replayed), runs created with the pull context and the first-time-author
  hold, the rescan floor, a forced manual pass, `unknown_repo`, `forbidden`
  for phones; the trust rules, candidate head parsing and the bad-object
  parser.
- **Reactions** (`reactions_test.go`): a release run's metadata reaches the
  served mirror in the core and `served` is in the event and the finished
  record; a failed run's auto-fix waits without a desktop and goes out at the
  next attach with the C++ prompt; the prompt's tail cut.
- **Parity** (`parity_test.go`, opt-in through `FORKMESH_ACTIONS_PARITY_BIN`).
  It drives a small harness linked against `ActionFile.cpp`,
  `ActionStore.cpp` and `LocalControlRepos.cpp`. It compares 100 000 random
  workflows, wildcard matches, `substitute`, labels, `meta.json` bytes,
  repository keys, log slices and confirmation summaries. All matched on
  2026-10-02.

## Merge queue and pull store

The merge queue's phone family and its executor run in the core when the
desktop is attached. The git work of a pass (behind-base checks, updates from
the base, merge checks, the merge itself) used to run on the GUI thread; it
now runs on the core's git queue, the same per-repository lane as
`core.git.run`.

### `internal/pulls`

The part of `PullStore` the queue needs, ported line for line:

- **Read path.** `canWrite` (a checkout with `.git` and a valid node
  identity), `metaWorkTree` (the `data/pull-meta/<hash>` ledger checkout on
  `forkmesh/pulls`), `loadAll`, front matter and event files, the isolated
  `forkmesh/pull/<n>` metadata branches, the mirror fallback, and the review
  gate (`independentReviewGateSatisfied`, `hasIndependentChangesRequested`).
- **The queue's steps.** `isBranchBehindBase`, `updateBranchFromBase`,
  `checkMergeable`, `conflictMergeInProgress`, `mergePull` with `writePull`,
  the ledger commit, `materializePullRef`, `materializePullMetadataRef`,
  `PullReward::advanceForMerge`, the stale `git am` heal and the same error
  texts. `pull.md` bytes match the desktop's (`TestWritePullBytesMatchTheDesktop`).
- **Nothing is signed here.** A merge changes only the status, merge and
  reward fields, which sit outside the contributor's signature, so the core
  never needs the private key.
- **Deletes.** Scratch worktrees and a stale `rebase-apply` folder go through
  `internal/safedelete`; the ledger checkout is re-linked only through
  `RemoveOwnWorktreeUnder(data/pull-meta, …)`.
- **The payload history rewrite stays in the desktop.** After the ledger
  commit, `mergePull` drops `changes.patch` and `commits.mbox` of the merged
  pull from the ledger's history (`git filter-branch`). That rewrite is
  history-destroying git, so the core asks the desktop:
  `ui.mergequeue.purge {owner, name, number}` runs
  `PullStore::purgeMergedPullPayload` on a worker thread. As in the C++, a
  failed or skipped rewrite does not fail the merge; the core logs it.

### `internal/mergequeue`

- **Phone methods.** `mergequeue.list`, `mergequeue.prepare` and
  `mergequeue.run` with the C++ policy (`LocalControlMergeQueue.cpp`):
  validation, confirmation targets, summaries, `precheck`, `applyToQueue`,
  result messages, `repoRow`, the owner gate, `phonefamily` tokens, the
  "Merge queue … from …" and "Refused a merge queue …" lines through
  `core.LogUI`, and `mergequeue.changed` to the requesting connection after
  the reply. They return `rpc.ErrDelegate` until the desktop has pushed
  `core.mergequeue.view` once, so an older desktop keeps answering.
- **Desktop state the core asks for.** The repository records (queue rows,
  on/off, paused, `requirePeerApproval`, preview-only, active relay, the
  writable checkout and mirror) live in the desktop's `m_repositories`, and
  `saveRepositories` rewrites the whole list, so the core never writes them
  with `core.WriteSettings`. It asks `ui.mergequeue.view` for a fresh copy on
  every request and before every queue entry, which also brings the UI-only
  facts: which repository page is open, whether the identity loads, the
  native pass flag (`m_mergeQueueBusy`), `m_aiFix`, `m_pullDeleteInProgress`
  and the live conflict-merge session of the open checkout.
- **Writes.** `ui.mergequeue.apply {owner, name, rows, expect}` replaces the
  rows through `saveMergeQueue` only if they still equal `expect`; otherwise
  it answers `conflict` and the core re-reads and retries (three times, then
  `busy`). `{enabled}` and `{paused}` go through `setRepoMergeQueueEnabled`
  and `setMergeQueuePaused` for the open repository (log line, panel, the
  settings check box, a new pass) and straight to the record otherwise, as
  the C++ phone handler did. Without a desktop a write answers
  `desktop_unavailable`; `list` still answers from the last view, with
  `open: false`.
- **Titles and pull status.** Read from disk by `internal/pulls` for the open
  repository only, as the C++ took them from `m_currentPulls` only there.
- **The executor.** `processMergeQueue` ported: the 1.5 s and 20 s timers
  (`core.mergequeue.schedule {delayMs}` keeps the earlier deadline), only the
  open repository, the busy and conflict-merge waits, dropped missing or
  decided pulls, the peer-review gate, update from the base, conflicts,
  merge, one merge per pass, and every state detail and log line. A merged
  pass sends `core.mergequeue.merged {owner, name, number, pull}`, any other
  pass `core.mergequeue.state {owner, name, pending}`. `core.mergequeue.state`
  as a request returns `{busy, configured, pendingMs}`. `Busy()` holds the
  core up while a pass runs.

### Qt facade (`MainWindowCoreMergeQueue.cpp`)

- `scheduleMergeQueueRun` and `processMergeQueue` hand off to
  `core.mergequeue.schedule` when the core lists the route (and stop the
  native timer); otherwise the C++ executor runs unchanged.
- On connect the desktop pushes `core.mergequeue.view` and, when the open
  repository has a queue, asks for a pass.
- `core.mergequeue.merged` runs what the C++ ran after a queue merge:
  "Merge queue merged pull request #N." and `afterPullMerged` with the pull
  from `m_currentPulls` (the event's copy when it is not there).
  `core.mergequeue.state` refreshes the panel.
- `PullStore` gained `conflictSessionLive` (the live `git am` session set)
  and `purgeMergedPullPayload` (the private history rewrite, made callable).

### Differences and gaps

- If the user leaves the repository page in the moment between the merge and
  the event, the desktop logs "Merged pull request #N." but skips
  `afterPullMerged` (linked issues, bounties, source control refresh, sync on
  merge), because those act on the open page.
- Everything else `PullStore` does now runs in the core as well; see
  [Pull store](#pull-store).

### Tests

- `internal/pulls`: `pull.md` bytes, front matter rules, the review gate, the
  reward advance, the ledger checkout path, metadata branch precedence,
  behind-then-update-then-merge, conflicts, the review gate refusing a merge,
  a patch-backed merge, the conflict-merge session, the synthetic mbox date.
- `internal/mergequeue`: delegation before the first view, the owner gate,
  `forbidden` for phones on `core.*`, a golden `list`, enqueue, move,
  remove and clear with exact messages and rows, every `run` error, writes
  without a desktop, and the executor with real git in temp repositories
  (merge with the purge request, review block, update failure, dropped
  pulls, the 20 s wait while the desktop is busy).

## Pull store

Round 4 moved the rest of `desktop/src/PullStore.cpp` into `internal/pulls`,
so the repository page's pull list, every pull write and the relay inbox
intake run in the core when the desktop is attached. The file formats, git
refs, commit messages, error texts and signatures are the C++ ones, line for
line.

### What runs in the core

- **Reads.** `loadAll` (with the one-time `forkmesh/pull/<n>` migration, the
  stale-branch drop, the per-checkout metadata heal and the stamp cache),
  `loadAllIncludingArchived` (code-branch records and the oldest
  `.forkmesh/pulls/open/*.json` layout), `readBranchReviewEvents`,
  `isBranchBehindBase`, `checkMergeable`.
- **Signed writes.** Comments, reviews, line comments, review threads with
  their patchset anchors, thread replies and states, suggestion states
  (`forkmesh-pull-comment`), branch review threads (`forkmesh-branch-comment`)
  and new pulls (`forkmesh-pull-event`, including the branch-backed creation
  OIDs). The core signs with the node identity (`data/identity/ed25519.pem`).
  The desktop sends its own public key with every request. If the core's
  key differs, the core reloads the key file once and otherwise answers
  `identity_mismatch`; the desktop then signs natively, so a key imported a
  moment ago can never sign under the old one.
- **Other writes.** `createPull` (numbering over archived records), `setStatus`,
  `recordRewardPayment`, `restoreArchivedPull`, `updateBranchFromBase`,
  interactive `mergePull` (with the owner's review-gate waiver and its ledger
  note), `deletePull`, `deletePulls`, `deletePullFile` (patch and mbox surgery),
  `ensureLocalBranch`, `ensurePullHeadRef`, `reconcileMergedPulls`,
  `prefetchForeignLedger` and `adoptForeignPulls` (with the desktop's
  environment, so the SSH agent is the desktop's).
- **Inbox intake.** `applyRemotePull`, `applyRemoteEvent` and
  `applyRemoteBranchEvent`. The network drain and the acknowledgement stay in
  `drainPullsInboxFor`, which is asynchronous and does not block the window.
  The relay verifies submissions; the store does not, which matches the C++.
- **Sessions.** Conflict resolution in the checkout (`git am` on a pull
  branch), the agent conflict worktree, the agent edit worktree and the file
  editor. The core keeps one session per checkout. The live `git am` set is
  core state (`pulls.SessionLive`), and the merge queue's stale-`am` heal reads
  it. A session belongs to the desktop connection that started it. When that
  desktop goes away, the session is dropped and the next merge heals the
  stale `am` state, as a restarted desktop always did.
- **Review checkouts.** `checkoutForReview` and `discardReviewCheckout`.
- **Ledger lock.** The core answers requests concurrently, so every ledger
  read-modify-commit section (event numbering, pull numbering with intake
  deduplication, status, reward, delete, merge recording, adopt, finalize)
  holds a per-checkout lock. The lock is never held across a call to the
  desktop, so a pumped GUI-thread call cannot deadlock against it.

### The history rewrite stays in the desktop

Closing a pull, deleting with history, any status other than `open`, and a
merge all drop pull files from the ledger's history: `git filter-branch` on
the ledger refs, then `git reflog expire --expire=now --all` and
`git gc --prune=now`. These last two steps are not scoped to the ledger. The
ledger checkout is a linked worktree of the user's repository and shares its
object store and reflogs, so they expire every reflog of the user's
repository and prune every unreachable object, including dangling stashes
and commits that are only recoverable through a reflog. Because that cannot
be proven to touch only pull data, the core does the ledger commit and the
ref teardown, then asks the desktop with
`ui.pulls.rewrite {workTree, mirror, number, paths}`. The desktop accepts only
`.forkmesh/pulls/<n>`, `…/changes.patch` and `…/commits.mbox`, works out the
ledger checkout itself, and runs the existing C++ rewrite on a worker thread.
A failed rewrite fails `setStatus` and a delete with history, as in the C++.
After a merge it is only logged. Without a desktop the answer is "no desktop
to run it".

### Desktop methods (`core.pulls.*`, `forbidden` for phones)

Every request carries `store {workTree, mirror, authorName, repoKey,
identityKey, identityValid, nativeAmLive, env}`. Every reply is
`{ok, error, …}` with the C++ error text.

| Method | C++ |
| --- | --- |
| `load` (`stamp` in, `unchanged` out), `loadArchived` | `loadAll`, `loadAllIncludingArchived` |
| `create`, `setStatus`, `recordReward`, `restoreArchived` | the same names |
| `behind`, `update`, `merge`, `checkMergeable` | `isBranchBehindBase`, `updateBranchFromBase`, `mergePull`, `checkMergeable` |
| `event`, `branchEvent`, `branchEvents` | `addComment` … `setSuggestionState`, `addBranchThread*`, `readBranchReviewEvents` |
| `applyPull`, `applyEvent`, `applyBranchEvent` | the relay inbox intake |
| `delete`, `deleteMany`, `deleteFile` | `deletePull`, `deletePulls`, `deletePullFile` |
| `prefetchForeign`, `adoptForeign`, `reconcile` | the pull-ledger sweep |
| `ensureBranch`, `ensureHeadRef` | `ensureLocalBranch`, `ensurePullHeadRef` |
| `reviewCheckout`, `discardReview` | `checkoutForReview`, `discardReviewCheckout` |
| `session.start/finish/abort/state` | conflict, agent-conflict, file-edit and agent-edit sessions |

After a successful write the core sends `core.pulls.changed {workTree, op,
numbers}` to the desktop. The desktop already reloads after its own writes, so
it does not react to the event yet. The event is there so that writes made in
the core on behalf of others can refresh an open page.

### Qt facade

- `PullStore` keeps its API. The routed methods start with a two-line prologue
  that calls `forkmesh::pullstorecore` (`PullStoreCore.{h,cpp}`, also linked
  into the test targets). That code goes through a backend interface, and
  the app installs `CorePulls.cpp` as the backend from
  `registerCoreMergeQueueHandlers`. Without a backend (tests,
  `FORKMESH_CORE=0`) or without the route, the native C++ runs unchanged.
- Calls use `coregit::callCore`. On the GUI thread the call runs on a helper
  thread while the window keeps pumping events, as `runGit` always did.
- **Fallback.** A request that was never sent, or that the core declines
  (`identity_mismatch`, `forbidden`, `unsupported`), runs natively. A write
  that was sent but got no answer (`core_gone`, `core_timeout`) fails with
  "forkmesh-core did not finish …" and is never run a second time. A read
  falls back to native.
- **Session objects.** An object that started a session natively keeps it
  native until it finishes. An object with no native state routes, so a
  core-held session is finished or aborted through any object for that
  checkout.
- **Pull list cache.** The desktop keeps the core's last load stamp for each
  checkout and sends it back. While the ledger and refs have not changed, the
  core answers `unchanged`, so repeated `loadAll` calls do not resend the
  patches.
- These stay native: the signing helpers (`makeSigned*`, `canonicalString`),
  `loadAllStrict*` (`StrictGitReader` limits for contribution checks),
  `metaWorkTree`, `baseTip`, `canWrite` and `purgeMergedPullPayload`.

### Phone methods

No phone family touches pulls directly. The merge queue (`mergequeue.*`)
was already native. Phones post review threads through the relay inbox, which
now lands through the core. The pull part of `agentops` deletes belongs to the
agent runtime.

### Tests

- Golden canonical strings. Every signature kind is verified by the relay's
  own verifier: `app/src/events.py` loaded the way the app tests load it,
  with a tampered event that must fail.
- With real temp repositories: event files byte for byte and commit messages
  for every event type, remote events, branch threads (with dedupe and a
  mirror read), branch-backed creation and numbering over archived and legacy
  JSON records, restore, intake dedupe and renumbering, status, delete and
  bulk delete with the rewrite requests, rewards, reconcile with the reward
  advance, adopting a mirror-drained ledger, `ensureLocalBranch` (local,
  mirror, rebuilt from a pull, missing), conflict sessions (resolve, markers,
  abort, a live session blocking a merge), the agent worktree and file edits,
  `deletePullFile`, review checkout and discard, the owner's waiver, the load
  migration and stamp, and the JSON round trip with Qt's conversions.
- The module: `forbidden` for phones, `identity_mismatch`, create, review, load
  and branch events through the handlers with `core.pulls.changed`, the
  `ui.pulls.rewrite` request and its error, sessions dropped with their
  desktop, and `unchanged` for a settled stamp.

## System log digest and repository quality

Two `stats.*` phone methods answered by the core: `stats.timeline`
(`internal/netlog`) and `stats.quality` (`internal/quality`). Both keep the
family's owner gate (`internal/phonefamily`: `not_owner` unless the device
has a key and its user is the node owner or the account).

### `stats.timeline`: the system-log digest

- **What the desktop keeps.** Every `logSystem` line is appended to
  `data/network_log.txt` as `yyyy-MM-dd HH:mm:ss  <message>[  [file:line|INFO|ERROR]]`.
  Past 40 000 lines the file is renamed into
  `network_log.d/netlog-<first>-<last>[-n].txt` (30-day retention, 512 MB
  backstop). The 7- and 30-day charts read a digest of 15-minute buckets
  (bucket start, badge, count), kept in memory and saved to
  `network_log.d/digest.tsv` at each quarter-hour rollover, after a rotation,
  after deletes and on quit. On start the desktop loads that file, or rebuilds
  it from the newest 64 MB of log files when it is missing.
- **Single writer.** The desktop stays the only writer of `digest.tsv` and the
  log files. The core keeps no state file: everything it needs is on disk.
- **How the core counts.** The saved digest is complete for every bucket
  before its newest one (the watermark W), because a save happens when a new
  bucket starts. So the core takes buckets older than W from `digest.tsv` and
  counts W and later from the log itself: the live file read incrementally
  (offset, file identity and a 64-byte probe before the offset, so a
  rotation, a truncation by Clear or a rewrite by a deleted line restarts the
  count) plus the rotated segments whose name says they reach W. Without a
  `digest.tsv` the core counts the way `rebuildDigest` does (live file first,
  then segments newest first, until 64 MB are spent). Buckets older than 30
  days are dropped. When the desktop saves a new digest, W moves forward and
  the older buckets come from the desktop's numbers again.
- **Badges.** `netlog.Badge` is `logBadgeFor` ported rule for rule: the
  `|INFO`/`|ERROR` source suffix, the fleet `Node …` prefix
  (`corelogs::nodeLineBadge`), the stall, cloud, merge, push and background
  lines, the `[body: …]` and address stripping, cleared tallies (`0 failed`),
  the error words, the route crumb of `net` lines, then the prefix and
  whole-word tables. `stats.timeline` only needs the severity
  (`severityForBadge`: `ERROR`/`CRASH`/`FATAL` are errors,
  `STALL`/`BGBLOCK`/`WARN`/`WARNING` warnings, the rest info).
- **`core.log.append`** (desktop only, `forbidden` for phones):
  `{lines:[{line, badge}]}`. `CoreStats.cpp` sends it from `logSystemFrom`,
  right where the desktop counts the line into its own digest, batched every
  50 ms (at most 256 lines a batch). It carries the desktop's badge, which the
  file cannot: a fleet or Worker line logged with a badge override (`CLOUD`,
  `DB`, `MIRROR`) would otherwise count as `ERROR` when it contains "failed".
  Nothing is counted twice: a line announced before the writer thread has
  appended it waits in a pending list and is matched (by its first line) when
  the file shows it; a line already read is claimed and only its badge is
  corrected. Unmatched announcements expire after 30 s.
- **Reply.** Byte for byte `statsctl::timeline`: `{days:[{dayTs, errors,
  warnings, info}]}`, local-midnight days, `days` clamped to 1 to 30 (30 when
  it is not a number).

Known differences, all transient or edge cases:

- After a desktop crash the lines logged since the last digest save are
  missing from the desktop's digest for good, while the core still counts
  them from the file until the next save moves W past them.
- A single deleted line older than W leaves the desktop's in-memory digest at
  once, but `digest.tsv` only at the next save. Deleting a whole category
  saves at once.
- The crash handler appends diagnostic lines that the desktop's live digest
  never counted; the core counts them when they fall at or after W (the
  desktop's own rebuild counts them too).
- Local times are parsed in the core's time zone, the same as the desktop's;
  an hour repeated by a daylight-saving change may land in the other bucket.

### `stats.quality`: `RepoQuality::scan`

- **Repository facts.** `ui.actions.repo` (the C++ owner gate, slug and
  `repoIndexFor`, so preview-only records, the active relay and materialized
  private mirrors are honoured) now also returns `writableLocalPath` and
  `writableMirrorPath` from `writableRecordFor`. The core uses the writable
  checkout, and the writable mirror or else the selected record's, exactly as
  `answerLocalControlStatsQuality` does. Errors: `bad_request` (slug),
  `unknown_repo`, then `busy` (one scan per `owner/name` in flight), and
  `desktop_unavailable` without a desktop, which is also the proxied answer.
- **Inputs read in the core.** The Actions runs come from the run store on
  disk (`actions.NewStore(data/actions).LoadAllRuns()`, exact owner and name,
  newest first). Issues come from the `IssueStore::loadAll` read path: the
  `open/`, `closed/` and legacy folders of the checkout when the node identity
  loads and the checkout has `.git` (`canWrite`), else `ls-tree` and
  `cat-file --batch` on the mirror's `HEAD` (or first branch); the folder wins
  over the record's status, and an unreadable record counts as open. The
  core never runs `migrateLegacyLayout` (a write); reading the legacy folder
  gives the same statuses.
- **The scan.** The same `git ls-files` (checkout) or `ls-tree` plus
  `cat-file` batches (mirror), the 1 MiB and NUL-byte filters, root docs,
  source and test path rules, `TODO|FIXME|HACK|XXX` markers (first 120 as
  findings), long files (over 1200 lines, warning over 3000), the
  `rev-list --count --since="30 days ago"` activity check and open issues.
  Only what the phone reply uses is computed: severities, paths and titles.
  Workflows, the detail texts and the security flags of `RepoSecurityInput`
  do not reach the reply, so the core does not load them.
- **Reply.** `statsctl::qualityReport` with `relativeFindingPath`: score,
  grade, `computedAt` (scan start), `findingCount` and up to 200 files of up
  to 10 notes, sorted the C++ way.
- **Runs the desktop holds that the store lacks.** `m_actionRuns` is reloaded
  from the same store. New runs go through `ActionStore::createRun`, which
  writes them at once, and both the core runner and the desktop's native
  fallback runner save the record when a run starts and when it ends. So the
  only difference is a status the desktop has changed in memory and not yet
  saved, for a moment; the report only looks at the status class (failed or
  rejected, active, passed).

### `stats.sentry` stays with the desktop

It answers from `remoteSnapshot()` of the Sentry package's desktop extension
(`m_desktopPackageExtensions["sentry"]`), an in-memory object the package's
own client fills. Nothing in the core can see it, so it stays proxied until
that package's client runs in the core.

Re-checked in round 4 (2026-10-04): there is no cheap way to hand the
snapshot to the core.

- **No change hook in the contract.** `DesktopPackageExtension` and
  `DesktopPackageContext` carry no "snapshot changed" callback. Only the
  package's own `SentryClient::issuesChanged`/`statusChanged` signals know,
  and the desktop cannot reach them. A push to the core needs a new callback
  in the shared contract (every in-tree package rebuilds) and a new
  `marketplace/packages/sentry` version (1.0.1 today) with its review and
  publish flow.
- **The data dies with the desktop anyway.** The client, its auth token
  (`market/packages/sentry`) and its polling live in the desktop process, and
  the C++ answers `sentry_not_connected` as soon as the client is inactive. A
  core copy could only be answered while a desktop is attached, which is
  exactly when the proxy already works.
- **The proxy is cheap.** One in-memory conversion of at most 100 issues on
  the GUI thread, no I/O. Moving it saves nothing measurable.

It moves when the Sentry client itself moves into the core (a Go port of
`SentryClient` behind the package, with the token in the vault).

### Tests

- `internal/netlog`: the classifier on 38 lines covering every rule family,
  `splitLogSource`, digest parsing, the watermark split, incremental reads,
  partial lines, rotation, rewrite and truncation, the digest save moving W,
  the rebuild without a digest, `core.log.append` before and after the write,
  multi-line entries, badge overrides surviving a rewrite, pending expiry, a
  golden timeline and the module's gate.
- `internal/quality`: a real checkout and its bare mirror (README, long
  files, markers, a binary file, a test file, issues in all three layouts)
  giving the same golden report from both, an empty repository, path and
  text rules, and the module's errors, writable-path handling and `busy`.

## Secrets at rest (the vault)

Security is the first pillar: no plaintext secret on disk, least privilege,
never a value in a log. Until 2026-10-04 the Actions variables (admin
passwords, the fleet `DATA_KEY`, Cloudflare, Vultr, Codemagic, Discord and
Mailtrap tokens, signing keys) sat in plaintext JSON under
`actions/variables` in `ForkMesh.conf`. They now live in a vault owned by the
core, `internal/vault`.

### Key

- **A random data key, wrapped by the node identity.** Every value is sealed
  with one random 256-bit data key. The data key is stored only wrapped
  (AES-256-GCM) under a key-encryption key derived from the node identity:
  `HKDF-SHA256(ikm = Ed25519 signature of "forkmesh-vault-kek-v1\n" + public
  key, salt = 32 random bytes per wrap, info = "forkmesh-vault-kek-v1")`.
  Ed25519 signatures are deterministic, so the same identity always derives
  the same key, and nothing derived is ever written down.
- **Why a signature, not the seed.** It is the convention the private-mirror
  vault already uses (`privateIdentityVaultSecret`), it needs only "sign" from
  the identity (no seed export), and the C++ side can derive it with OpenSSL.
- **Why a wrapped data key, not HKDF straight to the values.**
  - The identity can change (keyfile import, rotation). Re-wrapping one key is
    a single small write; re-encrypting every value would not be.
  - Several wraps can coexist (up to 8): the old identity keeps working if it
    is restored, and a future keychain or recovery wrap needs no format change.
  - The data key rotates on its own (`core.vault.rotate`).
- **Not stored next to the ciphertext.** The identity key is
  `data/identity/ed25519.pem`; the vault is `data/vault/secrets.json`;
  `ForkMesh.conf` (the config folder) holds no Actions secret any more.
- **No OS keychain.** The standard library has no keychain API. macOS
  `security` would put the key on an argv, the Linux Secret Service needs D-Bus
  bindings, and Windows DPAPI would make one platform special. The identity
  key is already the account's root of trust; a keychain adds protection only
  once the identity itself moves there, which a wrap can add later.

### File format (`forkmesh.vault`, version 1)

| Field | Meaning |
| --- | --- |
| `kind`, `v` | `forkmesh.vault`, `1`; anything else is refused (`vault_invalid`) |
| `keyId` | 16 hex: `HKDF(dataKey, info "forkmesh-vault-key-id-v1", 8 bytes)`; names the data key without revealing it |
| `generation` | Incremented on every write; inside the MAC |
| `wraps[]` | `{scheme "ed25519-sig-hkdf-sha256-v1", identity (public key), salt, nonce, key}`; AAD `"forkmesh-vault-wrap-v1\0" + identity + "\0" + keyId` |
| `entries{}` | `"<namespace>/<percent-encoded name>": {nonce, data (ciphertext and tag), at (ms)}`; key `HKDF(dataKey, info "forkmesh-vault-entry-key-v1")`; AAD `"forkmesh-vault-entry-v1\0" + keyId + "\0" + entry name`, so entries cannot be swapped |
| `mac` | HMAC-SHA256 (key `HKDF(dataKey, info "forkmesh-vault-mac-v1")`) over the version, key id, generation and every entry (sorted, length-prefixed name, nonce, data, at); a deleted, added or edited entry fails |

Names are percent-encoded UTF-8 (`QByteArray::toPercentEncoding` rules), so any
variable name is stored as printable ASCII and both sides sort it the same way.
Base64 is standard with padding. Limits: 1 MiB per value, 4096 entries,
32 MiB per file.

- **Writes:** temp file in the same folder, `0600`, `fsync`, `rename`, folder
  `fsync`; the folder is `0700`; a symlinked vault path is refused. Only the
  core writes the file.
- **Memory:** every call reads the file, unwraps the data key, opens only what
  it needs and zeroes the key and the plaintext buffers. Go strings cannot be
  wiped, so a value that crosses IPC as JSON lingers until it is collected.
  The desktop keeps the decrypted map for at most 30 s (dropped at once on
  `vault.changed` or a core reconnect, wiped best effort).
- **Identity change:** the core keeps the identity it started with in memory
  and checks `ed25519.pem` every 5 s. When it changes, the next unlock uses the
  old identity and adds a wrap for the new one (log line "the vault key is now
  wrapped for the new identity too"). If the identity is replaced while no core
  runs, the vault reports `locked`; restoring the previous key opens it again.
  Nothing is deleted or overwritten in that state.

### Migration

Runs in the core when a desktop attaches (the desktop also asks
`core.vault.migrate` on connect) and again whenever plaintext reappears:

1. Read `actions/variables`; every value that is not the marker is pending.
2. Seal the pending values into the vault in one atomic write. A vault entry
   newer than `ForkMesh.conf` (its `at` at or after the file's mtime) is kept:
   it was set through the vault after that plaintext was written.
3. Read every value back and compare.
4. If the settings changed meanwhile, start over (at most 3 times).
5. Replace `actions/variables` through `core.WriteSettings` with
   `{"NAME": "[stored in the ForkMesh vault]", …}`, one entry per vault name.
   Older desktops therefore list the names and show the marker instead of an
   empty page; they cannot use the values.
6. Log "Moved N Actions variables into the encrypted vault (path);
   ForkMesh.conf now keeps only their names" (counts only, never a value).

- **Exactly once, resumable:** without a desktop, step 5 fails with
  `desktop_unavailable`; the values are already sealed, readers already merge
  both, and the next attach finishes. A run with nothing pending writes
  nothing, and identical values are not re-sealed.
- **Reading in between (`vault.Variables`):** vault values, overlaid by any
  non-marker plaintext that is newer than its vault entry (or has none). An
  older desktop's plaintext write therefore wins until it is migrated, and a
  stale copy never beats a newer vault value.
- **No plaintext backup:** the migration writes nothing else. Plaintext that
  already left the file is out of its reach (see the audit).

### Desktop and release secrets (round 4)

Two more namespaces, same file, key and rules as `actions`:

| Namespace | Entries | Plaintext it replaces | Readers |
| --- | --- | --- | --- |
| `desktop` | `chat/e2ee/x25519PrivateV2` (the base64 text `RoomSeal` stores), `genie/token` | The same keys in `ForkMesh.conf`, which keep the marker `[stored in the ForkMesh vault]` | `RoomSeal` (C++), the core's mesh (`vault.Secret`), the Genie setup and launch, the agent redaction list |
| `release` | `apk-keystore-password`, `windows-cert-password` (file content as it was, newline included) | `data/release/.apk-keystore-password`, `.windows-cert-password`, which keep the marker and a newline | `tools/forkmesh-release-host.py` (`release_password`) |

- **Migration, `desktop`.** Same steps as `actions`: seal the non-marker,
  non-empty values unless a vault entry is newer than `ForkMesh.conf`, read
  back and compare, start over if the settings moved (3 times), then write
  the marker per key through `core.WriteSettings` (type string). Without a
  desktop it stops at `desktop_unavailable` and resumes on the next attach.
  Log: "Moved N desktop secrets (keys) into the encrypted vault (path);
  ForkMesh.conf now keeps only a marker".
- **Migration, `release`.** The core reads each password file itself (a
  regular file, no symlink, at most 64 KB, not already the marker), seals it
  unless the vault entry is newer than the file, reads it back, and only then
  replaces the file with the marker: temp file in the same folder with the
  old mode, `fsync`, a check that the file did not change meanwhile, rename,
  folder `fsync`. Nothing is deleted. It needs no desktop, and runs at most
  once a minute while a file is pending. Log: "Moved N release signing
  passwords (names) into the encrypted vault (path); <data>/release keeps
  only markers". The `.pfx` and `.jks` containers stay files.
- **Reading.** `core.vault.get {namespace: "desktop"|"release"}` (desktop
  process only) and `vault.Secret(core, namespace, name)` give the vault
  value, overlaid by non-marker plaintext that is newer than its entry (an
  older desktop's write, or a password file the operator rewrote), exactly as
  for `actions`; the next migration moves that plaintext in.
- **Qt facade (`SettingsSecretBackend.h`).** A header-only hook, so test
  binaries that link `RoomSeal.cpp` keep the plaintext behaviour. `CoreVault`
  installs it: `settingsecrets::value(settings, key)` asks `core.vault.get`
  (3 s, cached 30 s, dropped on `vault.changed`), without the core it opens
  the vault with `VaultFile` and applies the newer-plaintext rule, and with
  no vault it returns the plaintext (never the marker).
  `settingsecrets::store()` writes `core.vault.set` (or `core.vault.delete`
  for an empty value) and then the marker (or the empty string) through
  `QSettings`.
- **Without the core.** A vault present means writes are refused with a
  system-log line, as for Actions variables ("The Genie credential was not
  saved…" is flashed). One deliberate exception: a newly generated chat
  device key is written to `ForkMesh.conf` in plaintext, logged, and moved
  into the vault by the next core run. Refusing it would give the desktop a
  new X25519 identity on every start with `FORKMESH_CORE=0`, and peers could
  no longer seal to it.
- **The core's mesh** reads the key from the vault when the desktop's
  `sealKey` parameter is not a key (it sends the raw setting, which is the
  marker after the migration).
- **Python reader (`tools/forkmesh_desktop_vault.py`).** The release host
  is a systemd service with no desktop, so it cannot use `core.vault.get`. The
  module reads the vault with the `cryptography` package (Debian
  `python3-cryptography`) using the same derivation, wrap and entry AAD, MAC
  and percent-decoded names as `VaultFile.cpp`; it never writes.
  `release_secret(path, name, data)` returns the file's text unless it is the
  marker, then the vault entry. The release host maps a missing package or an
  unreadable vault to a `ConfigError` naming the error code, never the value.
  `actions_variable()` and the release signing key scan also follow the
  marker into the `actions` namespace now (they had been reading the marker
  text since the round-3 migration), and so does `tools/next_codemagic.py`
  for `CODEMAGIC_API_TOKEN`.
- **Threat model.** As above: a copy of `ForkMesh.conf` or of
  `data/release` alone no longer holds these secrets; anything that reads the
  identity key can still open them (the Python reader proves that on
  purpose).
- **Older builds.** An older desktop reads the marker as an invalid device
  key and generates a new one (written in plaintext, then migrated); an
  older release host would use the marker text as the password and fail to
  sign. Both ship from the same checkout as this change.

### Methods (`core.vault.*`, never for phones or the relay)

| Method | Who | Does |
| --- | --- | --- |
| `core.vault.status` | UI, local CLI | `{vault:{state, path, keyId, generation, count, identities}, migration:{state, pending, error}}`; states `empty`, `ready`, `locked`, `tampered`, `invalid`, `no_identity` |
| `core.vault.list` | UI, local CLI | `{namespace}` → names |
| `core.vault.migrate`, `core.vault.rotate` | UI, local CLI | Run the migration; new data key with every entry re-sealed and wraps for the current (and start) identity |
| `core.vault.get` | Desktop process only | `{namespace, names?}` → `{values}` |
| `core.vault.set` | Desktop process only | `{namespace, values, delete?, replace?}` → `{ok, changed}`; unchanged values are skipped |
| `core.vault.delete` | Desktop process only | `{namespace, names}` |

- Errors: `forbidden`, `bad_request`, `invalid_name`, `value_too_large`,
  `vault_locked`, `vault_tampered`, `vault_invalid`, `no_identity`,
  `vault_busy`, `vault_io`.
- **Desktop process only:** the UI connection, or a `cli` connection (the
  desktop's worker client `forkmesh::coregit::callCore`) whose kernel-verified
  peer pid (`SO_PEERCRED`, `LOCAL_PEERPID`; `ipc.Conn.PeerPID`, added for this)
  equals the UI connection's. `forkmesh-core call core.vault.get` from a shell
  or an agent is refused. Windows has no peer credentials on loopback TCP, so
  there the declared pids must match.
- Event `vault.changed {namespace, names}` to the desktop: names only.
- Go API for modules: `vault.Variables(core)`, `vault.Variable(core, name)`,
  `vault.ActionVariablesOr(settings)` (for code without a core handle).
  `secrets.*`, `stats.codemagic`, `diag.buildMac`, `hosts.options`, the hosts
  jobs (`hosts/env.go`) and `actions.Variables` (the core's Actions queue) use
  it.

### Qt facade

- `ActionStore::variables()` and `setVariables()` are the only C++ entry
  points (about 15 files call them), so the facade sits behind them:
  `ActionVariablesBackend.h` declares a reader/writer hook that
  `ActionStore.cpp` calls, and `CoreVault.cpp` installs it at startup (also
  with the core switched off). Test binaries that link `ActionStore.cpp`
  alone keep the plaintext behaviour and drop markers.
- **Core attached:** reads `core.vault.get` (3 s timeout, cached 30 s), writes
  `core.vault.set {replace: true}` and then writes the marker map to
  `QSettings` itself (the desktop is the settings writer), so no plaintext is
  written again.
- **Core absent, vault present:** read-only. `VaultFile.cpp` opens the vault
  with OpenSSL (same derivation, MAC and AAD; an opt-in Go test checks it
  against the Go writer) and overlays newer plaintext. Writes are refused with
  the system-log line "Actions variables were not saved: they live in the
  encrypted vault and forkmesh-core is not running; ForkMesh does not write
  them to ForkMesh.conf in plaintext again", and `secrets.set/delete` answer
  `vault_unavailable`. A vault that cannot be opened logs once and leaves
  only the not yet migrated plaintext.
- **Core absent, no vault yet:** the plaintext behaviour is unchanged; the
  first core run migrates it.

### Threat model

Protects against: a copy of `ForkMesh.conf` or of the config folder (bug
reports, config sync, the minimal backup's plain archive, an agent reading
settings), an edited or truncated vault (tamper detection), swapped entries,
a different identity, and casual `forkmesh-core call` access from shells and
agents.

Does not protect against: anything running as the user that reads
`data/identity/ed25519.pem` (the identity is the root key, as for the
private-mirror vault); a copy of the whole `data/` folder, which holds both
the identity and the vault (the in-app snapshots under `data/backups` do); a
process that takes over the desktop's UI connection with the endpoint token;
rollback of the whole file to an older complete copy; values in freed Go or
Qt memory; plaintext written before the migration (old disk blocks, existing
backups).

### Audit (2026-10-04, names only)

Scanned with scripts that print key names and lengths, never values.

| Where | Secret names | Home now |
| --- | --- | --- |
| `ForkMesh.conf` `actions/variables` (28 names) | `ADMIN_PASS`, `CLOUDFLARE_API_TOKEN`, `CODEMAGIC_API_TOKEN`, `DATA_KEY`, `DISCORD_BOT_TOKEN`, `DISCORD_CLIENT_SECRET`, `FORKMESH_RELEASE_SIGNING_KEY_PEM`, `FORKMESH_WIN_CERT_PASSWORD`, `FORKMESH_WIN_CERT_PFX`, `MAC_BUILD_SSH_KEY_B64`, `MAILTRAP_API_TOKEN`, `MAILTRAP_WEBHOOK_SECRET`, `MIRROR_ROUTER_SIGNING_SEED`, `SENTRY_DSN`, `SSH_GATEWAY_TOKEN`, `VULTR_API_KEY`; with them the non-secret settings `ADMIN_PATH`, `ADMIN_USER`, `CLOUDFLARE_ACCOUNT_ID`, `CLOUDFLARE_ZONE`, `DISCORD_CLIENT_ID`, `FORKMESH_ACTIONS_ALLOW_NETWORK`, `MAC_BUILD_KNOWN_HOSTS`, `MIRROR_ROUTER_PUBLIC_KEY`, `SSH_GATEWAY_HOST`, `SSH_GATEWAY_PORT`, `SSH_GATEWAY_REPOSITORIES`, `TREASURY_SOLANA_ADDRESS` | All 28 move to the vault (namespace `actions`) on the first core run with a desktop attached |
| `ForkMesh.conf` | `chat/e2ee/x25519PrivateV2` (room-seal private key; `RoomSeal.cpp`, `internal/mesh`), `genie/token` (MCP token; `MainWindowMcp.cpp`) | Vault namespace `desktop` since round 4; `ForkMesh.conf` keeps a marker ([Desktop and release secrets](#desktop-and-release-secrets-round-4)) |
| `ForkMesh.conf` (only when the Sentry package is connected; absent on this machine) | `market/packages/sentry/data/authToken` | Plaintext; it moves with the Sentry package's client (see `stats.sentry`) |
| `config/ForkMesh/ForkMesh.conf.before-restore-1712`, `ForkMesh.conf.guest-ion-socket-4970.bak` (mode 0664) | `chat/e2ee/x25519PrivateV2` | Stale, group-readable copies; the owner should remove them (agents never delete user data) |
| `data/release` | `.apk-keystore-password`, `.windows-cert-password` (0600) | Vault namespace `release` since round 4; the files keep a marker, `tools/forkmesh-release-host.py` reads through `tools/forkmesh_desktop_vault.py` |
| `data/release` | `windows-authenticode.pfx`, `android-direct-distribution-v3.jks` | Unchanged: password-protected containers, not passwords; useless without the two passwords above |
| `data/agent-accounts/claude/*` | `.credentials.json` (`accessToken`, `refreshToken`), `daemon/control.key` (0600) | Unchanged; the Claude CLI's own files |
| `config/fleet` | `secrets.env` (`ADMIN_PASS`, `APNS_AUTH_KEY`, `AUTOSCALE_CERTSYNC_SECRET`, `AUTOSCALE_IDS_SUBMASTER`, `DISCORD_BOT_TOKEN`, `DISCORD_CLIENT_SECRET`, `FMNEXT_IDS_MASTER`, `FORKMESH_CERTSYNC_SECRET`, `FORKMESH_MESH_SECRET`, `GIT_CI_PASSWORD`, `MAILTRAP_API_TOKEN`, `MAILTRAP_WEBHOOK_SECRET`, `MIRROR_ROUTER_SIGNING_SEED`, `OFFICE_TURN_SECRET`, `PRODUCT_ANALYSIS_PASS`, `RQLITE_PASSWORD`, `SENTRY_DSN`, `SSH_GATEWAY_TOKEN`, `TURNSTILE_SECRET_KEY`, `VULTR_API_KEY`, `WWW_EDGE_SECRET`), `DATA_KEY` (0400), `cloudflare.env` (`CLOUDFLARE_API_TOKEN`), `vultr.env` (`VULTR_API_KEY`), `aws.env` and `aws.csv` (`AWS_SECRET_ACCESS_KEY`), `next-admin.env` (`NEXT_ADMIN_PASSWORD`), `dkim/fmnext.private`, `ids-operator.key` | Unchanged on purpose: operator files of the ForkMesh Next stack's Python fleet tools ([VAULT.md](../../docs/VAULT.md), [NEXT_ADOPTION.md](../../docs/NEXT_ADOPTION.md)), backed up only age-encrypted; the desktop vault does not take them over |
| `~/Backups/forkmesh/forkmesh-minimal-*.tar.gz` (168 plain hourly archives) | Each holds `config/ForkMesh/ForkMesh.conf`, so every archive made before the migration contains the 28 plaintext variables, `DATA_KEY` included | They age out after a week (keep 168). Until then treat those values as exposed to anyone who can read the archives; rotating the API tokens and `DATA_KEY` is the safe course. `data/vault` is not in the minimal archive's list; it belongs in its age-encrypted part |

### Tests

- `store_test.go`: round trip, file and folder modes, no plaintext in the file,
  no temp leftovers, wrong identity (`locked`), no identity, six tamper cases
  (ciphertext, swapped entries, deleted entry, generation, timestamp, wrapped
  key), unknown version, rotation (re-sealed, timestamps kept, an old copy
  still opens), identity change re-wrap, limits, symlink refusal.
- `module_test.go`: the migration moves every value once and logs no value;
  resume after `desktop_unavailable` without re-sealing; an older desktop's
  plaintext is imported and a newer vault value is never overwritten; peers
  (LAN and relay `forbidden` for every method, an unverified local client
  refused for values), set, get, list, replace, delete, rotate and the
  names-only event; a locked vault falls back to plaintext only.
- `parity_test.go` (opt-in, `FORKMESH_VAULT_PARITY_BIN`): a build of
  `testdata/parity_harness.cpp` with `desktop/src/VaultFile.cpp` reads a
  Go-written vault value for value (UTF-8, quotes, a 70 KB value, an empty
  value), encodes names the same way, reads after rotation, refuses another
  identity and detects tampering. Passed on 2026-10-04, built with
  `g++ -std=gnu++17 -I desktop/src $(pkg-config --cflags Qt6Core) … $(pkg-config --libs Qt6Core) -lcrypto`.
- `ipc/peercred_test.go`: the verified peer pid of a Unix socket.
- `secrets_test.go`: `secrets.list` and `not_found` read the vault.
- `internal/vault/secrets_test.go`: the two `desktop` keys move once (markers
  in `ForkMesh.conf`, no value in the vault file or the log, a second run
  writes nothing), `core.vault.get` for `desktop` from the UI and `forbidden`
  for a shell and a phone, resume after `desktop_unavailable`, a newer
  plaintext wins and is migrated, the release files become markers with
  their mode kept (the `.pfx` untouched), a rewritten password file wins and
  is migrated, a symlinked password file is left alone, and the Python
  reader opens the Go-written vault (UTF-8 and quotes, plaintext files,
  a tampered generation reports `vault_tampered`).
- `internal/mesh`: the seal key comes from the vault when the setting holds
  the marker.
- `app/tests/test_release_host_completion.py`: a vaulted Actions variable is
  read from the vault; `release_password` reads plain files and reports an
  unreadable vault by code.

### Gaps

- Not in the vault, on purpose: the fleet operator files in `config/fleet`
  (they belong to the Next stack's own tooling and are only ever backed up
  age-encrypted). Not yet: the Sentry package's `authToken` (moves with its
  client).
- `desktop` secrets and the release passwords have run only in temp
  sandboxes; the core has not migrated the real `~/projects` tree.
- `MirrorActionsConfiguration` on mirror nodes: with a core on the host the
  pushed variables go to its vault and the journal holds only names
  ([Mirror Actions configuration](#mirror-actions-configuration)). A host with
  no core and no vault file yet still gets them in plaintext (the first core
  run migrates them); a host with a vault file and no core refuses them.
- Not yet run in a built desktop; the migration has run only in temp
  sandboxes, never against the real `~/projects` tree.
- No rollback protection for a complete older copy of the vault file.

## Configuration

`FORKMESH_CORE=0` / `core/enabled` (off switch), `FORKMESH_CORE_BIN` (binary
path), `serve --linger`, `core/agentProcesses` (agents in the core, default on).

## Build/run/test

- CMake builds the core with the app (target `forkmesh-core-go`, output
  `forkmesh-core` beside `forkmesh`, `-X main.version=${FORKMESH_VERSION}`)
  when `go` is on the PATH. Without Go the app still builds and runs without
  the core.
- `install.sh` installs it to `~/.local/bin/forkmesh-core`; `cmake --install`
  puts it beside the app (inside `ForkMesh.app/Contents/MacOS` on macOS).
- Standard library only, Go 1.24 (`crypto/hkdf`, `crypto/ecdh`).
- Tests: `cd desktop_go_new && env -u FORKMESH_HOME -u FORKMESH_PROJECTS_DIR -u
  FORKMESH_DATA_DIR -u FORKMESH_CONFIG_DIR -u FORKMESH_PROJECT -u
  FORKMESH_PROJECT_DIR go test ./...`. The desktop exports every one of those
  to agent shells (not just the two `AGENTS.md` names); the path tests resolve
  in a temp `HOME` and compare with `tools/forkmesh_paths.py`.
- Manual checks: `forkmesh-core status`, `forkmesh-core call core.routes`,
  `forkmesh-core call core.vitals`.

## Operations

- Log: `logs/core/forkmesh-core.log`, rolled to `.1` at 8 MB.
- The desktop's system log shows starts, connects, restarts and a missing
  binary.

## Security

- **Access.** The socket is owner-only. TCP needs the 192-bit token from the
  0600 endpoint file.
- **Desktop-only methods.** Every `core.*` method refuses phone and relay peers
  with `forbidden`.
- **Phone channel.** Encryption and approval are unchanged:
  same handshake, keys and approval rules as the C++. Account checks still go
  through the desktop's account session (`ui.control.accountCheck`).
- **Deletes.** Recursive deletes in the core go only through
  `internal/safedelete` (the `server/safedelete.go` rules). They are used for
  the rebuild scratch folder, stale generator entries, a failed restore's work
  folder and unclaimed Actions sandboxes. `git worktree remove --force` goes
  only through `safedelete.ForceRemoveWorktree`, after the dirty work is saved
  to a trash ref. Besides those, the core removes its own socket and endpoint
  files, single files it wrote (snapshot `.part` files, snapshots past the
  kept count) and, in the git queue, a stale `*.lock` that no process holds
  and that is at least 60 s old
  ([git-locking.md](../../docs/desktop/git-locking.md)).
- **Actions variables are encrypted at rest.** They live in the core-owned
  vault (`data/vault/secrets.json`); `ForkMesh.conf` keeps only their names
  with a marker. See [Secrets at rest](#secrets-at-rest-the-vault) for the
  format, the key, the migration, who may read, and what it does not protect.

## Traps

- **Pre-existing test flake.** `forkmesh-local-control-tests` aborts with heap
  corruption in about a third of runs (`malloc_consolidate(): unaligned
  fastbin chunk detected`). This happens on the commit before issue 1336 too,
  so it predates the core.
- **Two git queues, mostly one.** With the core attached, index-writing git
  commands from the desktop's worker threads and from `PullStore` on the GUI
  thread run through `core.git.run`, so they share the core's per-repository
  lane, and so does the Commits page `git pull` (with the desktop's
  environment). Two cases still take `GitIndexQueue`: a command issued while
  the same thread already holds a desktop lane, and any write while the
  desktop's own lane for that repository is held or has waiters. The phone
  `files.*` pull uses its own process and no lane. Git's `index.lock` and the
  shared retry rules settle a collision between those and the core.
- **Settings-based lookups.** `stats.repo` and `files.browse.roots` find
  repositories through the saved settings, which can trail the desktop's memory
  until Qt flushes them.
- **Shortcut counters read from settings.** `insights.shortcuts` reads
  `ForkMesh.conf` too (`secrets.list` now reads the vault). The desktop writes those keys
  through short-lived `QSettings` objects, which flush when they are destroyed,
  and `ui.secrets.apply` also calls `sync()`, so the lag is small.
- **Two writers of the shortcut counters.** The core's shortcut runner and the
  desktop (prompt drafts, shortcut agents, deletes) both update
  `shortcuts/usageCounts` and `shortcuts/runStats`. Every write still happens
  in the desktop, on its GUI thread, and the core applies its operations to the
  values it reads just before writing, but a desktop write that lands between
  that read and the core's write is lost. The window is one IPC round trip.
- **Non-ASCII bytes in `@ByteArray` values.** `internal/qsettings` decodes a
  `\xNN` escape as a character, not a byte. A value such as `café` inside an
  `@ByteArray(...)` therefore comes back as `cafÃ©`. Secret names are ASCII, so
  `secrets.*` is unaffected, but a non-ASCII value read by `stats.codemagic` or
  `diag` would be wrong. `internal/qvariant` decodes from `Value.Raw` and does
  not have this problem.

## Verification (2026-10-02)

- `go test ./...` in `desktop_go_new`: every package passes, including with
  `-race`. `GOOS=windows` and `GOOS=darwin` builds are clean.
- The desktop builds and links with the core (`forkmesh-core-go`).
- Live run (`TestLiveDesktop` in `internal/control`, opt-in through
  `FORKMESH_LIVE_MESH` and `FORKMESH_LIVE_SEED`). The setup:
  - a sandboxed desktop (offline, offscreen, its own `FORKMESH_HOME`);
  - a phone already listed in the sandbox's `paired-devices.json`.

  The checks:
  - The desktop started the core by itself.
  - The phone's TCP socket was handed from the Qt mesh to the core.
  - `ping`, `files.browse.roots` and `diag.log` were answered natively.
  - `repos.list` and `agents.list` were answered by the C++ handlers through
    the proxy.
  - `core.status` was refused for the phone.
  - After the desktop quit, the core kept running and the restarted desktop
    re-attached to the same core process, and the phone worked again.

### Second live run (2026-10-03, after round 2)

Same opt-in test, now with `FORKMESH_LIVE_CALLS` (a JSON list of `{method,
params}`) to script any sequence. The setup:

- the sandbox desktop with the mesh in the core;
- one repository record (`alice/demo`) pointing at a temp checkout;
- a fake `claude` early on `PATH`. It speaks stream-json, prints 40 `tick`
  lines 1.5 s apart, then `result`, and waits for stdin to close. Its logs go to
  `~/.cache` and `/tmp`, because `forkmesh_agent_guard.py` makes the rest of
  `HOME` read-only for agents: a fake that writes to `HOME` exits 1, which
  reads like "Claude Code exited before starting a session".

The results:

- **Phone requests:** with the core owning the mesh port, the phone's
  `agents.start` reaches the C++ handler through the proxy, and the agent runs
  under an `agentproc` shim.
- **Desktop quit mid-run:** the agent keeps writing while no desktop is up
  (ticks 7 to 14). The restarted desktop re-attaches at the acked offset, and the
  run ends `success` with ticks 1 to 40 each exactly once in `transcript.txt`.
- **Desktop quit after the turn:** the idle CLI is stopped, as it was before the
  core. After `result` the desktop calls `core.agentproc.setDetachable
  {detachable:false}`, which binds the process to that desktop connection; a new
  turn sets it back.
- **A dead core's leftovers:** a core that died without cleaning up leaves
  `endpoint.json` and the socket behind. The desktop now starts a new core when
  the old endpoint refuses the connection (before, it retried the dead socket
  forever).

### Third live run (2026-10-04, after round 3 and a merge of main)

The desktop builds and links with every round-3 change. At attach it reports
222 native methods, and the Actions run queue moved into the core ("Actions:
the run queue is in forkmesh-core").

**Phone requests, answered natively:** `ping`, `files.browse.roots`,
`diag.health`, `diag.processes`, `repos.list`, `hosts.list`, `fleet.summary`,
`mergequeue.list`, `stats.timeline`, `update.status`, `secrets.list` and
`agentops.bin`. `agents.list` is still answered by the C++ handler through the
proxy, and `core.status` is refused for the phone.

**Restart test, with the core parsing the stream:** the core logged "agentrun
… parsing claude output for session 9 from byte 0". The test steps:

- Quit the desktop at tick 7; the agent kept going to tick 14 with no desktop
  up.
- Start the desktop again; the run ended `success`.
- `transcript.txt` and `events.jsonl` each hold ticks 1 to 40 exactly once.
- Quit after the turn; the idle CLI is stopped.

### Codex restart test (recipe)

The sandbox has a fake Codex app-server next to the fake `claude`:
`live2/home/.local/bin/codex` runs `codex-fake.py`. It answers `--version`,
`login status` and `app-server`, and speaks the JSON-RPC the core's Codex
runtime uses: `initialize`, `account/rateLimits/read`, `model/list`,
`thread/start`, `thread/resume`, `turn/start`, `turn/steer`,
`turn/interrupt`, unknown methods as `-32601`. A turn sends `turn/started`,
then 40 `agentMessage` items 1.5 s apart (`item/started`, `item/completed`
with text `tick N`), `thread/tokenUsage/updated` and `turn/completed`. It
logs to `live2/home/.cache/fake-codex.log` (the agent guard leaves `~/.cache`
writable). It was checked against a throwaway `forkmesh-core serve` in a temp
`HOME`: the core's Codex runtime ran two turns through it, and after a soft
core stop mid-turn the next core resumed with every tick once in
`transcript.txt` and `events.jsonl`.

To run it (the lead; it needs the built desktop at
`builds/agent-1336/forkmesh` and no other agent using the sandbox):

1. `cd /home/f/projects/forkmesh/builds/core-1336/live2`
2. `./codex-restart.sh` (set `QUIT_AT=<tick>` to quit at another tick; the
   default is 7). The script:
   1. starts the sandbox desktop with `launch.sh` unless one is running (log in
      the next free `desktopN.log`) and waits until the phone gets a `ping`
      answer through `call.sh`;
   2. sends `agents.start {owner: alice, name: demo, provider: codex, task,
      worktree: false}` as the paired phone and reads the session id;
   3. waits for `tick 7` in the fake's log, then quits the desktop with
      SIGTERM (a normal quit) and waits for it to exit;
   4. checks the fake app-server is still running and ticks at least five
      more times with no desktop;
   5. starts the desktop again and waits for `turn/completed`;
   6. checks `data/agents/sessions/alice-demo/<id>/transcript.txt` holds
      `tick 1` to `tick 40` exactly once each, `events.jsonl` holds each
      `_codex_agent_complete` tick once, one `result` and one `init` event,
      and prints `agents.transcript` as the phone sees it;
   7. quits the desktop again after the turn and checks the idle app-server
      stopped with it (`SKIP_IDLE_QUIT=1` leaves the desktop running).
3. It ends with `PASS: …` or `FAIL: …` and exits 0 or 1. On a failure, look at
   the desktop log, `projects/forkmesh/logs/core/forkmesh-core.log` (the
   `agentrun … parsing codex output` and `agentproc: adopted` lines) and the
   fake's log.

Not run yet with the built desktop.

### Fourth live run (2026-10-04, after round 4, built from the overlay)

The integrated desktop was built only from `desktop_go_new/desktop-overlay/build.sh`
on an unchanged checkout. At attach the core reports 317 native methods. On
the phone, `ping` and `agents.list` are now answered by the core.

**Claude restart test (fake stream-json CLI):**

- Quit the desktop at tick 7; the agent kept going to tick 14 with no desktop up.
- The restarted desktop re-attached. Session 12 ended `success` with ticks 1 to
  40 exactly once.
- The idle CLI stopped with the next quit.

**Codex restart test (fake app-server, `live2/codex-restart.sh`):**

- Session 13 reached tick 8, the desktop quit, and the core kept writing to tick
  13 with no desktop up.
- The restarted desktop re-attached. `events.jsonl` holds one init and one
  result event, and ticks 1 to 40 are in `transcript.txt` and `events.jsonl`
  exactly once.
- The idle app-server stopped with the next quit.

## Checklist (issue 1336)

Done means implemented, tested in Go, and wired.

- [x] Core daemon: paths parity, identity, IPC, registry, linger, single instance, logs
- [x] Desktop launches, attaches to and upgrades the core; off switch; survives restarts
- [x] CMake, `install.sh` and `cmake --install` ship the binary
- [x] Phone transport in the core: LAN hand-off, relay, pairing, account check, files, proxy
- [x] `LocalControlServer` core mode on the desktop
- [x] `files.browse.*` native
- [x] `diag.watch`, `diag.unwatch`, `diag.log`, `diag.buildMac` native; desktop stops sampling vitals while attached
- [x] `repos.commits`, `repos.branches` native (`ui.repos.locate`)
- [x] `stats.repo`, `stats.ratchet.*`, `stats.codemagic`, `insights.shortcut` native
- [x] `core.git.*`, `core.vitals`, `core.terminal.*`, `core.agents.*` for the desktop
- [x] Desktop pages read from the core, part 1: worker-thread index writes (`core.git.run`, one queue with the core), the Agents page diff and worktree stats (`core.git.agentDiffStats`, cached by branch tips), transcript search (`core.agents.search`), `TerminalWidget` shells (`core.terminal.*`; the workspace terminal re-attaches after a restart)
- [x] Desktop pages read from the core, part 2: footer, Stats page and descriptor-check vitals from `core.vitals.sample` (byte totals, the desktop's own process stats); detached git (source-control status, the change badge, `git pull`) through `core.git.run` with the desktop's environment; `PullStore` GUI-thread writes on the core queue (pumped)
- [x] Desktop pages, part 3: worker git reads (`runGitCaptureDirect`, so the `runOffThread` branch panel, commit history and outgoing count) through `core.git.run`; the phone `files.*` pull on the core's repository lane (native and delegated paths); the Stats page disk tile from `core.vitals` (repository disk sampled); the agent terminal re-attaches to its session after a restart (persist key `agent`, `resumeCoreAgentTerminal`)
- [ ] A build-and-run measurement of the stall log after parts 2 and 3 (the app was not built in rounds 3 and 4); the agent terminal's launch path has no caller today
- [x] Agent processes in the core (`internal/agentproc`, `CoreProcess`): Claude, Codex and `AgentRunner` I/O off the UI thread; Agents-page Claude runs survive desktop restarts, crashes, updates and core upgrades and re-attach at the acked offset
- [x] Core-side owner of the agent store: `internal/agents` writer, `core.agentstore.*`, `AgentStore` facade (golden-tested against `AgentStore.cpp`)
- [x] Stream parsing and transcript writing in the core (`internal/agentrun`): Claude stream-json and the Codex app-server JSON-RPC, normalised events to the desktop (`core.agentrun.event`), redaction, usage and resume-id tracking
- [x] Codex app-server re-attach (JSON-RPC state in the core, snapshotted across core restarts); the external-call gate on a stable socket and the IDE bridge on its previous port for re-attached runs
- [x] `agents.transcript` and `agentops.bin` native (core-owned attachment salt)
- [x] The agent session model in the core (`internal/agentmodel`, facade `MainWindowCoreAgentModel.cpp`): session list from the store plus the pushed desktop context, queue view, the `agents.changed` bell, background-work completion checks (with the Success transition while no desktop is up), detached stop and queue move; `agents.list`, `agents.queue`, `agents.follow`/`agents.unfollow`/`agents.output`, outside `agents.transcript`, `agentops.prepare/approvals/grants/usage/diff/file/mergestats/worktrees/bin.*` native; `agentops.answer/merge/delete/bulk/revoke` with core tokens executed by the desktop (`rpc.DelegateWith`, `_coreConfirmed`); golden-tested against `LocalControlAgentOps.cpp` and `AgentLogText.cpp`
- [ ] Agent session model, rest: the queue pump, parking, usage-limit holds, resume/continue/takeover/handoff and `agents.start/prompt/continue/takeover/handoff/queue.limit/markRead`, `agents.image.*`, `agentops.accounts.*` still run on the desktop (the launch path and account profiles are there); `agents.attachment` previews stay proxied (Qt's `QImage` JPEG re-encode cannot be matched byte for byte); the desktop still reads its own session list from disk; a live run with a phone
- [x] Core parsing for the remaining runs: `AgentRunner` (`log` runtime; its agent phase survives a desktop restart and re-attaches), dialog sessions (Calendar, Market: transient runs, no store), and Windows (store link and shims over loopback TCP + token); durable stderr for every core-parsed run
- [ ] Round-4 agent runs, live: the `AgentRunner` re-attach and the dialog transient parse with the built desktop; the Windows-only C++ branches built on Windows
- [x] Live run of the restart test with the built desktop for Claude under core parsing (2026-10-04, see Verification)
- [ ] The same live restart test for Codex: the fake app-server and `live2/codex-restart.sh` are ready ([recipe](#codex-restart-test-recipe)); waiting for a run with the built desktop
- [x] Actions runner in the core (`internal/actions`, `core.actions.*`): parser, store, sandboxed steps, artifacts and release landing; runs survive a desktop restart and the desktop re-attaches (facade `CoreActions.cpp`)
- [x] `repos.actions`, `repos.actions.log`, `repos.prepare`, `repos.run` native, and the intents `workflow.run`, `run.approve/reject/rerun/stop/skip` applied by the core's queue (`workflow.enable/disable` ask the desktop through `ui.actions.workflowDisabled`; `ui.actions.perform` only for an older desktop)
- [x] `repos.list` native (repository snapshot, Actions summaries from the store, agent counts from the core's agent store)
- [x] Actions queue in the core: push, served-head and external-source scans, approvals and auto-approve (written with `core.WriteSettings`), `needs`, superseding, the runner pool, manual runs and reruns; facade `MainWindowCoreActionQueue.cpp`
- [x] Actions queue: pull-request head resolution in the core (`core.actions.queue.pullChecks`, pull records read by `internal/pulls`, signed-patch replay, mirror publication); runs the queue starts are followed from core events without a runner link (notifications and pings as desktop subscribers), the served-mirror release landing runs in the core, agent auto-fix is decided and built in the core and delivered with `ui.actions.autoFix` (queued while no desktop is attached)
- [ ] Actions: a live run in the sandbox desktop of a pull-request check, a release landing and an auto-fix across a desktop restart; the release CAS seeding, managed-host fan-out and rollout move with the mirror push engine; auto-fix through a core agent API once the session model is in the core
- [x] `MirrorActionsConfiguration` (the mirror Actions settings transaction) in the core (`core.actions.mirrorConfig.*`)
- [x] `--configure-mirror-actions-stdin` on mirror hosts delegates to the core when one runs there (native otherwise); the core settles the vault half of an interrupted transaction at start, at attach and before every apply (the settings half of `main.cpp` startup recovery stays C++: it must run before the desktop reads its settings, when no desktop is attached to write them)
- [x] Defer the core upgrade while work that cannot survive a restart is running (the hello and `core.status` list busy modules; `agentproc`, `control`, `mesh` and `relay` do not block it)
- [x] `diag.backups` native; hourly backups, restore unpack and export in the core (`internal/backup`, `core.backups.*`)
- [x] `diag.health`, `diag.processes`, `diag.stalls`, `diag.stall` native (core-owned stall list fed by `core.diag.stall`, `ui.diag.state` for agent counts and roots, `agentproc.Processes()` when no desktop is attached)
- [x] `diag.prepare/run` native: tokens in the core, `stopAgent` executed by the core while no desktop is attached and core-confirmed on the desktop otherwise; the stall-fix agents start on the desktop
- [x] `secrets.*` (writes through `ui.secrets.apply`), `teammates.options`, `insights.shortcuts` (`ui.insights.shortcutState`) native
- [x] Tool-efficiency, improvement and guardrail ledgers in the core (`internal/insights`, golden-tested against the C++ classes): fed in-process by `internal/agentrun` and by the desktop for the runs it parses, same files and save cadence, facade in the three C++ classes (`CoreInsightsHook.h`, `MainWindowCoreInsights.cpp`); `insights.tools`, `insights.guardrails`, `insights.network` (sock_diag sampler in the core) native
- [ ] `insights.usage` (session totals: moves with the session model, `internal/agentmodel`)
- [x] `insights.shortcutRun`, `insights.prepare/run` native: the shortcut script runner in the core (Shortcuts page and phones), run stats written with `core.WriteSettings` `variant` (exact `QDataStream` bytes) and queued while no desktop is attached; `toggleFamily` and agent shortcuts ask the desktop (`ui.insights.toggleFamily`, `ui.insights.runShortcutAgent`)
- [ ] Insights: a live run with the built desktop (core-parsed Claude run counted with the desktop quit, Shortcuts page run and stop, phone toggle)
- [x] `stats.timeline` (`internal/netlog`: the digest from `network_log.txt`, `core.log.append`) and `stats.quality` (`internal/quality`: `RepoQuality` scan with the run store and the `IssueStore` read path) native
- [ ] `stats.sentry`: re-checked in round 4, stays proxied (no change hook in the package contract, the data and token live in the desktop's Sentry client, the proxy costs no I/O); moves with that package's client
- [ ] `teammates.list/get/briefing/stats` once `internal/agentmodel` exposes sessions to modules and its desktop context carries the Team repository and the payout run; `teammates.prepare/run` with the agent runtime; `teammates.room` stays with the chat model (its file is saved only on some events); see [Teammates](#teammates-what-can-move)
- [x] `mergequeue.*` native; the merge executor and the `PullStore` subset it needs run in the core (`internal/mergequeue`, `internal/pulls`, facade `MainWindowCoreMergeQueue.cpp`)
- [x] `PullStore` in the core: the repository page's pull list, comments, reviews, threads and branch reviews (signed by the core), create, status, delete, conflict and edit sessions, review checkouts, inbox intake, ledger sweep (`internal/pulls`, `core.pulls.*`, facade `PullStoreCore`/`CorePulls`); the history rewrite stays in the desktop by design (`ui.pulls.rewrite`, `ui.mergequeue.purge`)
- [ ] `PullStore`: `loadAllStrict*` (contribution verification with `StrictGitReader` limits) in the core; the desktop reacting to `core.pulls.changed`; a live run of the pull page against the sandbox desktop
- [ ] `wallet.*` (tip queue, `TipSigner` and the Ledger payment state machine move together), `discord.*` (the Discord package's gateway client with the chat model), `pings.*` (notification list, fix ledger, Actions queue and agent runtime); re-checked in round 3, reasons in the table above
- [x] Actions variables encrypted at rest under a single owner: `internal/vault` (AES-256-GCM, data key wrapped by the node identity), one-time resumable migration out of `ForkMesh.conf`, `core.vault.*` (local, desktop process only for values), every C++ reader behind `ActionStore` via `CoreVault`, read-only C++ fallback without the core, `secrets.*` on the vault
- [x] Other plaintext secrets from the 2026-10-04 audit: `chat/e2ee/x25519PrivateV2` and `genie/token` (namespace `desktop`, `SettingsSecretBackend.h` facade, the core's mesh reads the vault) and the release signing passwords (namespace `release`, the release host reads through `tools/forkmesh_desktop_vault.py`), with resumable migrations; the fleet operator files in `config/fleet` stay with the Next stack on purpose (age-encrypted backups only)
- [ ] Secrets still in plaintext: the Sentry package's `authToken` when connected (moves with its client); `MirrorActionsConfiguration` on a mirror host with no core and no vault file yet (with a core the variables are sealed in its vault and the journal holds names only)
- [x] `internal/qsettings`: `@ByteArray` values decode as Latin-1, as Qt does
- [x] `hosts.list/detail/logs.*`, `hosts.prepare/run` (restart, stop, start), `hosts.options` (stored key), `fleet.disk/summary/provision/repos` native; host probes and mirror-node SSH run in the core (`core.hosts.*`, facade in `MainWindowCoreHosts.cpp`)
- [x] `hosts.prepare/run` update, install, destroy and all of `fleet.prepare/run` native: the host installer, uninstall, deploy-all, agent CLI install, Actions configuration, link codes, the Vultr provisioning machine with its DNS calls, destroy and node delete as core jobs with streamed transcripts the Hosts page renders and re-attaches to; saved-host writes through `WriteSettings` (queued while the desktop is away); facade `MainWindowCoreHostJobs.cpp`
- [x] Mirror push in the core: the push engine, release gates and artifact CAS of `MainWindowMirrorPush.cpp` (`core.mirror.*`, `fleet.run mirrorPush` without a desktop), git-remote unwiring of a dead node, agent login files read by the core, accepted Vultr/Cloudflare keys kept in the vault and the deploy `.env` ([Mirror push](#mirror-push-mirrorpushgo-mirrorhealthgo-mirrorprocgo-mirrormethodsgo))
- [ ] Mirror push follow-ups: the release follow-up steps after the managed fan-out (`pushConfigured`, clearing the publication, artifact prune, deployment) still run in the desktop's callback, so a desktop restart mid-release leaves the publication pending as before; the triggers (commit events, sync completion, safety pass) still start in the desktop; seed and inventory processes are not in the process activity view; not yet run against a real (non-production) managed host
- [x] Host log feeds: the SSH leg in the core (`core.hosts.feed.*`; phones keep getting lines while the desktop is away)
- [ ] Host log feeds: the QUIC leg stays in Qt (`Http3Get`); the Go standard library has no QUIC or HTTP/3 client
- [ ] Hosts: a live run of install, deploy-all and a Vultr deployment across a desktop restart against a real (non-production) host
- [x] `update.*` native; Rebuild & restart (pull, build passes, install, relaunch) runs in the core and survives a desktop quit (`internal/update`, facade `MainWindowCoreOps.cpp`)
- [x] Rebuild & restart: the install-mirror lookup, the sudo/other-user path, the prebuilt auto-update and the clean rebuild's build-folder wipe in the core (native fallbacks kept)
- [x] Rebuild & restart: the auto-update timer and its release check decide in the core (`autoupdate.go`, `ui.update.auto` starts the prebuilt or source update through the desktop's repository records) and Quick update is a core job (`kind: "quick"`); capabilities `autoUpdate`, `quick`
- [x] Cleanup worktree and Actions sandbox removals in the core (`core.cleanup.*`); `internal/safedelete` with the server parity table; the destructive-delete test scans `desktop_go_new`
- [x] Cleanup scanner (marketplace `cleanup` package 1.1.0) and its file and sandbox deletes in the core (`core.cleanup.scan/deleteFiles`, facade `CoreCleanup.cpp`)
- [x] Relay socket (`NodeEventSocket`) in the core: `internal/relay`, `core.relay.*`, facade in `NodeEventSocketCore.cpp`
- [ ] Relay socket: run against the served relay (n1 to n3) with a phone away from home across a desktop restart
- [x] Mesh discovery (`LocalMeshNode`) in the core: `internal/mesh`, `core.mesh.*`, facade `CoreMesh`, control hellos straight to `control.Adopt`
- [ ] Local mesh: run the core room with the built desktop, an iPhone and an Android phone on one LAN, across a desktop restart, and over Bluetooth on a build with Qt Bluetooth
- [x] A `ui.log` notification (`Core.LogUI`) so core-side "requested by" lines reach the desktop's system log
- [ ] Windows: run the TCP pipe hand-off and the core on a Windows box

## Related docs

[networking.md](../../docs/desktop/networking.md#phone-control-over-the-lan) (the phone protocol),
[lifecycle.md](../../docs/desktop/lifecycle.md), [git-locking.md](../../docs/desktop/git-locking.md),
[../native/DESKTOP_AGENTS.md](../../docs/native/DESKTOP_AGENTS.md).
