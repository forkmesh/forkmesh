# Gateways: HTTPS and SSH

The daemon never serves Git itself. It supervises the Python HTTPS gateway and
leaves SSH writes to the SSH gateway and broker.

| Component | Code | Guide |
| --- | --- | --- |
| HTTPS gateway (read-only `upload-pack`, dashboard, merge/Actions couriers, `/v1/node/logs`) | `tools/mirror_gateway.py` | [direct-https-mirror-gateway.md](../../www/docs/operations/direct-https-mirror-gateway.md) |
| SSH gateway (push over SSH, forced command) | `tools/ssh_gateway.py`, `server/packaging/ssh/ssh-gateway-entrypoint`, `tools/provision_ssh_gateway.sh` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md) |
| SSH authorization broker (local authorization on independent mirrors) | `tools/ssh_authorization_broker.py`, `server/packaging/systemd/forkmesh-ssh-authorization-broker.service` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md), [breakout-authorization-replica.md](../../www/docs/operations/breakout-authorization-replica.md) |
| Post-receive refresh | `tools/ssh_post_receive_refresh.py`, `server/packaging/ssh/ssh-refresh-notify` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md) |
| SSH host certificates | `tools/sign_ssh_host_certs.sh` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md) |

JSON schemas for these configs are in `www/docs/*.schema.json`
(`mirror-gateway-config`, `mirror-gateway-manifest`, `mirror-endpoint`,
`ssh-gateway-config`, `ssh-authorization-broker`, `ssh-post-receive-refresh`,
`mirror-actions-*`).

## Gateway generations

The daemon writes the gateway config and sends SIGHUP; the gateway acknowledges
right after the validated swap and warms caches afterwards in a background
thread. The daemon waits up to 90 s (`gatewayReloadTimeout`) and restarts only
a gateway that is not responding, after a random delay of up to 20 s, so a
slow reload never restarts the whole fleet at once.

## Issue index

`issue-index?ref=` answers one compact summary per issue record at a commit
(no descriptions), so the issues list does not download every record;
`issue-index?ref=&related=<n>` answers only the issues one issue page's
relations need; `issue-search?ref=&q=` answers the numbers whose title,
description or live comments contain a text. All three are carried by the
`blobs` permission (`ISSUE_INDEX_CARRIER_OPERATION`), never advertised on
their own, and sent as UTF-8 JSON; folding, caching and limits are in
[issue-scale-limits.md](../app/issue-scale-limits.md#the-issue-index).

## Startup and control signals

The gateway listens first and warms its caches afterwards on the
`gateway-prewarm` thread (`MirrorGatewayServer.start_prewarm`), the same path
a SIGHUP reload uses. It used to run `prewarm_memory_caches()` before binding:
per repository a commit-graph write, tree, history, branches, stats, sizes and
two ref advertisements, plus the dashboard catalog and assets. Measured on a
bare copy of the BLT repository (45k commits, 392 MB pack) pinned to one CPU
with a warm page cache: one repository warmed in about 3 s and seven in
16.5 s before the port answered; with the listener first the port answers in
0.26 s (one repository) and 0.34 s (seven). A single-server instance serving
seven repositories on a small VPS has a cold cache after a restart, which made
the ~40 s outage seen after deploys. Requests during the warmup are served on
demand; `_single_flight` keeps a request and the warmup from computing the
same answer twice.

The daemon sends SIGHUP (reload a new config generation) and SIGUSR1 (reload
the dashboard replica after `--sync-dashboard-replica`) to the live process.
Both handlers are installed before `GatewayApplication` is built and only
record the request; it is applied once the server exists. SIGUSR1 used to get
its handler only after the warmup, and its default action terminates the
process, so a replica refresh during the 40 s startup killed the gateway
("gateway exited: signal: user defined signal 1; retrying in 5s") and the next
start began from zero. The supervisor also restarts a child that died of a
SIGHUP or SIGUSR1 it sent without the 5 s crash backoff
(`diedOfControlSignal`), which covers the few hundred milliseconds of
interpreter start before `main()` installs the handlers.

## Traps

- An older gateway refuses to start on a config key it does not know. Deploy
  `tools/mirror_gateway.py` together with the daemon.
- A missing path is a repository answer (404 `not_found`), not an outage
  (503 `mirror_unavailable`). `git cat-file blob <commit>:<path>` exits 128
  both for an absent path and for a broken object store, so `blob`, `blobs`,
  `tree` and `raw` look the entry up with `ls-tree` on a literal pathspec and
  read the object by id.
- `tree` lists at most `MAX_TREE_ENTRIES` (500) entries of a code folder, with
  `truncated: true` past that. The collaboration folders
  (`COLLABORATION_TREE_ROOTS`: `.forkmesh/issues`, `pulls`, `discussions`,
  `releases`) list up to `MAX_COLLABORATION_TREE_ENTRIES` (50,000): they skip
  the per-entry history and analysis, so a long listing is cheap, and the
  issue list reads `.forkmesh/issues/open` as one listing. With the 500 cap a
  mirror of a forge with 547 open issues showed "Open 500" and never loaded
  the rest.
- `sizes` folds each directory to its 40 largest entries plus a `summary`
  (`MAX_SIZES_CHILDREN`), except inside the collaboration folders, which list
  every entry (up to `MAX_COLLABORATION_TREE_ENTRIES`). The code map turned a
  summary into "more" tiles of lazy stubs that each needed their own `/tree`
  read on zoom, so a 600-issue forum showed a grid of empty dashed folders;
  with every issue folder and file sized up front they render at once.
- The HTTPS gateway has no `receive-pack`. Pushes go through SSH or through the
  Worker's hosted-push path ([hosted-repositories.md](../../www/docs/hosted-repositories.md)).
- A search for a common word is a repository answer too, not an outage.
  `search` streams `git grep` and stops at `MAX_SEARCH_RESULTS` matches, at
  2 MB of output or at the process timeout, returning what it has with
  `truncated: true`. It answers 503 only when git fails before any match.
