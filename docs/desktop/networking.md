# Desktop: networking

Part of [the desktop docs](README.md).

## Relay transport

- `src/ServerNode.*` and `src/ChatBackend.h` hold the relay HTTP and WebSocket
  connection; `src/BackoffNetworkAccessManager.*` and `src/NetworkBackoff.h`
  back off per host on 429/5xx. A reply the relay marks
  `x-forkmesh-expected-degraded` is about one route (e.g. a per-account quota)
  and opens no host cooldown. A suppressed request fails locally naming the
  status that opened the cooldown (`NetworkBackoff::lastFailureStatus`).
  Failures arriving during a cooldown (from requests sent before it) do not
  extend it unless `Retry-After` asks for longer.
- `NetworkReplyError.h` marks replies the caller expects: the relay's
  `x-forkmesh-expected-degraded`, `markExpectedHttpStatus()` and
  `markExpectedTransportFailure()` (for third-party servers). The net log then
  writes `EXPECTED` instead of `ERR`, and the line raises no error ping.
- `markPrivatePayload()` (`NetworkReplyError.h`, attribute `User + 7`) keeps a
  request's bodies out of the endpoint stats and the archived network log:
  `BackoffNetworkAccessManager` records `<private request body: N bytes>` and
  `<private response body: N bytes>` instead. The URL, status and firewall
  decision are still recorded. Use it when the body itself carries a password,
  session id or key, as the OpenWrt Router package's ubus calls do
  ([../marketplace/openwrt.md](../marketplace/openwrt.md)).
- The account event socket delivers topics that replace polling;
  `src/MainWindowRepoSync.cpp` dispatches them (e.g. `cloud-agents`, `tasks`).
