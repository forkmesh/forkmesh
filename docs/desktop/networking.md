# Desktop: networking

How the desktop talks to the network: the relay connection and its HTTP/3
client, the host and fleet log feeds, the Network page's node list, host and
fleet operations, the LAN and Bluetooth mesh, phone control over the LAN, and
the application firewall rules agents need. How the relay, cores and mirrors
fit together is in [architecture](../architecture.md); the encryption used on
links and chat is in [encryption](../encryption.md).

Part of [Desktop](README.md).

## Relay connection

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
  ([docs/qt-client.md](../qt-client.md#multiple-relay-instances)).
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
  desktop over the internet until a successful `probeRelayLatency()` ends it.
  While it is on, the probe ignores its usual "pong in the last 90 s" skip,
  retries at most every 30 s (`kAutoOfflineProbeMaxDelayMs`), and the minute
  tick in `MainWindow.cpp` probes as a backstop, because a windowless desktop
  never gets the window-activation trigger and could otherwise stay offline.
  Online, nothing new is polled.
- Chat frames are sealed with chat protocol v3 (`src/RoomSeal.*`,
  [chat.md](chat.md)). Trust records, seen counters and held frames are written
  to QSettings two seconds after a change (and at quit), because each settings
  write rewrites the whole file on the GUI thread; approvals and blocks are
  written at once. v2 `sealed`/`e2ee-key` frames are ignored.
- `ServerNode` passes v3 frames through unchanged up to the 96 MiB WebSocket
  limit. Frames the relay marks `historyReplay` before `forkmesh-history-end`
  skip only the already-seen counter check.

### HTTP/3 client

QtNetwork stops at HTTP/2. `src/Http3Get.{h,cpp}` is an in-process HTTP/3 GET
client on OpenSSL's QUIC client and `libnghttp3`, driven by a
`QSocketNotifier` and a timer. It is compiled only with OpenSSL QUIC and
`libnghttp3` (`FORKMESH_HAVE_HTTP3`); otherwise `forkmesh::net::http3Available()`
is false. Trust comes from the system CA store (`SSL_CERT_FILE` works for
tests). QUIC's idle timeout is 30 s, so the mirror gateway
(`tools/mirror_gateway.py`, `NODE_LOG_KEEPALIVE_SECONDS`) sends keep-alives
every 10 s. Probe: `forkmesh-http3-probe <url> [Header: value]...`.

## Host and fleet logs

The Log page itself is in [diagnostics.md](diagnostics.md#log-page).

### Host logs

The Hosts rail's **Live view** (`src/MainWindowHosts.cpp`, `src/HostLogSources.h`)
reads the gateway's `/v1/node/logs` server-sent events over HTTP/3 when it can.
Only Ed25519 keys the desktop's host installer wrote to the gateway's allow file
may open the feed. Otherwise it falls back to one SSH connection multiplexing
the node log and the systemd journal, and retries QUIC periodically.

### Fleet node logs

`src/CoreNodeLogs.{h,cpp}` and `CoreLogFeed` in `src/MainWindowLogs.cpp` follow
every fleet node's journal live. Node side: [node logs](../operations/node-logs.md),
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
  `ERROR`/`CRITICAL` lines and IDS alerts. Caddy's "setting HTTP/3 Alt-Svc
  header … no port can be announced" is a warning
  (`corelogs::isCaddyReloadNoise`): quic-go logs it when a response goes out
  during a config reload, and the response only lacks the HTTP/3 advert.
  Python tracebacks fold into one entry;
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
  lines with source file and caller. Log → **D1** (the tab keeps the name of
  the app's D1-shaped database layer, `d1_rqlite.py`) shows per-node statement
  rates and the most frequent statements.
- **Visualizer.** Log → **Visualizer** (`src/FleetVisualizer.{h,cpp}`) draws
  statement flights to per-table bubbles, node vitals (memory, disk, file
  handles, CPU from the reader's `logread_vitals` sample), an **Every query**
  inspector (`src/QueryInspector.{h,cpp}`; callers, code sites, latency
  histogram, top statements; bounded memory) and **This desktop**
  (`src/DesktopProcessView.{h,cpp}`, sampling `/proc` every 2 s; Linux only).
  Everything runs only while the tab is visible. Clicking a table bubble opens
  that table in Log → **Database** (`DatabaseManagerWidget::showTable`): the
  tab's filter and view are reset so the table is listed, the name is matched
  case-insensitively (statement shapes lowercase it), and if the inventory has
  not loaded yet the table is selected once it arrives. The `(no table)` and
  `+N more` bubbles are not links.

### Cloudflare log tail (legacy)

`MainWindow::setCloudLogMonitorEnabled()` (`src/MainWindowLogs.cpp`, setting
`diagnostics/cloudLogMonitor`) can run `npx wrangler tail` for the Worker named
in `app/wrangler.toml`. The app and World run on the core nodes, not as
Workers, so the tail normally reports the Worker missing
(`cloudflareTailWorkerMissing()`): the monitor then stops, records the name in
`diagnostics/cloudLogMonitorMissingWorker`, and skips the tail on later
launches until the box is ticked again. The Node.js probe runs on a worker
thread once per session. App logs come from the fleet feed below.

## Network & nodes

The Nodes tab of the Network page (`src/MainWindowNodes.cpp`, `buildNodesSection`)
is one table of every kind of node: every node the relay directory, roster and
serving set know, one row per configured relay and per runner, every saved host,
every fleet core, and every self-hosted instance in the public directory. There
is no separate Hosts tab: the host controls sit beside the list in the Hosts
rail.

- **Saved hosts** (`fillSavedHostNodeRow`): a saved host whose name no other row
  has gets its own row (role from `NodeRole.h`, normally Mirror; Scope `saved
  host`; status from the last SSH probe; Endpoint is its address; **Logs** opens
  the SSH log tail). A node row whose name matches a saved host is marked
  instead (`markSavedHostNodeRows`, a **Host** badge). Clicking either picks
  that host in the rail (`selectSavedHostInRail`).
- **Fleet cores** (`fillFleetCoreNodeRow`): non-runner nodes from
  `fleet-instances.json`, `autoscaled-instances.json` and the cloud listing that
  no node row already covers (`n7`, `fmnext-n7` or the label). Status is the
  `readyz` answer the rail's fleet table fetches (`m_fleetNodeReadiness`).
- **Instances** (`fillInstanceNodeRow`, role Instance): `GET /api/start/instances`
  on the default server ([../app/instance-federation.md](../app/instance-federation.md)),
  fetched at most every 10 minutes while the page is visible (every minute on
  Refresh), `https` entries only, skipped when the host is a configured relay.
  Only listed, visible entries come back, so an instance shows up here once it
  is in the directory (OWASP BLT, for example). **Website** opens it. Not
  fetched under `FORKMESH_WINDOW_TESTS`.
- **Two-line rows** (`NodeCellDelegate`, 48 px rows, both designs): the Node
  cell shows a role icon with a status dot, the name and badges, and a second
  line with role, platform or region, endpoint and version. Every other cell
  shows its column's icon (`kNodeHeaderIconRole` on the header item) and value,
  with the column caption underneath; CPU, RAM and Disk draw a meter. The
  logical columns are unchanged; the header moves Status, CPU, RAM, Disk,
  Latency and Health right after Node, so the health of each node reads first.
- **Hosts rail** (`buildHostsSection`, `networkHostsRail`): a right column
  (`networkRightColumn`, a quarter of the page, 240 to 320 px) holds the node
  details panel (Next design) above the rail. The rail holds every Hosts
  control in compact form: icon-only buttons for live view, check all and the
  all-host install, reinstall and source update; the saved-host list with two
  lines per host and its ten per-host actions as a 2 by 5 grid of icon buttons;
  then collapsible sections for Add a host, Create a Vultr mirror, Runners,
  ForkMesh fleet (cores table, Add core node, Fleet autoscaling) and Live
  output. Each section's open state is `network/hostsRail/<key>`; a section
  opens itself when one of its fields is filled from elsewhere (the World
  launch handoff, a double-clicked host) or output arrives. The Hosts plugin
  switch hides the rail. **Live view** swaps the node list for the side-by-side
  host panes (`m_hostsViewStack`). `kNetworkHostsTab` is an alias of the
  Nodes tab, so every "open Hosts" path lands here. Saved hosts are probed only
  while the page is on screen (`hostsRailOnScreen`).

- **Repository copies** (`src/MainWindowReleases.cpp`, `loadMirrorNodesPanel`)
  for a chosen repository are built into a hidden per-repository model and merged
  onto node rows (`mergeMirrorRowsIntoNodesTable`): commit, sync lag, counts,
  sync-back backlog, route and reachability proofs, integrity pin. A node with
  several copies lists them as lines beneath it; `sortNodesTable` sorts groups,
  not rows.
- **Roles** (`src/NodeRole.h`, filter `network/nodesRoleFilter`): a catalog
  `role` field first, then fleet naming and fleet files (core, runner), this
  machine (desktop node), saved Hosts (mirror), catalog `runtimeMode`, and a
  name prefix. Directory instances are always Instance.
- **Relay rows**: each configured relay with Status, Latency, Version, Endpoint
  and Scope (`active`/`standby`); `refreshRelayNodes()` probes `/api/version`
  and patches rows in place.
- **Runner rows** (`mergeRunnerRows`) merge fleet files, the cloud tag listing
  and `GET /api/runners` (status, last check-in, `activeLeases`, scope, owner).
  Their **Logs** action opens the Log page filtered to that runner.
- Owner and version come from the catalog
  ([node ownership](../operations/node-ownership.md)).
- A node in the relay's serving set counts as online even when its endpoint
  lease is not current (`mirrorsync::catalogMirrorLiveness`); its tooltip says
  clones are not routed to it.

Tests: `forkmesh-control-tests` (`checkNodeRoles`, `checkCatalogMirrorLiveness`),
`forkmesh-window-tests --nodes-page-only`, `forkmesh-runner-login-tests`.

### New interface

In the [new interface](ui-redesign.md#pages) the Network page
(`buildNetworkDiagnosticsSection` in `MainWindowNetworkDiagnostics.cpp`,
`buildNodesSection` in `MainWindowNodes.cpp`, `buildMirrorNodesSection` in
`MainWindowReleases.cpp`, `82-network.qss`) follows the desktop mockup;
Classic keeps the layout above.

- **Header.**
  - The title is 15 px semibold, with the "About network diagnostics" help
    button beside it.
  - Under the title is one muted summary line (`networkRole="summary"`):
    "N of M nodes online · E endpoints · c / n websocket connected · X sent,
    Y received". The nodes part comes from the Nodes table and the rest from
    `refreshNetworkDiagnostics`. Each side stores its part in a dynamic
    property (`nextNodesPart`, `nextDiagPart`) on `m_networkDiagnosticsStatus`
    and joins both. The Classic text, with frame counts, becomes the line's
    tooltip.
  - On the right are the page's one Refresh (`repoAction`, ghost through
    `networkRole="ghost"`, same slot) and an inverse "Add node" button
    (`networkAddNodeButton`). Add node opens the Add a host section of the
    Hosts rail beside the node list and focuses its address field. It adds no
    new flow.
- **Tabs.** `#networkTabs` uses underline tabs (accent underline on the
  current tab) in place of the pills from `10-shell.qss`. Tabs that were
  title case are renamed in sentence case in Next ("Web requests",
  "Firewall rules", "Recent firewall decisions"). Every tab stays, in the
  same order and at the same index. Each tab page gets 16 px top padding.
  The five diagnostics tables get 36 px rows and a 12 px rounded hairline
  frame (`networkGrid`).
- **Nodes tab.** It is a left column plus a right detail panel. Every widget
  still belongs to the Nodes page.
  - **Stat cards** (`networkStatOnline`, `networkStatLatency`,
    `networkStatTraffic`, `networkStatRepos`):
    - Nodes online: online and total rows, with an offline breakdown.
    - Median latency: median of the Latency column, with how many nodes were
      measured.
    - Relay traffic: bytes sent plus received over the relay websockets this
      session.
    - Repositories served: distinct repositories in the roster's mirror
      lists, and how many nodes hold them.

    The cards are refreshed at the end of `sortNodesTable` and in
    `refreshNetworkDiagnostics`.
  - **Repository mirrors bar.** `#mirrorNodesSection` becomes a rounded card
    with the repository picker, the count, the Online only switch, Reset
    integrity pin, Refresh nodes and "Update all from this binary".
    - Online only is a `kit::Toggle` (a `QCheckBox`), with the same text,
      default and slot.
    - The update button keeps its text and object name, so a rail shortcut
      to it still resolves by label. It sits in the bar's
      `mirrorNodesHeaderRow`.
  - **Filter bar** (`networkNodesToolbar`).
    - A segmented control built from `nodesRoleFilter`'s items: All, Core,
      Runners, Mirrors, Desktop nodes, Relays. It sets the combo's index, so
      the saved setting and the filtering code are unchanged. The combo and
      its label are hidden, and the two stay in sync both ways.
    - The "Filter nodes" field hides rows whose name, role, owner, status,
      region, platform and endpoint all lack the text. It filters only what
      the role filter already shows, and hides child copy rows with their
      node.
    - The long lede sits behind an "About nodes" help button.
    - Sorting lives here, not in the column headers: a "Sort: <field>"
      combo (`networkNodesSort`, every column but Actions, in display order)
      and an ascending/descending button (`networkNodesSortOrder`). Both set
      the header's sort indicator, so `sortNodesTable` is unchanged; the
      choice is saved as `network/nodesSort` (the column's caption) and
      `network/nodesSortOrder` (`asc`/`desc`).
  - **Nodes table.**
    - The model keeps all 44 columns, its actions, context menus and
      double-click behaviour, but the table shows far fewer: the header is
      hidden (`nodePairs` property) and each shown cell holds two different
      fields, one per line (`nextNodePairs`):
      - Status over Activity, Latency over Checked, Health over Integrity,
        Revision over Message and Author, Route over Reach, Repos over
        Mirrors, Size over Git DB, Issues over Pulls and Discussions,
        Commits over Branches, Artifacts over Worktrees, Clones over Web
        hits, Caps over Scope.
      - CPU, RAM and Disk share one cell as three small meters.
      - Each line is its icon, a muted caption (left out where the value
        says what it is: status, activity, latency, revision, message) and
        the value. An empty value is a plain dash.
      - A count's sync-back backlog (`nextNodeBacklogColumn`) follows it in
        the warning colour as "↑N", instead of taking its own column.
    - Role, owner, platform (or region), endpoint and version are folded
      into the Node cell's second line, which absorbs the spare width
      (`makeColumnsResizable` keeps Node flexible). Node id is only in the
      details panel.
    - `nextFitNodeColumns` (run at the end of `applyNodeRoleFilter`) hides a
      cell whose fields are empty in every row the role and text filters
      show, so a role tab only lists the columns its nodes report.
    - Rows are 48 px.
    - `NextNodeCellDelegate` paints:
      - Node: a status dot (green online, grey offline, amber behind, red
        integrity failing), the mono name, and a soft "You" badge or outline
        "Source of truth" and "Active relay" badges.
      - Child copy rows: indented under "└".
      - Muted columns: role, scope, status ("Online · serving"), activity,
        owner, latency, region.
      - Version and Revision: mono.
    - `NextNodesTableChrome` (a viewport event filter) paints the hairlines,
      the whole-row hover fill and the selected-node band. The table stays
      `NoSelection`.
  - **Detail panel** (`networkNodePanel`, 320 px).
    - A click on a row opens it, and × closes it. The node is remembered by
      key (`nextPanelKey`) and refilled after every refresh.
    - Content, all read from that row's cells:
      - Header: a role icon tile, the mono name, "This node", and role ·
        region (or platform).
      - An offline notice made of the status and its tooltip.
      - Latency, Synced and Version tiles.
      - CPU, Memory and Disk bars (`kProgressBarRole`).
      - A Details list: status, activity, owner, platform, revision,
        endpoint, health, node id.
    - Footer: Profile (`showNodeProfile`), the row's own action (Website or
      Logs, through `activateNodeDirectoryRow`), and "…", which offers the
      row's Delete button by clicking it.
  - `m_nodesStatus` stays as a footnote under the table.
- **Left out on purpose.**
  - Topology graph: the app has no node-to-node link data and no graph
    widget.
  - Activity · 24h sparklines and the panel's Load · last 24h: no per-node
    history exists. Activity stays the existing text column.
  - The panel's Uptime tile: no uptime is reported. Synced is shown instead.
  - The Services list with running dots: nodes report no services.
    Capabilities stay in the Caps column tooltip.
  - Open shell and Restart: the Nodes directory has no per-node shell or
    restart. Those stay on saved hosts in the Hosts rail.
  - The Relay traffic delta ("+12%"), and the mockup's mesh invite command
    and dialog: no such data or flow exists.
  - The mockup hides most columns. Here they are paired two to a cell or
    folded into the Node cell instead, so no field is lost.
- **Tests.** `test_window_resize.cpp` pins compact two-line node rows (40 to
  52 px) and the "Web Requests" tab label. Both hold in Classic, the test
  default. The Next values are cosmetic.
- **Live switch.** The layout is built once, so a live switch shows it after
  the page is rebuilt.

## Hosts and fleet operations

`src/ControlNode.{h,cpp}` builds every remote command (install, update,
uninstall, Actions, size map, agent CLI install and sign-in, fleet binary
upload). All authenticate with the shared fleet key (`sharedHostKeyPath()`).
Passwords are only a fallback and travel via environment or stdin, never argv.

### Runners

The Hosts rail's Runners section lists the app's cloud agent runners (`hostsRunnersTable`,
`refreshRunners()`, `src/RunnerListView.*`) from `GET /api/runners`, with
status, scope, last check-in and per-provider sign-in state, and buttons that
open the runner sign-in dialog ([runners](../operations/runners.md#provider-credentials)).
Runners are never added to the saved hosts; they show in the node list as runners.

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
`rememberHost` refuses fleet names and addresses. After a successful run the
headline says whether the fleet commit landed on `main` (the result event's
`merged`), or gives the reason it did not and asks for the merge
(`fleetLandingText`, shared with Remove core node). Test:
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

### Replace core node

Each core row also has **Replace…**. It opens the Add core node dialog in
replace mode (`AddCoreNodeController::setReplaceTarget`): the title names the
old core, and the options, confirmation and run add `--replace nOLD`. The run
is one `add-core --replace` unit with an extra `remove` step, so Retry, closing
the dialog and re-attaching work as they do for an add. While it runs, the
header button reads "Replacing nOLD…", the old core's row shows
"Replacing…", and the other Replace and Remove buttons are disabled. Opening
the dialog from **Add core node** clears the target. The old instance is kept;
delete it later with **Remove…** and the instance box. Test:
`forkmesh-add-core-node-tests` (`testReplaceFlow`).

### Fleet autoscaling panel

The Hosts rail's ForkMesh fleet section carries **Fleet autoscaling** (`src/FleetAutoscalePanel.{h,cpp}`,
`src/FleetAutoscaleModel.h`), a control plane for `tools/next_autoscale.py`
(`FORKMESH_NEXT_AUTOSCALE`, `FORKMESH_FLEET_PYTHON`). The scaler itself runs
leaderless on the cores ([autoscaling](../operations/autoscaling.md)).
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
it is. Direct messages travel over the mesh
as `direct-sealed` frames to the other person's linked, approved devices
([chat.md](chat.md#direct-delivery)); iPhones that left the room are woken via
push ([../native/MESH_PUSH.md](../native/MESH_PUSH.md)).

`startLocalMesh()` waits for a chat session (`m_backend`, set by
`startSession`, which builds the on-demand Chat page first) and does nothing
while the window is closing. The network-reachability handler calls it on any
change, so before this a reachability change at startup, while the deferred
start sat in `getAccountSync`'s nested event loop, started the mesh before
Chat existed; `setChannels` then reached `refreshChannelList` with a null
`m_channelList` and crashed in `QTreeWidget::clear` (report #19,
2026-11-01). `refreshChannelList` and `setChannels` also keep the sidebar rows
and skip the widget work when Chat is not built. Test:
`forkmesh-local-mesh-gate-window-tests` (`--local-mesh-gate-only`). Window
tests build Chat eagerly, so the null-list guards themselves are not reached
there.

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
  and forgets the phone. This avoids waiting up to 8 s for the account server
  on every connect at home. Relayed channels and phones not confirmed for a
  day still wait for the answer.
- Any other phone gets a six-digit code shown on both screens; over the relay it
  is refused with `pair_on_lan`. The prompt (`showLocalControlPairingPrompt`)
  defaults to **Deny** (also the Escape button), opens without taking focus
  (`Qt::WA_ShowWithoutActivating`, no `raise()`) and only flashes the taskbar,
  so a keystroke meant for another window cannot approve a phone. A pending
  pairing expires after two minutes (`kPairingTimeoutMs`, a timer per request
  plus `maintain()`): the phone gets `pairing-denied` with reason `expired`,
  the prompt closes and the connection frees one of the eight slots.
  Approved devices live in
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
  without a manual refresh.
- The Remote control switch at the bottom of a session's Environment card
  (`LocalControlServer::setRemoteOff`) refuses every paired request with
  `remote_off` and holds back events while it is off; see
  [Sessions page side panels](sessions-page.md#side-panels).
- Test vectors are pinned in `desktop/tests/test_local_control.cpp` and
  `native/ios/ForkMeshCore/Tests/FMChatTests/LocalControlTests.swift`; change
  both together. Tests: `forkmesh-local-control-tests`.

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
about 11 minutes of retries. `000-forkmesh-agent-clis` allows
those paths for one user; run the install once per account that runs agents
(`--user <name>`).

The rules folder is root-only, so neither the desktop nor an agent installs
them. Tests: `tools/test_opensnitch_rules.py`.

## Request firewall prompt

With **Whitelist only** on, every request or WebSocket to a destination no rule
covers asks first in the **ForkMesh Firewall** dialog
(`MainWindow::promptFirewallRequest`, `MainWindowNetworkDiagnostics.cpp`). The
dialog runs inside the call that wants the network
(`BackoffNetworkAccessManager::createRequest` or
`authorizeFirewallConnection`), so it opens a nested event loop while it waits.

- **One dialog at a time.** Pollers keep firing while a prompt is open, and
  each request to another unlisted host used to open its own modal dialog
  nested inside the first. The newest one blocked the older one but often
  opened behind it, so **Allow** did nothing and the window looked frozen
  (fixed 2026-11-01). Now only one prompt is on screen. A request that comes in
  while it is open waits for the answer. A request to the same host takes that
  answer. A request to another host checks the rules again, because the answer
  is applied (and saved) as the dialog closes, before the outer request
  unwinds, so a wildcard just allowed covers it. Only a destination that is
  still unlisted opens the next dialog. If a waiting loop comes back with the
  prompt still unanswered (the app is quitting), the request is denied once
  instead of looping.
- Before the dialog opens, popups (menus, combo lists) are closed and any mouse
  or keyboard grab is released, because a grab sends the clicks meant for
  **Allow** to the popup. The dialog is then shown, raised and activated so it
  is not left behind another window.
- **Allow all for 1 minute** lets every unlisted destination through for 60
  seconds without asking, including the request that opened the dialog
  (`BackoffNetworkAccessManager::allowAllFirewallFor`, verdict
  `FirewallAllowOnce`). Nothing is remembered as a rule, and remembered
  denials still block. The Network page's firewall tab has the same button,
  which shows a countdown while the window is open and stops it early when
  clicked. The start and end are written to the system log
  (`firewallAllowAllChanged`). Requests let through this way show up in the
  decision history with no rule.

## Traps

- A code-approved phone stays in `paired-devices.json` regardless of the
  account; remove it in **Settings → Phone**.
- `accountDeviceCheck` may call `done` before it returns, and `requestPairing`
  closes a relayed connection; look the connection up by id again after either.
- `LocalControlServer` and `Http3Get` have no `Q_OBJECT`; register the header
  for moc before adding signals.
- A catalog record's `cloneUrl` is whatever the publishing node sent, and it
  is passed to `git clone --bare` (`MainWindowRepoFetch.cpp`) and
  `git fetch` (`fetchServedBranch`). Both put `--` before the source, so a URL
  that starts with `-` cannot become a git option. Sources are not limited to
  https, because local paths and LAN, ssh and http mirrors are real sources;
  git's own `protocol.allow` defaults still refuse `ext::`.
