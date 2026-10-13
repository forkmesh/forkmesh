# Node logs on the operator desktop

The desktop's Log page follows every fleet node's systemd journal live:
cores `n1`..`n3`, the runner `r1`, and any burst runner the autoscaler
creates. Every journal line reaches the Log page, mail and syslog included,
and so does every Caddy request, which also feeds the Network > Web Requests
tally. The operator desktop's own deploy output (the node-deploy agent, the
`tools/next_*.py` runs, the source publisher) is followed next to it. Nothing
is stored in the database.

## Transport: the desktop pulls over SSH

The desktop opens one SSH session per node with its own key,
`<fleet ssh dir>/next_logread_ed25519` (`tools/next_fleet.py logread-key`).
That key is not the fleet key. On each node it is installed as a forced command:

```
restrict,command="/usr/bin/python3 -I /opt/forkmesh-next/bin/fmnext-logread" ssh-ed25519 ... fmnext-logread
```

`fmnext-logread` (`deploy/core/logread.py`) reads the request from
`SSH_ORIGINAL_COMMAND`, which is `follow`, `follow lines N`,
`follow after <cursor>` or `hello`, and refuses anything else. It then drops
privileges to `nobody` plus the `systemd-journal` group, with `no_new_privs`,
and runs `journalctl --follow --output=json`. Every message is redacted on the
node before it leaves:

- the `tools/secret_gate.py` rules (a test pins them to the same list);
- bearer and basic credentials, `name=value` credential assignments, JSON
  credential fields, credential query parameters and URL passwords;
- mixed-case tokens of 40 characters or more.

The first line of the stream is a header (`{"logread":1,"node":"n3","role":"core"}`);
every other line is one journal entry with its `__CURSOR`, an idle heartbeat
(`logread_idle`) after 60 s without output, or a resource sample.

Since 2026-09-30 the reader sends a resource sample (`logread_vitals`) right
after the header and then every 15 s, whatever the journal is doing. It
feeds the desktop's Log -> Visualizer
([desktop/networking.md](desktop/networking.md)). `Vitals.sample` reads only
world-readable kernel counters, as `nobody`, after privileges are dropped:

- `/proc/meminfo`: `mem_total`, `mem_available`, `swap_total`, `swap_free`,
  in bytes;
- `/proc/loadavg`: `load` (1, 5 and 15 min), `procs_running` and
  `procs_total`;
- `/proc/sys/fs/file-nr`: `files_open` (allocated minus free handles) and
  `files_max`;
- `statvfs("/")`: `disk_total`, `disk_free`, `inodes_total` and
  `inodes_free`;
- `/proc/uptime` (`uptime`, in seconds) and `cpus`;
- `/proc/stat`: `cpu_busy`, the busy share since the previous sample.

A counter the node lacks is left out, never sent as zero. The sample carries
no journal text, so nothing in it needs redaction. A desktop from before the
sample drops it, because it has neither `__CURSOR` nor `MESSAGE`. A node
sends samples only after its reader is rolled (`install nN --steps logread`,
or the `files` step); until then the Visualizer shows the node waiting.

The desktop resumes from the cursor of the last line it handled, which it
saves on disk. Lines that arrive while the desktop is off wait in the node's
journal (`SystemMaxUse=64M`), and nothing is lost unless journald rotates
them out first. Each stream is authenticated by that node's SSH host key, so
a compromised node can only lie about its own logs, never another node's.

The alternative was nodes pushing signed batches to the app. It was rejected
because it would write every log line through rqlite on the 1 GB cores,
contrary to the rule that live-only stats stay on the desktop, and because
every node's stream would sit in one database that any compromised core can
write to.

## IDS

`render.ids_config` adds the reader key's fingerprint to
`ssh.fleet_fingerprints`. A reader login is therefore root with a known key,
running a command without a pty, and the IDS reports it as `ssh_tool_session`
(info), with the forced command in the detail. `restrict` refuses a pty, so
the key can never open an interactive session.