- A relay instance is a security boundary: repository lists, bearers, chat and
  queued agent work are scoped to one relay origin
  ([www/docs/qt-client.md](../../www/docs/qt-client.md#multiple-relay-instances)).
  `src/RelayRepositoryScope.h` decides which relay owns a repository record.
- The desktop signs requests with the node key and sends no `Origin` header, so
  routes that require a browser session token do not work from it.
- Frame loops (`ServerNode::onSocketReadyRead()`,
  `NodeEventSocket::drainReadBuffer()`) stop once the socket they started with
  is gone, since any handler may drop it. Test:
  `forkmesh-chat-transport-tests`.
- Cross-node pull submission (`submitPullToInbox`, `src/MainWindowPulls.cpp`)
  is at-least-once: the signed proposal is stored in QSettings
  (`pulls/pendingSubmissionsV1`) before the first POST, keyed by signature, and
  retried with backoff (5 s doubling to 5 min) on network errors, 408, 425, 429
  and 5xx. The relay answers a repeated signature with the original number and
  `duplicate: true`.
- Automatic offline mode (the OS reports no network, or three failed relay
  probes) drops the account event socket, so the phone cannot reach the
  desktop over the internet until it ends, and only a successful
  `probeRelayLatency()` ends it. That probe used to be skipped when a
  WebSocket pong had been seen in the last 90 s, which is always true right
  after a short outage, and its retries backed off to 5 minutes; the other
  trigger, window activation, never fires without a window, so a windowless
  desktop could stay offline for good. While automatic offline mode is on,
  the probe now ignores the 90 s gate, retries at most every 30 s
  (`kAutoOfflineProbeMaxDelayMs`), and the minute tick in `MainWindow.cpp`
  probes as a backstop. Online, nothing new is polled.
- Chat frames are sealed with chat protocol v3 (`src/RoomSeal.*`,
  [chat.md](chat.md)). Trust records, seen counters and held frames are written
  to QSettings two seconds after a change (and at quit), because each settings
  write rewrites the whole file on the GUI thread; approvals and blocks are
  written at once. v2 `sealed`/`e2ee-key` frames are ignored.
- `ServerNode` passes v3 frames through unchanged up to the 96 MiB WebSocket
  limit. Frames the relay marks `historyReplay` before `forkmesh-history-end`
  skip only the already-seen counter check.

## Cloudflare Worker log tail

`MainWindow::setCloudLogMonitorEnabled()` (`src/MainWindowLogs.cpp`) can run
`npx wrangler tail` for the Worker named in `app/wrangler.toml` (setting
`diagnostics/cloudLogMonitor`). The served stack has no Workers: when the tail
reports the Worker missing (`cloudflareTailWorkerMissing()`), the monitor stops,
records the name in `diagnostics/cloudLogMonitorMissingWorker`, and skips the
tail on later launches until the box is ticked again. The Node.js probe runs on
a worker thread once per session.

## HTTP/3 client

QtNetwork stops at HTTP/2. `src/Http3Get.{h,cpp}` is an in-process HTTP/3 GET
client on OpenSSL's QUIC client and `libnghttp3`, driven by a
`QSocketNotifier` and a timer. It is compiled only with OpenSSL QUIC and
`libnghttp3` (`FORKMESH_HAVE_HTTP3`); otherwise `forkmesh::net::http3Available()`
is false. Trust comes from the system CA store (`SSL_CERT_FILE` works for
tests). QUIC's idle timeout is 30 s, so the mirror gateway
(`tools/mirror_gateway.py`, `NODE_LOG_KEEPALIVE_SECONDS`) sends keep-alives
every 10 s. Probe: `forkmesh-http3-probe <url> [Header: value]...`.

## Host logs

**Network → Hosts** live view (`src/MainWindowHosts.cpp`, `src/HostLogSources.h`)
reads the gateway's `/v1/node/logs` server-sent events over HTTP/3 when it can.
Only Ed25519 keys the desktop's host installer wrote to the gateway's allow file
may open the feed. Otherwise it falls back to one SSH connection multiplexing
the node log and the systemd journal, and retries QUIC periodically.

## Fleet node logs

`src/CoreNodeLogs.{h,cpp}` and `CoreLogFeed` in `src/MainWindowLogs.cpp` follow
every fleet node's journal live. Node side: [../NODE_LOGS.md](../NODE_LOGS.md),
`deploy/core/logread.py`. Setting: "Follow the fleet nodes' logs, rqlite
statements and web traffic" (`diagnostics/coreNodeLogFeed`, on by default;
compiled out under `FORKMESH_WINDOW_TESTS`).

- **Why pull over SSH.** Works behind NAT, loses no lines (journald plus a
  resume cursor), each stream is authenticated by its own node's host key, and
  nothing is stored server-side.
- **Discovery.** Fleet config in `~/projects/forkmesh/config/fleet`, keys in
  `~/projects/forkmesh/ssh/fleet` (`FMNEXT_CONFIG_DIR`, `FORKMESH_PROJECTS_DIR`
  override). Nodes come from `fleet-instances.json`,
  `autoscaled-instances.json` and the cloud provider API (instances tagged
  `forkmesh-next`), re-listed periodically, on runner pool changes and when a
  file watcher sees the files change. Fleet nodes are never added to the saved
  Hosts list.
- **Transport.** One `ssh -T` per node with a dedicated reader key
  (`next_logread_ed25519`; `IdentitiesOnly`, `BatchMode`, no agent or
  forwarding) and the fleet `known_hosts`. On the node the key is a
  `restrict,command=` forced command that can only run `fmnext-logread`, which
  drops privileges, redacts messages and speaks `follow`, `follow lines N`,
  `follow after <cursor>` and `hello`. The cursor of the last handled line is
  saved in `<data>/fleet_logs/cursors.json` (0600) from a worker thread.
  Backlog older than 15 minutes (`corelogs::isBacklogEntry`) is logged as
  replayed and raises no ping. Past 20,000 queued lines streams pause and resume
  from their cursor. Retry backoff runs from 30 s to 10 min.
- **Main-log lines** read `Node <node> <core|runner> <unit> <err|warn|info>: ...`
  and are badged by group (`corelogs::nodeLineBadge`): App, Cron, DB, Web,
  Daemon, IDS, Cert, Ops, Job, Mail, System; live errors go under Error.
  Errors are journald err or worse, Caddy error/panic/fatal, Go panics,
  `ERROR`/`CRITICAL` lines and IDS alerts. Python tracebacks fold into one entry;
  identical lines fold for 10 minutes. `corelogs::nodeLineHtml` colours the
  prefix and body and pads them into columns with non-breaking spaces (the log
  views are monospace): node 3, role 6, unit 12, severity 5. Web lines also pad
  the method to 7 and the host and path to 64, then right-align the duration
  (6) and size (7), so the status and the client line up. Only the HTML is
  padded; the stored line keeps single spaces for filters and parsers. A longer
  field pushes the rest of its own line along. Deploy output is followed by
  `src/DeployLogs.{h,cpp}`.
- **Filters.** The **Nodes** button picks nodes, units and **Node errors only**.
- **Web traffic.** Each core's Caddy writes a filtered JSON access line per
  request (no headers except User-Agent, credential query parameters
  replaced). The desktop counts them into Network → **Web Requests**
  (`WebRequestTally::recordBatch`) and logs each as a Web line. Counts live in
  memory for 24 hours.
- **rqlite statements.** The app's statement lines (enabled by
  `FORKMESH_HOST_D1_LOG=1`, rendered from `fleet.toml` `[app] d1_log`) become DB
  lines with source file and caller. Log → **D1** shows per-node statement
  rates and the most frequent statements.
