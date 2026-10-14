# Desktop: networking

Part of [the desktop docs](README.md).

## Relay transport

- `src/ServerNode.*` and `src/ChatBackend.h` hold the relay HTTP and WebSocket
  connection; `src/BackoffNetworkAccessManager.*` and `src/NetworkBackoff.h`
  back off on 429/5xx. A 429 or 5xx the relay marks
  `x-forkmesh-expected-degraded` is an answer about that one route (a
  per-account quota such as the AI ask/workspace-name window) and opens no
  host cooldown; the caller backs off itself. A request suppressed by the
  cooldown fails locally with "ForkMesh relay is rate-limited (HTTP 429);
  backing off" only when a 429 opened it; a cooldown opened by a 5xx says
  "ForkMesh relay failed with HTTP 502; backing off" (the status that opened
  it, `NetworkBackoff::lastFailureStatus`). Before 2026-09-28 every suppressed
  request claimed a 429: the Tasks page's "relay is rate-limited" on
  2026-09-28 13:24 followed chat `room-access` 502s during a core restart, and
  on 2026-09-27 17:05 an AI quota 429 burst. The Tasks catalog pager itself is
  sequential (one page request at a time, up to 2000 tasks).
- A failure that lands while the host is already cooling down does not grow
  the cooldown, unless its `Retry-After` asks for longer. Such a reply came
  from a request sent before the cooldown opened, because no new request is
  sent during one. On 2026-10-01 00:48:58 a deploy restart of n3 answered
  about 15 in-flight desktop requests (chat `room-access`,
  `/api/nodes/events`, the world room socket) with Caddy 502s. Each one
  doubled the cooldown, so it reached the 5-minute cap. Two minutes later
  "delete agent" refused `DELETE /api/tasks/<id>` locally ("ForkMesh relay
  failed with HTTP 502; backing off") and no request reached a core. A burst
  now opens one 3 s cooldown. The agent-session delete
  (`deleteAgentSessionOrgTask`) also waits out a cooldown and retries up to 5
  times, as the Tasks page's delete already did, instead of keeping the
  session and reporting the error.
- The verbose net log writes `EXPECTED` instead of `ERR` for a reply its
  caller named as an answer, and such a line badges by its route crumb, never
  ERROR, whatever words Qt's failure text holds. `NetworkReplyError.h` has
  three marks: `x-forkmesh-expected-degraded` from the relay,
  `markExpectedHttpStatus()` (a Mastodon lookup's 404 "no such account"), and
  `markExpectedTransportFailure()`, for requests to a server someone else
  runs, where a TLS, DNS or connect failure means that server is broken. The
  CRM marks every request to a contact's Mastodon instance and every contact
  picture fetch (`instanceRequest()` in `CrmFeature.cpp`). This change followed
  a line from 2026-09-28: `net GET ERR SSL handshake failed: The certificate
  has expired https://mastodon.mxstbr.com/api/v1/accounts/lookup?acct=mxstbr`.
  That line badged ERROR and pinged about a contact's self-hosted instance. A
  transport failure on a marked request carries Qt's own text, not "you may
  be offline".
- The account event socket delivers topics that replace polling;
  `src/MainWindowRepoSync.cpp` dispatches them (for example `cloud-agents`
  refreshes cloud agent rows, `tasks` reads the private task board).
- A relay instance is a security boundary: repository lists, bearers, chat and
  queued agent work are scoped to one relay origin
  ([www/docs/qt-client.md](../../www/docs/qt-client.md#multiple-relay-instances)).
  `src/RelayRepositoryScope.h` decides which relay owns a repository record.
- The desktop signs requests with the node key and sends no `Origin` header, so
  routes that require a browser session token do not work from it.
- `ServerNode::onSocketReadyRead()` and `NodeEventSocket::drainReadBuffer()`
  parse several frames from one buffer. Any handler they call (a frame's
  signal, a failed write's `disconnected`) can drop the socket, so both loops
  stop once `m_socket` is no longer the socket they started with. The close
  path re-checks before `flush()` and before `disconnectFromHost()`. Without
  this, a close frame queued behind a frame that dropped the link dereferenced
  a null socket (SIGSEGV at `ServerNode.cpp:876` and in `drainReadBuffer`,
  2026-09-24 and 2026-09-25). `forkmesh-chat-transport-tests`
  (`testTeardownMidBufferLeavesSocketAlone`) covers the relay side.
- Cross-node pull request submission (`submitPullToInbox` in
  `src/MainWindowPulls.cpp`) is at-least-once. The signed proposal is stored in
  QSettings (`pulls/pendingSubmissionsV1`) before the first POST and restored
  at startup after `startNetworking()`. It stays there until the relay answers
  2xx. Submissions go out one at a time, with a 15 s transfer timeout. A
  failure with no HTTP status, or 408, 425, 429 or 5xx, is retried with
  backoff from 5 s doubling to 5 min, logged only. Any other failure
  drops the entry and shows the relay's `{"error": ...}` reason, as before.
  A retry sends the same signature, and the relay (`pulls_handler`,
  [app/README.md](../app/README.md)) answers it with the original number and
  `duplicate: true`. The pending entry is keyed by the signature, so
  submitting the same signed proposal twice queues it once. Success shows a
  status-bar message rather than a dialog, because a retried delivery can
  land long after the user moved on.
- `RoomSeal::acceptKeyFrame()` writes the peer list to QSettings only when the
  peer is new. A key id is derived from its key, so a repeated key frame only
  moves the peer to the end of the recency order, and the next new peer
  persists that order. Writing on every key frame rewrote the whole settings
  file (600 KB) on the GUI thread and was the most frequent stall.

## Cloudflare Worker log tail

- `MainWindow::setCloudLogMonitorEnabled()` (`src/MainWindowLogs.cpp`) runs
  `npx wrangler tail` in the Worker bundle directory, so it tails the Worker
  named in `app/wrangler.toml`. Setting `diagnostics/cloudLogMonitor`; Settings
  box "Watch the deployed Worker's log".
- The served stack has no Workers. A tail answers "This Worker does not exist
  on your account. [code: 10007]" (`cloudflareTailWorkerMissing()`), and
  retrying cannot help. The monitor stops without scheduling a restart, logs
  one line, and records the Worker name in
  `diagnostics/cloudLogMonitorMissingWorker`. Later launches skip the tail
  while `cloudflareWorkerName()` of the bundle still matches that name.
  Ticking the Settings box again clears the mark. Before this, every launch
  ran five failing Wrangler starts and logged the Cloudflare error each time.
- The Node.js probe (`findNodeToolchain()`, which spawns `node --version` for
  each candidate) runs on a worker thread at launch. Its result is kept for the
  session, so tail restarts do not probe again.

## HTTP/3 client

QtNetwork stops at HTTP/2. `src/Http3Get.{h,cpp}` is an in-process HTTP/3
GET client built on OpenSSL's QUIC client and `libnghttp3`, driven by a
`QSocketNotifier` and a timer. It is compiled only when CMake finds OpenSSL QUIC
support and `libnghttp3` (`FORKMESH_HAVE_HTTP3`); otherwise
`forkmesh::net::http3Available()` is false. Trust comes from the system CA
store, so `SSL_CERT_FILE` works for tests. `Http3Get` is not a `Q_OBJECT`.

QUIC's idle timeout is 30 s on both ends, so long streams need application
data; the mirror gateway (`tools/mirror_gateway.py`, `NODE_LOG_KEEPALIVE_SECONDS`)
sends a keep-alive every 10 s.

Probe: `forkmesh-http3-probe <url> [Header: value]...` (see
[build-and-test.md](build-and-test.md)).

## Host logs

**Network → Hosts** live view (`src/MainWindowHosts.cpp`,
`src/HostLogSources.h`). A host's main-log feed reads the gateway's
`/v1/node/logs` server-sent events (`tools/mirror_gateway.py`) over HTTP/3 when
it can (Caddy serves it on UDP 443). Only Ed25519 keys the desktop's host
installer wrote to the gateway's allow file may open the feed. A host without that endpoint, a blocked UDP 443, or a build without
HTTP/3 falls back to one SSH connection that multiplexes the node log and the
systemd journal, and retries QUIC periodically. Mirrors have no local
`node.log`; the taps read what the host actually keeps.

## Fleet node logs

Since the 2026-09-24 cutover the site runs on the ForkMesh Next fleet: cores
`n1`..`n3` (app, rqlite, Caddy, the mirror node and the IDS) and the runner
`r1`, plus burst runners from the autoscaler. The Cloudflare Worker tail no
longer sees production traffic. `src/CoreNodeLogs.{h,cpp}` and the
`CoreLogFeed` code in `src/MainWindowLogs.cpp` follow every node's journal live.
The node side is documented in the Next docs (`docs/NODE_LOGS.md`,
`deploy/core/logread.py`).

- **Why the desktop pulls over SSH.**
  - It keeps working behind NAT: the desktop dials out.
  - It loses no lines: journald keeps them, and a cursor on disk resumes after
    the last handled line, even across a desktop restart.
  - Each stream is authenticated by its own node's SSH host key, so a
    compromised node can only lie about its own logs.
  - It stores nothing server-side, following
    [traffic stats from the tail, not the DB](../app/README.md).

  The rejected alternative was nodes pushing signed batches to the app. It
  would replicate every line through rqlite on the 1 GB cores, and it would put
  all nodes' streams in one database that any compromised core can write to.
- **Discovery.**
  - The fleet config directory is `~/projects/forkmesh/config/fleet`, and the
    keys are in `~/projects/forkmesh/ssh/fleet` (since the 2026-09-24 folder
    move). The pre-move `~/projects/forkmesh-next/{config,ssh}` is only a
    fallback while the new folders do not exist. `FMNEXT_CONFIG_DIR` and
    `FORKMESH_PROJECTS_DIR` override it, as in `tools/forkmesh_paths.py`.
  - Nodes come from its `fleet-instances.json`: `fmnext-core-N` becomes core
    `nN`, `fmnext-run-N` becomes runner `rN`.
  - Nodes also come from the Vultr API, using `vultr.env` from the same
    directory: instances tagged `forkmesh-next` with one of those labels.
  - Vultr is asked at start-up, every 15 minutes, and 30, 90 and 180 s after a
    core logs `runner pool: creating|destroying fmnext-run-N`. Burst runners
    therefore appear and disappear on their own.
  - A `QFileSystemWatcher` re-reads the fleet file when it changes.
  - Nodes never go in the Hosts list.
- **Transport.**
  - One `ssh -T` per node, as `root@<ip>`, with the dedicated reader key
    `<fleet ssh dir>/next_logread_ed25519` (`IdentitiesOnly`, `BatchMode`, no
    agent, no forwarding) and the fleet `known_hosts`. The fleet (root) key is
    never used.
  - On the node the key is a `restrict,command=` forced command: it can only
    run `fmnext-logread`. That reader drops to `nobody` plus `systemd-journal`,
    redacts every message before it leaves, and speaks a four-word protocol:
    `follow`, `follow lines N`, `follow after <cursor>` and `hello`.
  - The IDS files each connection as `ssh_tool_session`, info level.
  - First contact replays 200 lines. After that, the cursor of the last
    handled line is saved in `<data>/fleet_logs/cursors.json` (0600, written at
    most every 5 s) and resumed from. The periodic write runs on a worker
    thread, one at a time (its `QSaveFile` commit fsyncs and stalled the GUI
    thread for 500-600 ms, 2026-09-24). Only the write at window close stays
    synchronous; it first waits for any write already running.
  - A line is stamped with the desktop's clock when it arrives, so backlog
    replayed from the cursor at launch looked like fresh errors: on
    2026-09-25 the overnight Caddy 503s and IDS alerts raised pings at
    10:18. A line whose journal `__REALTIME_TIMESTAMP` is more than 15 minutes
    old (`corelogs::isBacklogEntry`, `kBacklogAlertMs`) still enters the log,
    with "(logged <time>, replayed)" appended, but under CLOUD, not ERROR, so
    it raises no ping and no error-repair dispatch.
  - `corelogs::isKnownBootNoise` demotes the boot-time
    `systemd-ssh-generator` "Failed to query local AF_VSOCK CID" pair. Vultr
    VMs have no vsock, so the generator fails on every boot, harmlessly.
  - Past 20,000 queued lines every stream is stopped, not dropped. Streams
    restart from their handled cursor once the queue is under 2,000.
  - Retry backoff runs from 30 s, doubling to 10 min, and resets after 3 minutes
    up. The node table shows "reader key not accepted" for a node that was
    never rolled with the reader.
- **What reaches the main log.**
  - Forwarded: every journal line with a message, from every unit (since
    2026-09-26; before that only the app, rqlite, Caddy, the mirror node,
    `fmnext-*`, `forkmesh-*`, earlyoom, fail2ban, job scopes and non-info lines
    from elsewhere). Sources (`corelogs::journalSource`): `mail` (Postfix,
    OpenDKIM, `fmnext-mail-events`), `app`, `rqlite`, `web`, `job`, `mirror`,
    `ids`, `ops`, `ssh`, `kernel` and `system` (systemd, cron, logind and the
    rest). sshd stays at info whatever its priority (internet scanners), and
    the boot-time vsock noise below is info, not an error.
  - Each line reads `Node <node> <core|runner> <unit> <err|warn|info>: ...`.
    Node names are `nN` (core), `aN` (autoscaled app replica), `rN` (runner)
    and `bN` (burst runner); `parseNodeLinePrefix` accepted only `n`/`r` until
    2026-09-27, so lines from autoscaled and burst nodes fell through to the
    substring scan and the Nodes filter hid them. The unit drops `.service`;
    job and run scopes collapse to `job` and `run`; PID 1's `init.scope` reads
    `systemd`; SSH session scopes show their syslog identifier.
  - Each line goes under its group (`corelogs::nodeLineBadge`, since
    2026-09-27; before that everything but errors was **Cloud**): **App**
    (forkmesh-app), **Cron** (its `cron...` lines), **DB** (rqlite and
    statements), **Web** (Caddy), **Daemon** (the Go daemon: git on cores, jobs
    on runners), **IDS**, **Cert** (`fmnext-cert*`), **Ops** (earlyoom,
    fail2ban, other `fmnext-*`), **Job** (runner scopes), **Mail** (Postfix,
    OpenDKIM, `fmnext-mail-events`), **System** (kernel, systemd, the rest). **Cloud** is left for the Worker tail and the feed's own
    notes. A live error goes under **Error**; a replayed one (the
    "(logged ..., replayed)" note) keeps its group, also when re-read.
    `networkLogStyleFor` reads the tag, so a re-read line is never re-badged by
    the substring scan. The groups and their colours are in
    `LogActivityLights::mutableCategories()`; like CLOUD and DB they stay out of
    the footer tail (`corelogs::isFleetBadge`) but pulse the lights.
  - Readable text: the daemon's JSON request record becomes
    `<operation> <status> · <ms> ms · <size>` (`requestRecordLine`, tried
    before the generic JSON reader). IDS and other JSON records are described
    in [../NODE_LOGS.md](../NODE_LOGS.md).
  - Colour: `corelogs::nodeLineHtml` draws the prefix (node name in its node
    colour, unit in its group colour, severity red/amber/grey) and marks the
    body per group: SQL keywords, literals and placeholders for DB; HTTP
    status by class; durations green/amber/red at 250 ms and 1 s; earlyoom
    percentages amber under 25 % and red under 10 %; JSON keys and `key=`
    names muted. Both the Log page (`formatLogLineHtml`) and the footer tail use
    it.
  - These count as errors:
    - journald priority err or worse;
    - Caddy JSON with `level` error, panic or fatal (Caddy logs everything at
      priority info);
    - Go `panic:` and `fatal error:`;
    - `ERROR` and `CRITICAL` lines;
    - IDS `alert` and `lockdown` incidents.
  - A Python traceback, which arrives as one journal line per frame, is folded
    into one `Traceback: <Exception> · at <file>:<line> in <function>` entry,
    so the error monitors and Most frequent group it by exception and site.
  - An identical line (timestamps stripped) from the same node and source is
    logged once per 10 minutes; the next copy says how many were folded.
- **Deploy lines.** See [../NODE_LOGS.md](../NODE_LOGS.md#deploy-output-on-the-log-page):
  `src/DeployLogs.{h,cpp}` follows the fleet logs' `autodeploy/` and `ops/`
  files and the publisher and deploy agent's user journal, and logs
  `Deploy <kind>: <line>`.
- **Filters.** The **Nodes** button beside the log's text filter picks nodes,
  units (from the lines seen so far) and **Node errors only**. It combines with
  the category chips and the text filter. While it is active, the desktop's
  own lines are hidden.
- **Web traffic.**
  - Each core's Caddy writes a filtered JSON access line per request to the
    journal (`[logs] access_log`): no headers except the User-Agent (as
    `user_agent`), the full client address (masked to /24 before 2026-09-27),
    credential query parameters replaced, `/readyz` and `/health` skipped.
  - The desktop counts each line into Network -> **Web Requests**
    (`WebRequestTally::recordBatch` with a count of 1): `/api/...` by route
    group, and everything else as `pages`, `world` or `git`. The per-node name
    and `/git/*` count as `git`.
  - Every access line is also one main-log line (since 2026-09-26):
    `Node n1 core caddy info: GET app.forkmesh.com/api/x -> 200 12ms · 🇩🇪 203.0.113.9 · curl/8.5.0`,
    under **Web** (**Cloud** before 2026-09-27), with the query string cut
    off. The flag comes from a local DB-IP Lite lookup
    ([../NODE_LOGS.md](../NODE_LOGS.md#country-flags-2026-09-27)). A
    5xx, including the 502s the app never answers, is `err` under **Error** and
    carries no duration, so repeats group in Most frequent. A replayed 5xx is
    under **Web**.
    Access lines skip the repeat folding: each request is its own line.
  - Once a node's access lines arrive, that node's app `web-requests` batches
    are ignored so nothing is counted twice. The batches only cover nodes
    without an access log.
  - Nothing is stored; the numbers live in memory for 24 hours.
- **rqlite statements.** The app's `_log_d1_query` line becomes
  `Node <node> core rqlite <kind>: <sql> · from <file> · for <caller>` under
  **DB**, with the same source link the Worker's D1 lines get. The caller is
  the request (method, path, client IP), the signed-in user and a session-ID
  prefix, or `cron`, `queue <name>` or `alarm <class>`
  ([../app/d1.md](../app/d1.md)); it is split off before statements are
  grouped, so Most frequent still counts one row per statement. The host shim writes these lines
  only with `FORKMESH_HOST_D1_LOG=1` in the core's `app.env`. Since 2026-09-27
  `deploy/core/render.py` writes it from `fleet.toml` `[app] d1_log` (default
  true; false writes 0), so the next Next deploy switches the statements on;
  never edit a node by hand (the IDS locks down on hand edits). Log -> **D1** shows the per-node feed table, statements per minute and
  the most frequent statements, all in memory.
- **Visualizer (2026-09-30).** Log -> **Visualizer** (`src/FleetVisualizer.{h,cpp}`)
  draws the same feed as a live picture, all in memory and only while the tab
  is open. Since 2026-10-01 the tab is one scrolling page: the fleet picture,
  then Every query, then This desktop.
  - **Statement flights.** Every rqlite statement flies from its node's orb
    to a bubble for the table it touched. `corelogs::statementShape` reads the
    verb and the first real table: it looks through subqueries and
    `UPDATE OR IGNORE`, and skips `json_each`. The fifteen busiest tables get
    their own bubble; the rest fold into "+N more". A bubble's area is its
    traffic and its ring is its verb mix. The header has a per-verb legend with
    counts and a per-second bar strip for the last minute. Below are the
    busiest modules (`from <file>`) and statements per node, in node colours.
  - **Verb colours.** SELECT, UPDATE, INSERT, OTHER (schema, pragma and
    transaction statements) and DELETE take the dataviz reference slots blue,
    orange, aqua, violet and magenta, in that order. That order passes the
    palette validator in light and dark; magenta beside orange failed. Each
    verb also has its own shape (dot, triangle, square, ring, diamond), so
    identity never rests on colour alone.
  - **Node vitals.** One card per node:
    - memory is a tank whose liquid level is the share in use, with swap as a
      bar under it;
    - disk is a four-drawer cabinet;
    - open file handles are a stack of pages, with inode use beside them;
    - CPU is a heartbeat line of the last 48 samples, with the load averages.

    Status colours switch at 75 % and 90 %, and the words ok, high and critical
    are always written beside them. The numbers come from the reader's
    `logread_vitals` sample ([../NODE_LOGS.md](../NODE_LOGS.md)). A node whose
    reader predates the sample shows a dashed tank and the `install nN --steps
    logread` hint. A sample older than 90 s says how old it is.
  - **Cost.** Animation runs at 25 frames a second, only while the tab is
    visible. Hiding the tab stops the timer and drops the flights. Statements
    seen while it is hidden are only counted. Flights are capped at 360, and a
    replayed backlog is staggered, not launched all at once. Hover any bubble,
    orb, legend chip or card for the exact numbers.
  - **Every query (2026-10-01).** Under the flights, `src/QueryInspector.{h,cpp}`
    takes every field of each statement line plus its `D1-done` timing line
    ([../app/d1.md](../app/d1.md)). The two are joined by
    `corelogs::statementKey`.
    - Its header gives the totals: statements, distinct statements, timed
      count, median, p95, slowest, local-read share and failures.
    - Four bar lists show who asked, code (`file · function`), statement kind
      and users. Who asked is the route with numeric and long hex segments
      folded into `:id`, or cron, queue or alarm, coloured by caller kind.
      Users counts signed-out requests and background work separately.
    - A latency histogram stacks each bucket (<1, 1-5, 5-20, 20-100, 100-500
      and 500+ ms) by verb. Beside it, time spent per table, or statement
      counts until timings arrive.
    - The Statements table holds the 14 top statements by calls, total time,
      average, max or rows per call. Click a column header to sort; it sorts
      by total time once the first timing lands. Hovering a row gives the full
      SQL, bound-value count, code, nodes, the top callers, rows, changes and
      local share.
    - Latest statements lists the last 12 statements with node, verb, table,
      kind, time, rows, code and caller. A timing fills in the newest matching
      row from the same node.
    - Memory is bounded: 3,000 statements, 1,000 names per list, 60 recent
      rows and 4,096 latency samples. It repaints at most four times a second,
      only while visible. `D1-done` lines feed only this view and the per-node
      line count, never the main log.
  - **This desktop (2026-10-01).** Last, `src/DesktopProcessView.{h,cpp}` shows
    the machine the desktop runs on. `src/DesktopProcesses.{h,cpp}` samples
    every 2 s, only while the tab is visible.
    - What it reads: every `/proc/<pid>/stat` (name, state, parent, CPU ticks,
      threads, start time, RSS), the owner of `/proc/<pid>`, and
      `/proc/{meminfo,loadavg,stat,uptime,sys/fs/file-nr}`. It also runs
      `statvfs` on the home folder. A process's CPU % is its tick delta since
      the last sample; 100 % is one core. The machine CPU is its own
      `/proc/stat` delta (not `SystemStats::hostCpuPercent`, whose shared
      state other pages read).
    - Gauges show CPU (history line and load), memory, swap, the home disk and
      open files.
    - Two share strips show where the CPU and the memory go, grouped by process
      name. The CPU strip is scaled to every core and the memory strip to
      total RAM, so the empty part is idle CPU and free memory.
    - The table lists the top 16 processes. Sort by CPU, memory, threads or
      name, and pick All or ForkMesh only. "ForkMesh" is this app's process
      and every process below it, found from parent links: agents, git and
      builds. Hovering a row gives the full command line, which is read only
      for the rows on screen.
    - Linux only; elsewhere it says so.
  - **Tab order.** The tab comes after Cron. The window tests address D1, Build
    and Cron by index.
- **Toggle.** Settings, "Follow the fleet nodes' logs, rqlite statements and
  web traffic" (`diagnostics/coreNodeLogFeed`, on by default). The feed is
  compiled out under `FORKMESH_WINDOW_TESTS`.

## Network > Nodes

The Nodes tab of the Network page is one table. Until 2026-09-24 the
repository comparison was a separate **Mirror nodes** tab on every repository;
from then until 2026-09-28 it was a second **Repository mirrors** table under
the directory, in a vertical splitter. Since 2026-09-28 Nodes is also the first
tab of the Network page: the **Relays** tab is gone and its rows are Nodes rows
with the role Relay. Later on 2026-09-28 (issue 718) the two tables became one.

- **Directory rows** (`src/MainWindowNodes.cpp`, `buildNodesSection`): every
  node the relay directory, the roster and the relay's serving set know, online
  or not, with the website Mirrors-table columns from the node's freshest
  catalog record, its telemetry and the admin Delete action. After them come
  one row per configured relay and one row per runner (below).
- **Repository copies** (`src/MainWindowReleases.cpp`,
  `buildMirrorNodesSection` / `loadMirrorNodesPanel`): the nodes holding a copy
  of one repository, compared with its source of truth (commit, sync lag,
  counts, the `↑` sync-back backlog, route and reachability proofs, integrity
  pin). The **Repository mirrors** row above the table holds the repository
  chooser, the summary, Online only, Reset integrity pin and Refresh nodes. The
  chooser follows the repository you open (`openRepoDetail` clears the pick)
  and otherwise falls back to the first repository. Double-clicking Health (or
  selecting it and pressing Enter) drafts the diagnostics prompt in that
  repository's composer, opening the repository first.

### One table (issue 718)

`loadMirrorNodesPanel` still builds its rows into `m_mirrorNodesTable`, which
is now hidden and serves as the per-repository model. Every rebuild of it
refreshes the Nodes table, and `mergeMirrorRowsIntoNodesTable` copies its cells
onto the node rows:

- A mirror row is matched to a directory row by node name (`kMirrorNodeKeysRole`
  holds the label, display name, chat name and node name; the desktop's own
  copy matches the "(this machine)" row). Relay rows are never matched.
- The mirror-only columns are Message, Author, Git DB, the five `↑` columns,
  Route and Reach. They sit beside their counterparts: Message and Author after
  Revision, Git DB after Size, each `↑` after its count, Route and Reach after
  Integrity. The mirror Commit goes into Revision, Repos into Mirrors, Website
  into Web hits.
- Repository facts (Revision, Synced, Size, the counts, Artifacts, Clones, Web
  hits, Health) take the mirror's value, which is relative to the chosen
  repository and carries the mismatch underline, the sync countdown and the
  diagnostics prompt. Node facts (Role, Owner, Version, Platform, CPU, RAM,
  Disk, Node id) keep the directory's value and take the mirror's only where
  the directory has none.
- A node with one copy shows it on its own row. Its Node cell gains the
  "★ source of truth" mark, the mirror's status tooltip and, when the copy is
  behind or failing the integrity pin, the spinning status light.
- A node with several copies keeps its own row and lists each copy on a line
  beneath it (`└ name`), grouped under it.
- A copy on a host the directory does not list (an SSH-only saved host, a
  catalog-only core) gets its own row, with Status from the mirror's liveness.
- Online only hides only the copies of offline nodes; every node stays listed.

Sorting is done by `sortNodesTable`, not by `QTableWidget`: Qt's sort moves
rows one by one, which would separate a node's copy lines from it. The table
keeps sorting disabled and re-orders groups (a node row and its copy lines) on
`sortIndicatorChanged`, comparing the node rows the same way
`SortTableWidgetItem` does and leaving empty cells last. The Role filter hides
copy lines with their node, and the status line does not count them as nodes.

### Roles

The table has a **Role** column and one Role filter (saved as
`network/nodesRoleFilter`). `src/NodeRole.h` classifies, in order:

1. a `role` field in the catalog record (`core`, `runner`, `mirror`, `desktop`);
2. core: `fmnext-nN`, `fmnext-core-N`, an autoscaled app replica
   `fmnext-aN` / `fmnext-auto-N`, or a name from the fleet file
   `<fleet config>/fleet-instances.json` or `autoscaled-instances.json` (`nN`, `aN`); runner: `fmnext-rN`,
   `fmnext-run-N`, a burst runner `fmnext-bN` / `fmnext-burst-N`, or the fleet
   file (`rN`, `bN`);
3. this machine: desktop node;
4. a saved Hosts entry: mirror;
5. catalog `runtimeMode` `headless`: mirror, `desktop`: desktop node;
6. a name starting with `mirror`: mirror; anything else: desktop node.

The mirrors section used to list only saved Hosts (plus this machine), so it
went empty once mirror35-38 were destroyed. It now also adds the repository's
published `/api/repo/<owner>/<repo>/mirrors` rows for core and runner nodes
(`fetchMirrorSectionCatalog`, sharing the Network Repos 5-minute cache).
Published rows of other desktop nodes stay out, as before.

The Role filter offers All roles, Core, Runner, Mirror, Desktop node and Relay.

### Relay rows

The former Relays tab (`buildRelaysSection`, `m_relaysTable`) is merged into
the directory. Every configured relay (`m_servers`) is a row with role
**Relay**, and every fact that tab showed is on it:

| Relays tab | Nodes row |
| --- | --- |
| Relay (favicon, name, bold when active, URL and instance id tooltip) | Node, same icon, bold and tooltip |
| Status: Checking…, Online, Offline with the failure reason tooltip | Status, same text and tooltip |
| Response time | Latency |
| Version (`version · rev`) | Version |
| "N of M relay(s) online · updated HH:mm:ss" | the page status line |
| double-click opens the relay website | double-click, or **Website** in Actions |

The Endpoint column shows the relay host and the Scope column says `active` or
`standby`. `refreshRelayNodes()` (was `refreshRelaysTable()`) resets every probe
to Checking… and sends one `/api/version` probe per relay (`probeRelayRow`) once the Nodes page exists (as the Relays tab did when built);
results are kept in `m_relayProbes`, keyed by host, and patched into the relay
rows in place (`updateRelayNodeRows`), so a probe answering while the page is
hidden is not lost. Pure helpers are in `src/NodeDirectoryRows.h`.

Everything that used to open Relays opens Nodes filtered to Relay
(`showNodesForRole`): section index 8, the top search's "Relays" entry, and the
account menu's instance item. The Stats page's Relays count, the top-bar relay
menu and relay search results are unchanged.

### Runner rows

Runners never join the chat room or the relay's serving set, so the directory
missed them. They are now merged from three sources by `mergeRunnerRows`:

- the fleet files `fleet-instances.json` and `autoscaled-instances.json`
  (`fmnext-run-N` is `rN`, `fmnext-burst-N` is `bN`), through
  `corelogs::loadFleetNodes()`;
- the `forkmesh-next` Vultr tag listing the log feed already reads
  (`m_vultrFleetNodes`);
- `GET /api/runners` (`m_runners`, the Hosts page's runner list), which gives
  scope, status, registrant, last check-in and, since 2026-09-28,
  `activeLeases`: the runner's unexpired `runner_job_leases`
  (`app/src/runner_logins.py`, the same count `/api/runners/pool-status` uses).

`fmnext-r1`, `r1` and `fmnext-run-1` are one runner (`runnerKey`). A registered
runner whose name is already a directory row (a core registered as a runner)
keeps its directory row only. Each runner row shows Role Runner, Status
(Online when an active runner checked in within 2 minutes; it polls every
~20 s), when it was last seen (Checked column), its job leases (Activity), its
scope (`pool` or `personal`), its registrant (Owner) and its address
(Endpoint). A fleet runner the account cannot see in `/api/runners` (the
registry is admin-only for pool runners) says **Not registered**.

Its action is **Logs**: it opens the Log page with the node filter set to that
runner's feed (`r1`, `bN`), which the fleet node log feed follows
([NODE_LOGS.md](../NODE_LOGS.md)). Runners are never added to the saved hosts
(`AGENTS.md`).

### Owner and version

Per [NODE_OWNERSHIP.md](../NODE_OWNERSHIP.md): the owner shown for a core is
the catalog `ownerUser`, and its version is the catalog `version`. The status
line also shows the app release from the relay's `/api/version` (`version` and
`rev`, via `noteAppRelease`); each relay row shows the same pair for that relay.
Runners publish no catalog, so their version is only in the daemon status.

### Mirrors "Online only" and a diverged core

On 2026-09-28 n1 was missing from Repository mirrors again. It was up: its
`/health` answered, the relay's `/api/network/stats` `onlineNodes` listed
`fmnext-n1`, and `git ls-remote` against it worked. But its `main` had diverged
from n2 and n3 (its own `issue #730: mirror intake` commit and a chain of
`ForkMesh Mirror Reconciler` merges that n2 and n3 never took; the core intake
fix `4fec2bcae6` was not deployed yet). Its signed refs were not the accepted
fleet state, so its `mirror_https_endpoints` row was not `forkmesh_active`, and
`/api/repo/<owner>/<repo>/mirrors` reported it `status: offline`,
`endpointFresh: false`, `lastSeen: null` (`app/src/entry.py`
`repo_mirrors_handler`: `reachable_nodes` needs the active attestation, and
`build_repo_mirrors_payload` ANDs it into `online`). The desktop's catalog-only
rows counted a node online only on room presence, a fresh endpoint probe or an
SSH snapshot, so the default **Online only** filter hid it: the same failure as
the earlier "a healthy fleet shown offline" fix, with a new cause.

The row now also counts as online when the relay's serving set lists the node
(`mirrorsync::catalogMirrorLiveness`, the set the directory already trusts).
Its tooltip says the relay reports it live but its endpoint lease is not
current, so clones are not routed to it, and the commit and count mismatches
still mark it. `forkmesh-control-tests` (`checkCatalogMirrorLiveness`) and the
window suite pin it with n1's live record.

### Redirects from the old tab

The repository stack keeps a plain page at index 13
(`kRetiredMirrorNodesDetailTab`) so later tab indices do not move. A saved
`kLastRepoDetailTabSetting` of 13 is rewritten to 0 at startup and opens
Network > Nodes for that repository; a navigation-history entry or an
`openRepoDetail` landing on 13 does the same.

### Tests

- `forkmesh-control-tests` (`checkNodeRoles`): the classifier and fleet file.
- `forkmesh-window-tests` (issue 718): the one Nodes table carries every
  former Repository mirrors column (Commit as Revision, Repos as Mirrors,
  Website as Web hits); a core's repository copy lands on its row; one copy
  merges into its node's row and two copies are grouped as lines under it in
  both sort orders. The single-line, draggable-column and compact-header checks
  now look at the Nodes table.
- `forkmesh-window-tests`: every former Mirror nodes column caption is present
  on the merged page plus Role; Online only, Reset integrity pin, Refresh nodes
  and the two choosers exist; a core row appears from the published catalog;
  the Role filter; the redirect; no Mirrors tab in the repository row.
  `FORKMESH_NODES_SHOT` and `FORKMESH_MIRROR_NODES_SHOT` save screenshots.
- `forkmesh-window-tests` (`--nodes-page-only` runs just these): no Relays tab
  and Nodes first; relay rows with Role, Status, Latency, Version, Endpoint and
  Scope; section 8 opens Nodes filtered to Relay; runner rows from the fleet
  and `/api/runners` with status, lease count, scope and the Logs action; the
  Role filter's five choices; n1 with the diverged live record stays in
  Repository mirrors with Online only.
- `forkmesh-runner-login-tests`: `runnerKey`, `mergeRunnerRows`, runner status
  text and `activeLeases` parsing.
- `app/tests/test_runner_logins.py`: `activeLeases` counts only unexpired
  leases per runner.

## Hosts and fleet operations

`src/ControlNode.{h,cpp}` builds every remote command: install, update, uninstall,
Actions, size map, agent CLI install and sign-in, and the fleet binary upload.
All of them authenticate with the shared fleet key (`sharedHostKeyPath()`:
`ssh/forkmesh_shared_ed25519`, or legacy `ssh/vultr_mirror_ed25519` when that
is the one present). Passwords are only a fallback and are passed via
environment/stdin, never argv. Manual Vultr mirror provisioning:
`src/MainWindowVultrMirrors.cpp`.

### Runners

Network → Hosts has a **Runners** list right under the saved-host table
(`hostsRunnersTable`, filled by `refreshRunners()` in
`src/MainWindowHosts.cpp`, rows built by `src/RunnerListView.*`). It shows the
app's cloud agent runners (for example `fmnext-n1`) from `GET /api/runners`,
which answers pool runners to app admins and a personal runner to the account
that registered it. Runners are never added to the saved hosts (see
`AGENTS.md`). The list loads when the page is built, when the Hosts tab is
opened, on its **Refresh runners** button, and on the page's 30 s timer while
Hosts is visible. Without an account session token it says to sign in.

Each row shows status and scope, when the runner last checked in, and per
provider **Signed in**, **Needs sign-in**, **Not installed** or **Not
reported**, with "sign-in open" when a sign-in session is still running.
Active runners get **Sign in Claude** / **Sign in Codex** (**Re-sign in** when
already signed in, **Open … sign-in** to re-attach to an open session). The
button opens the runner sign-in dialog described in
[../RUNNERS.md](../RUNNERS.md#desktop-app-integration).

### Add core node

The ForkMesh fleet table on Network → Hosts has an **Add core node** button
(`hostsAddCoreNodeButton`). It is shown only to an admin account on a desktop
that holds the fleet config (`config/fleet/fleet-instances.json`) and finds
`tools/next_fleet.py` (`findNextFleetScript`, override `FORKMESH_NEXT_FLEET`;
Python as for the autoscaling panel). While a run is going, the button stays
visible and reads "Adding core node…". The UI is
`src/AddCoreNodeDialog.{h,cpp}`; parsing, formatting and state are in
`src/AddCoreNodeModel.{h,cpp}`.

- **Choose.** A **Cloud** choice (`addCoreProvider`: Vultr or AWS) comes
  first. `next_fleet.py add-core-options --json` (with `--provider aws` for
  AWS) fills in the next name, the region and plan the existing cores use,
  the plans in each region with their monthly price, the time estimate and
  the rqlite role with its reason. AWS plans are the built-in instance-type
  table with estimated prices ("(estimate)" after the cost, `price_note` as
  its tooltip), defaulting to the cheapest x86_64 type with 1 GiB, `t3a.micro`
  ([../deploy/README.md](../deploy/README.md#cloud-providers)). Without
  `aws.env` the dialog says how to write it and keeps Next disabled. When a
  half-added core is on record, Next is disabled and the dialog opens on its
  progress with Retry.
- **Confirm.** Lists what will happen: a new VM billed to the Vultr or AWS
  account, the peers updated one at a time, the rqlite role, the release and
  the repository sync, DNS only after health, and the branch to merge. For
  AWS it also names the tags, IMDSv2, the encrypted disk, the security group
  and the missing reverse DNS. Create runs `add-core --execute --provider aws
  --region R --plan T`. Retry passes only `--name`, because the provider is
  in the run's state file.
- **Progress.** A step list built from the `FMNEXT-ADD-CORE {json}` lines the
  command prints (start, step running/done/failed, result), and the rest of
  the output as the log. Success reads "nN is serving, in DNS" and names the
  branch to merge. A failure shows the step, the error and the containment
  notes, and **Retry** runs `add-core --execute --name nN`, which resumes.
- **Lifetime.** `AddCoreNodeController` belongs to the main window, not to
  the dialog. It starts the command as a `systemd-run --user` unit
  (`fmnext-add-core-<stamp>`) that writes to
  `logs/fleet/add-core/<stamp>.log`, and it tails that file. Closing the
  dialog, or quitting the desktop, does not stop the run; at startup the
  controller reattaches through `logs/fleet/add-core/current.json`. Without
  `systemd-run` it falls back to a child process.
- `rememberHost` refuses a name starting with `fmnext-` and any address in
  the fleet table (`AGENTS.md`), so no path can add a fleet node to the saved
  hosts.

Tests: `forkmesh-add-core-node-tests` (`tests/test_add_core_node.cpp`) drives
the real dialog and controller against a fake `next_fleet.py`. It checks the
defaults, prices, the confirm text, the failure and Retry flow, reopening,
and a run that outlives its dialog. Pass `--screenshots DIR` to save each
page. With `FORKMESH_ADD_CORE_TEST_SYSTEMD=1` it also checks that a
`systemd-run` run outlives its controller and that a new controller
reattaches to it.

### AWS credentials

**Settings → Cloud** (`src/AwsSettingsPanel.{h,cpp}`, storage and signing in
`src/AwsCredentials.{h,cpp}`) takes an IAM access key ID, the secret access
key (a password field) and a region (a list of the commercial regions).

- **Save** encrypts `{accessKeyId, secretAccessKey}` with AES-256-GCM
  (associated data `forkmesh-cloud-credentials-v1|aws`) and stores
  `v1:<base64 nonce|ciphertext|tag>` under `cloud/aws/sealed` in
  `ForkMesh.conf`. The region is stored in plain text under
  `cloud/aws/region`. The 32-byte key is a random file,
  `data/secrets/cloud-credentials.key`, mode 0600 in a 0700 folder. So the
  settings file, a settings export or a copied `ForkMesh.conf` holds no
  usable credential. Without that key file the credentials cannot be
  decrypted, and the panel says so. A tampered blob fails the GCM tag check.
  This is encryption at rest separate from the settings, not protection
  against code running as the same user.
- The secret is never shown again. A reopened panel shows the key ID and
  "stored (encrypted); type to replace". Save or Test with an empty secret
  field uses the stored one when the key ID matches.
- **Test** sends `sts:GetCallerIdentity` (read-only, no IAM permission
  needed) to `sts.<region>.amazonaws.com`, signed with Signature V4 in C++
  (`authorization()`; checked against the AWS test-suite vectors), and shows
  "Connected: AWS account <id> (<arn>)" or the AWS error code. The status
  line never contains the secret.
- **Write aws.env for the fleet tools** writes `aws.env` (mode 0600) into
  the fleet config folder (`corelogs::fleetConfigDirectory()`) for
  `next_fleet.py` and Add core node. That copy is plain text, like
  `vultr.env`, and is written only when you press the button.
- **Forget** removes the sealed blob. The key file stays, because it holds
  nothing on its own.
- Only `AKIA` user keys are accepted here. Temporary `ASIA` keys need a
  session token, so put those in `aws.env` by hand.
- The panel also shows the cheapest fleet-ready instance type
  (`cheapestInstanceType()`, `t3a.micro`) and its estimated monthly cost.

**Vultr gap.** The Vultr API key is still stored as the plain-text device
variable `VULTR_API_KEY` (`actions/variables` in `ForkMesh.conf`,
`ActionStore::setVariables`), and the one-click Vultr mirror card also writes
it to `app/.env.production`. Moving it into the store above is follow-up
work. The one-click **Create a Vultr mirror** card stays Vultr-only. AWS
nodes are created through Add core node or `next_fleet.py`.

Tests: `forkmesh-aws-credentials-tests` (`tests/test_aws_credentials.cpp`)
points HOME, the XDG folders and QSettings at a temporary folder and drops
`FORKMESH_HOME` and `FORKMESH_PROJECTS_DIR`. It covers the signature
vectors, the STS request and response parsing, the instance-type choice, the
store (no plaintext in the settings file, 0600 key, round trip, tamper and
missing-key refusals), `aws.env`, and the real panel driven through a fake
transport (Save, reopen, Test success and failure, Write aws.env, Forget).
`forkmesh-add-core-node-tests` also drives the AWS choice end to end.
### Remove core node

Each core row (`nN`) of the fleet table has a **Remove…** button
(`fleetNodeRemoveButton`) for the same admins on the same operator desktops
as Add core node. With 3 or fewer cores listed it is disabled and its
tooltip says why (`coreCountBlocker`): removing one would break rqlite
quorum. Only one removal runs at a time; while it runs, that row reads
"Removing…" and reopens the progress. The UI is
`src/RemoveCoreNodeDialog.{h,cpp}`; parsing, blockers and texts are in
`src/RemoveCoreNodeModel.{h,cpp}`, and the run reuses `AddCoreRun` with the
`FMNEXT-REMOVE-CORE` prefix and "Removing" wording.

- **Plan.** `next_fleet.py remove-core-plan nN --json` gives what will
  happen, the voters before and after, the leader and the provider. When a
  non-voter core must become a voter first so 3 voters remain (`promote`,
  for example removing `n1` after Add core node made `n4` a non-voter), the
  summary says so first: "make n4 an rqlite voter first … so 3 voters
  remain" ([../deploy/README.md](../deploy/README.md#removing-a-core-node)). When the
  tool refuses (quorum, an unreachable cluster, an unfinished add-core) the
  reason is shown in red and the confirmation is disabled.
- **Instance.** "Also delete the cloud instance (irreversible)" reloads the
  plan with `--destroy-instance`; the summary then says the instance is
  deleted and the button reads "Remove nN and delete its instance". Unticked,
  the instance is kept and listed.
- **Confirmation.** The node name must be typed exactly; until then the
  button stays disabled (`removeCoreBlocker`).
- **Progress.** `remove-core nN --execute [--destroy-instance]` runs as a
  `systemd-run --user` unit (`fmnext-remove-core-<stamp>`, log in
  `logs/fleet/remove-core/`), and `RemoveCoreNodeController` reattaches
  through `logs/fleet/remove-core/current.json`, as for add-core. Retry runs
  the same command, which resumes. Success names the branch to merge.

Tests: `forkmesh-remove-core-node-tests` (`tests/test_remove_core_node.cpp`)
checks parsing, the blockers and typed confirmation, the arguments (no
`--destroy-instance` unless ticked), and drives the dialog against a fake
`next_fleet.py`: a refused plan, a failed run with Retry, and a destroy run.

### Fleet autoscaling panel

Network → Hosts carries the **Fleet autoscaling** panel
(`src/FleetAutoscalePanel.{h,cpp}`, parsing and validation in
`src/FleetAutoscaleModel.h`), between the saved-host table and the
provisioning cards. It replaces the old "Automatic fleet sizing" of saved
Vultr mirrors (`hosts/healthyMirrorFleet/*`): that code is gone, so the
desktop never creates or destroys a mirror on its own any more. The old
settings keys are ignored.

The scaler runs leaderless on the core servers; the desktop is only its
control plane. The panel is a front end for `tools/next_autoscale.py` (found
through `findNextAutoscaleScript`, override `FORKMESH_NEXT_AUTOSCALE`; run with
the source checkout's `.venv` Python, override `FORKMESH_FLEET_PYTHON`), which
signs with the operator key and talks to the cluster. The design, limits and
defaults are in [../fleet/AUTOSCALING.md](../fleet/AUTOSCALING.md).

- **Kill switch** at the top (`killSwitch` in the settings): when saved on,
  no core takes any scaling action, whatever the pools say.
- **Settings**, per pool (App capacity, Burst runners): enable, dry run,
  min/max, scale-up thresholds and duration, scale-down rule, cooldown, Vultr
  plan and region, monthly cap, and the boot snapshot id (both pools boot
  from snapshots; the snapshot the cores report and when it was built are
  shown read-only). **Save settings** validates them (at most 4 nodes,
  min ≤ max, cooldown ≥ 5 min, cap ≤ $100), then pipes them to
  `next_autoscale.py settings set`, which signs them. Turning a pool live
  (enabled, not dry run, kill switch off) asks first.
- **Status**: `next_autoscale.py status --json` on show and every 60 s while
  the panel is visible (no timer runs while Hosts is hidden): whether a core
  ran a scaler tick in the last 3 minutes (owner core and last tick), whether
  the settings verify, each pool's last decision and reason, spend against the
  cap, the signals, the scaled nodes (`aN`, `bN`, label, address, state, age)
  and the last scaling events.
- **Scale up now / Scale down now** run `next_autoscale.py scale <pool>
  <up|down> --execute` after a confirmation that names the plan's cost and the
  cap. The request is queued on the cluster and a core carries it out; the
  command returns at once, and progress shows in the refreshed status. Pool
  limits, the cap, the cooldown and the kill switch still apply.
- **Node logs**: after each status refresh the panel writes the autoscaled
  nodes (`fmnext-auto-N`, `fmnext-burst-N` → `{"id", "ip"}`) to
  `<fleet config>/autoscaled-instances.json` (0600, atomic, only when it
  changed; nodes the status no longer reports drop out). Node-log discovery
  and the Nodes role classifier read it next to `fleet-instances.json`, and
  the file watcher re-lists when it changes, so `aN` and `bN` show on
  Network → Nodes and in the Log page like cores and runners
  ([../NODE_LOGS.md](../NODE_LOGS.md)).

Tests: `forkmesh-control-tests` (`checkFleetAutoscaleModel`, and
`checkNodeRoles` for the labels); `forkmesh-core-node-log-tests` (label
discovery and `autoscaled-instances.json`); `forkmesh-mirror-fleet-window-tests`
(`--mirror-fleet-only`) runs the panel against a fake scaler script with an
isolated `FMNEXT_CONFIG_DIR` and checks the old sizing widgets are gone.

## Local mesh

`src/LocalMeshNode.*` discovers other ForkMesh devices on the LAN (UDP beacons)
and links them over TCP, both on port 48732 (`kDefaultDiscoveryPort`). The
`#mesh` room key is shared by every install and proves nothing about identity.
`src/MeshFirewall.*` generates the firewall rule for `ufw`, `firewalld` or
Windows `netsh`. An empty `#mesh` usually means the firewall drops 48732.

The setting `localMesh/firewallOpened` records the firewall and port
(`ufw:48732`) once this computer is known to let the mesh through: the Allow
button's command succeeded, or a device on the network linked in. After that
the desktop never shows the Allow banner or the `sudo ufw allow …` command
again. If a nearby device is heard but cannot link, it logs once that this
computer's firewall is already open and the other device's firewall is the
likely block. Before 2026-10-06 it showed this computer's ufw command again
whenever any peer failed to link, even with the rules already in place. A
user who resets their firewall can clear the setting to get the banner back.
A `#mesh` message wakes iPhones that have left the room through their wake
tokens ([../native/MESH_PUSH.md](../native/MESH_PUSH.md)).
Direct messages also travel over the mesh, sealed to the other person's
linked devices only
([../app/chat-private-rooms.md](../app/chat-private-rooms.md#direct-messages-over-the-local-mesh-2026-10-09)).

The one-line transport diagnostics (`udp 48732 bound · tcp 48732 · … · bt off`,
`LocalMeshNode::diagnosticsLine`) sit behind the pulse button in the `#mesh`
header since 2026-10-12 (issue 1928). The bar starts hidden on every launch;
the button shows it, and its one-second refresh timer runs only while it is
shown. Before, the bar was always visible in `#mesh`. iOS does the same: a
pulse (`waveform.path.ecg`) circle in the `#mesh` header of `ChatScreen.swift`
shows `MeshDebugBar`, which starts hidden. Android never showed the line in its
chat UI; only its tests read `LocalMeshRoom.diagnosticsLine()`.

### Over Bluetooth

The room runs over Bluetooth as well as the LAN, so it works with no Wi-Fi and
no internet. Bluetooth only carries frames that are already encrypted; framing,
the room key, the seal, history and the duplicate-link rule are the same as on
TCP, so a device on both bridges them. Built only when Qt Bluetooth is found
(`FORKMESH_HAS_BLUETOOTH`). The Linux release binary is built without it
(`docs/release/linux-baseline.md`), so there the mesh is LAN-only.

- **RFCOMM** (Bluetooth Classic), SDP service `7f9e5c10-…-2b7ad1e05f11`:
  desktop ↔ desktop and desktop ↔ Android. Both sides listen and dial.
- **BLE GATT** (added 2026-09-28): the iPhone path. iOS gives apps no RFCOMM,
  so the iPhone advertises GATT service `7f9e5c14-…`. It has an inbox
  characteristic `…c15` (write with response, central → peripheral) and an
  outbox characteristic `…c16` (notify, peripheral → central). Together they
  carry the link's byte stream in order; the room's 4-byte length framing
  runs on top. A zero-length value in either direction closes the link.
  The desktop (`src/BleMeshLink.*`, a `QIODevice`, so `attachLink` treats it
  like a socket) and Android are BLE **centrals** only. They scan for the
  service, connect, subscribe to the outbox, and open the link once the CCCD
  write lands. The iPhone is both central and peripheral, so two iPhones
  also link.
- Write chunks are `MTU - 3`, capped at 512. When Qt reports no MTU the
  desktop sends 512 and BlueZ turns it into a long (prepared) write, which
  the iPhone reassembles by offset. Throughput is a few KB/s: fine for chat,
  slow for large files.
- The BLE scan runs after each RFCOMM service discovery finishes, on the same
  two-minute beat, so the two inquiries never overlap on BlueZ.
- Two iPhones would each connect to the other. Each advertises a random
  per-launch token as its local name (`FM-xxxxxxxx`), and only the higher
  token dials. A desktop or Android phone does not advertise, so it always
  dials.
- A Bluetooth link the room drops as a `duplicate link` (the same device is
  also on the LAN) puts that address on a 10-minute hold-off. Without it,
  every rescan would redial and be dropped again. This applies to RFCOMM
  dials too.
- iOS asks for Bluetooth permission only after the user opens `#mesh`
  (`ChatHub.select` / `roomShown` → `LocalMeshService.roomOpened`), not at
  launch. The app has no Bluetooth background modes, so a backgrounded iPhone
  stays reachable only by other iPhones.
- The diagnostics detail has a `ble: seen · dial · open` line while Bluetooth
  is on.

Tests: `forkmesh-chat-transport-tests` pins the UUIDs and chunk sizes
(`testLocalMeshCrossClientConstants`). Two machines can't be linked over
the air in CI, so a real link needs a device check.

### Staying linked (issue 866, 2026-09-30)

The room used to wedge: a device that dropped off Wi-Fi, slept or crashed left
a half-open TCP link that nothing noticed. Its peer stayed "linked" and never
redialed, and messages written into the dead link showed as delivered. When
the device came back and dialed, the duplicate-link rule often closed the new,
live link and kept the dead one, on every redial, until a restart. The desktop,
Android and iOS now behave the same way:

- **Keepalive.** Every mesh TCP link sets `SO_KEEPALIVE` with a 10 s idle,
  a 5 s interval and 3 probes, plus `TCP_USER_TIMEOUT` 30 s where the OS has it
  (`enableLinkKeepAlive`). A dead link closes in about 25 s. Because this is
  done in the kernel, it works against peers running older versions too.
- **Duplicate links.** When a hello names a peer that already has another
  link, both ends apply the same rule in this order:
  1. If exactly one of the two links is Bluetooth, keep the TCP link and
     close the Bluetooth one as a `duplicate link`.
  2. Otherwise, if the older link's hello is at least 10 s old, close the
     older link as `replaced`. A peer only dials when it has no link to us,
     so a new dial means the old link is dead on its side.
  3. Otherwise (both links are new, so the two ends dialed at once), use the
     node-id tiebreak.
- **Dials** give up after 8 s. Before this, a dial to a device that had left
  kept its `address:port` in flight for the OS SYN timeout (about 2 minutes on
  Linux), which blocked the redial when the device came back.
- **Redial.** When a LAN link that had been up for 5 s drops (for any reason
  except `duplicate link`, `replaced`, `identity changed` or a `bye`), the
  node redials the peer's last address after 1.5 s. Any such drop, over TCP
  or Bluetooth, also starts a Bluetooth scan after 5 s instead of waiting for
  the next two-minute rescan. A TCP link that drops lifts every Bluetooth
  `duplicate link` hold-off, so the room can fall back to Bluetooth right away
  when Wi-Fi goes.
- **Network changes.** The node never gives up for lack of a transport:
  `start()` fails only when encryption is unavailable. Each beacon beat (and
  `announceNow()`, which the desktop calls on every reachability change)
  opens the LAN listener as soon as an interface appears. When the set of
  local addresses changes, it closes links bound to an address that is gone
  and sweeps the subnet on that beat.

Phone specifics:

- **Android** (`LocalMeshRoom.kt`, `AndroidMeshTransport.kt`,
  `ChatFeature.kt`):
  - Each link has its own writer, so one stuck socket can't stall the rest.
    A write stuck for 20 s closes its link as `write stalled`.
  - A link that sends no hello within 15 s closes as `handshake timeout`.
  - Multicast is joined and sent on every private, multicast-capable
    interface. That includes hotspot `ap0`/`swlan0`/`p2p-*`; cellular, VPN and
    container interfaces are excluded (`isMeshInterfaceName`).
  - A Wi-Fi/Ethernet `NetworkCallback` and a return to the foreground both
    call `announceNow()`.
  - The Bluetooth permission is asked again until a resumed Activity has
    actually shown the prompt.
  - An adapter that is switched off and on restarts the RFCOMM server.
  - Keepalive is applied with `Os.setsockoptInt`
    (`MeshTransport.tuneLinkSocket`), because `jdk.net` is missing on Android.
- **iOS** (`LocalMeshService.swift`, `MeshSockets.swift`, `ChatFeature.swift`):
  - Coming back from the background calls `resume()`, which rebuilds the
    listener and discovery sockets (the port is kept) and announces.
  - An `NWPathMonitor` calls `networkChanged()`.
  - A UDP or accept socket that fails closes itself and is rebuilt on the
    next beat, instead of spinning on its error.
  - The hello carries `port`, and the listener tries 48732 first.
  - The Bluetooth `duplicate link` hold-off is 60 s, and it is lifted when
    the TCP link to that peer closes.

Still open: the Linux release is built without Qt Bluetooth, so the desktop
is LAN-only there. iOS has no Bluetooth background modes and no multicast
entitlement, so a locked iPhone leaves the room and finds peers only by
sweeping its subnet. iOS also asks for Bluetooth only once `#mesh` has been
opened.

### Over a phone's tether or hotspot (issue 1325, 2026-10-02)

A computer on the phone's Personal Hotspot, or tethered to it over USB or
Bluetooth, is on a network the phone itself hosts. On an iPhone every such
computer sits behind `bridge100`, a 172.20.10.0/28 with the phone at
172.20.10.1; on Linux the iPhone's USB tether shows up as an `ipheth`
interface (`enx…`) at 172.20.10.2–14.

- **iOS** used to sweep and broadcast only on `en*`, `wlan*` and `eth*`, and
  always as a /24, so it never looked for the desktop on `bridge100`.
  `isSweptInterfaceName` (`MeshSockets.swift`) now also takes `bridge*`;
  cellular (`pdp_ip*`), VPN (`utun*`, `ipsec*`) and AirDrop (`awdl*`,
  `llw*`) are still left out. `MeshInterfaces.list()` carries each address's
  prefix length from `ifa_netmask` (Darwin may shorten its `sa_len`, so the
  bytes past it count as zero), and `sweepAddresses` /
  `sweepBroadcastAddress` take it: a /28 sweeps its 14 hosts and broadcasts
  to .15, and anything wider than /24 (or unknown) is still swept as the /24
  around the address, as on the desktop. Before, the /24 sweep sent most of
  its datagrams out of the cellular interface and the broadcast to .255
  went the same way.
- **Android** already swept hotspot and tether interfaces (`ap0`, `swlan0`,
  `rndis0`, `ncm0`, `bt-pan`): `isMeshInterfaceName` only skips cellular,
  VPN and container names.
- **Desktop** needed no change: a USB tether is an Ethernet interface to Qt,
  `sweepAddresses` already honours the /28, and the mesh firewall rule
  allows all of 172.16.0.0/12.

Tests: `testLocalMeshReturningDeviceReplacesStaleLink` in
`forkmesh-chat-transport-tests` (also run by `--mesh-only`),
`LocalMeshLinkTest.kt` on Android, and the new `LocalMeshTests.swift` cases on
iOS.

## Phone control over the LAN

`src/LocalControl.{h,cpp}`, `src/MainWindowLocalControl.cpp`. The phone app
lists, starts, steers, stops and continues agents and moves files both ways,
with nothing sent through the relay.

- `LocalMeshNode` hands any inbound link whose first frame is a
  `fm-control-hello` to `LocalControlServer`, so the mesh firewall rule covers
  it.
- Handshake: hello / welcome / auth, each side's Ed25519 identity plus a fresh
  X25519 key, both signing the transcript; frames are AES-256-GCM under
  per-direction HKDF keys with a counter nonce.
- A new phone signed in to the same account as the desktop is approved without
  a code (2026-09-27). After the auth frame the connection sits in `Checking`
  while `MainWindow::checkLocalControlAccountDevice` posts the phone's key to
  `POST /api/accounts/devices/check` with the desktop's account session. The
  server answers `owned` only when the key is the account's primary key or an
  enabled, unrevoked `owner_sign` device key. The phone's login bound that same
  key, so the server's answer is proof the phone is on the account; the
  self-reported `user` in the hello is not trusted for this. The desktop also
  requires the answer's `nodeName` to match the account it is signed in to now.
  The same check lets a same-account phone in over the relay.
- A desktop signed in without a password session (the silent login) has no
  session token, so the check used to answer "not owned" and the phone got a
  code on the LAN and `pair_on_lan` over the relay (#652, 2026-09-28). The
  desktop now signs the check with its account key instead:
  `forkmesh-device-check-v1\n<account>\n<phone pubkey>\n<ts>` in the body as
  `node`/`ts`/`sig`, which `_account_device_check` accepts when there is no
  session. The phone's key is inside the signature, so a proof cannot be
  replayed to ask about another key.
- The phone connects by itself to a desktop on the LAN whose beacon
  `ownerUser` is its own account, once per app launch, without being picked
  on Desktop on this network first (#652). Before, only an already-paired
  desktop was dialled automatically, so the Desktops screen showed a static
  "open ForkMesh on your phone" sentence while both sat on the same Wi-Fi.
  iOS: `LocalControlService.automaticTarget`; Android:
  `automaticControlTarget`. Disconnect and Forget still hold it off.
- Any other phone (another account, no desktop session, server unreachable
  within 8 s) shows a six-digit code on both screens; only the desktop user can
  approve it, and over the relay it is refused with `pair_on_lan`. Approved
  devices, both kinds: `data/local_control/paired-devices.json`, managed in
  **Settings → Phone**.
- A web browser uses the same channel over the relay (2026-09-28,
  [desktop-agents-web.md](../app/desktop-agents-web.md)). Its hello says
  `platform: "web"`; such a device is never stored in `paired-devices.json`
  and is checked against the account on every connect, so revoking the
  browser's `browser_control` key on the website locks it out at once. The
  agentops family gained `agentops.diff` (the branch's diff against its base
  plus uncommitted worktree changes) and a confirmed `agentops.delete`.
- Since 2026-10-02 the agentops family also serves the agent recycle bin:
  `agentops.bin` lists it, `agentops.bin.restore` brings agents back,
  `agentops.bin.purge` (after `agentops.prepare {action:"purge"}`) removes
  them for good and `agentops.bin.retention` sets how long they are kept
  ([../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md), "Recycle bin
  from the phone").
- New request methods go in `MainWindow::handleLocalControlRequest`. The phone
  is told about agent changes with an `agents.changed` push (at once, then at
  most every 300 ms). It follows a transcript with `agents.follow`, and the
  desktop then pushes `agents.output` as the log grows
  ([../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md), "Streaming
  transcripts and a faster channel").
- Both the LAN control socket and the relay socket (`NodeEventSocket`) set
  `LowDelayOption` (`TCP_NODELAY`), so a small frame is not held back by
  Nagle's algorithm waiting on the phone's delayed ACK.
- Relayed control frames go through a sliding-window pacer
  (`LocalControlStream.h` `SendBudget`) at 90% of the budget the relay
  advertises in its hello (`controlBudget`; 200 per 10 s when it does not).
  Frames over the budget wait in a queue (16 MB at most) instead of being
  dropped by the relay, which would end the channel.
- Test vectors are pinned in both `desktop/tests/test_local_control.cpp` and
  `native/ios/ForkMeshCore/Tests/FMChatTests/LocalControlTests.swift`; change
  both together.

### Relayed control: reliability and revocation (2026-10-01)

Phone control away from home mostly worked, but it failed in ways nobody saw.
A phone kept a channel that had died silently, so every request timed out
until the app was restarted. This happened after a desktop restart, after a
relay socket reconnect, or after a cluster ownership move (1012). The desktop
could also sit up to ten minutes between relay reconnect attempts. A phone
approved through the account kept control after it was revoked from the
account, because `paired-devices.json` never asked again.

- **Account-held approvals follow the account.** `Device` now carries
  `approval` (`"account"` or `"code"`) and `accountCheckedMs`, both saved in
  `paired-devices.json`.
  - `LocalControlServer::needsAccountCheck` asks the account on every connect
    for a browser, for a phone not stored yet, for an account-held phone, and
    for a relayed phone stored before this change (no `approval`).
  - A phone approved in person with the code is never asked about.
  - `accountDeviceCheck` answers `AccountCheck::Owned`, `NotOwned` or
    `Unreachable`. `checkLocalControlAccountDevice` maps 2xx `ok` to an answer
    and 400 `invalid_pubkey` to not owned. A 401, a 5xx, a network error, a
    missing account or a failed proof signature map to unreachable.
  - Not owned: the desktop forgets an account-held phone and refuses it
    (`pair_on_lan` over the relay, the code prompt on the LAN). A legacy
    record is refused over the relay but kept for the LAN.
  - Unreachable: a stored phone is admitted on the LAN. Over the relay it is
    admitted only if the account confirmed it within 24 hours
    (`kAccountGraceMs`); otherwise the desktop refuses with
    `account_check_failed`.
  - `maintain()` runs every minute. It re-asks for every paired connection that
    needs a check once ten minutes have passed since the last answer, and two
    minutes after an unreachable one. A not-owned answer closes the channel
    with `revoked`.
- **Dead channels are reported, not left to time out.** A relayed `data` frame
  for a session the desktop does not have is answered once with `close`,
  reason `channel_gone`. `m_relayToldGone` (64 entries) keeps the desktop from
  answering a stream of late chunks. When the close for a relayed channel
  cannot be sent because the relay socket is down, the session goes into
  `m_relayUndelivered`. `relayRestored()` sends those closes (`relay_reconnected`)
  as soon as `NodeEventSocket::connectedChanged(true)` fires
  (`onNodeControlRelayRestored`).
- **Bounded relay state.** A relayed channel silent for 15 minutes is closed
  (`idle`). When all `kMaxConnections` (8) slots are full, a new LAN or relay
  connection evicts the relayed channel that has been idle longest, if it has
  been idle for at least a minute (`evictIdleRelayed`). Half-received hellos are
  capped at 16 and expire after 30 seconds. Before, a phone could leave
  unbounded `m_relayOpening` buffers behind.
- **Relay socket.** `NodeEventSocket`'s reconnect backoff is capped at two
  minutes instead of ten. `reconnectNow()` skips the wait when
  `QNetworkInformation` reports the network back
  (`initRelayReachabilityWatch`).
- **Phones.** `RelayControlConnection` (iOS and Android) pings when it has
  heard nothing for 30 seconds (`keepalive`) and closes with
  `desktop_unreachable` when the ping times out. A desktop's error answer still
  counts as alive. `DesktopControlHub.link()` pings a relayed channel that has
  been idle for 45 seconds (`relayIdleProbe`) before handing it out, and dials
  a fresh one if it is dead. A request therefore never goes out on a dead
  channel, and a non-idempotent call such as `agents.start` is never sent
  twice.
- Tests: `testRelayResilience` and `testAccountRevocation` in
  `forkmesh-local-control-tests`. On iOS, `RelayControlTests.swift`
  (keepalive, `channel_gone`, hub probe). On Android, `LocalControlTest.kt`
  (the same three).

## Traps

- A phone approved in person (the six-digit code) stays in
  `paired-devices.json` whatever the account says; remove it in
  **Settings → Phone**. Phones approved through the account are dropped when
  the account stops listing them (above).
- Records written before 2026-10-01 have no `approval`. Over the relay they are
  checked against the account and become `"account"` on the first owned
  answer; on the LAN they are still let in without a check.
- `accountDeviceCheck` may call `done` before it returns (no session), and
  `requestPairing` closes a relayed connection. Look the connection up by id
  again after either.
- `LocalControlServer` and `Http3Get` have no `Q_OBJECT`; do not add signals
  without registering the header for moc.
- A relay that answers 429 makes fleet sizing skip a check; it retries on the
  next countdown rather than acting on partial data.

## OpenSnitch rules for ForkMesh (2026-09-30)

OpenSnitch keys an "allow always" answer to a program's exact path. Every new
desktop build, AppImage mount (`/tmp/.mount_*`) and test program built by an
agent in a worktree or a `~/.cache/fm*` folder runs from a new path, so each
one raised a firewall prompt on first launch; a prompt nobody answered timed
out into a per-path **deny** rule (the log on 2026-09-30 shows them for
`forkmesh-chat-transport-tests`, `forkmesh-window-tests` and
`ForkMeshCorePackageTests.xctest`), and denied loopback broke the publisher,
deploys and the iOS builds the same day.

`tools/opensnitch_rules.py` writes three rules, scoped to one account's user id,
named `000-forkmesh-*` and marked as priority rules so they win over the older
per-path deny rules:

| Rule | Allows |
| --- | --- |
| `000-forkmesh-programs` | any program whose file name contains `forkmesh` (any case) under a home folder, `/opt`, `/usr/local` or an AppImage mount |
| `000-forkmesh-dev-trees` | any program under `~/projects/forkmesh/` or `~/.cache/fm*/` (only written when `~/projects/forkmesh` exists; `--no-dev-trees` leaves it out) |
| `000-forkmesh-loopback` | connections to `127.0.0.0/8` and `::1` (`--no-loopback` leaves it out) |

Install once, as root, for the account that runs ForkMesh:

```sh
sudo python3 tools/opensnitch_rules.py install
python3 tools/opensnitch_rules.py print
```

The rules folder `/etc/opensnitchd/rules` is root-only, which is why neither
the desktop nor an agent installs them. The daemon reloads the folder on its
own. Programs ForkMesh starts that are not its own (`git`, `ssh`, `curl`, the
agent CLIs) keep their existing rules. Tests: `tools/test_opensnitch_rules.py`.
