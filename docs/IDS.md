# ForkMesh Next intrusion detection (IDS)

Every core node (`n1`..`n3`) and the runner (`r1`) run `fmnext-ids`, a
small root service (`deploy/core/ids.py`, installed as
`/opt/forkmesh-next/bin/fmnext-ids`, config `/etc/forkmesh-next/ids.json`,
state in `/var/lib/fmnext-ids`). It uses the Python standard library, plus
Debian's `python3-cryptography` when it signs something. It takes about
28 MB RSS, has `CPUQuota=25%` and idle I/O priority, stats files instead of
re-reading them, and re-hashes everything every 6 hours.

When it sees an intrusion it **locks the node down**. The vault key leaves
memory, the node is marked quarantined in rqlite, the firewall closes and
the services stop. It also **pages the admins**. Only the operator can end
a lockdown (`tools/next_ids.py release`).

## Threat model

**Detects, and responds within seconds:**

- An attacker who gets a shell as a service user (`forkmesh-app`, `rqlite`,
  `caddy`, `fmnext-certsync`, `forkmesh-node`) through an app, Caddy or
  daemon bug.
- A login with a stolen or planted SSH key, a password login, a new account
  with uid 0 or a login shell, or a new member of a privileged group.
- Tampering with code, units or security configuration: binaries in
  `/usr/local/{bin,sbin}`, the release tree, systemd units, sshd, sudoers,
  PAM, `ld.so.preload`, nftables, the Caddyfile, authorized_keys.
- Attempts to read the app's memory with ptrace.
- A runner job escaping its scope.
- Persistence through cron, new listeners, unusual egress from service
  users, and kernel modules.
- An IDS that stops reporting while its node keeps serving.

**Does not stop:**

- **root that moves faster than the IDS.** Root can kill the IDS, edit its
  baseline (HMAC-keyed, but the key is on the node), rewrite the firewall
  and read process memory. The mitigations:
  - Detection is fast. Journald lines are read as they arrive, processes
    are scanned every 5 s and hot files every 10 s. A lockdown seals the key
    first, in milliseconds.
  - Peers watch each other's IDS heartbeat. A node whose app runs while its
    IDS has been silent for 3 minutes is voted into quarantine by the other
    cores. They then refuse it the vault key and mesh traffic, drop its
    WireGuard packets, and remove it as an rqlite voter when that is
    quorum-safe.
  - This covers only nodes whose IDS has reported at least once. A node
    that never ran the IDS is not watched.
- **An unsealed node that is compromised before detection.** Its memory held
  `DATA_KEY`, and root can read that (docs/VAULT.md). Rotate `DATA_KEY` and
  the vault secrets after any real lockdown (docs/REKEY.md).
- **A compromise of the operator desktop.** It holds `DATA_KEY`, the fleet
  SSH key, the IDS master key and the operator release key.
- **Silent exfiltration over HTTPS by the app user.** The egress rule logs
  only non-HTTP(S) ports, because the app legitimately talks to arbitrary
  HTTPS hosts (webhooks, APNs, Mailtrap).
- **An attacker who uses the stolen fleet key only non-interactively.** With
  `ssh_allow` empty this raises only `info`, unless it changes watched
  files. Set `ssh_allow`.

The desktop's log reader key (`[logs] reader_public_key`) is added to the
fleet fingerprints by the renderer; it only runs `fmnext-logread` as a forced
command (docs/NODE_LOGS.md).

## What is detected

Default severities are shown below. Override any detector with
`[ids] severity = { detector = "lockdown" }` in `deploy/fleet.toml`.