- **Visualizer.** Log → **Visualizer** (`src/FleetVisualizer.{h,cpp}`) draws
  statement flights to per-table bubbles, node vitals (memory, disk, file
  handles, CPU from the reader's `logread_vitals` sample), an **Every query**
  inspector (`src/QueryInspector.{h,cpp}`; callers, code sites, latency
  histogram, top statements; bounded memory) and **This desktop**
  (`src/DesktopProcessView.{h,cpp}`, sampling `/proc` every 2 s; Linux only).
  Everything runs only while the tab is visible.

## Network & nodes

The Nodes tab of the Network page (`src/MainWindowNodes.cpp`, `buildNodesSection`)
is one table: every node the relay directory, roster and serving set know, plus
one row per configured relay and per runner.

- **Repository copies** (`src/MainWindowReleases.cpp`, `loadMirrorNodesPanel`)
  for a chosen repository are built into a hidden per-repository model and merged
  onto node rows (`mergeMirrorRowsIntoNodesTable`): commit, sync lag, counts,
  sync-back backlog, route and reachability proofs, integrity pin. A node with
  several copies lists them as lines beneath it; `sortNodesTable` sorts groups,
  not rows.
- **Roles** (`src/NodeRole.h`, filter `network/nodesRoleFilter`): a catalog
  `role` field first, then fleet naming and fleet files (core, runner), this
  machine (desktop node), saved Hosts (mirror), catalog `runtimeMode`, and a
  name prefix.
- **Relay rows**: each configured relay with Status, Latency, Version, Endpoint
  and Scope (`active`/`standby`); `refreshRelayNodes()` probes `/api/version`
  and patches rows in place.
- **Runner rows** (`mergeRunnerRows`) merge fleet files, the cloud tag listing
  and `GET /api/runners` (status, last check-in, `activeLeases`, scope, owner).
  Their **Logs** action opens the Log page filtered to that runner.
- Owner and version come from the catalog
  ([NODE_OWNERSHIP.md](../NODE_OWNERSHIP.md)).
- A node in the relay's serving set counts as online even when its endpoint
  lease is not current (`mirrorsync::catalogMirrorLiveness`); its tooltip says
  clones are not routed to it.

Tests: `forkmesh-control-tests` (`checkNodeRoles`, `checkCatalogMirrorLiveness`),
`forkmesh-window-tests --nodes-page-only`, `forkmesh-runner-login-tests`.

## Hosts and fleet operations

`src/ControlNode.{h,cpp}` builds every remote command (install, update,
uninstall, Actions, size map, agent CLI install and sign-in, fleet binary
upload). All authenticate with the shared fleet key (`sharedHostKeyPath()`).
Passwords are only a fallback and travel via environment or stdin, never argv.

### Runners

Network → Hosts lists the app's cloud agent runners (`hostsRunnersTable`,
`refreshRunners()`, `src/RunnerListView.*`) from `GET /api/runners`, with
status, scope, last check-in and per-provider sign-in state, and buttons that
open the runner sign-in dialog ([../RUNNERS.md](../RUNNERS.md#provider-credentials)).
Runners are never added to the saved hosts.

### Add core node

**Add core node** (`src/AddCoreNodeDialog.{h,cpp}`, model in
`src/AddCoreNodeModel.{h,cpp}`) is shown to admins on a desktop holding the
fleet config and `tools/next_fleet.py` (`FORKMESH_NEXT_FLEET`). It chooses a
provider, region and plan from `next_fleet.py add-core-options --json`, confirms
the plan, then runs `add-core --execute` and shows a step list parsed from
`FMNEXT-ADD-CORE {json}` lines. Retry resumes. The run is a
`systemd-run --user` unit logging to `logs/fleet/add-core/<stamp>.log`;
`AddCoreNodeController` (owned by the main window) re-attaches via
`logs/fleet/add-core/current.json` after the dialog or desktop closes.
`rememberHost` refuses fleet names and addresses. Test:
`forkmesh-add-core-node-tests`.

### AWS credentials

**Settings → Cloud** (`src/AwsSettingsPanel.{h,cpp}`, `src/AwsCredentials.{h,cpp}`):

- **Save** encrypts `{accessKeyId, secretAccessKey}` with AES-256-GCM
  (associated data `forkmesh-cloud-credentials-v1|aws`) into
  `cloud/aws/sealed`; the region is plain `cloud/aws/region`. The key is a
  random 32-byte file `data/secrets/cloud-credentials.key` (0600 in a 0700
  folder), so settings files and exports hold no usable credential. This is
  encryption at rest, not protection against code running as the same user.
- The secret is never shown again; an empty secret field reuses the stored one.
- **Test** sends a SigV4-signed `sts:GetCallerIdentity`; the status never
  contains the secret.
- **Write aws.env for the fleet tools** writes a plain-text 0600 `aws.env` to
  the fleet config folder only on request. **Forget** removes the sealed blob.
- Only long-term `AKIA` keys are accepted.

Other provider API keys may still be stored as plain device variables. Test:
`forkmesh-aws-credentials-tests` (sandboxed `HOME`, XDG and QSettings).

### Remove core node

Each core row has **Remove…** (`src/RemoveCoreNodeDialog.{h,cpp}`,
`src/RemoveCoreNodeModel.{h,cpp}`), disabled with 3 or fewer cores to protect
rqlite quorum. `next_fleet.py remove-core-plan nN --json` shows the voter
changes (including a promotion so 3 voters remain); a refused plan disables
confirmation. Deleting the cloud instance is an explicit opt-in
(`--destroy-instance`), and the node name must be typed exactly. The run is a
`systemd-run --user` unit that the controller re-attaches to, like add-core.
Test: `forkmesh-remove-core-node-tests`.

### Fleet autoscaling panel

Network → Hosts carries **Fleet autoscaling** (`src/FleetAutoscalePanel.{h,cpp}`,
`src/FleetAutoscaleModel.h`), a control plane for `tools/next_autoscale.py`
(`FORKMESH_NEXT_AUTOSCALE`, `FORKMESH_FLEET_PYTHON`). The scaler itself runs
leaderless on the cores ([../fleet/AUTOSCALING.md](../fleet/AUTOSCALING.md)).
The panel offers a kill switch, per-pool settings (validated, then signed by
`next_autoscale.py settings set`; going live asks first), a status view
refreshed every 60 s only while visible, and manual scale requests queued to the
cluster. It writes autoscaled nodes to `autoscaled-instances.json` (0600,
atomic) for log discovery and role classification. The desktop never creates or
destroys mirrors on its own.

## Local mesh

`src/LocalMeshNode.*` discovers other ForkMesh devices on the LAN by UDP beacon
and links them over TCP, both on port 48732 (`kDefaultDiscoveryPort`). The
beacon's room key is shared by every install and proves nothing about identity,
so a beacon carries only `{t, v:3, id: kid, port, ts, reply, control, node}`;
names, account and platform travel in the mesh `hello` inside the link. The
dial tie-break compares key ids (`kid`), not node ids.
`src/MeshFirewall.*` generates the rule for `ufw`, `firewalld` or Windows
`netsh`; `localMesh/firewallOpened` records once the port is known to be open so
the Allow banner is not shown again. `checkLocalMeshFirewall()` stays silent
while the Chat package is hidden: the mesh still runs, but the banner and its
log line (which the log classifier badges ERROR, so it raises an Alert) only
appear once Chat is shown, and `syncPackageSurfaces()` re-runs the check when
it is. A fresh install without Chat used to get an Alert about `#mesh`. Direct messages travel over the mesh
as `direct-sealed` frames to the other person's linked, approved devices
([chat.md](chat.md#direct-delivery)); iPhones that left the room are woken via
push ([../native/MESH_PUSH.md](../native/MESH_PUSH.md)).

### Mesh links v3

Every LAN and Bluetooth link runs the `fm-link-hello` / `fm-link-auth`
handshake of [chat protocol v3](../app/chat-protocol-v3.md#mesh-links-v3):
both sides send a plaintext hello on connect (the dialer and the BLE central
are the initiator), sign the transcript, and then every frame is AES-256-GCM
under a per-direction key with a counter nonce. A bad signature, the
desktop's own identity key, a blocked key id or a frame that fails to open
closes the link. Duplicate links are resolved when the link authenticates, by
key id.

- After auth the desktop sends its `#mesh` bundle first, then the bundles it
  has learned this session, then its DM focus. The mesh `hello` waits until
  the peer's bundle has arrived, so it can be sealed to that device.
- `#mesh` messages are v3 sealed frames (open policy, acct and node = node
  id). Every frame is forwarded at most once (by signature) and keeps its
  original signature and counter: a chat frame this node opened goes to every
  other link, a frame it cannot open (not addressed to it, or from a device
  whose bundle it lacks) only to the links whose device is among the frame's
  recipients. New bundles go to every other link. `hello`, `typing` and `bye`
  are accepted only from the link's own device and never forwarded.
- Because a forwarded frame opens only for the devices its author sealed it
  to, a node that learns a new device's bundle from a link re-seals its own
  recent `#mesh` messages to that device and sends them over that link. A
  joiner therefore gets other people's history only if their author is still
  reachable, or if it was already a recipient. Others' sealed frames are kept
  in memory only, so after a restart only this device's own history is
  replayed.
- The local-control port still accepts `fm-control-hello` as the first frame
  of an inbound TCP link; the desktop's own `fm-link-hello` has already been
  written by then, and the phone's control client skips plain frames before
  its welcome.

The transport diagnostics line (`LocalMeshNode::diagnosticsLine`) is behind the
pulse button in the `#mesh` header and refreshes only while shown.

### Over Bluetooth

Bluetooth links run the same v3 handshake and carry the same frames, so a
device on both transports bridges them. Direct-message files go over a
Bluetooth link only up to 256 KiB (1 MiB over TCP). Built only with Qt Bluetooth (`FORKMESH_HAS_BLUETOOTH`);
the Linux release binary is LAN-only.

- **RFCOMM** (SDP service `7f9e5c10-…-2b7ad1e05f11`): desktop ↔ desktop and
  desktop ↔ Android; both sides listen and dial.
- **BLE GATT** for iPhone: service `7f9e5c14-…`, inbox characteristic `…c15`
  (write with response) and outbox `…c16` (notify) carry the byte stream in
  order with the room's 4-byte length framing; a zero-length value closes. The
  desktop (`src/BleMeshLink.*`, a `QIODevice`) and Android are centrals only.
  Chunks are `MTU - 3`, capped at 512.
- The BLE scan follows each RFCOMM discovery so inquiries never overlap.
  Between iPhones, only the higher advertised token dials.
- A Bluetooth link dropped as a `duplicate link` puts the address on a hold-off.

Test: `forkmesh-chat-transport-tests` (`testLocalMeshCrossClientConstants`).

### Link liveness

- **Keepalive.** Every mesh TCP link sets `SO_KEEPALIVE` (10 s idle, 5 s
  interval, 3 probes) and `TCP_USER_TIMEOUT` 30 s where available
  (`enableLinkKeepAlive`).
- **Duplicate links.** Both ends apply the same rule when a link authenticates:
  prefer TCP over Bluetooth; else close the older link as `replaced` if it
  authenticated at least 10 s ago; else keep the link the higher key id
  dialled.
- **Dials** time out after 8 s. A LAN link that drops after being up 5 s is
  redialed after 1.5 s and triggers an early Bluetooth scan; a TCP drop lifts
  Bluetooth hold-offs.
- **Network changes.** The node opens its listener whenever an interface
  appears and closes links bound to vanished addresses. Subnet sweeps honour the
  interface prefix (e.g. a /28 tether), wider prefixes sweep the surrounding
  /24.

## Phone control over the LAN

`src/LocalControl.{h,cpp}`, `src/MainWindowLocalControl.cpp`. A phone (or a web
browser over the relay) lists, starts, steers, stops and continues agents and
moves files, without relaying content in clear.

- `LocalMeshNode` hands inbound links whose first frame is `fm-control-hello`
  to `LocalControlServer`, so the mesh firewall rule covers it; a first frame
  of `fm-link-hello` starts a v3 mesh link. The beacon's `node` field (with
  `control: true`) is the desktop's Ed25519 key that local control signs with.
- Handshake: hello / welcome / auth with each side's Ed25519 identity and a
  fresh X25519 key, both signing the transcript; frames are AES-256-GCM under
  per-direction HKDF keys with a counter nonce.
- **Same-account approval.** After auth the connection waits in `Checking` while
  `checkLocalControlAccountDevice` asks `POST /api/accounts/devices/check`
  whether the phone's key is the account's primary or an enabled `owner_sign`
  device key, and requires the answer's `nodeName` to match the signed-in
  account. Without a session token the desktop signs
  `forkmesh-device-check-v1\n<account>\n<phone pubkey>\n<ts>`. The phone's
  self-reported user is never trusted.
- **Instant on the LAN.** A phone already approved through the account whose
  last check passed less than 24 hours ago (`admitsBeforeCheck`) is admitted on
  the LAN as soon as the handshake ends; the account is asked in the background
  (`recheckAccount`), and a `NotOwned` answer closes the channel with `revoked`
  and forgets the phone. Before this every connect waited in `Checking` for the
  account server (up to 8 s), so the phone's Agents list and every other
  desktop page took seconds at home. Relayed channels and phones not confirmed
  for a day still wait for the answer.
- Any other phone gets a six-digit code shown on both screens; over the relay it
  is refused with `pair_on_lan`. The prompt (`showLocalControlPairingPrompt`)
  defaults to **Deny** (also the Escape button), opens without taking focus
  (`Qt::WA_ShowWithoutActivating`, no `raise()`) and only flashes the taskbar,
  so a keystroke meant for another window cannot approve a phone. A pending
  pairing expires after two minutes (`kPairingTimeoutMs`, a timer per request
  plus `maintain()`): the phone gets `pairing-denied` with reason `expired`,
  the prompt closes and the connection frees its slot. Unanswered prompts used
  to hold one of the eight connection slots forever. Approved devices live in
  `data/local_control/paired-devices.json` (**Settings → Phone**), written
  owner-only through `forkmesh::ownerfile::write`. Browsers are
  never stored and are checked on every connect.
- **Revocation.** Each device records `approval` (`account` or `code`) and
  `accountCheckedMs`. Account-held approvals are re-checked on connect and every
  ten minutes (`maintain()`); not owned closes with `revoked`; unreachable is
  tolerated on the LAN and for 24 hours over the relay (`kAccountGraceMs`).
  Code-approved phones are never re-checked.
- **Relayed channels.** Unknown sessions are answered once with `close`
  (`channel_gone`); closes that could not be sent go out on reconnect
  (`relay_reconnected`). Idle relayed channels close after 15 minutes, and a
  full table (`kMaxConnections`, 8) evicts the longest-idle relayed channel.
  `NodeEventSocket` reconnect backoff is capped at two minutes and skipped when
  the network returns. Phones ping idle channels before use.
- Relayed frames go through a sliding-window pacer (`LocalControlStream.h`
  `SendBudget`) at 90% of the relay's advertised `controlBudget`. Both sockets
  set `TCP_NODELAY`.
- New request methods go in `MainWindow::handleLocalControlRequest`; agent
  changes push `agents.changed`, transcripts stream via `agents.follow` /
  `agents.output` ([../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).
- Deleting an agent (`deleteStoredAgentSession`) and restoring one from the
  recycle bin call `scheduleAgentSessionsPush()`, which rings `agents.changed`
  and re-posts the relay snapshot, so the phone drops or re-adds the row
  without a manual refresh. Before this, an idle agent deleted on the desktop
  stayed in the phone's list until the user pulled to refresh.
- Test vectors are pinned in `desktop/tests/test_local_control.cpp` and
  `native/ios/ForkMeshCore/Tests/FMChatTests/LocalControlTests.swift`; change
  both together. Tests: `forkmesh-local-control-tests`.

## Traps

- A code-approved phone stays in `paired-devices.json` regardless of the
  account; remove it in **Settings → Phone**.
- `accountDeviceCheck` may call `done` before it returns, and `requestPairing`
  closes a relayed connection; look the connection up by id again after either.
- `LocalControlServer` and `Http3Get` have no `Q_OBJECT`; register the header
  for moc before adding signals.

## Application firewall rules

Path-keyed application firewalls (OpenSnitch) prompt for every new build path.
`tools/opensnitch_rules.py` writes priority rules scoped to one user id:

| Rule | Allows |
| --- | --- |
| `000-forkmesh-programs` | programs whose file name contains `forkmesh` under a home folder, `/opt`, `/usr/local` or an AppImage mount |
| `000-forkmesh-dev-trees` | programs under `~/projects/forkmesh/` or `~/.cache/fm*/` (`--no-dev-trees` omits it) |
| `000-forkmesh-agent-clis` | every version of Claude Code (`~/.local/share/claude/versions/*`, the Claude app's `~/.config/Claude/claude-code/*/*/claude`) and Codex (`~/.codex/packages/standalone/*/bin/codex`) (`--no-agent-clis` omits it) |
| `000-forkmesh-loopback` | connections to `127.0.0.0/8` and `::1` (`--no-loopback` omits it) |

```sh
sudo python3 tools/opensnitch_rules.py install
python3 tools/opensnitch_rules.py print
```

Each Claude Code update installs a new binary path, so OpenSnitch prompts
again. When nobody is at the screen (another user is switched in, or the
screen is locked) the prompt times out into the UI's default action, often a
deny rule, and every agent then fails its API calls with `EAI_AGAIN` after
about 11 minutes of retries: on 2026-10-22 2.1.308 was denied at 16:32 and
every agent run failed until about 18:20. `000-forkmesh-agent-clis` allows
those paths for one user; run the install once per account that runs agents
(`--user <name>`).

The rules folder is root-only, so neither the desktop nor an agent installs
them. Tests: `tools/test_opensnitch_rules.py`.
