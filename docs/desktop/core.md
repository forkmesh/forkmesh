# forkmesh-core, the desktop's Go background engine

`forkmesh-core` is a lightweight Go engine that does the processing behind the
ForkMesh desktop, so the Qt UI stays responsive. The desktop and the phones
(iOS, Android, and the web over the relay) reach the same engine. A method
family moves to the core only where the core can produce the same reply; every
method the core has not taken over is handed back to the desktop's existing C++
handler, so nothing stops working while a family waits.

## Layout, build and run

| Path | What it is |
| --- | --- |
| `desktop/core/cmd`, `desktop/core/internal` | The Go engine, `forkmesh-core` (module `forkmesh.com/desktop-core`) |
| `desktop/src/Core*.{h,cpp}`, `desktop/src/MainWindowCore*.cpp` | The desktop's client for the core and the facades over it |
| `tools/forkmesh_desktop_vault.py` | Read-only Python vault reader for the release host |
| `desktop/core/tools/desktop_reconnect_test.sh` | End-to-end lifecycle test against a built desktop and core |
| `desktop/core/tools/check_package_modules.py` | Checks every package module resolves its symbols (CTest `forkmesh-package-modules-resolve`) |
| `desktop/core/examples/service-plugins/` | Example service plug-in packages ([service-plugins.md](service-plugins.md)) |

- `desktop/CMakeLists.txt` builds `forkmesh-core` beside `forkmesh` when Go is
  installed (`FORKMESH_BUILD_CORE`, on by default), so `desktop/run.sh` and
  Rebuild & restart build both. Without Go the desktop runs without the core.
- `desktop/install.sh` installs the built binary next to the app's launcher, or
  builds it with `go build` when the build tree has none.
