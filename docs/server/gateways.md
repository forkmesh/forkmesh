# Gateways: HTTPS and SSH

The daemon ([README.md](README.md)) never serves Git itself. It supervises the
Python HTTPS gateway and leaves SSH writes to the SSH gateway and broker. These
gateways are what a mirror serves clones from
([mirroring](../mirroring.md)).

| Component | Code | Guide |
| --- | --- | --- |
| HTTPS gateway (read-only `upload-pack`, dashboard, merge/Actions couriers, `/v1/node/logs`) | `tools/mirror_gateway.py` | [direct-https-mirror-gateway.md](../operations/direct-https-mirror-gateway.md) |
| SSH gateway (push over SSH, forced command) | `tools/ssh_gateway.py`, `server/packaging/ssh/ssh-gateway-entrypoint`, `tools/provision_ssh_gateway.sh` | [git-ssh-gateway.md](../operations/git-ssh-gateway.md) |
| SSH authorization broker (local authorization on independent mirrors) | `tools/ssh_authorization_broker.py`, `server/packaging/systemd/forkmesh-ssh-authorization-broker.service` | [git-ssh-gateway.md](../operations/git-ssh-gateway.md), [breakout-authorization-replica.md](../operations/breakout-authorization-replica.md) |
| Post-receive refresh | `tools/ssh_post_receive_refresh.py`, `server/packaging/ssh/ssh-refresh-notify` | [git-ssh-gateway.md](../operations/git-ssh-gateway.md) |
| SSH host certificates | `tools/sign_ssh_host_certs.sh` | [git-ssh-gateway.md](../operations/git-ssh-gateway.md) |

JSON schemas for these configs are in `docs/*.schema.json`
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
a SIGHUP reload uses. Warming before binding (per repository a commit-graph
write, tree, history, branches, stats, sizes, two ref advertisements, plus the
dashboard catalog and assets) kept the port closed for about 3 s per repository
on one CPU (16.5 s for seven, about 40 s on a small VPS with a cold cache);
listening first answers in 0.26-0.34 s. Requests during the warmup are served
on demand; `_single_flight` keeps a request and the warmup from computing the
same answer twice.

The daemon sends SIGHUP (reload a new config generation) and SIGUSR1 (reload
the dashboard replica after `--sync-dashboard-replica`) to the live process.
Both handlers are installed before `GatewayApplication` is built and only
record the request; it is applied once the server exists. SIGUSR1's default
action terminates the process, so a handler installed late lets a replica
refresh kill a starting gateway ("gateway exited: signal: user defined signal
1; retrying in 5s"). The supervisor also restarts a child that died of a
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
- A comparison past its bounds is a repository answer too (413
  `comparison_too_large`), not an outage. `compare` and `cherry` turn
  `MAX_COMPARE_COMMITS`, `MAX_CHERRY_UPSTREAM_COMMITS` and the
  `MAX_COMPARE_BYTES` output limit (`_run_git` raises `GitOutputLimit`, a
  `GitError`) into `ComparisonTooLarge`, so callers do not retry a request
  that can never succeed and the compare page does not call the mirror down.
  The dashboard words a 413.
- `tree` lists at most `MAX_TREE_ENTRIES` (500) entries of a code folder, with
  `truncated: true` past that. The collaboration folders
  (`COLLABORATION_TREE_ROOTS`: `.forkmesh/issues`, `pulls`, `discussions`,
  `releases`) list up to `MAX_COLLABORATION_TREE_ENTRIES` (50,000): they skip
  the per-entry history and analysis, so a long listing is cheap, and the
  issue list reads `.forkmesh/issues/open` as one listing (a 500 cap made a
  forge with 547 open issues show "Open 500").
- A code folder's `tree` carries a dependency and coverage analysis
  (`_analysis`), which takes seconds on a large repository. It reads only
  code: every path outside `.forkmesh/`, plus coverage artifacts
  (`COVERAGE_ARTIFACT_NAMES`) anywhere (`_analysis_path_skipped`). Issues,
  pulls, reviews and the Send list live under `.forkmesh/` and move the
  branch tip many times a day without touching code, so the analysis is
  keyed on a digest of the `ls-tree` records it reads
  (`_analysis_content_key`), and a tip whose key matches reuses the previous
  value with only its `commit` changed; a push only forgets the commit.
  Otherwise each metadata commit cost a rebuild (8 s on the next view), and
  `.forkmesh/` files would count toward `MAX_ANALYSIS_REPOSITORY_FILES` and
  push code files out of a large repository's window.
- `sizes` folds each directory to its 40 largest entries plus a `summary`
  (`MAX_SIZES_CHILDREN`), except inside the collaboration folders, which list
  every entry (up to `MAX_COLLABORATION_TREE_ENTRIES`), so the code map
  renders every issue folder at once instead of "more" tiles that each need
  their own `/tree` read on zoom.
- The HTTPS gateway has no `receive-pack`. Pushes go through SSH or through the
  App's hosted-push path ([hosted-repositories.md](../hosted-repositories.md)).
- A search for a common word is a repository answer too, not an outage.
  `search` streams `git grep` and stops at `MAX_SEARCH_RESULTS` matches, at
  2 MB of output or at the process timeout, returning what it has with
  `truncated: true`. It answers 503 only when git fails before any match.