The authorized-keys line is written only after `fmnext-ids` has restarted
with the new `ids.json`, both in the `start` step and in the `logread` step.
Otherwise a reader login in between would be an `ssh_unknown_key` lockdown.
The block touches only lines ending in ` fmnext-logread`, and it refuses to
write if any other key would change.

## Caddy access log

`[logs] access_log = true` adds the `(access_log)` snippet to `site_common`
(the shared app and world names) and to the per-node name. Its format:

- JSON to stderr, so the lines go to the journal under `caddy.service`, logger `http.log.access.logN`;
- request and response headers dropped, TLS details and the client port deleted;
- the User-Agent kept as a top-level `user_agent` field
  (`log_append user_agent {http.request.header.User-Agent}`);
- the full client address kept (`request>client_ip`, `request>remote_ip`);
- when the TCP peer is in Cloudflare's published ranges, the visitor Cloudflare
  saw: `cf_ip`, `cf_country`, `cf_region`, `cf_city` from the `Cf-*` headers,
  and, for requests the forkmesh.com Worker relays, `edge_ip`, `edge_country`
  from `x-forkmesh-edge-client` plus `edge_where` from `x-forkmesh-edge-where`
  (`deploy/core/edge_client.md`). A `vars` handler copies them before
  `client_headers` drops the `Cf-*` headers, so the App still never sees them;
- `expected_degraded`, the App's `X-Forkmesh-Expected-Degraded` response header;
- credential query parameters replaced with `REDACTED`;
- `/readyz` and `/health` skipped.

Each line is about 300 bytes, plus the User-Agent.

Until 2026-09-27 the client address was masked to /24 (IPv4) and /48 (IPv6)
and the User-Agent went with the other headers. The operator asked for the
full address, the User-Agent and a country on every request line in the main
log, so both are now kept. A node already running the masked snippet has a
Caddyfile the `logread` step refuses; roll it with the node drained:
`tools/next_fleet.py install nN --steps files,start --execute`.

## Rolling it out

1. `tools/next_fleet.py logread-key --execute`, then copy the printed
   `[logs]` block into `deploy/fleet.toml`.
2. For each core, one at a time, with the node out of DNS:

   ```
   tools/next_dns.py drain nN --execute
   tools/next_fleet.py install nN --steps logread --execute
   tools/next_dns.py undrain nN --execute
   tools/next_ids.py status nN
   ```

   For the runner, only `install r1 --steps logread --execute` and the status check.

The `logread` step pushes an expected-change window for exactly the reader,
`ids.json`, the key line and the Caddyfile, plus a rescan of
`/root/.ssh/authorized_keys`. It refuses a node whose live Caddyfile or
`ids.json` is neither this tree's render nor that render without `[logs]`,
because another change is pending there: roll that change first. If Caddy
rejects the reload, the previous Caddyfile is put back.

A full `files,start` roll installs the same files; the `start` step writes
the key line after the IDS restart.

## JSON lines in the main log

A journal message that is a JSON object is shown as one readable line, not as
raw JSON (`displayMessage` in `desktop/src/CoreNodeLogs.cpp`):

- a line with `msg` (Caddy, zap) keeps its level, logger, message and the
  error and host fields;
- an IDS line (`ids` key) reads as its message and `key=value` fields; an
  incident reads as `severity detector: summary`, then the detail fields the
  summary does not already say, then `incident <id>`, joined by ` · `. For
  example: `alert listener_unexpected: root listens on tcp port 25 on
  0.0.0.0, :: by exim4 pid 812 in exim4.service · incident c3Z2tm-jzynWAkBr`.
  The node is left out because the line prefix already names it;
- any other object reads as its level, logger, message and event, then the
  remaining fields as `key=value` (timestamps dropped). Nested objects are
  shown as `(key=value ...)` and arrays as comma-separated values.