- `FORKMESH_CORE_BIN` points the desktop at a different `forkmesh-core`.
- Go tests: see [Build and test](#build-and-test).

### Merged into the desktop (2026-10-15)

Until 2026-10-15 this lived in `desktop_go_new/`: the Go engine plus an overlay
of new Qt sources and patches that a script copied over a staged copy of
`desktop/` at build time (`~/projects/forkmesh/builds/desktop-go-new`, no
longer used). The engine moved to `desktop/core/`, the overlay was applied to
`desktop/`, `marketplace/`, `docs/`, `tools/` and `www/`, and the staging
scripts were deleted.

`main` had changed shortcuts in the meantime: the desktop runs several
shortcuts at once. The core's shortcut runner stays single-run; the desktop
starts a script in the core only when no core run is going and runs the rest
itself (see [Insights ledgers](#insights-ledgers-and-the-shortcut-runner)).

The stage built only the `forkmesh` target, so the overlay never noticed that
`forkmesh-agent-boundary-tests` compiles `ClaudeStreamSession.cpp`,
`CodexAppServerSession.cpp` and `AgentStore.cpp` without the core sources they
now call. That test target now links `CoreAgentRun.cpp`, `AgentProcess.cpp`
and `CoreAgentStore.cpp` like the other agent test targets.

### Secrets: copy until finalized

Older desktops and the release tools still read some secrets in
plaintext, so the vault copies them in and keeps the plaintext until the vault
is finalized (`core/vaultFinalize=true` or `FORKMESH_VAULT_FINALIZE=1`), which
replaces the plaintext with markers. While not finalized, the desktop facade
writes both the vault and the plaintext.

## Code map

| Path | Responsibility |
| --- | --- |
| `cmd/forkmesh-core` | The binary: `serve`, `start`, `call <method> [json]`, `status`, `stop`, `background`, `autostart`, `plugins`, `version`; `modules.go` lists the modules |
| `internal/daemon` | `Core`: layout, identity, registry, hub, settings cache, built-ins (`core.ping`, `core.status`, `core.routes`, `core.owners`, `core.shutdown`, `core.daemon.*`), single-instance lock, linger, background mode, crash record, in-place upgrade |
| `internal/autostart` | Login start: systemd user unit or XDG autostart entry, LaunchAgent, Windows Run key |
| `internal/ipc` | Local transport: newline-delimited JSON, protocol 2 (cancel, progress), requests both ways, `control.adopt` socket hand-off, peer credentials |
| `internal/plugins` | Service plug-ins: discovery, signatures, consent, sandboxed processes, `core.plugins.*`, `plugin.<id>.*` ([service-plugins.md](service-plugins.md)) |
| `internal/rpc` | Method registry (exact names and `prefix.` families), `ErrDelegate`, `DelegateWith`, peers, hub broadcast |
| `internal/paths` | The `~/projects/<project>` layout, identical to `tools/forkmesh_paths.py` (a test runs both) |
| `internal/identity` | The Ed25519 node key from `data/identity/ed25519.pem` |
| `internal/qsettings`, `internal/qvariant` | Read-only `ForkMesh.conf` reader; decoder and encoder for `@Variant(...)` values (`QDataStream` `Qt_4_0`) |
| `internal/qtjson` | JSON that parses and prints exactly as `QJsonDocument` does |
| `internal/control` | The phone control protocol: handshake, AES-GCM frames, pairing, account checks, relay chunking and pacing, file transfer, proxy to the desktop |
| `internal/phonefamily`, `internal/ownergate` | Shared owner gate, confirmation tokens, parameter reading, device labels |
| `internal/files` | `files.browse.*` |
| `internal/diag` | `diag.*`, `core.vitals`, `core.diag.stall/stalls` |
| `internal/repos`, `internal/git` | `repos.commits`, `repos.branches`, `core.git.*`; git runner with the per-repository index queue |
| `internal/stats`, `internal/netlog`, `internal/quality` | `stats.*`, the system-log digest, the repository quality scan |
| `internal/secrets`, `internal/vault` | `secrets.*`; encrypted secrets at rest (`core.vault.*`) |
| `internal/teammates`, `internal/insights` | `teammates.options`; tool-efficiency, improvement and guardrail ledgers, the shortcut runner, the agent socket sampler |
| `internal/terminal` | PTY sessions: `core.terminal.*` |
| `internal/agents` | The agent store: read-only queries and the single writer of `data/agents/**` (`core.agentstore.*`) |
| `internal/agentproc` | Agent processes behind a per-process shim (`core.agentproc.*`) |
| `internal/agentrun` | Agent output parsed in the core: Claude stream-json, the Codex app-server JSON-RPC, the managed-runner log (`core.agentrun.*`) |
| `internal/agentmodel` | The agent session model for phones and the desktop |
| `internal/relay` | The node-role WebSocket to the relay (`core.relay.*`) |
| `internal/hosts` | Saved hosts, the SSH runner, host jobs, provisioning, log feeds, the mirror push engine (`hosts.*`, `fleet.*`, `core.hosts.*`, `core.mirror.*`) |
| `internal/mesh` | The `#mesh` LAN room (`core.mesh.*`) |
| `internal/update`, `internal/backup`, `internal/cleanup` | Rebuild & restart and auto-update; hourly backups and restore; cleanup removals and the Disk Cleanup scanner |
| `internal/safedelete` | Guarded tree delete (port of `server/safedelete.go`) and the linked-worktree guard |
| `internal/actions` | `.forkmesh` workflows: parser, run store, sandboxed runner, run queue, pull-request checks, mirror Actions configuration |
| `internal/pulls`, `internal/mergequeue` | The pull store and the merge queue executor |
| `internal/coretest` | Test harness: a core in a temp sandbox, fake peers |

Desktop-side facades in `desktop/src/` are named `Core*.{h,cpp}`
and `MainWindowCore*.cpp`; `CoreClient` finds or launches the core, connects as
the UI and answers `ui.*` requests, and `CoreGit` is a blocking client for
worker threads.

## Architecture

- **One core per project.** `forkmesh-core serve --project <slug> --state-dir
  <state>/core` holds `core.lock` (flock) and writes `endpoint.json` (pid,
  version, socket path, loopback TCP port, token; mode 0600) in `state/core/`.
- **Launch.** `CoreClient` reads `endpoint.json` and connects. When nothing
  listens (including a dead core's stale endpoint), it starts the binary
  through `processreaper::startReleased()`, which drops the instance marker and
  double-forks so a desktop quit or Rebuild & restart does not reap the core
  ([lifecycle.md](lifecycle.md#what-a-quit-deliberately-leaves-running)).
- **Finding the binary.** `FORKMESH_CORE_BIN`, then `forkmesh-core` beside the
  app (also `../libexec/forkmesh/` and `../Resources/`).
- **Linger.** The core outlives the desktop by `--linger` (2 minutes by
  default) and stays up while a module reports `Busy()` (an open phone
  connection, a PTY, a running job), so phone channels, terminals and jobs
  survive a desktop restart. In background mode it never exits for want of a
  desktop ([Independent lifecycle](#independent-lifecycle)).
- **Upgrades.** When the binary on disk is newer than the running core, the
  desktop asks it to `core.shutdown` and starts the new one. The hello and
  `core.status` list busy modules, and the upgrade is deferred while work that
  cannot survive a restart runs. With no desktop attached the core upgrades
  itself.
- **Off switch.** `FORKMESH_CORE=0` or `core/enabled=false`: the desktop runs
  with no core.
- **Who answers a method.** Modules register exact method names on
  `rpc.Registry`. For a phone request the control module runs the native
  handler if there is one, otherwise sends the request to the desktop as
  `ui.control.request`, which answers through its unchanged C++ handler. A
  native handler can return `rpc.ErrDelegate` to pass one request on, or
  `rpc.DelegateWith` to pass it on with replacement parameters. The desktop
  learns the native routes from the hello reply.
- **Owner checks.** Owner-only families answer `not_owner` unless the phone's
  user matches the desktop's node owner or account (pushed with `core.owners`).
- **Desktop-only methods.** Every `core.*` method refuses phone and relay peers
  with `forbidden`.
- **Settings.** The desktop remains the only writer of `ForkMesh.conf`. The core
  writes through `Core.WriteSettings` (a `ui.settings.write` request); writes
  that meet `desktop_unavailable` are queued (last value per key) and retried,
  and reads see the queued values.
- **Facade rule.** Each desktop facade checks per call that the core is
  connected and lists the route. A request that never reached the core runs
  natively. A write that was sent but got no answer is reported as failed and
  never re-run, because the core may already have executed it.

## Protocols and interfaces

### Local IPC (desktop and CLI to core)

- **Transport.** A Unix socket `state/core/core.sock` (0600, in a 0700 folder)
  on Linux and macOS; loopback TCP everywhere (Windows uses this). When the
  socket path is too long for the platform (about 104–108 bytes, a deep
  `HOME`), the core serves TCP only instead of failing to start.
- **Authentication.** Every connection, socket or TCP, must present the 192-bit
  token from the 0600 `endpoint.json` in its first line (`bad_token`). On Linux
  a socket peer must also run as the same user (`SO_PEERCRED`, `forbidden`).
- **Hello.** First line:
  `{"id":N,"method":"core.hello","params":{"role":"ui"|"cli","token":…,"pid":…,"protocol":2}}`.
  The reply carries `version`, `protocol` (2), `protocolMin` (1), `features`
  (`cancel`, `progress`, `peer-uid`, `token-always`) and the route table. A
  client older than `protocolMin` gets `protocol_unsupported`; a client that
  sends no `protocol` is treated as 1. A newer `ui` hello takes over.
- **Messages.** Requests `{id,method,params}`, responses `{id,ok,result}` or
  `{id,ok:false,error}`, events `{event,data}`, notifications (requests with no
  id). Both sides may send requests. Notifications from one connection are
  handled in order; requests run concurrently.
- **Cancel.** `{"method":"core.cancel","params":{"id":N}}` (or with its own id,
  answered `{id, cancelled}`) cancels that connection's request `N`; the
  request answers `cancelled`. A dropped connection does not cancel anything:
  work a closed desktop started keeps running. `CoreClient::cancel(id)`.
- **Progress.** A handler calls `req.Progress(data)`; only the requesting
  connection gets `{"event":"core.progress","data":{"id","method","progress"}}`.
  `CoreClient` emits `progress(id, data)` while the request is pending.
- **No polling.** State changes are pushed as events (`core.daemon.changed`,
  `core.plugins.changed`, each module's own); the desktop only asks again after
  a reconnect.

### Phone sockets

- **LAN.** On a control hello, the desktop (or the core's mesh, when it owns
  the port) hands the connection to the control module. On Unix the TCP
  descriptor is passed with `SCM_RIGHTS` in a `control.adopt` message, taking
  the desktop out of the path; elsewhere the connection becomes a byte pipe.
- **Relay.** When the core owns the relay socket, phone frames go straight
  from `internal/relay` to the control module and back.
- **Desktop state.** `LocalControlServer` runs in core mode: connections the
  core reports get ids from `kCoreConnectionBase`, the core owns
  `paired-devices.json` and pushes `ui.control.devices`, and approve, deny,
  forget and send-file become core requests. If the core goes away the desktop
  reloads the device file and handles phones natively again.
- **Full contract.** Every message name is in `internal/control` and its tests.

## Phone families: what is native

Native handlers keep each family's owner gate and confirmation rules
(`internal/phonefamily`: a token is bound to the device, action and target,
lasts 60 s, is spent by any attempt, at most 32 outstanding).

| Family | Where |
| --- | --- |
| `files.browse.*`, `diag.*`, `repos.*`, `stats.timeline`, `stats.quality`, `stats.repo`, `stats.ratchet.*`, `stats.codemagic`, `update.*`, `hosts.*`, `fleet.*`, `mergequeue.*`, `secrets.*`, `teammates.options`, `insights.shortcuts/tools/guardrails/network/shortcutRun/prepare/run` | Native |
| `agents.list/queue/follow/unfollow/output/transcript`, `agentops.prepare/approvals/grants/usage/diff/file/mergestats/worktrees/bin*` | Native ([Agent session model](#agent-session-model)) |
| `agentops.answer/merge/delete/bulk/revoke`, `diag.prepare/run` | Token checked in the core; the desktop executes |
| `agents.start/prompt/continue/takeover/handoff/queue.limit/markRead`, `agents.image.*`, `agentops.accounts.*` | Desktop (the launch path and account profiles live there) |
| `teammates.list/get/briefing/stats/room/selectRepo/prepare/run`, `insights.usage` | Desktop (Team page selection, session totals and the chat model are desktop state) |
| `wallet.*`, `discord.*`, `pings.*`, `stats.sentry` | Desktop (tip queue and passphrase dialog, the Discord client and chat model, the notification list, the Sentry package's in-memory client) |

`secrets.*` writes go through the desktop: once the core has spent the token it
asks `ui.secrets.apply`, the desktop re-checks and writes through the vault
facade, and the core logs the request and notifies phones with
`secrets.changed`. The core never logs or returns a value. Names are
`[A-Za-z_][A-Za-z0-9_]*` (at most 128 characters); values at most 16 KiB.

## Insights ledgers and the shortcut runner

`internal/insights` ports the tool-efficiency, tool-improvement and guardrail
ledgers and the insights policy. The core is the only writer of their files
while a desktop is attached.

| File (`data/agents/`) | Written |
| --- | --- |
| `tool-efficiency.json` | 5 s after the first change of a burst, at core stop, at once after a reset; compact `QJsonDocument` form, atomic |
| `tool-improvements.jsonl` | One line per occurrence; rotated to `.1` at 2 MiB; moved to `.sent` when handed to an agent |
| `guardrails.json` | 5 s after the first change, only when it has events |
| `state/core/insights-offsets.json` | With every ledger save: the last stdout offset taken from each core-parsed run |

- **Feeding.** Core-parsed runs are consumed in-process from `internal/agentrun`
  starting after the saved offset. Runs the desktop parses are forwarded with
  `core.insights.ingest` and `core.insights.abandon`; events the core delivered
  are not forwarded, so nothing counts twice. Guardrail events go to
  `core.insights.guardrail`.
- **Desktop facade.** When core-owned, the three C++ ledger objects forward
  every write and become mirrors of `core.insights.changed` snapshots (150 ms
  debounce, changed sections only). On detach the desktop reloads the files.
- **Shortcut runner.** `core.insights.shortcut.start/stop/state` runs
  `bash <script>` in the repository checkout (stdout and stderr merged, 96 KiB
  kept; stop is SIGTERM then kill after 3 s), one shortcut at a time. Events go
  to the desktop and to phones (`insights.shortcutOutput`, `insights.changed`).
  Run stats (`shortcuts/usageCounts`, `shortcuts/runStats`) are written through
  `WriteSettings` as exact `QVariant` bytes. Prompt and skill shortcuts start
  agents, so they ask the desktop (`ui.insights.runShortcutAgent`).
- **`insights.network`** samples every TCP socket via `sock_diag` and maps
  them to agent process trees via `/proc/<pid>/fd` (Linux only).
- **Tests.** `golden_test.go` compares against output from the real C++
  classes (`testdata/golden.cpp`, `testdata/gen.sh`); `module_test.go` covers
  the feeds, gates, runner and network method.

## Relay socket

The node-role WebSocket to the relay (`wss://<relay>/api/nodes/events`) runs in
`internal/relay`, a port of `NodeEventSocket.cpp`. A phone away from home keeps
its control channel while the desktop restarts.

- **Methods (desktop only).** `core.relay.configure {url, account, node,
  identity?}` (the URL carries the signed `owner`, `ts`, `sig` query; the core
  checks the signature against its node key and answers `identity_mismatch`,
  `invalid_url` or `invalid_owner`); `core.relay.stop`, `.reconnect`, `.state`;
  `core.relay.send {kind: signed|terminal|control, frame}`.
- **Events.** `core.relay.state` (with a monotonic `seq`) and `core.relay.event`
  carrying `event`, `terminal`, `agent-status-result` and `heartbeat-result`
  frames unchanged.
- **Authentication.** Each attempt signs
  `forkmesh-issues-pull-v1\n<owner>\n<ts>` with the node key; no account
  session token is sent. User-Agent `ForkMeshDesktop/<version> (account=<a>;
  node=<n>)`.
- **Constants.** Connect, TLS and upgrade within 30 s; reconnect backoff 2 s
  doubling to 2 minutes plus jitter; protocol ping every 25 s, application ping
  after 4 minutes of silence, link dropped after 70 s with nothing received;
  inbound frames above 64 KiB drop the link, outbound frames above 60 KiB are
  refused.
- **Control pacing.** Control frames to browsers pass the same pacer as the
  C++ (90% of the hello's `controlBudget`, 200 per 10 s when absent) with a
  16 MB queue that a link loss empties.
- **Facade.** `NodeEventSocket` keeps its API and mirrors `core.relay.state`.
  It gives a connecting core 5 s before opening its own socket, and after a
  core connection drop opens its own after 15 s. `stop()` (sign-out, offline
  mode) sends `core.relay.stop`; a desktop quit sends nothing, so the core keeps
  the socket. If a newly attached desktop sends no configure within 2 minutes,
  the core closes the socket.
- **Traps.** The relay connection now comes from the `forkmesh-core`
  executable, so a path-keyed firewall needs its own rule for it. If only the
  IPC link drops, both processes may briefly hold a node socket.

## Hosts and fleet

`internal/hosts` runs the work behind Network → Hosts and answers `hosts.*` and
`fleet.*`. Every host job (installer, uninstall, deploy-all, agent CLI install,
mirror Actions configuration, link codes, Vultr provisioning with its DNS
calls, destroy, node delete) runs in the core, so a desktop restart does not
stop it, and the next desktop re-attaches to its output.

- **Saved hosts.** Read from the `hosts/list` setting; written by ports of the
  C++ `rememberHost`/`forget*` functions through `WriteSettings` (`hosts/list`,
  `hosts/decommissionedNodes`, `nodes/deletedNodes`). Legacy password fields
  are dropped in memory and never written; a session password lives only in
  the core's memory. Fleet instances (`fmnext-*` names and fleet addresses) are
  refused (`protected_node`). `core.hosts.saved` tells the desktop to refresh.
- **SSH runner.** Reproduces `buildHostSshCommand`: `ssh` (or `sshpass -e ssh`
  with a session password in `SSHPASS`), `StrictHostKeyChecking=accept-new`,
  `HashKnownHosts=yes`, `UpdateHostKeys=yes`, `ConnectTimeout=30`, `BatchMode`
  and public-key auth when there is no password, `known_hosts` in the project's
  `ssh/` folder, the host's `identityFile` or the shared key with
  `IdentitiesOnly=yes`. Endpoints are validated as `isSafeSshEndpoint()` does.
  Probes time out at 35 s; output is trimmed, ANSI-stripped and redacted.
- **Jobs.** Each long operation is a job with id, kind, host, origin, times,
  result and named transcript channels (1 MiB each, absolute offsets, clear
  epochs). Events: `core.hosts.job`, `.output`, `.status`, `.deploy`,
  `.provision`, `.linkPrompt`, `.notify`, `.flash`, `.nodeDeleted`,
  `.feedLines`. `core.hosts.jobs` and `core.hosts.transcript` serve re-attach;
  `core.hosts.cancel` cancels.
- **Installers.** Command builders and shell scripts are byte-for-byte ports
  of the C++ ones. Build-specific inputs (installer URLs, version, binary,
  catalog and account URLs, signer, mirror package files) come from
  `ui.hosts.environment`. Link codes are signed by the core with the node key
  (`forkmesh-link-v1\n<signer>\n<code>\n<ts>`).
- **Provisioning.** The six-stage Vultr machine (credentials and shared key,
  instance, boot polling, install, optional agent CLIs, DNS record, seed push,
  signed health proof), checkpointed in `hosts/vultrProvision/v1` and resumed
  after a restart once a desktop is attached. The install transcript is kept in
  `data/deployments/vultr-mirror-latest.log`.
- **Agent CLI logins.** The core reads the active account's login files (4 MiB
  cap, JSON objects of at most 256 KiB) and sends them only on the child's
  stdin, wiped after writing; no log, transcript or IPC message carries them.
- **Host log feeds.** The SSH leg runs in the core (`core.hosts.feed.*`) and
  pushes lines to phone streams directly. The QUIC leg stays in Qt, because the
  core is standard-library only and Go has no HTTP/3 client.
- **Phone log streams.** Window of 4 unacknowledged batches, at most 2000
  buffered lines, 200 per batch, one batch per 400 ms, 120 lines of backlog.

### Mirror push

The push to ssh:// mirror remotes, the ref fan-out to managed hosts, release
gates and the artifact CAS run in the core (`core.mirror.*`). The desktop keeps
the triggers and the release follow-up steps.

- **SSH remotes.** `git push --porcelain --no-force --(no-)prune <url>
  refs/heads/*:refs/heads/* refs/tags/*:refs/tags/* [+]refs/pr/*:refs/pr/*`
  with `GIT_TERMINAL_PROMPT=0`; one slot per repository and URL; overlapping
  requests queue one replay.
- **Gateway health.** Only transport failures count. Backoff from 10 s to
  5 minutes; after a day without an answer a gateway is parked at 6 h. Streaks
  are stored in `mirrors/sshGatewayHealth`.
- **Managed hosts.** Only `forkmesh/forkmesh`, only saved active hosts, as
  `root`, one per address: push with `--receive-pack=runuser -u forkmesh-node
  -- git-receive-pack` (75 s), then a wake call (45 s). One writer per
  repository and host; the newest state is replayed once after a batch.
- **Release gates and CAS.** An ordinary push is held while a release
  publication of the repository is pending. A release's signed artifact digests
  must exist as regular files at
  `<mirror>/forkmesh-releases/sha256/<2>/<hash>/data`; the core seeds every
  artifact on every host before pushing refs, and one failed host fails the
  release with refs unchanged. Ordinary pushes also reconcile missing CAS
  objects (at most 512 digests per inventory), never ahead of the ref push.
- **Unwiring a dead node.** Removes the node's ssh:// remotes from every
  checkout and bare mirror with `git remote remove` (config only), drops its
  gateways from health tracking, and emits `core.mirror.unwired`.
- **Accepted keys.** A Vultr API key the API accepted and a deployment's
  Cloudflare token go to the vault (`actions` namespace); `ForkMesh.conf` gets
  only markers. Without a usable vault nothing is saved in plaintext. The
  deploy `.env` is rewritten atomically (no symlinks, at most 512 KiB, mode
  0600).

### Facade and tests

`MainWindowCoreHosts.cpp`, `MainWindowCoreHostJobs.cpp` and
`MainWindowCoreMirrorPush.cpp` start each entry point with a `…ThroughCore`
call, render the core's transcripts, statuses and snapshots, and re-attach on
connect. A runner-setup install (it needs the host-key dialog) stays native. No
test reaches a real host, Vultr or Cloudflare: fake `ssh`, `sshpass`,
`ssh-keygen` and `git` go first on `PATH`, provider APIs are loopback
`httptest` servers, and a fake desktop records settings writes.

## Local mesh

`internal/mesh` runs the `#mesh` LAN room in the core (UDP and TCP port 48732),
a rule-for-rule port of `LocalMeshNode.cpp`, so the room and its queued
messages survive a desktop restart.

- **Wire.** Beacons, multicast, broadcast, unicast and paced subnet sweeps,
  the lower-id-dials and duplicate-link rules, keepalive, history replay,
  relaying, wake tokens. `RoomCrypto` (PBKDF2-SHA256, 210 000 rounds,
  AES-256-GCM) and `RoomSeal` (X25519, HKDF-SHA256, per-frame content keys) are
  byte compatible with the desktop and the phones.
- **Keys and files.** The X25519 key comes from the desktop in
  `core.mesh.start`, else from the vault. History is
  `data/local_mesh/history-<sha16>.json` (one writer at a time); learned seal
  keys go to `data/local_mesh/seal-peers-<sha16>.json`.
- **Phone control.** An `fm-control-hello` on an inbound link goes straight to
  the control module.
- **Methods (desktop only).** `core.mesh.start/stop/state/history/diagnostics/connect`;
  ordered notifications for sends, typing and Bluetooth link bytes; events for
  messages, peers, queue and link writes.
- **Facade.** `LocalMeshNode` keeps its API and asks the backend first. It
  waits up to 4 s for a connecting core, and runs natively 8 s after the core
  goes away. Bluetooth (RFCOMM, BLE) stays in Qt; each link becomes a byte pipe
  so framing and crypto run in the core. `MeshFirewall` stays in Qt.
- **Tests.** Never bind 48732 or a non-loopback address; cover crypto vectors
  shared with the phones, wire rules, rooms on loopback and two cores.

## Git and pages on the core

### `core.git.run`

`core.git.run {path, args, write, timeoutMs, env, input | inputBase64}` returns
`{code, ok, timedOut, stdout | stdoutBase64, stderr, error, truncated,
attempts, write, bypassed, queuedMs}`.

- `write` (ORed with the runner's own classification) queues the command on
  the core's per-repository lane; reads run outside it. Timeouts: 8 s reads,
  60 s writes, 120 s merges, unless `timeoutMs` says otherwise.
- `env` (at most 1,024 entries, 1 MB) is applied over the core's environment
  and fixed `LC_ALL=C`, `GIT_OPTIONAL_LOCKS=0`, `GIT_TERMINAL_PROMPT=0`,
  `GIT_PAGER=cat`. The desktop sends its own environment so localized output,
  SSH agent and credential helpers match a native run.
- **Desktop routing.** Index writers from worker threads and `PullStore`
  (`routeGitIndexWrite`), detached git (source-control status, change badge,
  `git pull`) and worker reads (`runGitCaptureDirect`) go to the core. The
  GUI-thread call runs on a helper thread while events are pumped. Commands
  issued while a thread holds a desktop lane, and `filter-branch` rewrites,
  stay native. Reads fall back natively on any failure or truncation (32 MB);
  `core/gitReads=false` keeps reads native.
- **Transport.** Workers open a short-lived connection (`cli` hello, one
  request) with a deadline of the git timeout plus the queue wait.
- **Diagnostics.** Each routed command is a `git` background ticket marked
  `[core]` or `[core queue]`.

### Agents page and transcript search

- `core.git.agentDiffStats` computes the Agents page's patch summary, worktree
  status, owned paths, numstat, ahead/behind and merge-tree conflict probe,
  four sessions at a time, streaming one `core.git.agentDiffStat` event per
  session. History-dependent results are cached by (repository, base tip,
  branch tip), up to 8,192 entries, so an unchanged agent costs one
  `git status` and one `rev-parse`.
- `core.agents.search {needle, ids}` searches transcripts eight files at a
  time after the desktop flushes its write lanes.

### Terminals

`TerminalWidget` gets a backend through `TerminalBackend::hooks()`. While the
core is attached (not on Windows), shells run as `core.terminal.*` PTYs: input
as ordered notifications, coalesced resizes, output events with a `reset` frame
before replay. The workspace terminal (persist key `workspace`) and the agent
terminal (persist key `agent`) keep their shells across a desktop restart and
re-attach with a 128 KB replay; orphaned shells are closed on connect. A start
failure falls back to `forkpty`.

### Vitals and stall records

- `core.vitals {pid, watch, detail, disks}` makes the core send
  `core.vitals.sample` once a second: memory, swap, CPU, load, per-path disk
  totals and the desktop process's CPU, resident memory, threads and open
  files. The footer, Stats page tiles and the descriptor-pressure check read
  this cached sample (under 3 s old) instead of `/proc` and `statvfs` on the
  GUI thread. The core samples at most 8 disk paths.
- The stall watchdog stays in Qt; stall entries are pushed with
  `core.diag.stall` and the core keeps the last 100, so `diag.stalls`,
  `diag.stall` and `diag.health` answer natively. `diag.processes` runs `ps`,
  builds the process tree and attributes agent roots from the desktop or from
  `agentproc`.

### Phone `files.*` pull

`files.browse.run` `pull` holds the shared repository lane before starting
git, so a phone pull never races a merge or commit routed to the core; `fetch`
does not take the lane. The 120 s limit starts once the lane is held.

## Rebuild & restart, backups and cleanup

### Rebuild & restart (`internal/update`)

- The desktop decides when to start (Restart buttons, the queue behind running
  agents, Update, the phone) and hands the job to `core.update.start`. The core
  advertises optional capabilities (`installSource`, `asUser`, `prebuilt`,
  `clean`, `autoUpdate`, `quick`); an older core gets the native path.
- **Steps.** For an update: `git remote set-url` and `git pull --ff-only`, or
  `git clone --depth 1` into a missing or empty folder. Then the free-space
  check, clearing the scratch folder through `safedelete`, configure unless the
  cache matches, up to three `cmake --build` passes while sources keep
  changing, the Go mirror-node companion, the `--version` start check, install
  with a `.bak-update` copy, and relaunch.
- **A failed pull stops the run** ("Nothing in <src> was changed or removed").
  The core never deletes or re-clones a checkout
  (`TestFailedPullStopsWithoutTouchingTheCheckout`).
- **Process groups.** Each step runs in its own session; cancel sends SIGTERM
  to the group, then SIGKILL after 1.5 s.
- **Relaunch.** With a desktop attached: release the single-instance lock,
  start the new binary (`setsid`, without `FORKMESH_DESKTOP_INSTANCE`), then
  quit the desktop. Under a matching systemd user service the desktop quits and
  systemd restarts it. With no desktop attached the core starts the desktop
  itself. A run counts as running until a new desktop reports in, or 20
  minutes.
- **Install-mirror lookup.** `GET <catalogUrl>/api/install-source` (30 s) gives
  `{node, repo}`; the clone URL is `<catalogUrl>/forkmesh/<repo>`. With no
  mirror the run stops before any git call.
- **Run as another user.** When the desktop runs as root under `sudo`, every
  step runs as `sudo -u <user> -H …`; cleanup still goes through `safedelete`
  in-process rather than a raw `rm -rf`.
- **Prebuilt auto-update** (`core.update.prebuilt`). Reads
  `.forkmesh/releases/latest/release.json` at the tag commit, checks the tag
  and ancestry, selects the asset by OS and architecture, takes the blob from
  the node's own CAS or downloads it to a `.part` file hashed while streaming,
  and installs only on a SHA-256 match. Any failure removes only the `.part`
  file and falls back to a source build.
- **Clean rebuild.** The build folder is removed with
  `safedelete.RemoveTreeUnder(clientDir, clientDir/build)` after a refusal
  check; symlinks inside are unlinked, not followed.
- **Auto-update timer.** In the core: first check 5–15 minutes after start,
  then hourly ±10 minutes. Gates in order: the desktop delegated and is
  attached, `update/autoUpdate` (default off) and `update/autoUpdateRestart`,
  no check or rebuild in flight and no running agents, a checkout with
  `CMakeLists.txt`, a newer tag not yet contained in `HEAD`. The desktop then
  starts the prebuilt or source update.
- **Quick update** (`kind: "quick"`) pulls with fallbacks or clones, then
  builds, installs and relaunches; a failed step stops before the build.
- **Phone family.** `update.status/unwatch/prepare/run` are native once the
  desktop has sent `core.update.config` (300-line backlog, batched progress).

### Backups (`internal/backup`)

Hourly snapshots (`tar` to `.part`, rename, prune to the kept count), restore
unpack into a temp import folder (the desktop finishes the restore), export,
cancel, and `diag.backups`. The desktop pushes its settings with
`core.backups.config` and stops its own timer while attached.

### Cleanup (`internal/cleanup`)

- `core.cleanup.removeWorktree`: linked-worktree check, dirty worktrees kept
  unless force-removed through `safedelete.ForceRemoveWorktree` after a
  `stash create` backup to `refs/forkmesh/trash/cleanup/<date>/…`, branch
  deleted with `update-ref -d` against the saved tip.
- `core.cleanup.removeSandbox`: refuses sandboxes claimed by the desktop or by
  the core's own Actions runs; removes through `safedelete`.
- **Disk Cleanup scanner** (`core.cleanup.scan`, `.cancel`, `.state`,
  `.deleteFiles`, `.appendLog`): a rule-for-rule port of `CleanupScanner.cpp`
  and `CleanupPlan.cpp` (five categories, four statuses, `/proc` snapshot,
  disk-block measurement, git reads without the index lane). The desktop sends
  the context only it knows (folders, repositories, agent sessions, claimed
  sandboxes). Deletes re-apply the plan refusals and remove each member with
  `safedelete.RemoveTreeUnder`; nothing inside the project's `data`, `config`,
  `ssh`, `state` or `logs` folders is removed. Outcomes are appended to
  `<logs>/cleanup.log`.

### Guarded deletes (`internal/safedelete`)

A port of `server/safedelete.go`; `parity_test.go` runs the server's refusal
table against it. `worktree.go` adds the linked-worktree guard for
`git worktree remove --force`.
`app/tests/test_no_unguarded_destructive_deletes.py` scans `desktop/` (the core included)
and checks the core's disposable-folder list against the other helpers.

## Agent processes

`internal/agentproc` runs Claude Code, Codex app-server and managed-runner
agents in the core instead of as `QProcess` children of the UI, so their I/O
leaves the UI thread and they survive a desktop restart.

- **Shim.** `core.agentproc.start` runs `forkmesh-core agentproc-shim <dir>`,
  a session leader that owns the agent (its own process group). With
  `systemd-run` available, the shim runs in a transient scope
  `forkmesh-agent-core-<label>-<core pid>-<serial>.scope` with the agent memory
  limits, so it leaves the desktop's and core's cgroups.
- **Spool.** The shim writes stdout and stderr to
  `state/core/agentproc/<key>/out.<offset>` and `err.<offset>` (4 MB segments)
  and notifies the core over `<dir>/s` (a Unix socket with 5-byte frames; on
  Windows loopback TCP authenticated by a 192-bit token in `tcp.json`). The
  socket also carries stdin, close-stdin, signals, kill and stop.
- **Records.** `run.json` (pids, start time, boot id), `exit.json` (code,
  signal, final lengths, written before the exit is reported), `spec.json`
  (never the environment), `meta.json`, `ack.json`.
- **Core restarts.** A new core scans `state/core/agentproc`, reconnects to
  live shims, marks dead shims without `exit.json` as `lost`, and drops exited
  entries after 7 days. Only `core.shutdown {"hard":true}` stops agents.

| Method | Params → result |
| --- | --- |
| `core.agentproc.start` | `key`, `program`, `args`, `env`, `cwd`, `label`, `detachable`, `merge`, `scope`, `meta`, `attach`, `window`, `skipStdout`/`skipStderr`, `secrets` → `{key, pid, shimPid, unit, contained, parsed, parsedStderr}` |
| `core.agentproc.write`, `.closeStdin`, `.kill`, `.signal`, `.stop` | stdin and signals; `stop` takes `graceMs` (default 1500) and `forget` |
| `core.agentproc.list`, `.attach`, `.detach` | list processes; replay from offsets then stream |
| `core.agentproc.ack`, `.flow` | persist acknowledged offsets (segments below are deleted); flow-control window |
| `core.agentproc.setMeta`, `.forget` | merge meta; remove the module's files once exited |

Events `core.agentproc.stdout/stderr {key, data, offset, end}` (with `gap` for
trimmed bytes) and `core.agentproc.exit` go only to attached peers.
Non-detachable processes belong to the connection that started them and stop
with it; adopted orphans are stopped after 60 s if no desktop claims them.
Unacknowledged output is kept, capped at 512 MB per stream.
`FORKMESH_DESKTOP_INSTANCE` is stripped from the agent environment.

**Facade.** `AgentProcess` is the QProcess-shaped interface the session classes
hold; `CoreProcess` is the core-backed implementation (queued ordered writes,
automatic re-attach, local `QProcess` fallback), enabled by
`core/agentProcesses` (default on).

**Re-attach.** In core mode each stored event carries `_so` (the stdout offset
where its line ends) and `_sk` (the process key). At startup sessions with a
live core process keep their status instead of being re-queued; on connect the
desktop re-attaches at max(ack, largest stored `_so`), so output produced while
it was down replays in order exactly once. If the core does not hand a session
back within 30 s, the session is resumed the old way.

| Scenario | Claude runs, Codex turns, managed-runner agent phase | Idle Codex, runner worktree phase, dialog sessions |
| --- | --- | --- |
| Desktop restart, update, crash | keep running; re-attached | stopped with the connection; resumed |
| Core upgrade, crash, linger exit | keep running; adopted by the next core | keep running; re-claimed within 60 s |
| `systemctl --user restart` of a desktop service | keep running when a scope was used | resumed |
| Hard core shutdown, logout, reboot | stopped; resumed | stopped; resumed |
| Core off or unresponsive | resumed as before | local `QProcess` |

## Agent engine

The core is the only writer of `data/agents/**` and parses agent output itself.

### Store writer (`internal/agents`, `core.agentstore.*`)

A byte-exact port of `AgentStore.cpp`: ordered event and transcript lanes,
tombstones, atomic `meta.json` (0600, `QJsonDocument::Indented` form via
`internal/qtjson`), identical-save skipping, `nextId`, recycle bin with
restore and purge through `safedelete`, `model-outcomes.json`.

| Method (desktop and CLI only) | Params → result |
| --- | --- |
| `core.agentstore.root` | → `{root}` |
| `core.agentstore.create`, `.save` | `session` → record, `{saved}` |
| `core.agentstore.delete`, `.recycle`, `.restore`, `.purge`, `.purgeExpired` | record lifecycle |
| `core.agentstore.appendLog`, `.appendEvent`, `.clearEvents` | ordered notifications per lane |
| `core.agentstore.writePatch`, `.sync`, `.outcomes` | patch write; lane barrier; outcomes |

`AgentStore` keeps its API; a store routes to the core only when its canonical
root equals the core's. Each transcript lane keeps its own blocking connection
so append order holds per lane.

### Parsed runs (`internal/agentrun`, `core.agentrun.*`)

- A process whose meta has `parse: claude`, `parse: codex` or `parse: log` is
  consumed from its spool by the core; the desktop skips that stream.
- **Claude.** Framing (8 MB frames), `_so`/`_sk`, credential redaction,
  account and timestamp tagging, events and transcript text, plus resume id,
  idle/waiting state and usage.
- **Codex.** The app-server JSON-RPC session lives in the core (`initialize`,
  `model/list`, `thread/start`/`resume`, `turn/start`/`steer`, approvals and
  user-input requests), driven by `core.agentrun.codex {key, op}` and
  snapshotted into the process meta so it survives a core restart.
- **Managed runner (`log`).** Merged output decoded (split UTF-8 kept),
  redacted with the run's API key (held in memory only, never in `meta.json`)
  and the pushed provider credentials, appended to `transcript.txt`.
- **Dialog sessions** use transient parses: nothing is stored, the last 4,096
  events are kept in memory for replay, and the process stops with its
  desktop connection.
- **Credentials.** The desktop pushes provider credential values
  (`core.agentrun.credentials`, memory only); until it has, the core parses and
  stores nothing, so nothing is written unredacted.
- **Stderr.** Read, redacted and offered to subscribers with `stderr: true`;
  replay of at most the last 256 KB.
- Events: `core.agentrun.event {key, so, line}` and `core.agentrun.exit`, sent
  after the last event is written.

The agent terminal, the external-call gate (a stable socket path
`$TMPDIR/forkmesh-agent-gate-<user>-<hash>`) and the IDE bridge (previous port
and token) are stable across restarts so re-attached runs reconnect.

## Agent session model

`internal/agentmodel` owns the agent read model and the phone protocol for
agents, runs background-work completion checks, and keeps answering while the
desktop is closed. Launching (prompt building, worktrees, CLI arguments,
accounts) remains in the desktop.

- **Sessions** come from the core's store, cached and invalidated by writer
  observers (never older than 2 s).
- **Desktop context.** `core.agents.model.push` carries what only the desktop
  knows: unread flags, diff stats, outside-session rows, repository records,
  providers, the queue order and limit, pending approvals and grants, with a
  sequence number. Approvals and grants are fetched fresh
  (`ui.agents.context`); usage inputs come from `ui.agents.usage`.
- **Follows.** `agents.follow/unfollow/output`: at most 8 per connection,
  windowed log reads, 15 ms (LAN) and 80 ms (relay) spacing, paused while the
  relay socket has a backlog.
- **Background work after a turn.** `core.agents.background.watch` polls
  `/proc` for processes a finished turn left running (CPU ticks decide busy;
  8 s idle or 15 minutes overall stop stragglers with SIGTERM then SIGKILL).
  Without a desktop the core writes the notices and marks the session Success.
  Linux only.
- **Without a desktop.** Lists, queue, follows, transcripts, diffs, files,
  worktrees, merge stats and the recycle bin still answer; `agents.stop` stops
  the session's processes and saves it Stopped; `agents.queue.move` updates
  priorities. Starting or prompting answers `desktop_unavailable`.
- **Core-confirmed requests.** For actions the desktop executes, the core
  checks and spends the token, then delegates with `_coreConfirmed: <secret>`;
  the desktop skips its own check only when the secret matches and the request
  arrives on a core connection. The core strips any `_coreConfirmed` a phone
  sends.
- `agents.attachment` previews stay proxied: the Qt JPEG re-encode cannot be
  reproduced byte for byte.

## Actions

`internal/actions` runs the `.forkmesh` workflow runtime.

- **Parsing** (`workflow.go`): `ActionFile` rule for rule, including path
  wildcards, labels, pins, `needs` and `defaultBranchRetiresPush`.
- **Run store** (`store.go`): `actions/runs/<v2-key>/<id>/meta.json` and
  `log.txt`, written in `QJsonDocument::Indented` form so either side reads the
  other's history.
- **Runner.** Checkout worktree or disposable clone, approved-snapshot
  verification, steps, quota check, artifacts and release landing, cleanup.
  Steps run as `systemd-run --user --scope … -- bwrap … /bin/bash -se` with the
  step script on stdin, unit `forkmesh-action-<core pid>-<run>-<step>.scope`,
  16 KiB log chunks, 1 MiB cap, secret redaction. Resource limits are applied
  by a re-exec helper (`/proc/self/exe __forkmesh-actions-exec`) because Go
  cannot run code between `fork` and `exec`.
- **Sandbox claims.** Trees are claimed in memory and by `flock` on
  `<tree>/.forkmesh-claim`; both the core and the desktop skip trees another
  process holds. Deletes go through `safedelete`.
- **Run queue.** Armed by `core.actions.queue.attach` with a repository
  snapshot and kept for the life of the core. Every 4 s it scans external
  sources, `*.push` spool files and served heads, then runs the queue pass:
  push, release, pull-request and manual triggers, path filters, node pins,
  superseding, first-time pull-request authors held for approval, `needs`
  ordering, a runner pool of `clamp(NumCPU, 2, 4)`. Approvals and served heads
  are written through `WriteSettings` (a head whose write fails is not queued).
- **Pull-request checks** (`core.actions.queue.pullChecks`). Pull records are
  read from the ledger by `internal/pulls`; heads are published into the
  served mirror (`refs/pr/<n>/head`) with `--force-with-lease`; at most 25
  resolved and 4 queued per pass with a 60 s rescan floor.
- **Reactions.** Runs the queue starts are followed from events
  (`actions.started`, `.log`, `.step`, `.status`, `.release`, `.finished`).
  Release landing into the served mirror runs in the core. Agent auto-fix for a
  failed run is decided in the core and delivered with `ui.actions.autoFix`
  (queued while no desktop is attached).
- **Mirror Actions configuration** (`core.actions.mirrorConfig.apply/recover/reset`).
  A transactional settings update with a journal (schema 2) and a
  `QLockFile`-compatible lock. Pushed variables are sealed in the vault
  atomically with the settings change; the journal never holds a value.
  `forkmesh --configure-mirror-actions-stdin` on a mirror host delegates to a
  running core.
- **Phone methods.** `repos.list`, `repos.actions`, `repos.actions.log`,
  `repos.prepare`, `repos.run` are native; `workflow.enable/disable` asks the
  desktop because it owns the repository record.
- **Restarts.** An active run keeps the core `Busy()`. On reconnect the desktop
  re-attaches active runs, replays finishes that happened while it was away,
  and requeues the rest (`RESTARTING`).

## Merge queue and pull store

- **`internal/pulls`** is the full `PullStore`: pull list loading (with a load
  stamp so unchanged ledgers answer `unchanged`), signed comments, reviews,
  threads and branch reviews (signed by the core with the node identity; a key
  mismatch with the desktop answers `identity_mismatch` and the desktop signs
  natively), create, status, rewards, delete, inbox intake, conflict and edit
  sessions, review checkouts. Ledger read-modify-commit sections hold a
  per-checkout lock.
- **The history rewrite stays in the desktop.** Dropping pull payloads from
  ledger history uses `git filter-branch`, `git reflog expire --expire=now
  --all` and `git gc --prune=now`, which affect the whole repository's object
  store and reflogs. The core therefore asks the desktop
  (`ui.pulls.rewrite`, `ui.mergequeue.purge`), which accepts only
  `.forkmesh/pulls/<n>` paths.
- **`internal/mergequeue`.** `mergequeue.list/prepare/run` with the C++
  policy, and the executor (`core.mergequeue.schedule`): only the open
  repository, peer-review gate, update from base, conflict detection, one merge
  per pass, all on the core's git lane. Repository records stay desktop state
  (`ui.mergequeue.view`, compare-and-set writes through `ui.mergequeue.apply`).
- Desktop methods: `core.pulls.*`, `core.mergequeue.*`; event
  `core.pulls.changed`.

## System log digest and repository quality

- **`stats.timeline`** (`internal/netlog`). The desktop appends every system
  log line to `data/network_log.txt` (rotated into `network_log.d/` past 40 000
  lines, 30-day retention) and keeps a 15-minute bucket digest in
  `network_log.d/digest.tsv`. The core takes complete buckets from the digest
  and counts newer lines from the log incrementally, with the desktop's badges
  supplied by `core.log.append` (batched every 50 ms). The badge classifier is a
  port of `logBadgeFor`. Reply: `{days:[{dayTs, errors, warnings, info}]}`,
  1 to 30 days.
- **`stats.quality`** (`internal/quality`). A port of `RepoQuality::scan`
  over the writable checkout or the mirror: root docs, source and test paths,
  `TODO|FIXME|HACK|XXX` markers, long files, recent activity, open issues and
  Actions runs from disk.

## Secrets at rest (the vault)

`internal/vault` keeps Actions variables and other secrets encrypted in
`data/vault/secrets.json`; `ForkMesh.conf` keeps only names with the marker
`[stored in the ForkMesh vault]`.

### Key

Every value is sealed with one random 256-bit data key, stored only wrapped
(AES-256-GCM) under a key-encryption key derived from the node identity:
`HKDF-SHA256(ikm = Ed25519 signature of "forkmesh-vault-kek-v1\n" + public key,
salt = 32 random bytes per wrap, info = "forkmesh-vault-kek-v1")`. Ed25519
signatures are deterministic, so the same identity always derives the same key
and nothing derived is written down. Up to 8 wraps can coexist (an identity
change adds a wrap for the new identity), and the data key can be rotated
(`core.vault.rotate`). No OS keychain is used; the identity key is the root of
trust.

### File format (`forkmesh.vault`, version 1)

| Field | Meaning |
| --- | --- |
| `kind`, `v` | `forkmesh.vault`, `1`; anything else is refused (`vault_invalid`) |
| `keyId` | 16 hex: `HKDF(dataKey, info "forkmesh-vault-key-id-v1", 8 bytes)` |
| `generation` | Incremented on every write; inside the MAC |
| `wraps[]` | `{scheme "ed25519-sig-hkdf-sha256-v1", identity, salt, nonce, key}`; AAD `"forkmesh-vault-wrap-v1\0" + identity + "\0" + keyId` |
| `entries{}` | `"<namespace>/<percent-encoded name>": {nonce, data, at}`; key `HKDF(dataKey, info "forkmesh-vault-entry-key-v1")`; AAD `"forkmesh-vault-entry-v1\0" + keyId + "\0" + entry name` |
| `mac` | HMAC-SHA256 (key `HKDF(dataKey, info "forkmesh-vault-mac-v1")`) over version, key id, generation and every entry |

Limits: 1 MiB per value, 4096 entries, 32 MiB per file. Writes use a temp file
(0600), `fsync`, rename and folder `fsync`; the folder is 0700 and a symlinked
path is refused. Only the core writes the file. Buffers holding keys and
plaintext are zeroed after use; the desktop caches decrypted values for at most
30 s.

### Namespaces and migration

| Namespace | Entries | Plaintext it replaces |
| --- | --- | --- |
| `actions` | Actions variables | `actions/variables` in `ForkMesh.conf` |
| `desktop` | `chat/e2ee/x25519PrivateV2`, `genie/token` | The same keys in `ForkMesh.conf` |
| `release` | `apk-keystore-password`, `windows-cert-password` | Password files under `data/release/` |

Migration seals pending plaintext in one atomic write (a vault entry newer than
the plaintext is kept), reads every value back and compares, restarts if the
settings changed meanwhile (at most 3 times), then replaces the plaintext with
markers. It is resumable: without a desktop the marker write waits for the next
attach, and readers merge both sources in the meantime (newer non-marker
plaintext wins). Release password files are replaced atomically with the
marker, keeping their mode; key containers (`.pfx`, `.jks`) stay files. Log
lines carry counts and names only.

### Access

| Method | Who | Does |
| --- | --- | --- |
| `core.vault.status`, `.list` | UI, local CLI | State (`empty`, `ready`, `locked`, `tampered`, `invalid`, `no_identity`), names |
| `core.vault.migrate`, `.rotate` | UI, local CLI | Run the migration; new data key |
| `core.vault.get`, `.set`, `.delete` | Desktop process only | Values |

"Desktop process only" means the UI connection, or a `cli` connection whose
kernel-verified peer pid (`SO_PEERCRED`, `LOCAL_PEERPID`) equals the UI
connection's; `forkmesh-core call core.vault.get` from a shell or agent is
refused. The event `vault.changed` carries names only. Without a core,
`VaultFile.cpp` (C++) and `tools/forkmesh_desktop_vault.py` read the vault
read-only with the same derivation; writes are refused rather than stored in
plaintext.

### Threat model

Protects against a copy of `ForkMesh.conf` or the config folder, edited or
truncated vault files, swapped entries, a different identity, and casual
`forkmesh-core call` access. Does not protect against anything that can read
`data/identity/ed25519.pem`, a copy of the whole `data/` folder, rollback to an
older complete vault file, values in freed process memory, or plaintext written
before the migration.

## Independent lifecycle

The desktop and the core are managed separately: the window can close, crash
or restart while the core keeps phone channels, host jobs, terminals, agents
and service plug-ins running, and the next window re-attaches to the same core.

| Mode | How it starts | When it stops |
| --- | --- | --- |
| Default | The desktop launches it on demand | 2 minutes after the last window detaches, once no module is busy |
| Background | `serve --background`, or `background: true` in `config/core/daemon.json` (Settings → General → STARTUP, `forkmesh-core background on`) | Only on `forkmesh-core stop`, `core.shutdown`, a signal, or log-out |
| Login start | `forkmesh-core autostart enable` or the STARTUP checkbox; implies `--background` | As background |

- **Login start** (`internal/autostart`, `core.daemon.set {autostart, binary}`):
  - Linux: a systemd user unit `~/.config/systemd/user/forkmesh-core-<project>.service`
    (`Restart=on-failure`, `RestartSec=5`, at most 5 starts in 10 minutes,
    `KillMode=process` so agents started by the core outlive a core restart,
    `Nice=5`, idle I/O class, `NoNewPrivileges`, the ForkMesh path variables
    unset), enabled with `systemctl --user enable`. Without a user systemd it
    writes `~/.config/autostart/forkmesh-core-<project>.desktop`.
  - macOS: `~/Library/LaunchAgents/com.forkmesh.core.<project>.plist`
    (`RunAtLoad`, `KeepAlive` on unsuccessful exit, `ProcessType`
    `Background`, low-priority I/O), loaded with `launchctl bootstrap`.
  - Windows: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\forkmesh-core-<project>`;
    `--hide-console` detaches the console window.
  - Paths are quoted for each format (tested with spaces, `%`, `$`, quotes and
    backslashes); the project must be a valid slug and the binary an existing
    absolute path. Disabling removes only the entry it wrote.
- **Single instance.** `core.lock` is an exclusive lock on every platform
  (`flock`; `LockFileEx` on Windows, which previously only opened the file). A
  second `serve --background` exits 0 so a supervisor does not restart-loop
  against a core the desktop already started.
- **Crash recovery.** `state/core/running.json` is written at start and removed
  at a clean stop (which writes `stopped.json`). A marker left behind means the
  previous core died; the new one logs it and reports
  `lastExit: {kind: "crash", pid, version, startedMs}` in `core.status` and
  `core.daemon.get` (`first` and `clean` otherwise). The desktop relaunches a
  core that vanishes; under a supervisor the unit restarts it.
- **In-place upgrade.** With no desktop attached and nothing busy, the core
  checks every 10 s whether its executable was replaced. When the new file has
  been stable for two checks and `<binary> version` runs, it stops cleanly and
  re-executes itself (same pid on Unix; a new detached process on Windows). A
  binary that does not run is ignored until it changes again.
- **Idle footprint.** A minute after the last desktop detaches with nothing
  busy, the core returns freed memory to the system once
  (`debug.FreeOSMemory`). Module busy checks only run while no desktop is
  attached.
- **Methods (desktop and CLI only).** `core.daemon.get`; `core.daemon.set
  {background?, autostart?, binary?}` (answers the new state and broadcasts
  `core.daemon.changed`; errors `bad_binary`, `bad_project`,
  `autostart_failed`, `config_write_failed`); `core.daemon.restart`.
- **CLI.** `forkmesh-core start` (detached background core, waits until it
  answers), `stop`, `status`, `background on|off|status`,
  `autostart enable|disable|status [--binary <path>]`.
- **Desktop.** Settings → General → STARTUP shows "Keep the background engine
  running after ForkMesh closes", "Start the background engine when you log
  in", the core's version, pid and last stop, and one checkbox per service
  plug-in (enabling asks for consent and lists its permissions)
  (`CoreDaemonSettings`).

## Service plug-ins

Marketplace packages with a `service` block run inside `forkmesh-core` as
separate sandboxed processes, so their work continues without the window and
the owner's phones can use them as mini-apps. Signatures, consent, the
sandbox, the protocol and the methods are in
[service-plugins.md](service-plugins.md); the phone side is
[native/ios/docs/FMMiniApps.md](../../native/ios/docs/FMMiniApps.md).

## Desktop package modules

Optional marketplace packages that only talk to the window through
`NativePackageRegistration` are built as separate shared modules instead of
being linked into `forkmesh` (`cmake/ForkMeshPackageModules.cmake`,
`src/PackageModules.{h,cpp}`).

- **Build.** `FORKMESH_PACKAGE_MODULES` (default on for Linux, off elsewhere)
  and `FORKMESH_PACKAGE_MODULE_IDS` (default: app-store-status, aquarium,
  backups, calories, computer-security, crm, marketing, people, rss-reader,
  sleep, steps, stretch, sugar-break, voice-changer, water). Each becomes
  `build/packages/<id>.so` with a sidecar `<id>.json` (`api`, `abi`, `package`,
  `file`, `entrypoints`); configure removes module files the configuration no
  longer builds. Modules resolve the app's symbols through the `-rdynamic`
  export the stall watchdog already needs, so the app binary does not grow.
  Stay compiled in: packages installed by default (agent-watch, analytics,
  docs: as modules they were loaded at every start and cost about 30 ms and
  3 MB, see [Measurements](#measurements)), packages with a `desktopFactory`
  (sentry, vscodium), and the ones the window references directly. Sources the
  window calls from a modular package stay in the app through
  `FORKMESH_PACKAGE_HOST_SHARED_SOURCES` (agent-watch's
  `AgentNetworkSampler.cpp`, for builds that list agent-watch).
- **Loading.** At start the app reads only the sidecars (from `packages/` beside
  the binary, `../lib/forkmesh/packages`, the install prefix's
  `lib/forkmesh/packages`, and `../PlugIns/forkmesh-packages` on macOS). A
  module is loaded the first time `nativePackageContribution()` asks for its
  entrypoint, which happens only for packages installed on this desktop: a
  package that is not installed is never mapped.
- **Versioned API.** The sidecar's `api` must equal the app's
  `FORKMESH_PACKAGE_MODULE_API` (1) and its `abi` the app's build identity
  (version, compiler, Qt, build type), checked before `dlopen`. After loading,
  the exported `forkmesh_package_module_api/abi/id` must match again, or the
  module's entrypoints are unregistered. A refused or broken module only
  removes that package's page; the log says why (`[package module] …`).
- **Trust.** Modules load only from the application's own folders, never from
  user-installed marketplace content. A module file or its folder that another
  user (or a group other than the user's own) can write is refused.
- **Install.** `install.sh` copies the modules to `~/.local/lib/forkmesh/packages`
  and removes ones the build no longer produces; Rebuild & restart / auto-update
  (`internal/update`) does the same beside the installed app; `cmake --install`
  puts them in `<libdir>/forkmesh/packages`.
- **Test.** `ctest -R forkmesh-package-modules-resolve` checks that every
  module's undefined symbols are provided by the app or its own libraries, so a
  new window-to-package reference fails the build's tests instead of the user's
  start.

## Measurements

Taken 2026-10-15 on the 36-core Linux build box while other agents were
building (load average 9–33). Debug builds with `-O0 -g0`, the same stage, run
offscreen in a fresh sandboxed `HOME` (no packages installed), interleaved
A/B: "compiled in" is `-DFORKMESH_PACKAGE_MODULES=OFF`, "modules" the default.

| | Compiled in | Modules | Change |
| --- | --- | --- | --- |
| `forkmesh` file | 206.4 MB | 188.8 MB (+ 29.7 MB in 15 modules, loaded only when installed) | −8.5 % |
| `forkmesh` text segment | 129.2 MB | 118.6 MB | −8.2 % |
| Resident memory 8 s after start, median of 12 | 178.5 MB | 175.2 MB | −3.3 MB |
| Window shown (`[startup +Nms]`), median of 12 | 608 ms | 632 ms | within noise at this load |
| Editing a module package's source | recompiles the file, relinks the app (2.7 s link) | recompiles the file, relinks only its module | no app relink |

An earlier list that also modularised the three packages installed by default
measured +28 ms and +3 MB (n = 15), which is why they stay compiled in.

Idle `forkmesh-core` with no desktop attached, 75 s, same machine, side by
side:

| | Before (`HEAD` of 2026-10-15) | After |
| --- | --- | --- |
| CPU time | 7 ticks (70 ms) | 3 ticks (30 ms) |
| Resident memory | 21.8 MB | 20.8 MB |
| Files opened in 20 s | 115 (1 Hz vitals sampler) | 0 |

The idle saving comes from the vitals sampler: with no watcher and no desktop
it samples every 30 s instead of every second, and its 30-second history starts
fresh when someone watches again.

## Configuration

`FORKMESH_CORE=0` / `core/enabled` (off switch), `FORKMESH_CORE_BIN` (binary
path), `serve --linger`, `serve --background`, `core/agentProcesses` (agents in
the core, default on), `core/gitReads` (git reads through the core, default
on), `core/vaultFinalize` / `FORKMESH_VAULT_FINALIZE`. The core's own settings
live in `config/core/daemon.json` (`background`), which the core alone writes
(`core.daemon.set`, or the file directly through `forkmesh-core background`
while no core runs). CMake: `FORKMESH_PACKAGE_MODULES` and
`FORKMESH_PACKAGE_MODULE_IDS`.

## Build and test

- CMake builds the core with the app (target `forkmesh-core-go`, output
  `forkmesh-core` beside `forkmesh`, `-X main.version=${FORKMESH_VERSION}`)
  when `go` is on the PATH; without Go the app builds and runs without the core.
- `install.sh` installs it to `~/.local/bin/forkmesh-core`; `cmake --install`
  puts it beside the app (inside `ForkMesh.app/Contents/MacOS` on macOS).
- Standard library only, Go 1.24.
- Tests:

  ```sh
  cd desktop/core && env -u FORKMESH_HOME -u FORKMESH_PROJECTS_DIR -u FORKMESH_DATA_DIR \
      -u FORKMESH_CONFIG_DIR -u FORKMESH_PROJECT -u FORKMESH_PROJECT_DIR go test ./...
  ```

  The desktop exports all of those to agent shells; the path tests resolve in a
  temp `HOME` and compare with `tools/forkmesh_paths.py`. Golden tests compare
  against fixtures generated from the real C++ classes (`testdata/gen.sh`);
  opt-in parity tests (`FORKMESH_ACTIONS_PARITY_BIN`,
  `FORKMESH_VAULT_PARITY_BIN`) drive harnesses linked against the C++ sources;
  `TestLiveDesktop` in `internal/control` (opt-in via `FORKMESH_LIVE_MESH`,
  `FORKMESH_LIVE_SEED`, `FORKMESH_LIVE_CALLS`) drives a sandboxed desktop as a
  paired phone.
- Lifecycle tests: `cmd/forkmesh-core/daemon_integration_test.go` builds the
  binary and runs it in a temp `HOME` with no desktop (background start, CLI,
  crash record, background setting against linger, in-place upgrade);
  `go test -short` skips them. Unix sockets need a short temp path: run with
  `TMPDIR` set to a short folder when the checkout is deep.
- Desktop end to end: `tools/desktop_reconnect_test.sh <forkmesh> <forkmesh-core> [sandbox]`
  starts the real desktop offscreen in a sandboxed `HOME` and checks that the
  core survives a desktop crash, that a new desktop re-attaches to the same
  core, that a service plug-in keeps its process across the UI restart, that the
  desktop relaunches a killed core (which reports the crash), that background
  mode keeps the core after the window quits, and that `stop` ends the core and
  its plug-ins.
- Package modules: `ctest -R forkmesh-package-modules-resolve`.
- Manual checks: `forkmesh-core status`, `forkmesh-core call core.routes`,
  `forkmesh-core call core.vitals`, `forkmesh-core plugins list`.

## Operations

- Log: `logs/core/forkmesh-core.log`, rolled to `.1` at 8 MB; service
  plug-ins log to `logs/core/plugins/<id>.log` (1 MiB, rolled once).
- The desktop's system log shows core starts, connects, restarts and a missing
  binary.

## Security

- **Access.** The socket is owner-only, and every connection (socket or TCP)
  needs the token from the 0600 endpoint file; on Linux a socket peer must be
  the same user. `core.*` methods refuse phone and relay peers.
- **Service plug-ins** run only when signed by a trusted key and enabled by the
  user, in a separate process with a reduced environment, resource limits,
  `no_new_privs` and (Linux) no network unless granted; phones reach only
  methods the manifest lists, and only the owner ([service-plugins.md](service-plugins.md)).
- **Package modules** (in-process native code) load only from the app's own
  folders and only when API and build identity match.
- **Phone channel.** Same handshake, keys and approval rules as the C++;
  account checks go through the desktop's account session
  (`ui.control.accountCheck`).
- **Deletes.** Recursive deletes go only through `internal/safedelete`;
  `git worktree remove --force` only through `safedelete.ForceRemoveWorktree`
  after dirty work is saved to a trash ref. Otherwise the core removes only its
  own socket and endpoint files, single files it wrote, and stale git `*.lock`
  files no process holds that are at least 60 s old
  ([git-locking.md](git-locking.md)).
- **Secrets** are encrypted at rest in the vault and never logged.

## Traps

- **Lanes.** Commands issued while a thread holds a desktop git lane still use
  `GitIndexQueue`; git's `index.lock` and the shared retry rules settle a
  collision with the core.
- **Settings lag.** Lookups that read `ForkMesh.conf` (`stats.repo`,
  `files.browse.roots`, `insights.shortcuts`) can trail the desktop's memory
  until Qt flushes.
- **Two writers of shortcut counters.** The core applies its operations to the
  values it reads just before writing; a desktop write in that round trip can
  be lost.
- **Firewalls.** Network traffic the core now makes (relay socket, host jobs,
  downloads) comes from the `forkmesh-core` binary path.
- **Two cores under a supervisor.** A login-started core and a desktop-started
  one cannot both run (the lock); the second exits 0. If the desktop started the
  core first, the systemd unit stays inactive until the next login.
- **Stale modules after a manual copy.** Copying only `forkmesh` over an
  installed app leaves modules from the old build; they are refused (ABI
  mismatch) and those package pages disappear until `install.sh` or an update
  installs the matching modules.

## Not yet in the core

- Package modules are Linux-only by default; macOS and Windows build the same
  packages into the app until their symbol export (`-undefined dynamic_lookup`,
  an import library for the executable) is set up and tested.
- Network isolation for service plug-ins exists only on Linux; macOS
  (`sandbox-exec`/App Sandbox) and Windows (job objects, AppContainer) run them
  with the reduced environment only.
- Agent launching: the queue pump, parking, usage-limit holds, resume,
  continue, takeover and handoff.
- `teammates.*` (except `options`), `insights.usage`, `wallet.*`,
  `discord.*`, `pings.*`, `stats.sentry`.
- Mirror push follow-ups after a managed release fan-out, and the push
  triggers.
- The QUIC leg of host log feeds.
- `PullStore` strict loads (`loadAllStrict*`) and the payload history rewrite
  (by design).
- The settings half of mirror Actions startup recovery in `main.cpp` (it runs
  before any desktop is attached).

## Related docs

[networking.md](networking.md#phone-control-over-the-lan) (the phone protocol),
[lifecycle.md](lifecycle.md), [git-locking.md](git-locking.md),
[../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md).