| Detector | Trigger | Default |
|---|---|---|
| `ssh_unknown_key` | sshd accepted a key whose fingerprint is not in `ssh_fleet_fingerprints` | lockdown |
| `ssh_non_key_login` | sshd accepted a password or keyboard-interactive login | lockdown |
| `ssh_foreign_ip` | login from outside `ssh_allow` (only when `ssh_allow` is set) | `ssh_foreign_ip_severity` (alert) |
| `ssh_interactive_login` | a shell, or a command on a pty; `info` from an allowlisted address | alert |
| `ssh_login_user` | a login as anyone but root | alert |
| `ssh_tool_session` | a command without a pty, sftp, or a `-N` tunnel (the fleet tools' mark: `next_common.Ssh` runs BatchMode commands without `-t`) | info |
| `integrity_*` (modified/added/removed/permissions) | a change outside a signed expected-change window. Lockdown for `/usr/local/{bin,sbin,share/forkmesh}`, `/opt/forkmesh-next/bin`, the `current` release tree, unit files in `/etc/systemd/system`, sshd config, sudoers, PAM, `ld.so.preload`, `nftables.conf`, the Caddyfile, authorized_keys, and a new setuid bit. Alert for `/etc/forkmesh-next`, the daemon config, cron, sysctl, `/etc/hosts`, `wg0.conf`, and enable/mask symlinks | lockdown / alert |
| `account_uid0`, `account_login_shell`, `group_privileged`, `root_password_set` | diffs of `/etc/passwd`, `group`, `shadow` | lockdown |
| `account_added` / `_changed` / `_removed`, `group_changed`, `shadow_changed`, `gshadow_changed` | other account changes | alert |
| `service_user_outside_unit` | a service user's process outside its own systemd unit (the outermost unit in its cgroup path; a process that exits mid-scan is skipped, see Traps) | lockdown |
| `service_user_shell` | a shell or interpreter (sh, bash, nc, socat, perl...) under a service user; alert for the core daemon's `forkmesh-node` | lockdown |
| `service_user_exe` | an executable outside the user's allowlist | alert |
| `exe_suspicious_path` | an executable in or deleted from `/tmp`, `/dev/shm`, `/var/tmp`, or a memfd (root or a service user) | lockdown |
| `ptrace` | a traced process that is protected (the app, rqlite, Caddy, certsync, the IDS) or runs as a service user | lockdown |
| `job_user_outside_scope` | runner: a `forkmesh-job` process outside `fmnext-job-*.scope` / `run-*.scope` | lockdown |
| `listener_unexpected` | a TCP listener, or a UDP socket below 10240, on a port outside the expected set (`ids_config` in `deploy/core/render.py`; UDP 546 for the DHCPv6 client is expected on cores and runners). One incident per protocol, port and user; the detail and summary name the bound addresses (IPv4 and IPv6 together, marked "loopback only" when none is public) and up to four owning processes (pid, name, exe, systemd unit, parent pid), found by matching the socket inode under `/proc/*/fd`, so the triage can start from the service rather than the port. On runners, `listeners.loopback_allow` skips a loopback-only listener whose every owner is `forkmesh-job`'s `/usr/local/lib/claude/claude` in a `run-*.scope`: the web sign-in's `claude auth login` (`server/runnerlogin.go`) waits for its OAuth callback on an ephemeral 127.0.0.1 port (incident 62HEXCPq_RZslfSl, 2026-10-03). A second rule skips a Go test binary (`*.test` under the runner work root `/var/lib/forkmesh-runner/work`, `/tmp` or `/var/tmp`) in a `fmnext-job-*.scope`: `go test` builds into the job's `TMPDIR` and `httptest` servers bind ephemeral 127.0.0.1 ports, which the scope's `IPAddressDeny` already makes unreachable from anything (incident XV3qODWOA2w58qC-, 2026-10-06: `mirror-node.test` on 127.0.0.1:14259). Other binaries in job scopes, public binds and test binaries outside a job scope still alert | alert |
| `egress_unexpected` | the nftables log rule: a new outbound connection from `forkmesh-app`, `caddy`, `rqlite` or `fmnext-certsync` to a port other than 80/443 (TCP) or 53/123/443 (UDP), outside loopback and the mesh | alert |
| `kernel_module_loaded`, `kernel_module_taint` | a module loaded after start-up, or a tainting module; `inet_diag` and `tcp_diag` (auto-loaded when an admin runs `ss`) and `dm_mod` (device-mapper, auto-loaded when the nightly `apt-daily-upgrade` runs grub or initramfs hooks; n3 alerted on it at 2026-10-01 07:01 UTC) are expected (`modules.expected` in `ids.json`, `IDS_EXPECTED_MODULES` in `deploy/core/render.py`) | alert |
| `privilege_tool` | sudo or su used by a non-root account to gain privilege (root logs in by key, so nothing legitimate does this) | alert |
| `privilege_tool_root` | root dropping to a service account through su or sudo (the operator tools run `su -s /bin/sh forkmesh-node -c ...`), or a `session closed` line | info |
| `peer_ids_silent` | a peer's app runs but its IDS heartbeat is 3 minutes stale; this node votes | alert |
| `quarantined_by_peers` | a majority voted this node into quarantine | lockdown |
| `expectation_rejected` | a forged, replayed or stale expected-change manifest | alert |
| `baseline_tampered` / `baseline_missing` | the baseline failed its HMAC check, or disappeared | lockdown |
| `reboot` | the boot id changed outside a window | info |

## Response

### Lockdown on a core node, in order

1. **Seal**: `seal-restart` over the unseal socket. The app drops
   `DATA_KEY` and every vault value, overwrites their bytes (best effort,
   CPython), replies, and exits. The unit's `ConditionPathExists=` keeps it
   from restarting.
2. **Marker**: `/var/lib/fmnext-ids/LOCKDOWN`. Rendered drop-ins
   (`<unit>.d/50-fmnext-ids.conf`) keep the app, the daemon/Git gateway,
   Caddy and certsync from starting while it exists, including after a
   reboot.
3. **Quarantine**: a `self` row in `cluster_quarantine`, signed with the
   node's IDS key, and the incident row in `security_incidents`. Both are
   written with the node's own rqlite credentials before rqlite can stop.
4. **rqlite**, by the quorum-safety rule below.
5. **Firewall**: the nftables table `inet fmnext_ids` (priority -10).
   - Inbound: only established replies, loopback, SSH (from `ssh_allow`
     when set, which also cuts other open SSH sessions), WireGuard from the
     cores, and Raft only if rqlite stays.
   - Outbound: only root (the IDS itself), plus rqlite to the cores if it
     stays.
6. **Stop** `forkmesh-mirror-node`, `forkmesh-app`, `caddy`,
   `fmnext-certsync` and the cert-sync timer.
7. **Alerts**: see "Alert paths".

The node then fails `/readyz`, and its peers stop counting it:

- The app refuses every unseal and heart-beats `quarantined`.
- `/readyz` reports `quarantined` whenever the app runs, as in a
  vote-based quarantine where the node's IDS is dead.
- Peers exclude it from the live set: no Durable Objects, no cron, no DNS
  vote. They refuse it peer unseal and mesh requests, and add its WireGuard
  address to their `blocked` set.
- The DNS health votes see it down. The fleet runs the node reconciler
  `dry`, with no node-held Cloudflare token. The operator box's
  `fmnext-dns-health` loop removes the node's A and AAAA records from
  `app`/`world` (production and `next.*`) after 3 failed probes
  (docs/OPERATIONS.md, "DNS health failover").

### rqlite: the quorum-safety rule

A compromised node's rqlite holds only ciphertext for the secrets. It still
holds plaintext columns (such as `users.is_admin`), and with
`RQLITE_PASSWORD` it could write to the cluster. We therefore prefer to stop
it and remove it as a voter, but the IDS never takes the cluster down:

- The locked-down node stops its own rqlite only when its `/nodes` shows
  **other reachable voters that are a majority of all the voters (at least
  two) and a leader**, and no other node
  is already quarantined. Otherwise rqlite keeps running, and the lockdown
  firewall admits only Raft from the cores. A second simultaneous lockdown
  (for example a bad deploy on every node) therefore never costs quorum.
- A healthy peer then removes the quarantined node as a voter. Only one
  peer does it (the rendezvous-first healthy node). It waits until the node
  has been unreachable for 2 minutes and two reachable voters with a leader
  remain, and it never shrinks the cluster below two voters.
- The majority rule replaced a fixed "two others" on 2026-10-13: with four
  voters (a promoted spare during a core replacement) two reachable others
  are not a quorum without the node, so stopping it would have taken the
  cluster down ([deploy/README.md](deploy/README.md#no-fixed-main-node)).
- With three voters, removing a dead one leaves the same fault tolerance as
  keeping it (zero further failures). Removal adds that the node cannot
  rejoin, vote, or serve reads if the attacker restarts it.
- Peers also drop its WireGuard traffic, so even a running rqlite on it is
  cut off.
- A removed node is not released: rebuild it with `next_fleet.py
  replace-core` (see "Release").

### Lockdown on the runner

1. Marker.
2. Kill every `fmnext-job-*.scope` and every `forkmesh-job` process.
3. Post the incident to `/api/runners/security-incident`, signed with the
   runner's node key. The app records it and **revokes the runner**. A
   runner can only ever revoke itself this way.
4. Firewall: egress root-only, SSH only.
5. Stop the daemon.

### Alert paths

Every incident is:

- logged as a JSON line to the journal (`journalctl -u fmnext-ids`);
- recorded durably in rqlite `security_incidents` (core), or through the
  signed runner API (runner).

For `alert`, `lockdown` and `test`, every **unsealed** core app
(`forkmesh_host/ids_alerts.py`, leaderless) claims the row with a
compare-and-set and delivers it by severity:

| Severity | Journal + `security_incidents` | Ping (inbox, live clients, APNs) | Email |
|---|---|---|---|
| `info` | yes | no | no |
| `alert` | yes | every admin | no |
| `lockdown`, `test` | yes | every admin | yes, within the limits below |

The Ping is a `security_alert` through the normal Pings rail, including the
**APNs phone push**. This kind cannot be muted and ignores quiet hours.

This works while the reporting node is sealed or locked down, because the
peers deliver.

The Ping carries the whole incident, the same text as the email: the
summary, node, severity, detector, incident id and time, every `detail`
field in full, and for a lockdown a "Lockdown steps" list (`seal`,
`marker`, `quarantine`, `record`, `rqlite`, `firewall`, `stop` on a core;
`revoke` from the app on a runner), each `ok` or `FAILED` with its note.
`security_alert` rows keep up to 6000 characters (`SECURITY_ALERT_BODY_MAX`
in `app/src/entry.py`), where every other ping kind keeps 900; the phone
banner still clips at `PUSH_ALERT_BODY_MAX`, and the inbox shows the rest.

A core writes its incident row before it runs the lockdown steps, and
adds their outcome to `detail.lockdown` a moment later. The dispatcher
therefore holds a `lockdown` row without `detail.lockdown` for up to
`LOCKDOWN_SETTLE_MS` (15 s) after `created_ms`, so the one Ping and the one
email name what the lockdown did. A row whose steps never arrive (rqlite
already unreachable, or a second lockdown incident on a node that is
already locked down) goes out after the 15 s without them. The runner API
adds the `revoke` step when it records a runner lockdown, so runner
lockdowns are not held.

### Alert email limits

Emails go through Mailtrap to every admin's verified account email, plus
`ADMIN_ALERT_EMAILS`, with these limits:

- **Dedupe:** at most one email per node and detector per rolling hour.
- **Daily cap:** at most 5 IDS emails per UTC day in total, across all nodes.
- Anything over either limit gets Pings only. Its row's `email_state` is
  `dedupe` or `cap`. The next email that is allowed carries one line,
  "N more IDS alerts suppressed ...", and sets those rows to `digested`.
- If no recipient accepts the email, the hour slot and the daily slot are
  given back (`email_state` `failed`).

Both limits are counters in the rqlite table `mail_budget` (migration 0208),
so they hold across the three cores. The buckets are
`ids-mail:<node>:<detector>` and `ids-mail:<day>`. The row's `delivery` JSON
records the outcome in `email`: `sent`, `failed`, `severity`, `dedupe` or
`cap`, plus `digest` when the line was added.

Recipients are skipped when:

- the admin account is marked as a service account
  (`service_account: true` on its record);
- the email is not verified;
- the address is on a domain we don't receive mail at: `next.forkmesh.com`
  and its subdomains, `example.com/.net/.org`, and the reserved TLDs
  `.invalid`, `.test`, `.example`, `.localhost` and `.local`. Add more with
  `MAIL_NO_RECEIVE_DOMAINS` (comma-separated).

The `fmnext` admin account (`admin@next.forkmesh.com`, a domain without
mail) is a service account. It was marked on 2026-09-24 with:

```sh
fmnext-app-ops service-account fmnext      # as root on any core node
fmnext-app-ops show-account fmnext         # "serviceAccount": true
```

The account is kept; it only gets no mail. `_send_email` also refuses any
address on a domain without mail, whoever sends to it.

### Shared daily mail budget

Alert email shares the Mailtrap account's daily sending limit with all
other mail. On 2026-09-24 IDS alert bursts (07:49 and 20:14 UTC) went to
every admin, 13 of them to the undeliverable `admin@next.forkmesh.com`, and
used up the Free plan's daily limit. Mailtrap then answered 403 ("Your
account has reached its daily sending limit") to every send, including
verification and password reset mail, until the window reset.
`journalctl -u forkmesh-app` logs each rejection as
`mailtrap send rejected: status <code> kind <kind>: <Mailtrap error>`.

Every send made with the platform token now spends one shared counter first
(`app/src/mail_budget.py`, called by `_send_email` in `app/src/entry.py` and
by the package mail API when a package has no token of its own):

- `MAIL_DAILY_LIMIT` (default 150) messages per UTC day in total, bucket
  `mail:<day>`.
- `MAIL_ACCOUNT_RESERVE_PERCENT` (default 70) of that is kept for account
  mail: kinds `verification`, `password_reset`, `password_change` and
  `account`. Every other kind (IDS `security`, `status_alert`,
  `cron_watchdog`, `notifications`, `general_chat`, `tip`, `feedback`,
  `reward_wallet_reminder`, `security_report`, `package`) shares the
  remaining 30%, bucket `mail-bulk:<day>` (45 by default). Account mail can
  use the whole daily limit.
- A send over budget is not attempted; the app logs
  `mail skipped: kind <kind>: daily mail budget spent`. Refused recipients
  log `recipient domain receives no mail` and spend nothing.
- If the counter cannot be read, account mail is still sent and other mail
  is held.
- Buckets older than 3 days are deleted when a new bucket is created.

Set `MAIL_DAILY_LIMIT` to the provider plan's real daily limit, or a little
below it.

Expected volume: IDS mail is at most 5 emails a day, times the deliverable
admin recipients (today 1 to 2 messages each), instead of one message per
alert per admin. All bulk mail together stays under 45 messages a day,
which leaves at least 105 of 150 for verification and password reset.

The IDS process's own **direct mail** (below) uses its own Mailtrap token
and does not go through this budget. It is off (`direct_mail = false`).

### Direct mail and webhook from the IDS

Optional, from the IDS process itself (works when every app is sealed or
down):

- **Direct mail** (`[ids] direct_mail = true`): a separate Mailtrap
  sending token `FMNEXT_IDS_MAIL_TOKEN` and the recipient
  `FMNEXT_IDS_ALERT_EMAIL`. Both are bootstrap credentials: never in the
  vault, encrypted per node. Use a token limited to the sending domain.
  Sent for every lockdown and test, and for any alert that could not be
  recorded.
- **Signed webhook** to the operator desktop (`[ids] webhook_url`). The
  header is `x-fmnext-ids-signature: v1:<ms>:<hex>`, the HMAC-SHA256 of
  `<ms>\n<body>` under `HMAC(FMNEXT_IDS_KEY, "fmnext-ids-webhook-v1")`.
  The desktop can derive that key from the master.

Once a node's own rqlite has stopped, later incidents on that node go only
to the journal and to direct mail. rqlite's HTTP API listens on loopback
only, so the IDS cannot reach a peer's.

## Admin console page

The admin console has a Security section, "Intrusion detection"
(`?view=ids`, `app/src/ids_console.py`). It reads `security_incidents`,
`ids_heartbeats` and `cluster_quarantine` and changes nothing: a release
still goes through `tools/next_ids.py release`.

- **KPIs**: incidents, alerts (`alert` and `test`, with how many are not yet
  delivered) and lockdowns in the window, the first two against the previous
  window of the same length; and how many core heartbeats are fresh.
- **Nodes**: every node seen in the heartbeats, the quarantine records or the
  window's incidents. IDS state is `Normal`, `Lockdown` (the heartbeat's
  state), `Silent` (heartbeat older than 3 minutes, the peers' stale window)
  or `No heartbeat` (runners). The quarantine column shows the newest
  `cluster_quarantine` row as stored; the page does not check its signature,
  the hosts do (`ids_records.py`).
- **Filters**: window (24 hours by hour, 7 days by 6 hours, 30 days by day),
  severity (or "Alerts and lockdowns", every paged severity), and a node or
  detector picked from any link on the page. Every window starts on a bucket
  boundary, so the KPIs, chart, groups and log count the same rows.
- **Chart**: incidents per bucket, with the paged ones drawn on top.
- **Groups**: by detector or by node, each with its severity counts, a
  per-bucket sparkline, its share of the busiest group, how many nodes (or
  detectors) it spans and its newest summary.
- **Incident log**: 50 per page, newest first, with the detail JSON one
  click away and the delivery state (`Paged`, `Pending`, plus `email_state`;
  `info` rows are only logged).

The sidebar badge counts the last 24 hours: lockdowns in red, otherwise
alerts in amber. The time queries use `idx_security_incidents_created`
(migration 0243), since `info` rows (every fleet tool session) make the
table grow without bound.

## Quarantine records

Records live in `cluster_quarantine` and are verified by
`app/host/forkmesh_host/ids_records.py`, which is shared by the app, the
IDS and the desktop. rqlite has one user for every node, so a row is
trusted only through its Ed25519 signature:

| Kind | Signed by | Effect |
|---|---|---|
| `self` | the node's own IDS key (`/var/lib/fmnext-ids/sign.key`, generated on the node) | the node is quarantined |
| `vote` | the voting peer's IDS key | counts once a strict majority of the core nodes (2 of 3, the target excluded) voted |
| `release` | the operator key (`config/ids-operator.key`, desktop only) | clears the records it names or predates |

A single compromised node therefore cannot quarantine a healthy peer, and
cannot release itself. Every verifier keeps what it has seen (the app in
`ids-quarantine.json` in its data dir, the IDS in its state). Deleting
rows does not lift a quarantine.

After a release, peers keep refusing the node peer unseal until the
operator has unsealed it **from the desktop** once. That socket unseal
stamps `reunsealed_ms` on the release row.

## Expected changes (deploys)

Legitimate changes happen through the fleet tools. They push an
**expected-change manifest** to the node's IDS before each step. The
manifest is HMAC-SHA256-signed with that node's own key,
`HMAC(FMNEXT_IDS_MASTER, "fmnext-ids-node-key-v1:<node>")`, pushed as the
credential `FMNEXT_IDS_KEY`. A key taken from one node cannot authorize
changes on another. Each manifest:

- is single-use (nonce);
- is valid for at most 60 minutes (15 by default), with a ±5-minute clock
  skew allowed;
- lists exact sha256 values (`files`), paths whose changes are accepted
  (`rescan`), and which other detectors are relaxed (`allow`: users,
  modules, listeners, reboot, ptrace, processes).

The IDS refuses a forged, replayed or stale manifest (`expectation_rejected`),
and the tool then stops, because the step would look like an intrusion. A
changed file whose content differs from the manifest's sha256 is still an
incident.

| Tool step | Expectation |
|---|---|
| `next_fleet install --steps files` | exact sha256 of every rendered file; removal of the retired units; rescan of `/etc/hosts` and `wg0.conf` (edited on the node) |
| `base` | accounts, sshd, sudoers, PAM, `/etc/forkmesh`, the masked tty unit; allow users, modules, listeners, processes |
| `rqlite` / `daemon` / `secrets` / `start` / `wireguard` | the binaries, creds, `*.wants` links they touch |
| `next_deploy restart` / `rollback` | `current` may point at any release (the gate may roll back); rescan of `releases/` and the two Caddy route snippets |

Without `FMNEXT_IDS_MASTER` in `secrets.env` the tools say so and push
nothing. For a manual change (such as `apt upgrade`, or `py-spy` on the
app), open a window first:

```sh
python3 tools/next_ids.py expect n2 --path /etc/ssh --allow modules --execute
python3 tools/next_ids.py expect n2 --allow ptrace --minutes 10 --execute
python3 tools/next_ids.py baseline n2 --execute   # accept everything now present
```

## Operator commands

```sh
python3 tools/next_ids.py status [nN|rN ...]          # state, keys, recent incidents
python3 tools/next_ids.py test-fire n1 --execute       # harmless incident, waits for delivery
python3 tools/next_ids.py baseline nN --execute        # signed full refresh + rescan
python3 tools/next_ids.py release nN --execute         # after investigation
python3 tools/next_vault.py seal nN --restart --execute  # seal and end the app process
```

`test-fire` always Pings. It emails only within the alert email limits (one
per node per hour, 5 IDS emails a day), and prints the outcome as
`(email sent|dedupe|cap|failed)`.

## Rollout (in this order; dry-run first without `--execute`)

The new credentials are all bootstrap credentials, never in the vault:

- `FMNEXT_IDS_MASTER`: desktop only, in `secrets.env`.
- `FMNEXT_IDS_KEY`: per node, derived from the master; also pushed to r1.
- Optional: `FMNEXT_IDS_MAIL_TOKEN` and `FMNEXT_IDS_ALERT_EMAIL`.

The steps:

1. **App release** (migration 0197; quarantine-aware vault, mesh and
   membership; the dispatcher; the runner incident route):
   `python3 tools/next_deploy.py all --execute`. Until the node files carry
   IDS keys, nothing changes.
2. **Secrets**: `python3 tools/next_fleet.py init-secrets --execute` (adds
   `FMNEXT_IDS_MASTER`).
3. **Operator key and switch**: `python3 tools/next_ids.py keygen --execute`.
   Copy the printed `operator_public_key` into `deploy/fleet.toml [ids]`, set
   `enabled = true` there (it ships `false`, so no earlier `files` step
   installs a half-configured IDS), and back up `config/ids-operator.key`
   offline. `ssh_fleet_fingerprints` is already set (`next_ids.py
   fingerprint` prints it). Optionally set `ssh_allow`.
4. **Node signing keys**, on every node first:
   `for n in n1 n2 n3 r1; do python3 tools/next_fleet.py install $n --steps ids --execute; done`.
5. **Install**, one node at a time (keep an SSH session open on the node):
   `for n in n1 n2 n3 r1; do python3 tools/next_fleet.py install $n --steps base,secrets,files,start --execute; done`.
   - `base` sets sshd `LogLevel VERBOSE`.
   - `secrets` pushes `FMNEXT_IDS_KEY`.
   - `files` installs the IDS, the drop-ins, the egress log rule and the
     app's IDS public keys.
   - `start` restarts the IDS and the app, which comes back sealed and is
     peer-unsealed.
   - Check before the next node:
     `python3 tools/next_ids.py status $n` (normal, keys `expect,sign`).
6. **Baseline**: `python3 tools/next_ids.py baseline nN --execute` on each
   node (the first start already created one; this signs it off).
7. **Test-fire**:
   `python3 tools/next_ids.py test-fire n1 --execute` and
   `test-fire r1 --execute`. Both must report pings and emails delivered. A
   phone with the app registered for APNs must show the banner.
8. Commit `deploy/fleet.toml` (`enabled = true`, the operator key). Done on
   2026-09-24 (863ed6a); the fleet runs with the IDS on.

The `fmnext-ids` unit passes `--config` before the subcommand
(`deploy/core/templates/fmnext-ids.service`): the option belongs to the
top-level parser, and `run --config ...` crash-loops the service
(`deploy/tests/test_ids_daemon.py` feeds the unit's `ExecStart` through the
real parser).

A runner's incident report keeps the IDS's own incident id when it is
well-formed and unused (`app/src/runner_leases.py`), so `next_ids.py
test-fire r1` can follow it.

## Release (after investigating a lockdown)

1. `python3 tools/next_ids.py status nN`, then
   `journalctl -u fmnext-ids` and `/var/lib/fmnext-ids/state.json` on the
   node. SSH still works.
2. If the compromise is real: **do not release**. Rebuild the node
   (`next_fleet.py replace-core nN --new nX`), rotate `DATA_KEY`
   (docs/REKEY.md), the vault secrets, `RQLITE_PASSWORD`,
   `FORKMESH_MESH_SECRET` and the fleet SSH key.
3. For a false positive:
   `python3 tools/next_ids.py release nN --note "why" --execute`. It:
   - signs a release that clears every quarantine record of the node;
   - stores it through a healthy peer;
   - tells the IDS, which removes the marker, lifts the firewall and starts
     rqlite (if it stopped it) and the services;
   - unseals the node from this desktop.

   It refuses a node that is no longer an rqlite voter. A released runner
   must be registered again from `/dashboard/runners`.

## Traps

- **Never run anything as a service user from SSH.** `systemd-run -p
  User=forkmesh-app ...`, `sudo -u`, `runuser` and `su` all start a process
  as that user outside its unit, and that is a lockdown
  (`service_user_outside_unit`; `service_user_shell` for an interpreter).
  On 2026-10-02 at 04:39:46 UTC an agent tested DNS under the app's sandbox
  this way (`python3` in `run-p2129035-i2129335.service`) and locked down n1
  (incident `UkbJ1Tnv1AecReBl`, released as a false positive with release
  `NxAcDZDkvYTUhWhb`). Run read-only probes as root. To test the app's view,
  read its journal or `/proc/<MainPID>`. To test a unit property, use
  `systemd-run` without `User=`.
- **A process that exits during the scan has no unit.** The process scan
  reads `/proc/<pid>/status`, then `exe`, then `cgroup`. The daemon's git
  children (`git upload-pack` and friends) live for a few milliseconds, so
  one can exit between those reads: the `cgroup` read fails, the unit comes
  back empty, and the old code reported it as `service_user_outside_unit`
  with unit `(?)`. That happened twice on n3 on 2026-09-25 (incidents
  `4cNL8r_s3lnMvRfP` at 10:44:07 UTC, `/usr/lib/git-core/git` pid 2378250,
  and `I9dZcPe6pk9y6S6J` at 11:36:47 UTC, `/usr/bin/git` pid 2507681). The
  journal shows only the daemon's own `git-info-refs`/`git-upload-pack`
  requests around them: no SSH session, `su` or `runuser` ran git then.
  Now, when the cgroup has no unit, `process_info` reads the status again.
  If the process is gone, or is a zombie (`State: Z`/`X`), it is marked
  `vanished` and the unit checks (`service_user_outside_unit`,
  `job_user_outside_scope`) are skipped for it. If it is still alive, the
  cgroup is read once more and the result is used, so a live process with
  no unit (the root cgroup `0::/`, or an unreadable cgroup) still alerts.
  The other checks (shell, exe allowlist, suspicious path, ptrace) still use
  what was read before the process exited.
- A user whose only allowed unit is `-` (`forkmesh-node` on a runner, where
  no process should run as that user at all) still alerts for a vanished
  process: its unit would not have mattered.
- For a service user the unit that counts is the outermost `.service`/
  `.scope` in the cgroup path (`system_unit` in the detail), so a child
  cgroup under `forkmesh-mirror-node.service` counts as inside it, and a user
  unit that borrows the daemon's name under a lingering user manager
  (`user@997.service/app.slice/forkmesh-mirror-node.service`) does not: it is
  reported as outside `user@997.service`. Before 2026-09-25 the innermost
  unit was matched, which let that name pass. A process running as the
  service user in a login session (`session-N.scope`), in the root cgroup
  or in any other unit still alerts. `job_user_outside_scope` and the
  `ptrace` severity still use the innermost unit (`unit`), since runner jobs
  may run in a scope under a user manager.

## Residual risks

- Root on a node can disable the IDS before it reacts. The peer vote
  catches a silent IDS, but only after about 4 minutes. Root can also fake
  heartbeats with the node's own key.
- The runner has no peer watching its IDS heartbeat.
- A false lockdown takes a node out until the operator releases it.
  Rendered changes must go through the tools. An `apt upgrade` that edits
  sshd or PAM configuration needs an `expect` window first.
- Expected-change windows accept any change under their `rescan` paths for
  up to 15 minutes.
- A forged quarantine needs two nodes' IDS keys. A forged release needs the
  operator key: keep `config/ids-operator.key` and `FMNEXT_IDS_MASTER`
  offline-backed and off shared machines.
- DNS is not changed by the nodes (the reconciler runs `dry`): after a
  lockdown the node's A records stay until `next_dns.py` removes them.
  Connections to it fail fast (inbound is closed), and clients try the
  other records.

- 2026-09-28: The TCP egress log rules only count connection attempts
  (`tcp flags & (syn | ack) == syn`). With `ct state new` alone, a packet from
  a connection the kernel no longer tracks is also "new" mid-stream: a Caddy
  reply, FIN or RST retransmit to an inbound client after a firewall reload
  or Caddy restart. That was the case during a core roll, when n1's daemon
  (fetching from its peer n3, per the peer-upstream change) had a connection
  open to n3's Caddy. It raised two `egress_unexpected` alerts, "caddy opened
  TCP to <n1> port 33960", where 33960 was n1's client port. A real outbound
  connection always starts with a SYN, so nothing real is missed
  (`test_ids_egress_log_rules_only_count_connection_attempts`).

## Release through a peer with a node down (2026-10-01)

`release` lists the cluster through a peer's rqlite `/nodes`. Without a
`timeout` parameter rqlite waits on every member, and the locked-down node is
firewalled, so the call outlived curl's 10 s and the n1 release on 2026-10-01
failed with `curl: (28)`. `/nodes` queries in `next_ids.py` and
`next_fleet.py` now pass `timeout=2s`; an unreachable member comes back with
`reachable: false` instead of stalling the query.

## New unit drop-in directories are expected (2026-10-01)

The files step's expectation listed every rendered file by hash but not the
directories they create. The TURN unit added
`/etc/systemd/system/fmnext-turn.service.d/50-fmnext-ids.conf`, the new
directory itself was reported as `integrity_added`, and n1 went into lockdown
during the 2026-10-01 roll (incident `k6imdgf0t4x_FvyY`, released as a false
positive). `files_expectation` now also expects each `*.d` parent directory
under `/etc/systemd/system` of a rendered file as `"dir"`; any other new
directory there still alarms.
