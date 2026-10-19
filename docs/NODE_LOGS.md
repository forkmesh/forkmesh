# Node logs on the operator desktop

The desktop's Log page follows every fleet node's systemd journal live (cores,
runners, autoscaled replicas and burst runners), every Caddy request, and the
operator box's own deploy output. Nothing is stored in the database: live
logs stay on the desktop.

## Transport: the desktop pulls over SSH

The desktop opens one SSH session per node with a dedicated reader key
(`next_logread_ed25519`, created by `tools/next_fleet.py logread-key`), not the
fleet key. On each node it is a forced command:

```
restrict,command="/usr/bin/python3 -I /opt/forkmesh-next/bin/fmnext-logread" ssh-ed25519 ... fmnext-logread
```

`fmnext-logread` (`deploy/core/logread.py`) accepts only `follow`,
`follow lines N`, `follow after <cursor>` or `hello`, drops privileges to
`nobody` plus the journal group with `no_new_privs`, and runs
`journalctl --follow --output=json`. Messages are redacted on the node
(the `tools/secret_gate.py` rules, credentials in headers, assignments, JSON
and URLs, long tokens).

Stream format: a header line (`{"logread":1,"node":…,"role":…}`), one line per
journal entry with its `__CURSOR`, an idle heartbeat after 60 s, and a
resource sample (`logread_vitals`) every 15 s from world-readable kernel
counters (memory, load, file handles, disk, uptime, CPU busy share) for the
Visualizer. Missing counters are omitted, never sent as zero.

The desktop resumes from the last cursor it saved, so lines written while it
was off are read later (bounded by journald rotation). Each stream is
authenticated by its node's host key, so a compromised node can only lie
about its own logs. Pushing logs into the app database was rejected: it would
write every line through rqlite and let any compromised core write every
node's stream.

## IDS

The reader key's fingerprint is part of the IDS fleet fingerprints, so a
reader login is an `info` `ssh_tool_session`. `restrict` refuses a pty. The
authorized-keys line is written only after the IDS has restarted with the new
config, and a fresh node starts its IDS after the line is written
([IDS.md](IDS.md)).

## Caddy access log

`[logs] access_log = true` adds JSON access logging to the journal: request and
response headers dropped, client address and `user_agent` kept, Cloudflare and
edge visitor fields (`cf_*`, `edge_*`) only when the TCP peer is in
Cloudflare's published ranges, the `expected_degraded` response header,
credential query parameters redacted, `/readyz` and `/health` skipped. The app
never sees the `Cf-*` headers.

## Rolling it out

1. `tools/next_fleet.py logread-key --execute`; copy the printed `[logs]`
   block into `deploy/fleet.toml`.
2. Per core, drained from DNS: `install nN --steps logread --execute`, undrain,
   `next_ids.py status nN`. For a runner, only the install and status check.

The `logread` step pushes an expected-change window for exactly its files and
refuses a node with another pending change; if Caddy rejects the reload, the
previous Caddyfile is restored.

## New and autoscaled nodes

A node's logs register on the desktop when it is:

- **Discoverable:** labelled `fmnext-core-N`, `fmnext-auto-N`, `fmnext-run-N`
  or `fmnext-burst-N` and tagged `forkmesh-next`. The desktop reads
  `fleet-instances.json`, `autoscaled-instances.json` and a cached provider
  listing (refreshed hourly or shortly after a scaler create/destroy line, with
  backoff on failure).
- **Carrying the reader:** installed by the `files`/`start` or `logread`
  steps. Burst-runner snapshots must be taken after that.

Autoscaler rules: app replicas are `fmnext-auto-N`, burst runners
`fmnext-burst-N`, never `fmnext-run-N` (reserved for always-on runners); keep
the tag; report scaled nodes in `next_autoscale.py status --json`; rebuild the
snapshot when the reader or its key changes.

## What reaches the Log page

`desktop/src/CoreNodeLogs.cpp` forwards every journal line with a message,
sorted into a source (mail, app, rqlite, web, job, mirror, ids, ops, ssh,
kernel, system) and a fleet category; error lines go under **Error**.
Identical lines from one node and source fold to one per 10 minutes. Streams
pause past 20,000 queued lines and resume from the cursor.

- JSON journal messages are shown as one readable line (level, logger,
  message, fields; IDS incidents as `severity detector: summary`).
- Each Caddy access record is one line with method, host, path, status,
  duration, size, client flag and address, and User-Agent. A 5xx is an error,
  except an expected-degraded 503, which is a warning.
- Client details: country from a cached DB-IP Lite table
  (`desktop/src/IpCountry.cpp`), labels for fleet nodes, loopback, the mesh
  and Cloudflare egress, the relayed visitor from the signed edge header, and
  an optional `whois` owner per network (`desktop/src/IpWhois.cpp`, rate
  limited, never for private ranges). The same enrichment applies to any line
  with an `IP <addr>` field.
- A host-side 500 is reported by `app/host/forkmesh_host/error_log.py` as an
  `ERROR <context> -> 500: <repr>` line plus a traceback, which the desktop
  joins into one error. The client response is unchanged.

## Deploy output

`desktop/src/DeployLogs.cpp` follows `<fleet logs>/autodeploy/*.log`,
`<fleet logs>/ops/*.log` and the user journal of the publisher, fan-out and
node-deploy units (`<fleet logs>` is `~/projects/forkmesh/logs/fleet` or
`FMNEXT_LOGS_DIR`). Each line reads `Deploy <kind>: <line>`; duplicates
between file and journal are dropped. Only real failures (journal err,
`result: FAILED`, tracebacks) are errors; per-test failure lines are not.

`next_common.Runner` tees every `--execute` run's output to
`<fleet logs>/ops/<stamp>-<tool>-<command>.log` (0600). Dry runs and
children of the deploy agent write nothing. Deploy tests point
`FMNEXT_LOGS_DIR` at a temp folder.

## Planned restarts are quiet

`desktop/src/PlannedRestarts.cpp` keeps one planned-restart window per node,
opened by the node's IDS "expectation accepted" line for a deploy or roll (or
the desktop's own deploy line) and closed by "back in DNS" or the deploy's
version check, at most 15 minutes. Inside a window, restart symptoms
(connection refused to the app or gateway, Caddy 502/503, local 500s, sealed
vault) raise one info Ping instead of error Pings; they still count as errors
in the Log. A window that expires logs "did not come back" as an error. IDS
lines and other errors always ping. Roll nodes with `roll` or undrain through
`next_dns.py` so the window closes.

## Turning all logging off

**Stop logging** in the Log page header (`logs/allOff`) drops every line before
it is recorded, stops every network log feed (Worker tail, node SSH streams,
host feeds, provider inventory polls) and the deploy-log follower, and flushes
the queue with a final `Logs: all logging is off.` line. Turning it back on
restarts each feed whose own setting is on.

## Recording only some log types

Each category chip on the Log page has a record checkbox under it, a shortcut
for the Log types dialog's switches (`logs/mutedCategories`), so the choice
survives a restart. A ticked box means the category is recorded right now:
logging is on and the category is not muted.

- Ticking a box while all logging is off turns logging on for that category
  alone (every other category is muted), so picking a few types from the off
  state is a matter of ticking each one.
- Ticking a box while logging is on adds that category back; unticking mutes it.
- The **All** box is checked when every category is recorded, partly checked
  when some are muted, and empty while logging is off. Unticking it turns all
  logging off; ticking it unmutes everything and turns logging on.

Muted categories still let their network feeds run; only **Stop logging**
stops those.