## New and autoscaled nodes

A node's logs register on the desktop by themselves when both of these hold:

- **It is discoverable.** Its Vultr label is `fmnext-core-N`,
  `fmnext-auto-N` (autoscaled app replica), `fmnext-run-N` (always-on runner)
  or `fmnext-burst-N` (burst runner), and it carries the `forkmesh-next` tag.
  The desktop reads `fleet-instances.json` (written by `next_fleet.py
  create-*`) and `autoscaled-instances.json` (written by the Network → Hosts
  autoscaling panel from the scaler's status; only `fmnext-auto-N` and
  `fmnext-burst-N` entries count there), and lists the tagged instances
  through the Vultr API. A change to either file is picked up at once (so a
  scale-up or scale-down shows at the next panel refresh) without calling
  Vultr. The Vultr listing is kept in `data/fleet_logs/vultr-inventory.json`
  (owner-only: labels and addresses, the time of the last good listing and
  the failure backoff), so a restart reuses it instead of listing again. The
  desktop lists once an hour, and two minutes after a core's journal says
  `runner pool: creating ...`, `runner pool: destroying ...`, `autoscaler:
  creating ...` or `autoscaler: destroying ...` for an `fmnext-run-`,
  `fmnext-burst-` or `fmnext-auto-` label; several such lines share one
  listing. A failed listing (Vultr's 502 maintenance page, a 429, a timeout)
  waits 15 minutes, then doubles up to 6 hours, or longer if Vultr's
  `Retry-After` asks; that backoff survives a restart too. This replaced a
  15-minute poll plus a listing at every start-up and three listings per pool
  line (2026-09-30), which kept calling Vultr through its maintenance window.
  The log's favicon lookup skips `api.vultr.com`, which has no favicon and
  was probed on every launch. The node is shown as `nN` (core), `aN` (app replica, followed like a
  core), `rN` (runner) or `bN` (burst runner, followed like a runner).
- **It carries the reader.** A node built by `install` gets it through the
  `files` and `start` steps. The burst-runner snapshot must be taken after
  those steps, or after `install rN --steps logread`, from a runner rendered
  with `[logs]`. Otherwise the desktop shows the node as "reader key not
  accepted" and follows nothing.

Rules for the autoscaler:

- label app replicas `fmnext-auto-N` and burst runners `fmnext-burst-N`, never
  `fmnext-run-N`: that prefix is reserved for always-on runners, and the old
  pool prefix `fmnext-run-` matched r1 (`fmnext-run-1`), so a pool would have
  counted r1 as its own and destroyed it when idle;
- keep the `forkmesh-next` tag, report every scaled node (label, instance id,
  address) in `next_autoscale.py status --json` so the desktop can write
  `autoscaled-instances.json`, and keep the two log lines above;
- rebuild the snapshot whenever `deploy/core/logread.py` or the `[logs]` key changes.

## What reaches the Log page (2026-09-26)

Until 2026-09-26 the desktop dropped most of each stream. `classifyEntry`
forwarded only the app, rqlite, Caddy, the mirror node, `fmnext-*` and a few
others, plus warning- and error-level lines. Info-level Postfix, OpenDKIM,
systemd, cron, sshd and kernel lines never showed, so n1's mail traffic was
invisible on the desktop although the node was sending mail. Caddy access
lines only counted toward Web Requests; only a 5xx made a log line.

Now (`desktop/src/CoreNodeLogs.cpp`, `MainWindowLogs.cpp`):

- **Every journal line with a message is forwarded.** `journalSource` sorts it
  first into `mail` (a `postfix*` identifier or unit, `opendkim`,
  `fmnext-mail-events`), then `app`, `rqlite`, `web`, `job`, `mirror`, `ids`,
  `ops`, `ssh`, `kernel`, and `system` for the rest. The line reads
  `Node <node> <role> <unit> <err|warn|info>: ...`.
- **Severity is unchanged.** Error lines go under **Error**, the rest under
  their fleet group (App, Cron, DB, Web, Daemon, IDS, Cert, Ops, Job, Mail,
  System; [desktop/networking.md](desktop/networking.md#fleet-node-logs)); they
  were all **Cloud** before 2026-09-27. sshd is always info (internet scanners), and the known boot noise
  is info. A replayed backlog line is never an error.
- **Repeat folding still applies.** An identical line from the same node and
  source is logged once per 10 minutes, and the next copy says how many were
  folded. Session and timer churn therefore collapses.
- **Every Caddy access record is one line.** For example,
  `Node n1 core caddy info: GET app.forkmesh.com/api/x -> 200 12ms 2.6 KB · 🇩🇪 203.0.113.9 · git/2.45.2`,
  under **Web**, with the query string dropped. After the duration come the
  response size, the client's country flag and address, then its User-Agent
  (control characters flattened, cut at 160 characters). Caddy has already
  redacted credential parameters. A 5xx is `err` under **Error**, without the
  duration or size, so repeats from one client group in Most frequent. The
  tally is unchanged.
  Only what Caddy skips (`/readyz`, `/health`) is missing. These lines are not
  folded.

Volume: expect about one line per request per core, plus Postfix's 5-10 lines
per message and the systemd, cron and logind lines. Streams pause past 20,000
queued lines and resume from the cursor, so a burst delays lines but loses
none. Settings "Follow the fleet nodes' logs" (`diagnostics/coreNodeLogFeed`)
switches the node streams off. The **Nodes** filter and the category chips
narrow the view.

## Country flags (2026-09-27)

The desktop looks up the client address itself; Caddy sets no country
(`deploy/core/edge_client.md`). `desktop/src/IpCountry.cpp` reads DB-IP's IP
to Country Lite data (CC-BY 4.0, "IP Geolocation by DB-IP",
<https://db-ip.com>) as the plain CSVs `dbip-country-ipv4.csv` and
`dbip-country-ipv6.csv` from the `@ip-location-db/dbip-country` package on
jsDelivr. They are plain text, so the desktop needs no gzip support. The files
live in `<cache>/ip-country/`:

- the first access line starts the download; a copy older than 30 days is
  fetched again, checked at most once a day, and a failed fetch is retried
  after an hour while the old copy stays in use;
- a download is saved only when its first row parses as `first,last,CC`;
- the table is built off the GUI thread; until it is ready the lines have
  the address without a flag;
- IPv6 ranges are kept to their upper 64 bits, which is finer than any
  country allocation; `ZZ` (unknown) rows are skipped.

Requests the homepage Worker relays showed a Cloudflare egress address and its
country, not the visitor's, until 2026-09-28 (see "Who the visitor really
is").

### Flags that render, and who the client is (2026-09-27)

The flags drew as two boxed letters (`U S`). The log font lists DejaVu Sans
Mono before Noto Color Emoji, and DejaVu (or Symbola) has its own glyph for
each regional indicator letter. Qt picks a fallback font per character, so the
pair never reached the emoji font and never became one flag.
`corelogs::withEmojiFont` now wraps each run of regional indicators in the
node line's HTML in a span whose family is `'Noto Color Emoji','Apple Color
Emoji','Segoe UI Emoji'`. Noto Color Emoji was bundled until 2026-10-13; it
now comes from the system (the installer adds the distribution package on
Linux, `docs/desktop/README.md`). Windows' Segoe UI Emoji has no flags and
still shows the letters.

`corelogs::accessClientLabel` names the client after its address:

- a fleet node's own address (from the node list the feeds use) gets its name,
  for example `203.0.113.21 (r1)`, and no flag;
- loopback is `(local)` and the WireGuard subnet `10.77.0.0/24` is `(fleet)`;
- an address in Cloudflare's published ranges is `(Cloudflare)` and keeps its
  flag. It is the Worker's or proxy's egress, not the visitor.

A node that still runs the masked snippet logs `x.y.z.0`, which matches no
fleet node; redeploy the access log to get the names.

In the Log page the host is muted so the path stands out; the size and the
User-Agent are muted too, and a node name or label is bold.

## Who the visitor really is (2026-09-28)

Every `info/refs` 503 in the log named a Cloudflare address such as
`🇺🇸 172.71.203.0 (Cloudflare)`. Two things hid the visitor:

- the nodes still ran the masked snippet (`ip_mask 24 48`), hence the `.0`
  and `::` endings. Rolling the current snippet
  (`tools/next_fleet.py install nN --steps files,start --execute`) fixes that;
- `app.forkmesh.com` is not proxied by Cloudflare, so a Cloudflare peer is the
  forkmesh.com homepage Worker relaying a git request for a visitor.

The Worker already sends the visitor in the signed `x-forkmesh-edge-client`
header. It now also sends `x-forkmesh-edge-where` with the city, region, AS
number and AS organisation from `request.cf`. Caddy logs both, and the
`Cf-Connecting-Ip`/`Cf-Ipcountry`/`Cf-Region`/`Cf-Ipcity` headers a proxied
name would get, only when the TCP peer is a Cloudflare address, so nobody else
can put a place in the log. The App is unchanged: it still verifies the signed
header itself and never sees the `Cf-*` headers. `x-forkmesh-edge-where` is
unsigned and only for the log; Caddy drops it before the App.

`corelogs::parseAccessRecord` puts the visitor in place of the edge and keeps
the edge in `edgeIp`. The line reads

`GET app.forkmesh.com/forkmesh/forkmesh/info/refs -> 503 degraded · 🇺🇸 203.0.113.7 (Ashburn, Virginia, US; Charter Communications Inc, AS20115; via forkmesh.com) · git/2.43.0`

The details in the parentheses are a fleet label, the place, the network, and
`via Cloudflare` or `via forkmesh.com`. They are separated by `;` and never
contain `·`, so the User-Agent stays the last `·` segment and stays muted. The
country comes from Cloudflare when it sent one; otherwise from DB-IP.

### whois for everything else

For a public address with no label and no network from Cloudflare,
`MainWindow::accessWhoisLabel` runs the system `whois` client
(`desktop/src/IpWhois.cpp` parses ARIN, RIPE, APNIC, LACNIC and AFRINIC
answers). The answer is logged once, under **Cloud**:

`whois 2603:7000:8c00:: · owner: Charter Communications Inc · network: RRNY-IPV6-12271-11351 · cidr: 2603:7000::/24 · country: US · abuse: abuse@charter.net · …`

Later access lines from the same /24 (IPv4) or /48 (IPv6) carry the owner and
AS in their parentheses. The first line from a network goes out without them
and is not held back. Limits: two lookups at a time, 20 s each, at most 64
waiting, and 4096 networks remembered for the session. A failed lookup is not
retried. Private, loopback, link-local, CGNAT and documentation ranges are
never looked up. With no `whois` on PATH (Windows), nothing is looked up.

### The same client details on database and app lines (2026-09-28)

The rqlite statement lines name the request's client in their caller clause
(`app/host/forkmesh_host/d1_caller.py`: `for GET /api/x, IP <addr>, user ...`),
but only the Caddy lines had a flag and a provider, so the same visitor read
differently in two lines of one request. Now every node line with an
`IP <addr>` field gets the same treatment as the Caddy client:

`Node n3 core rqlite query: SELECT ... · from entry.py in push_alert_handler · for GET /api/push/alert, IP 🇺🇸 2001:db8:8c00:27ce:457:5433:101e:49cf (Charter Communications Inc), user jo, session AbCdEfGh`

- `corelogs::lineClientAddresses` finds each `IP ` followed by a whole IPv4
  (four parts) or IPv6 address; SQL such as `FROM IP WHERE` or `VIP 1.2.3.4`
  is left alone. `corelogs::withClientDetails` puts the flag before the
  address and `(label; network)` after it.
- `MainWindow::logClientDetails` is the one lookup both paths use: the DB-IP
  table in memory, `accessClientLabel` for fleet node names, `(local)`,
  `(fleet)` and `(Cloudflare)`, and the whois cache. A labelled address other
  than Cloudflare has no flag and no provider, exactly as in Caddy lines.
- It applies to the rqlite `query:`, `primary:`, `batch:` and other statement
  lines, and to every other forwarded line (`forkmesh-app` and the rest).
- Nothing extra goes over the network: the table download and whois run only
  through the paths the Caddy lines already use, with the same limits, and a
  line with no `IP ` does no work. The per-line work is a substring check, one
  regex and an in-memory range search, so it stays on the GUI thread with the
  Caddy lines; the table build and whois already run elsewhere.
- As with Caddy lines, the first line from a new network goes out without its
  provider and is not rewritten later; the whois answer is logged once and
  later lines carry it.
- The enrichment happens before `redactProcessOutput`, and after the repeat
  filter, so a provider arriving later does not break folding.
- In the Log page the parentheses after the address are muted on DB lines, so
  SQL keyword colouring never reaches the provider name.

### Expected-degraded 5xx

When the cores disagree about a repository's refs, the App answers git with
`503 mirror_unavailable` and `x-forkmesh-expected-degraded: 1`
(`entry.py`, the mirror proxy). That is the App working as designed, not a
failure, so such a line is now `warn` with `degraded` after the status. It is
filed under **Web**, not **Error**, and does not count as an error in the
node tally. A 5xx without the header is still `err`.

## The reason behind a 5xx (2026-09-27)

A Caddy `err` line such as
`Node n1 core caddy err: POST app.forkmesh.com/api/fleet/autoscale/status -> 500`
said nothing about the cause. The app printed one info-level line with only
the exception's repr (`fleet api: ... failed: KeyError(...)`), so it went under
**Cloud** and the **Nodes** "errors only" filter hid it. The client got just
`{"error": "internal"}`.

Now every place in the host that turns an exception into a 500 calls
`error_log.report` (`app/host/forkmesh_host/error_log.py`): the fleet API, the
worker fetch and WebSocket paths in `asgi.py`, and the mesh route in
`durable_cluster.py`. It prints two things to stdout:

- `ERROR <context> -> 500: <repr>`. The `ERROR ` prefix makes `classifyEntry`
  rate it an error, so it sits under **Error** next to Caddy's line.
- The full Python traceback. The desktop's `TracebackAssembler` joins it into
  one `Traceback: <exception> · at <file>:<line> in <function>` error line and
  keeps the frames as its detail.

The response body is unchanged, so nothing internal reaches the client. The
peer-unseal handler still logs only the exception's type name, because vault
errors can carry key material. Its line now starts with `ERROR ` too.

## Deploy output on the Log page

`desktop/src/DeployLogs.{h,cpp}` (`DeployLogFollower`, started with the node
streams) follows three things on the operator desktop:

- **`<fleet logs>/autodeploy/*.log`**: the node-deploy agent's run logs, and
  `next_autodeploy.py deploy|rollback` runs by hand.
- **`<fleet logs>/ops/*.log`**: every executing `tools/next_*.py` run
  (see below).
- **The user journal** of `forkmesh-source-publish.service`,
  `forkmesh-mirror-fanout.service` and `forkmesh-node-deploy.service`:
  `journalctl --user --follow --output=json`.

`<fleet logs>` is `~/projects/forkmesh/logs/fleet`, or `FMNEXT_LOGS_DIR`, as
in `next_common.logs_dir()`.

How the files and the journal are followed:

- **Files.** They are polled every 2 s: the eight newest files per folder, plus
  any new file. Files that exist at start-up are tailed from their end, a new
  file is read from its start, and a truncated file is read again. Only whole
  lines are read.
- **Journal.** It resumes from the cursor in `<data>/fleet_logs/deploy_cursor.json`
  (0600, written at most every 5 s). A first start begins at the end. If
  `journalctl` exits, it restarts after 30 s.

Each line becomes `Deploy <kind>: <line>`, without the log's own `HH:MM:SS`
stamp:

- `<kind>` is `autodeploy <rev>` or `autodeploy manual-<rev>` for the
  autodeploy files;
- `<tool> <command>` for the ops files, for example `next_deploy restart`;
- `publisher` for the publisher and fan-out units;
- `agent` for the deploy agent's unit.

The agent echoes each file line to its journal, so the same stamped line is
shown once: the first copy from either side wins
(`deploylogs::CrossSourceDedupe`, 10-minute window). Its idle lines, which
exist only in the journal, still show.

Categories:

- **Error** is for journal priority err or worse, or a line containing
  `FAILED`, `result: fail` or `Traceback`. For example, the agent's
  `result: FAILED: ...` is an error.
- The per-test `FAILED path::test` lines from the test gate are not errors,
  and neither is anything else. They go under **Deploy** (**Cloud** before
  2026-09-27). A deploy with known
  failures prints those lines every time, and the gate's own `result:` line
  already gives the verdict. Leaving them out of Error means they raise no ping
  or error-repair dispatch.

All deploy lines pass through `redactProcessOutput`.

### Ops logs from the operator tools

`next_common.Runner` writes every `say` line to
`<fleet logs>/ops/<YYYYmmdd-HHMMSS>-<tool>-<command>.log` when it is created
by `runner_from_args` for an `--execute` run. `<tool>` is the script name
without `.py`, and `<command>` is the subcommand. The file is 0600 in a 0700
folder, and each line is stamped `HH:MM:SS`.

- **Dry runs** write nothing.
- **Write errors** turn the tee off for the rest of the run and never fail the
  tool.
- **Children of the deploy agent** write nothing. Such a run carries
  `FMNEXT_DEPLOY_LOCK_FD` and is already captured, line by line, in the
  agent's autodeploy log.
- **Direct `print` calls** are not captured. Only what a tool says through
  `Runner.say` or `Runner.step` lands in the file.

The deploy tests' sandbox (`deploy/tests/conftest.py`) sets `FMNEXT_LOGS_DIR`
to a temp folder, so no test run writes into the real fleet logs.

## Planned restarts are quiet (2026-09-28)

A deploy (`tools/next_deploy.py`, the deploy agent) or a core roll
(`tools/next_fleet.py roll`) restarts the app, the mirror node and Caddy on
each node in turn. Until 2026-09-28 every restart sent a burst of error Pings
from the node streams, even though the node was out of DNS first:
`dial tcp 127.0.0.1:8080: connect: connection refused`, the same for the 8790
gateway, `-> 502` for `/api/nodes/events` from the local mirror node, a
browser's `OPTIONS /api/world/ticket -> 502`, and a peer's `info/refs -> 502`.
The errors were expected, and each restart announces itself in the same feed
first.

`desktop/src/PlannedRestarts.{h,cpp}` (`forkmesh::PlannedRestarts`) keeps one
planned-restart window per node. `MainWindow::logSystemFrom` feeds it every
line before the Log types filter, so a hidden category cannot hide a signal.

**Opening a window.** Either of these signals opens it:

- the node's own `Node nN <role> fmnext-ids info: expectation accepted ...
  reason=next_deploy <rev>` line (a deploy) or `reason=next_fleet
  daemon|files|start` (a roll). A replayed backlog line opens nothing;
- the desktop's own Deploy line `[execute] nN: tell the IDS to expect
  next_deploy ...` or `... next_fleet daemon|files|start ...`.

Other `next_fleet` expectations, such as `delete-refs` and `logread`, restart
nothing and open no window. A signal for a node whose window is already open
extends it. A roll signal turns a deploy window into a roll window.

**Closing a window.** Only a Deploy line closes a window, never a node's own
journal:

- `nN: rolled and back in DNS` or `nN passed K health probes and is back in
  DNS` closes any window;
- `nN: current <rev>, running <rev>, app active` closes a deploy window. It
  does not close a roll window, because the deploy agent can land a deploy on
  a node in the middle of its roll, while the node is still out of DNS.

The window also ends after the duration its signal states (`..., 15 min)`),
capped at 15 minutes and counted from the latest signal. A window that ends
this way means the node did not report back. The desktop then logs
`nN did not come back after its <deploy|roll> - ...` as an error, which raises
a normal error Ping.

**What stays quiet.** An ERROR line from the node in its window raises no error
Ping when it is a restart symptom:

- `connection refused` to `127.0.0.1:8080` (the app) or `127.0.0.1:8790` (the
  mirror gateway);
- a Caddy access line with 502 or 503, from any client (Caddy's upstream is
  down, which is what a restart is);
- a Caddy access line with 500 from the node itself, the WireGuard subnet or a
  fleet node (`(local)`, `(fleet)`, `(nN)`, `(rN)`). A 500 given to an outside
  visitor is an answer from the app, so it still pings;
- the mirror node's `upgrade returned HTTP 5xx`, and `sealed` (the vault
  before its unseal).

These lines keep their ERROR badge in the Log, their place in the error tally
and the error-repair monitors. Instead of an error Ping, the first quieted line
of a window raises one info Ping: `nN restarting for <deploy|roll> — errors
during the restart are in the Log`, with that line as its body. A restart that
logs no symptom raises no Ping at all.

**What still pings.** Every `fmnext-ids` line (an IDS alert always pings),
every other error from the node (a traceback, a 500 given to a visitor), and
every error from the other nodes.

A bare `tools/next_fleet.py install nN --steps files,start` (without `roll`)
opens a roll window, but nothing in it says "back in DNS". It therefore ends
with the "did not come back" Ping after 15 minutes. Roll a node with `roll`,
or undrain it through `next_dns.py`, whose "back in DNS" line closes the
window.

The windows live in memory only; a desktop restart forgets them.

## Turning all logging off (2026-10-03)

The **Stop logging** button in the Log page header switches every kind of
logging off at once. It is the master switch above the per-category switches in
**Log types**. The choice is saved as `logs/allOff` in the desktop settings, so
it lasts across restarts. While it is off, the button reads **Logging is off**.
Clicking it again turns logging back on.

While logging is off:

- `MainWindow::logSystemFrom` drops every line before it is recorded. Nothing
  goes to the buffer, the live file, the 7- and 30-day digest, the activity
  lights, the error alerts or the teammate log monitors. Only the
  planned-restart tracker still sees each line, because it keeps no record.
- Every log feed that makes network traffic is stopped and stays stopped:
  - the Cloudflare Worker tail (`setCloudLogMonitorEnabled`);
  - the per-node SSH journal streams to the fleet (`syncCoreLogFeeds`), along
    with the hourly Vultr inventory poll that serves them;
  - the saved hosts' log feeds (`syncHostLogFeeds`).
  Their own **Watch the deployed Worker's log** and **Follow the fleet nodes'
  logs** settings are left as they were. Turning logging back on restarts each
  feed whose setting is on.
- The local deploy-log follower (`startDeployLogFollower`) is stopped too.
- Lines still waiting in the write queue are flushed to disk, so the log on
  disk ends with the line `Logs: all logging is off.`.

A Hosts live view that the user opens still streams its own tap. That is a page
being viewed, not logging, and it records nothing.
