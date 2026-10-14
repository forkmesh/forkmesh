# Host intrusion detection (IDS)

Every core node and runner runs `fmnext-ids`, a small root service
(`deploy/core/ids.py`, installed as `/opt/forkmesh-next/bin/fmnext-ids`,
config `/etc/forkmesh-next/ids.json`, state in `/var/lib/fmnext-ids`). It uses
the Python standard library (plus `python3-cryptography` for signing), runs
with a CPU quota and idle I/O priority, stats files instead of re-reading
them, and re-hashes everything every 6 hours.

On an intrusion it **locks the node down**: the vault key leaves memory, the
node is quarantined in rqlite, the firewall closes and services stop. It also
pages the admins. Only the operator can end a lockdown
(`tools/next_ids.py release`).

## Threat model

Detects within seconds: a shell as a service user, logins with unknown keys or
passwords, new privileged accounts, tampering with code, units or security
configuration, ptrace of protected processes, runner jobs escaping their
scope, persistence through cron, listeners, unusual egress or kernel modules,
and an IDS that stops reporting while its node keeps serving.

Does not stop:

- **Root acting faster than the IDS.** Mitigated by fast detection (journald
  lines as they arrive, process scans every 5 s), sealing the key first, and
  peers that vote a node into quarantine when its app runs while its IDS has
  been silent for 3 minutes.
- **A node compromised while unsealed**: its memory held `DATA_KEY`. Rotate
  `DATA_KEY` and vault secrets after any real lockdown ([REKEY.md](REKEY.md)).
- **A compromised operator box**, which holds `DATA_KEY`, the fleet SSH key,
  the IDS master key and the operator release key.
- **HTTPS exfiltration by the app user**: only non-HTTP(S) egress is logged.
- **Non-interactive use of a stolen fleet key** raises only `info` unless
  `ssh_allow` is set.

## Detectors

Override any severity with `[ids] severity = { detector = "lockdown" }` in
`deploy/fleet.toml`.

| Detector | Trigger | Default |
|---|---|---|
| `ssh_unknown_key` | key not in `ssh_fleet_fingerprints` | lockdown |
| `ssh_non_key_login` | password or keyboard-interactive login | lockdown |
| `ssh_foreign_ip` | login from outside `ssh_allow` (when set) | alert |
| `ssh_interactive_login`, `ssh_login_user` | a pty shell; a non-root login | alert |
| `ssh_tool_session` | a command without a pty, sftp or tunnel (the fleet tools) | info |
| `integrity_*` | a change outside a signed expected-change window | lockdown for binaries, the release tree, units, sshd, sudoers, PAM, firewall; else alert |
| `account_uid0`, `account_login_shell`, `group_privileged`, `root_password_set` | account database diffs | lockdown |
| other `account_*`, `group_changed`, `shadow_changed` | account changes | alert |
| `service_user_outside_unit` | a service user's process outside its own unit (outermost unit in the cgroup path) | lockdown |
| `service_user_shell` | a shell or interpreter under a service user | lockdown |
| `service_user_exe` | an executable outside the user's allowlist | alert |
| `exe_suspicious_path` | an executable in `/tmp`, `/dev/shm`, `/var/tmp` or a memfd | lockdown |
| `ptrace` | a traced protected process | lockdown |
| `job_user_outside_scope` | runner job-user process outside a job scope | lockdown |
| `listener_unexpected` | a listener on an unexpected port | alert |
| `egress_unexpected` | a new outbound connection (SYN only) from a service user to a non-web port | alert |
| `kernel_module_loaded`, `kernel_module_taint` | a module loaded after start or a tainting module | alert |
| `privilege_tool` | sudo/su by a non-root account | alert |
| `peer_ids_silent` | a peer's IDS heartbeat is stale while its app runs | alert |
| `quarantined_by_peers` | a majority voted this node into quarantine | lockdown |
| `expectation_rejected` | a forged, replayed or stale expected-change manifest | alert |
| `baseline_tampered`, `baseline_missing` | the baseline failed its HMAC check or vanished | lockdown |
| `reboot` | boot id changed outside a window | info |

A process that exits mid-scan is marked `vanished` and skipped by the unit
checks; a live process with no unit still alerts.

## Lockdown on a core

1. **Seal**: the app drops `DATA_KEY` and every vault value and exits.
2. **Marker** `/var/lib/fmnext-ids/LOCKDOWN`; drop-ins keep the app, daemon,
   Caddy and certsync from starting, also after reboot.
3. **Quarantine**: a signed `self` row in `cluster_quarantine` and the
   incident in `security_incidents`.
4. **rqlite**: stopped only if the other reachable voters are a majority (at
   least two) with a leader and no other node is quarantined; otherwise it
   keeps running behind the firewall. One healthy peer later removes the node
   as a voter when that is quorum-safe, never below two voters.
5. **Firewall** (`inet fmnext_ids`): inbound only replies, loopback, SSH and
   mesh; outbound only root.
6. **Stop** services.
7. **Alerts.**

Peers then exclude the node from the live set, refuse it unseal and mesh
traffic, and drop its WireGuard packets. The operator-box DNS loop removes it
from DNS after failed probes.

On a runner: marker, kill all job scopes, report to
`/api/runners/security-incident` (the app revokes the runner), firewall, stop
the daemon.

## Alerts

Every incident is logged to the journal and recorded in `security_incidents`
(cores) or through the signed runner API. Unsealed core apps
(`app/host/forkmesh_host/ids_alerts.py`, leaderless, compare-and-set claim)
deliver them, so delivery works while the reporting node is down:

| Severity | Record | Ping (inbox, live clients, push) | Email |
|---|---|---|---|
| `info` | yes | no | no |
| `alert` | yes | every admin | no |
| `lockdown`, `test` | yes | every admin | yes, within limits |

The `security_alert` Ping cannot be muted and carries the full incident and
lockdown step outcomes. A lockdown row waits up to 15 s for its step results
before delivery.

Email limits (counters in the rqlite `mail_budget` table): one email per node
and detector per hour, five IDS emails per day in total; suppressed alerts
are summarised in the next allowed email. Service accounts, unverified
addresses and domains that receive no mail are skipped.

All platform mail shares one daily budget (`app/src/mail_budget.py`):
`MAIL_DAILY_LIMIT` per day with `MAIL_ACCOUNT_RESERVE_PERCENT` reserved for
verification, password and account mail. Set the limit to the provider's real
daily limit.

Optional, from the IDS process itself: direct mail with its own token
(`[ids] direct_mail`, `FMNEXT_IDS_MAIL_TOKEN`, `FMNEXT_IDS_ALERT_EMAIL`) and a
signed webhook (`[ids] webhook_url`, header
`x-fmnext-ids-signature: v1:<ms>:<hex>`).

## Admin console

Security, Intrusion detection (`?view=ids`, `app/src/ids_console.py`): KPIs,
node states (Normal, Lockdown, Silent, No heartbeat), filters, an incident
chart, groups by detector or node, and the incident log. Read-only.

## Quarantine records

Verified by `app/host/forkmesh_host/ids_records.py` (shared by the app, the
IDS and the desktop); rows are trusted only by Ed25519 signature:

| Kind | Signed by | Effect |
|---|---|---|
| `self` | the node's own IDS key | the node is quarantined |
| `vote` | a peer's IDS key | counts once a strict majority of other cores voted |
| `release` | the operator key (operator box only) | clears the records it names or predates |

One compromised node cannot quarantine a peer or release itself. Verifiers
remember what they have seen, so deleting rows does not lift a quarantine.
After a release, peers refuse peer unseal until the operator unseals the node
once.

## Expected changes

Fleet tools push an **expected-change manifest** before each step, signed
with the node's own key `HMAC(FMNEXT_IDS_MASTER, "fmnext-ids-node-key-v1:<node>")`
(pushed as `FMNEXT_IDS_KEY`). Manifests are single-use, valid at most 60
minutes, and list exact file hashes (`files`), accepted paths (`rescan`) and
relaxed detectors (`allow`). The `files` step also expects new unit drop-in
directories. A fresh node starts its IDS last, so its first baseline already
contains everything the start step wrote.

For a manual change, open a window first:

```sh
python3 tools/next_ids.py expect n2 --path /etc/ssh --allow modules --execute
python3 tools/next_ids.py expect n2 --allow ptrace --minutes 10 --execute
python3 tools/next_ids.py baseline n2 --execute
```

## Operator commands

```sh
python3 tools/next_ids.py status [nN|rN ...]
python3 tools/next_ids.py test-fire n1 --execute
python3 tools/next_ids.py baseline nN --execute
python3 tools/next_ids.py release nN --note "why" --execute
python3 tools/next_vault.py seal nN --restart --execute
```

Setup: `next_fleet.py init-secrets` adds `FMNEXT_IDS_MASTER`;
`next_ids.py keygen` creates the operator key (its public half goes into
`deploy/fleet.toml [ids]` with `enabled = true`); `install --steps ids`, then
`base,secrets,files,start` one node at a time; `baseline`; `test-fire`. The
unit passes `--config` before the subcommand.

## Release

1. Investigate: `next_ids.py status`, `journalctl -u fmnext-ids`, the state
   file. SSH still works.
2. Real compromise: do not release. Rebuild the node and rotate `DATA_KEY`,
   vault secrets, `RQLITE_PASSWORD`, `FORKMESH_MESH_SECRET` and the fleet SSH
   key.
3. False positive: `release` signs a release, stores it through a healthy peer
   (rqlite `/nodes` queries use a short timeout), tells the IDS to lift the
   lockdown, and unseals the node. A released runner must be registered again.

## Rules

- Never run anything as a service user over SSH (`systemd-run -p User=`,
  `sudo -u`, `runuser`, `su`): that is a lockdown. Probe as root, or read the
  app's journal and `/proc/<MainPID>`.
- Change nodes only through the fleet tools or inside an `expect` window.

## Residual risks

- Root can disable the IDS before it reacts; the peer vote takes about four
  minutes. Runners have no peer watching their heartbeat.
- A false lockdown takes a node out until released.
- Expected-change windows accept any change under `rescan` paths while open.
- A forged quarantine needs two nodes' IDS keys; a forged release needs the
  operator key. Keep the operator key and IDS master offline-backed.
