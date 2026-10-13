# Auto-deploy: main to the core nodes

A push to `main` that changes the served stack deploys it to the core nodes
`n1` to `n3`. This is the same deploy an operator runs by hand with
`tools/next_deploy.py`, now driven by a ForkMesh Actions workflow and an agent
on the source node. It never deploys the old Cloudflare Workers: the only
Cloudflare workflow, `.forkmesh/deploy.yml`, is manual-only and its first step
refuses. Older branches still carry its push-triggered copy; since
2026-09-28 the desktop skips a push-triggered workflow on any branch when the
default branch's copy of it has no `push` trigger
([../desktop/README.md](../desktop/README.md#traps)).

**Status:** armed since 2026-09-26. The Next repository is frozen and the
operator folders moved on 2026-09-24 ([NEXT_ADOPTION.md](../NEXT_ADOPTION.md)).
Before that, pushes to `main` reached the cores within a minute through the
source publisher but nothing deployed them (`main` ffa724fae, the World floor
notes, sat undeployed behind `3c2cb9790`). `python3 tools/next_autodeploy.py
pause <reason>` or `disarm` stops it.

## Pieces

| Piece | Where | What it does |
| --- | --- | --- |
| Workflow | `.forkmesh/deploy-nodes.yml` ("Deploy nodes") | Queued by a push to `main` whose changed paths match `on.push.paths`, or by *Run workflow*. Pinned to the source desktop with `runs-on: forkmesh`, that node's Actions label (its node name). The first version said `jett`, the owner's account, which is not a node label, so the desktop logged "dedicated to jett - this node (forkmesh, linux) is skipping it" and queued nothing. Its one step follows the agent's log and exits with the agent's verdict. |
| Agent | `tools/next_autodeploy.py serve`, run by the user unit `forkmesh-node-deploy.service` from the pinned copy in `~/projects/forkmesh/bin/node-deploy/` | Finds running "Deploy nodes" runs, decides whether each may deploy, debounces, takes the deploy lock, runs the gates, writes the log into the run and Pings the admins on failure. |
| Deploy tool | `tools/next_deploy.py`, run from a clean worktree pinned to the commit | Builds, uploads, migrates and does the rolling restart, exactly as by hand. It takes the same deploy lock and records every full-fleet release in the deploy manifest. |
| Alerts | `POST /api/accounts/admin-operator-alert` (`app/src/operator_alerts.py`) | An admin-signed request from the source node. Every admin gets an `operational_alert` Ping; with `severity: email`, the admin addresses also get an email through the normal mail path (daily mail budget, `#email-log` note). |

### Why an agent outside the sandbox

A workflow step runs in a bwrap sandbox as `nobody`, with no host home. It
cannot reach the fleet SSH key, `config/DATA_KEY` or the fleet config, and it
must not: copying them into Actions variables would put fleet root credentials
in a store that every workflow on this node can reference. So the step only
watches. The agent runs as the operator, reads the fleet files in place, as a
manual deploy does, and never prints them.

The agent reads only files the desktop itself writes about a run
(`data/actions/runs/<repo key>/<id>/meta.json`: owner, name, ref, commit,
workflow path). Nothing a step writes is trusted. It writes the log into the
run's workspace as `node-deploy.log`: it creates the file with `O_EXCL` and
`O_NOFOLLOW` beside a workspace directory opened with `O_NOFOLLOW`, so a step
cannot redirect the write through a planted file or link. The run's sandbox
directory appears before the desktop has cloned `workspace/` into it, so a run
whose workspace does not exist yet is retried on the next tick instead of being
given up on (run 1427 failed that way: the agent looked one second after the
sandbox appeared and never looked again). The last line is
`::node-deploy-result:: ok|fail <message>`, which the step turns into its exit
code.

The agent's own code is the installed copy, not the checkout: a commit on
`main` cannot change how the agent schedules, locks or decides. It is updated
only by `python3 tools/next_autodeploy.py install`.

## Trigger

- **Only `main`.** The desktop queues push runs only for the default branch of
  the served mirror (the publisher syncs with `git fetch`, which runs no
  hooks). The agent also refuses anything else: another ref ("skipped: not
  main"), a pull-request run ("never deploys"), another repository or a fork
  ("refused: not forkmesh/forkmesh"), and a commit that is not on the source
  checkout's `main`.
- **Merges made on the cores** (a runner session's **Merge & clean up**, a web
  pull-request merge, an owner's push) reach `main` on the cores first. They
  deploy once the source publisher takes them into the checkout, within about
  20 s (`forkmesh-source-cores.timer`,
  [../tools/source-publisher.md](../tools/source-publisher.md#merges-made-on-the-cores-2026-10-03)).
  Until then the agent would refuse the run as "not on main".
- **Only served-stack paths.** Two classes (`classify` in the agent):
  - *Release paths* start an app release: `app/`, `office/` (the office
    client is built into `app/dist/office` from `office/public`; until
    2026-10-04 it was missing here, so an office-only commit never deployed),
    `world/`, `www/public/`,
    `deploy/`, `marketplace/packages/`, `marketplace/catalog.json`,
    `marketplace/manifest.schema.json`, `marketplace/tools/marketplace_lib.py`,
    `tools/rqlite_migrate.py` and `tools/next_*.py`.
  - *Roll paths* need a node roll (see below): `server/`, `deploy/core/` and
    `tools/mirror_gateway.py`.

  Test directories (`tests`, `tests_rqlite`, `tests_cluster`, `browser_tests`)
  and Go `_test.go` files never count. `www/docs/` is not served by the nodes:
  the release stages only `www/public/` files.
- **The workflow filter is a prefilter.** The desktop matches `on.push.paths`
  with Qt's path wildcard, where `*` and `**` never cross a `/`. So the list
  names each prefix at every depth (`app/*`, `app/*/*`, and so on, 12 levels;
  the deepest served file today is 6). `python3 tools/next_autodeploy.py
  workflow-paths` prints the list, and `deploy/tests/test_next_autodeploy.py`
  checks that the workflow carries exactly that list and that it matches
  every tracked served file. The agent's `classify` is the rule that decides.
  The changed paths are those of the whole push (`old..new`), not the tip
  commit's: until 2026-09-29 a merge tip counted only its first-parent diff,
  which missed the `world/` changes of `65d76176dc` and skipped that deploy
  (see [desktop/README.md](../desktop/README.md), Traps).
- **Debounce and coalescing.** A deploy starts once `main` has not moved for
  60 s and no new run has arrived for 10 s, or 10 min after the first waiting
  run, whichever is first. It deploys the tip of `main` at that moment. When
  it ends, every waiting run whose commit the deployed commit contains gets
  the result ("covers <commit>"). A run that arrives during a deploy follows
  that deploy's log and then waits for the next one. The desktop cancels an
  older run of the same workflow when a newer one arrives; the deploy itself
  is not affected, and the newer run shows the whole log.

## Gates, in order

1. **On main and a fast-forward, compared by content.**
   The target must be on `main`. The agent reads the running revision of
   every core node from `https://<app host>/api/version`, pinned to each node's
   address. It locates each revision by content, because commit ids differ
   between the Next repository and this one (Next's `8190b54` is `7fa138f0d`
   here). A revision is located in this order:
   1. the deploy manifest (`<fleet config>/deploy-manifest.json`, written by
      `next_deploy.py` after every full-fleet restart);
   2. the commit itself, if this repository has it (the Next import keeps
      Next's objects under `refs/next-import/source`);
   3. its **served signature**: the sha256 of the tree ids of `app`, `world`,
      `www` and `deploy`. The import replays these trees byte for byte, so the
      newest commit on the target's history with the same signature is where
      production stands.

   A revision that is newer than the target, or not on its line of history,
   is refused ("refusing to roll production back") unless the run is a manual
   rollback. A revision that matches nothing is refused for auto-deploy and
   allowed, with a note, for a manual deploy. If every node already runs the
   target, or no release path changed since the deployed position, the run
   ends green without deploying.
2. **Tests** from a clean `git archive` of the target, unpacked under
   `~/projects/forkmesh/cache/node-deploy/`. They run in bwrap with no network,
   no home and only `/usr`, `/etc`, the archive, the test venv and the fleet's
   `data/bin` (rqlited, Caddy) visible, so test code never sees a fleet
   credential.
   - The must-pass set: `app/tests/host/test_host_e2e.py`, `deploy/tests`,
     `app/tests/host/test_host_shim_units.py`,
     `app/tests/host/test_host_request_guard.py`,
     `app/tests/host/test_apps_e2e.py`, `app/tests/test_timer_pings.py`,
     `app/tests/test_desktop_pings.py` and
     `app/tests/test_operator_alerts.py`, about a minute in total. Each path runs in its own
     pytest process: in one process `deploy/tests` leaves `tools/` and the host
     package on `sys.path`, so `import js` in the shim tests found
     `forkmesh_host/js.py` as a top-level module and failed, and the mixed
     rootdir mangled node ids (`app/test_next_autodeploy.py`). The workflow
     prefilter test lists files with `git ls-files` in a checkout and walks
     the tree in the gate's `git archive`, which has no `.git`.
   - *No new failures*: `app/tests/host` runs on the target and, once per
     deployed position (the result is cached in `state.json`), on the deployed
     commit. A failure that the deployed commit does not have fails the gate.
     This takes about 100 s per run. The full `app/tests` (about 190 failures
     that were already there) is not run.
   - The test venv is built once per `app/host/requirements.txt` digest.
3. **Build:** `next_deploy.py build --execute` in `~/projects/forkmesh/worktrees/node-deploy`,
   a fresh detached worktree at the target.
4. **Deploy:** `next_deploy.py upload --rev <rev> --execute`, then `migrate
   --execute`, then `restart --rev <rev> --execute`. Together with the build
   this is `next_deploy.py all --execute` without building twice. Migrations
   run before any restart. The rolling restart is gated per node on `/readyz`
   and the running revision, rolls a failing node back and stops. Caddy is
   2.11.4 and the IDS gets a signed expect window for each step, as for any
   `next_deploy` run.
5. **Verify:** on each node, pinned to its address:
   - `https://<app host>/readyz` answers 200;
   - `/api/version` reports the new revision;
   - `https://nN.<domain>/readyz` answers 200;
   - `/dashboard` and `/login` answer 200;
   - the World page carries `forkmesh-world-deploy` equal to the new revision.

   The agent retries for up to 2 minutes.

## Daemon and gateway changes: rolled automatically

Changes under `server/`, `deploy/core/` or `tools/mirror_gateway.py` reach a
node only through `next_fleet.py install nN --steps daemon` or `--steps
files,start`. Since 2026-09-29 the agent runs that itself: after the app
release (or a roll-only push) passes, it runs `python3 tools/next_fleet.py
roll --execute` from the worktree pinned to the deployed commit, still holding
the deploy lock, and Pings "Cores rolled after <rev>". Markdown files under
those prefixes never count as roll paths (a doc edit in `deploy/core/` used to
ask for a roll).

The operator asked for this; the risks the roll was kept manual for are
handled by `next_fleet.py roll` itself:

- **One core at a time, stop at the first failure.** Each core is drained,
  installed, checked (readyz, units, IDS, up to 90 s) and undrained before
  the next starts. A failure leaves only that core drained, so the other two
  keep serving, and the agent Pings and emails "Node roll failed after <rev>"
  with the error. `state.json` keeps `roll` (with `error`) until `ack-roll`.
- **DNS drain.** Operator undrain re-adds at once and never delays the health
  loop's own removals (see "Speed").
- **IDS.** Every roll step opens its signed expect window first.

To go back to report-only, create `<fleet config>/autodeploy/manual-roll`;
the agent then Pings "Node roll needed after <rev>" as before.

**The roll runs before the website step (2026-10-07).** The forkmesh.com
`wrangler deploy` used to run first, and its failure marked the run failed,
which skipped the roll and never recorded it as pending. The next push then
compared against the already-deployed app revision, so the gateway change was
lost: from 2026-10-06 the website build refused `www/public` edits made
without their `www/templates` source (hackathon commission table), and
the cores kept the old `tools/mirror_gateway.py`, whose missing `branch-trash`
route answered 404 and made `POST /api/git/branch-trash` return 503. Now the
roll depends only on the app release, and a failed roll after a failed website
step still keeps `roll` in `state.json`.

**Roll paths count from the last roll (2026-10-07).** That reorder did not
bring back the roll already lost: the next deploy (`3b7ef154a5b4`) compared
against `acebbc34eece`, which the cores already ran as the app, saw no gateway
change, and the status page kept showing the 503 (the cores still had the
2026-10-06 19:41 gateway). Now `Pipeline.roll_paths` adds the roll paths
changed since `lastRoll` (when it is an ancestor of the deployed base) and
any still-pending `roll` paths, so a skipped or lost roll is rolled by the
next deploy, even one with no app change. `lastRoll` now records the full
`commit`; `ack-roll` sets `lastRoll` to the pending roll's `since` rev, since
the operator rolled by hand. An operator roll without `ack-roll` leaves
`lastRoll` behind, which costs at most one extra roll.

### Runners (2026-10-04)

The cores were the only nodes the agent rolled, so a `server/` change never
reached the runner tier (`r1`): the runner kept its old Go daemon until an
operator ran `next_fleet.py install r1 --steps daemon` by hand
([../RUNNERS.md](../RUNNERS.md)). Now, after the cores roll and when the push
changed anything under `server/` (not only `deploy/core/` or
`tools/mirror_gateway.py`, which a runner does not use), the agent runs
`python3 tools/next_fleet.py roll-runners --idle-wait 0 --execute` from the
same pinned worktree and lock.

- **Never mid-job.** A runner's agent jobs run in `fmnext-job-*` scopes
  outside the daemon's unit, so a daemon restart would orphan a running
  session and leave its lease stuck. `roll-runners` counts the active
  `fmnext-job-*` units over SSH and skips a runner that has any, before it
  builds or uploads anything. The install script checks again right before it
  replaces the binary (`RUNNER_BUSY_SCRIPT`, exit 75), which closes the gap
  while the binary uploads; nothing on the runner changes then.
- **Busy is not a failure.** `roll-runners` exits 3 when a runner was busy.
  The deploy stays green ("runner roll waits for running agent jobs") and
  `state.json` keeps `runnerRoll` (commit, rev). While no deploy is queued the
  agent retries every 10 minutes (`RUNNER_ROLL_RETRY_SECONDS`) with the
  deploy lock taken without waiting, so a retry never delays a deploy. Once
  every runner is idle it rolls them and Pings "Runners rolled after <rev>". A
  later `server/` deploy replaces the pending commit.
- **Health.** After the restart `forkmesh-mirror-node` must be `active`
  within 90 s. A failure Pings and emails "Runner roll failed after <rev>"
  and stops the retries until `next_autodeploy.py ack-roll`.
- `manual-roll` and `pause`/`disarm` turn the runner roll off together with
  the core roll. Burst runners (`bN`) are not in `[[runners]]` and are not
  rolled; they are created from the snapshot and destroyed when idle
  ([../fleet/AUTOSCALING.md](../fleet/AUTOSCALING.md)).

By hand: `python3 tools/next_fleet.py roll-runners [rN ...] --execute` waits
up to 15 minutes (`--idle-wait`) for the runner's jobs to finish.

The agent runs its installed copy, so this takes effect on the source node
only after `python3 tools/next_autodeploy.py install` from a checkout that
has it.

The roll, by hand, one node at a time:

```sh
python3 tools/next_fleet.py roll --execute          # n1, n2, n3 in turn
python3 tools/next_fleet.py roll n2 --mail --execute  # one core, with mail
```

It runs drain, install, checks and undrain for each core (see "Speed"
below). Then `python3 tools/next_autodeploy.py ack-roll`.
Never pipe `next_dns.py` output into `head`: closing the pipe early can stop
the command half way.

## Safety

- **One deploy at a time.** `<fleet config>/deploy.lock` is an `flock`.
  Every `next_deploy.py --execute` takes it, whether started by hand or by the
  agent, and it holds the holder's pid, command and start time. A manual
  `next_deploy` that finds it held exits with the holder's name. The agent
  waits up to an hour for a manual deploy and says so in the run log. The
  agent hands the lock to its own `next_deploy` children as an inherited file
  descriptor (`FMNEXT_DEPLOY_LOCK_FD`). A child accepts that only when the
  descriptor refers to the lock file itself.
- **Kill switch.** Auto-deploy needs `<fleet config>/autodeploy/armed` and no
  `<fleet config>/autodeploy/paused`:
  - `python3 tools/next_autodeploy.py pause <reason>` writes `paused`, and
    `touch`ing that file by hand works as well;
  - `resume` removes `paused` and arms;
  - `disarm` removes `armed`.

  While paused or disarmed, each run ends green with "auto-deploy is paused
  (<reason>); nothing was deployed". Manual deploys still work.
- **Manual triggers** (they take the lock, run the same gates and log to
  `<fleet logs>/autodeploy/`):
  - `python3 tools/next_autodeploy.py deploy --commit <sha> [--skip-tests]`
    deploys a commit on `main`, even when the deployed revision is unknown.
    `--commit main` resolves to the tip of the source checkout's `main`;
  - **Deploy app to all nodes** on the desktop's Control → Deploy tab runs
    `next_autodeploy.py deploy --commit main` after a confirmation and streams
    its output into the pane below the button (the same lines also land in
    the run log above and on the Log page). The button stays disabled while a
    run is in progress. It has no Cancel: stopping the agent mid-rollout would
    leave its `next_deploy` child running without a log, so a bad run is
    stopped by the per-node gate and rollback, or by hand;
  - `python3 tools/next_autodeploy.py rollback --rev <rev> [--run-tests]`
    deploys an older revision (by commit or by manifest entry), skipping the
    tests unless asked. It is also the way past gate 1 after a history
    rewrite: on 2026-10-13 the nodes ran 739e5b8626d4 from the old lineage
    (rewritten as 5150cb83ef) and every deploy of daf9a217b17a was refused
    as "not an ancestor". The content-signature fallback cannot match either,
    because every rewritten commit except the tip has an empty tree.
    `rollback --rev <main tip> --run-tests` lets the old revision through,
    rolls the whole stack (no base, so every served path counts as changed)
    and still enforces the must-pass tests. With no base it rolls no daemons
    or runners, so check `classify()` of `git diff --name-only <old live>
    <tip>` for roll paths first (none that day). Once the nodes run a commit
    on the new `main`, auto-deploy fast-forwards as before;
  - *Run workflow* on "Deploy nodes" in the Actions tab deploys the tip of
    `main` through the agent;
  - `next_deploy.py rollback nN --execute` is still the fastest per-node
    relink.
- **Logs.** The whole log is in the Actions run and in
  `<fleet logs>/autodeploy/<time>-<rev>.log`. The agent's own lines are in
  `journalctl --user -u forkmesh-node-deploy`.
- **Alerts.** Every failed or refused deploy Pings the admins (dedupe key
  `deploy-failed:<rev>`). The second failure in a row also emails them, once
  per streak, through the mail budget, so it is noted in `#email-log`. A
  success resets the streak. The request is signed with this node's identity
  key as the admin account named in the desktop settings (`nodeName`).
- **Status.** `python3 tools/next_autodeploy.py status` prints the switch, the
  lock holder, the last deploy and failure, any pending roll and the live
  revision of every node.
- **A weak uplink (2026-10-03).** The operator box can be on one bar of LTE.
  That day two deploys in a row failed while the cores were healthy (all
  probes from r1 answered 200): `scp to 203.0.113.201 failed: lost
  connection` in the upload, then `n1 did not answer /api/version (HTTP 0)`
  at gate 1. From the box about half of all HTTPS requests timed out, to the
  cores and to any other site. That left the "No core can merge" fix
  (78fe5dc76d, [RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md)) unshipped
  for hours. Now:
  - every probe (`next_autodeploy.curl`, used by gate 1 and by verify) tries
    a dropped connection (HTTP 0 or a timeout) up to `CURL_ATTEMPTS` (5)
    times, 2 s apart. An HTTP answer, even a 5xx, is never retried;
  - `Ssh.put` (`tools/next_common.py`) resumes a single-file upload that
    scp dropped. It reads the size already on the node with `stat` and
    appends the rest with `ssh … cat >>`, up to `PUT_RESUME_ATTEMPTS` (6)
    times, re-opening the control master first. The release upload script
    still checks the sha256 before unpacking, so a bad join fails there.
- **An offline box (2026-10-04).** From 13:43 to 13:58 the operator box had
  no network at all ("Temporary failure in name resolution"; every probe from
  it failed, IPv4 and IPv6). The cores were healthy throughout. Three "Deploy
  nodes" runs (6f38aa0c9f, f1f0fa1357, a3bcf45bdd) failed gate 1 in 8 s with
  `n1 did not answer /api/version (HTTP 0)`, which blamed n1 for this box's
  outage. The DNS health loop in the same window counted three failures for
  every core and asked Cloudflare to drop n1 from `app`/`world`. Only the dead
  uplink stopped that change. Now:
  - gate 1 (`live_revs`) tells the two cases apart. When **no** core answers
    at all (HTTP 0 from every node), it says "this machine looks offline",
    re-probes every `OFFLINE_POLL_SECONDS` (30 s) for up to
    `OFFLINE_WAIT_SECONDS` (30 min) and continues once the network is back.
    The Actions step follows for 85 min, so the run passes rather than fails.
    If the box is still offline after 30 min, the cause names this machine and
    not a node. If some cores answer and one does not, gate 1 fails at once,
    as before. `status` never waits;
  - the DNS health loop treats a round in which every core fails as "this box
    is offline". It counts no failure and changes no record
    ([OPERATIONS.md](../OPERATIONS.md), DNS health failover).

## Speed (2026-09-26)

Target: a push is on the cores within a few seconds and live within about a
minute. Where the time went, and what changed:

| Step | Before | Now |
|---|---|---|
| Commit to cores (publisher) | up to 54 s: `MIN_PASS_SPACING_SECONDS` was 30 s, left over from the SSH fan-out, and the path and timer passes queue on one lock | the push itself (about 2 s); spacing is 2 s |
| Debounce | 60 s | 5 s quiet on `main`, 3 s settle (10 min cap) |
| Tests | about 3 min 20 s: suites one after another, then the host suite twice (target and deployed base) | about 35 s, and off the critical path (below) |
| Build + upload | 17 s + 90 s (one node after another; venv, pip and compile on each) | parallel upload; the venv is shared per requirements hash |
| Migrate | 33 s when a migration was pending (about 690 single DDL requests) | batched, 100 statements per request |
| Restart | about 9 s per node, one at a time | n1 first as the canary, then the other cores together |

- **Tests in parallel.** The gate unpacks the target once, stages
  `app/src_build` once (`tools/build_worker_python.py`, so the host
  conftest's `staged_src` never rebuilds it under a running test), then runs
  every file of `app/tests/host`, every file of `deploy/tests` and the
  other must-pass files in their own bwrap sandbox and pytest process, 12 at a
  time (`TEST_WORKERS`). Output is kept per file, and the log gets one
  summary line per file plus the last 40 lines of a failing must-pass file
  and of each file behind a new failure against the deployed commit.
  The compare set is the union of `app/tests/host` failures. The target's
  failures are stored as its baseline (`remember_baseline`), so the next
  deploy never re-runs the deployed commit.
- **Tests overlap build and upload.** `upload` only unpacks a release next
  to the others and never activates it. So the agent runs the test gate in a
  thread while it builds and uploads, and waits for the tests before
  `migrate`. A failing test leaves an unused release directory, nothing else.
- **World notice** now says "deploying" from `migrate` (or a whole-fleet
  `restart`) onward, not from `upload`. A test failure while uploading never
  shows visitors a deploy that is not happening.
- **Shared venv.** `upload_script` builds the venv once per
  `sha256(requirements.txt + wheel names + python -V)` under
  `/opt/forkmesh-next/app/venvs/<hash>` and links it as the release's
  `.venv` (the units run `current/.venv/bin/python`). It saves about 8 s per
  node and 75 MB per release.
- **Release pruning.** Only tarballs were pruned, so each core held about 60
  release directories (137 MB each; the disk was at 78%). Upload now keeps the
  5 newest releases plus `current`, `previous` and the new one.
- **Restart** (`restart(..., parallel=True)` from the CLI): the first core
  is gated alone as before. If it passes, the others activate in parallel,
  each with its own gate and relink-on-failure. The canary keeps serving.
- **SSH reuse** (`next_common.Ssh`): one detached `ControlMaster` per node
  (`ssh -f -N`, stdio closed, so `capture_output` never waits for it), 120 s
  `ControlPersist`, sockets in `$TMPDIR/fmnext-ssh-<uid>` (0700, owner
  checked, else no reuse). Each command after the first takes 0.05 s instead
  of 0.3 s. The migrate port-forward keeps its own connection, because a
  multiplexed forward outlives its client.
- **QUIC** was considered and would not help. Each SSH command is already
  0.05 s, the git push is one HTTPS request, and the time was spent in waits
  and serial work, not in round trips.

Second pass (measured on `7baa2ffa6d2b`, 22:00 EDT: commit to live in
1 min 43 s, with build 35 s and upload 27 s the slowest steps):

- **Worker Python compaction is cached.** `app/tools/build_worker_python.py`
  round-tripped all 7 MB of app Python through `ast.parse`, `unparse` and
  `dump` on every build (about 12 s), even with nothing changed.
  `_compact_cached` keeps each compacted file under
  `$FORKMESH_BUILD_CACHE` (default `$XDG_CACHE_HOME/forkmesh/worker-python`),
  keyed on the source bytes and the tool's own source. A warm run takes 0.4 s
  and gives byte-identical output. The build worktree is recreated for every
  deploy, which is why the cache lives outside it. The test gate mounts the
  cache **read-only** into its sandbox (`sandbox_command`,
  `FORKMESH_BUILD_CACHE`), so its own staging of `app/src_build` hits the
  cache as well (about 12 s saved). Test code can never write to it, and
  therefore can never change what ships. A miss is compacted inside the
  sandbox and not stored.
- **Wheels leave the release tarball.** The release carries `WHEELHOUSE`
  (the wheelhouse key) instead of 14 MB of wheels. Each core keeps
  `/opt/forkmesh-next/app/wheelhouses/<key>/` and gets the wheel tar
  (`wheelhouse_tarball`, uncompressed, since wheels already are) only when
  `wheelhouse_script` answers `need`. `upload_script` refuses a release whose
  wheelhouse is missing (exit 4) and builds the shared venv from it.
- **Tests run at `nice 10`** (`sandbox_command`). The 12 test processes had
  stretched the build that runs beside them from about 5 s to 20 s.
- **Debounce** is 3 s quiet on `main` with a 2 s settle.

- **Trap: fast-forwarding `main` to a commit a branch already pushed.**
  The desktop queues a push run once per commit. When `main` is
  fast-forwarded to a commit that was first pushed on a branch
  (`git merge --ff-only`), the only "Deploy nodes" run is the branch's, and
  it ends with "not main; only pushes to main deploy". Merge with `--no-ff`,
  as this repository does, or run
  `python3 tools/next_autodeploy.py deploy --commit <sha>` (same gates, 43 s
  for `79786f545ce6` on 2026-09-26).

Node rolls:

- **Operator undrain re-adds at once.** `next_dns.py undrain nN` probes the
  node three times, 2 s apart (IPv4 and IPv6). If every probe passes, it puts
  the node back in DNS immediately, without advancing the health loop's
  change timer, so an automatic removal of a failing node is never delayed.
  Before this, the loop waited 120 s healthy and at most one change per
  300 s. A three-core roll waited more than 10 minutes, and draining the
  third core was refused as "the last node in DNS".
- **`python3 tools/next_fleet.py roll [nN ...] [--mail] --execute`**: per
  core, Vultr instance check, drain, `install --steps daemon,files,start`
  (plus `next_mail.py install` with `--mail`), readyz + units + IDS check (up
  to 90 s), undrain, wait until it is back in DNS, then the next core. It
  stops at the first failure and leaves that core drained. n3 took 63 s.
- **Roll-only pushes ping.** A push that changes only roll paths (for example
  `tools/mirror_gateway.py`) used to end with "no served-stack change" and
  never sent the "Node roll needed" Ping. It now does.

## Files

| File | Written by |
| --- | --- |
| `<fleet config>/deploy.lock` | `next_deploy.py --execute`, the agent |
| `<fleet config>/deploy-manifest.json` | `next_deploy.py restart` / `all` after a full-fleet restart |
| `<fleet config>/autodeploy/{armed,paused,state.json}` | `next_autodeploy.py` |
| `<fleet logs>/autodeploy/*.log` | the agent and manual runs |
| `~/projects/forkmesh/cache/node-deploy/` | test archives (removed after each run) and the test venvs |
| `~/projects/forkmesh/worktrees/node-deploy` | the pinned build worktree (replaced on each deploy) |
| `~/projects/forkmesh/bin/node-deploy/` | the installed agent |

`<fleet config>` is `next_common.config_dir()`: `$FMNEXT_CONFIG_DIR`, or
`~/projects/forkmesh/config/fleet`. `<fleet logs>` is `next_common.logs_dir()`:
`$FMNEXT_LOGS_DIR`, or `~/projects/forkmesh/logs/fleet`. Both come from
`forkmesh_paths` (`fleet_config`, `fleet_logs`); the branch that added the
agent predates the move and carried its own copies of these helpers, which
were dropped when it was merged.

## World deploy notice

Visitors in the World see "ForkMesh is deploying now" while a release rolls
out, then "The new World build is ready" with a Refresh button (or "The
deployment needs attention" if it failed). The client
(`world/public/world/world.js`, `checkDeployStatus`) polls
`GET /api/world/deploy-status` every 60 s, every 2.5 s while deploying, and
reads the `world_deploy_status` row.

On Cloudflare `app/deploy.sh` (`signal_deploy_status`) wrote that row. The
Next tools did not, so the notice had been silent since the move to the core
nodes. `next_deploy.py` now writes it (`signal_world_deploy`), through
`/db/execute` on the first core with a healthy rqlite, with the body on SSH
stdin:

| Command (whole fleet, `--execute`) | Row |
| --- | --- |
| `upload` or `all`, before the upload | `deploying`, `started_at` now |
| `restart` or `all`, after every node activated | `ready`, `finished_at` now |
| `upload`, `migrate`, `restart` or `all` failing after that | `failed` |

The agent runs `upload`, `migrate` and `restart` as separate commands, so the
same rows come out of an auto-deploy and a manual `all`. A command naming
nodes (`restart n2`) never writes the row, and a failed write is logged and
never fails the deploy. The `ready` notice appears only to visitors whose page
saw `deploying` for that revision; everyone else picks up the new build
through the World fingerprint check (`checkForWorldUpdate`). While the row is
`deploying` (up to 30 min) and for 5 min after `ready`, the status monitor
suppresses alerts (`_status_deploy_semaphore_active`), as it did on
Cloudflare.

## The rqlite tunnel (2026-09-30)

`migrate`, `next_backup`, `next_vault` and `status_history_repair` reach
rqlite through `ssh -N -L 127.0.0.1:<free port>:127.0.0.1:4001` and wait up to
15 s for that local port to accept a connection. Until 2026-09-30 every
failure read "SSH tunnel to n1 did not open", and ssh's stderr was thrown away.

The deploys of `a11e86ffe279` (02:39) and `d2010f82b6dd` (02:42) failed that
way, but the tunnel was fine. ssh authenticated and listened within 0.3 s.
What had broken was this machine: every TCP connect to 127.0.0.1 timed out,
because the SYN was dropped before delivery. 127.0.0.2 and ::1 still
connected, which points at a local packet filter (OpenSnitch runs here), not
at the fleet.

`next_deploy.tunnel_error` now tells the two cases apart:

| ssh at the deadline | Message |
| --- | --- |
| still running | `SSH tunnel to n1 is up, but this machine does not accept connections to its own 127.0.0.1:<port> (a local firewall dropping loopback TCP?)` |
| exited | `SSH tunnel to n1 exited <code>: <last 500 bytes of ssh's stderr>` |

The first case needs a fix on the deploy machine, not the fleet. Check with
`python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); socket.create_connection(s.getsockname(), timeout=2)'`,
then look for a deny rule on 127.0.0.1 (OpenSnitch rules, `sudo nft list
ruleset`). Trying another core node would not help, so the tools do not.

### Loopback fallback

The same drop came back for the deploys of `c4d891b95bf5`, `d2010f82b6dd` and
`aa2218363ef1` (02:39 to 02:53). Only 127.0.0.1 was dropped: ::1 and
127.0.0.2 still connected. The tools no longer depend on that one address.
`next_deploy.open_rqlite_tunnel` is now the only way `migrate`,
`next_backup`, `next_vault` and `status_history_repair` open the tunnel. It:

1. probes 127.0.0.1, ::1 and 127.0.0.2 in that order: bind a listener and
   connect to it within 1 s (`loopback_accepts`). A healthy machine answers
   on the first one, so this costs nothing there;
2. has ssh listen on the first address that answered
   (`ssh -L [::1]:<port>:127.0.0.1:4001`; `Ssh.tunnel(bind=...)`);
3. waits up to 15 s for that port and returns the URL
   (`http://[::1]:<port>`) and a `close()` that stops ssh. A failure there
   stops ssh before it raises.

With 127.0.0.1 blocked on this desktop, a read-only `/status` through the
new tunnel reached n1 over ::1 (2026-09-30).

When every loopback address is dropped, the tunnel is not opened. The error
names the addresses it tried, the check command and the fix.

### Errors carry a fix

`tunnel_error` and `loopback_host` end their message with `Fix: …`:

| Cause | Fix in the message |
| --- | --- |
| a live tunnel this machine cannot reach, or no loopback address answers | allow loopback TCP for this user in OpenSnitch or nftables, then deploy again |
| ssh `Permission denied` | check that `~/projects/forkmesh/ssh/fleet/next_fleet_ed25519` is the key in the node's `authorized_keys` |
| `Host key verification failed` / `REMOTE HOST IDENTIFICATION HAS CHANGED` | remove the rebuilt node's line from `~/projects/forkmesh/config/fleet/known_hosts` |
| `timed out`, `No route to host` | check that the node is up (`next_fleet.py list`) and this machine has network |
| `Connection refused` | sshd on the node is down; check its Vultr console |
| `Could not resolve` | DNS on this machine |
| `cannot listen`, `Address already in use` | a local port race; deploy again |

An ssh exit with no known cause keeps the bare `exited <code>: <stderr>`.

`next_autodeploy` used to report only `next_deploy migrate exited 1`. That
line was the Actions verdict, the `Deploy of <rev> failed` Ping and the
e-mail. `Pipeline.tool` now keeps the last `next_deploy: …` line the tool
printed and adds it: `next_deploy migrate exited 1: SSH tunnel to n1 …
Fix: …`. The desktop's "Action failed" ping splits that line into `Cause:`
and `Fix:` ([desktop README](../desktop/README.md#recent-changes)).

## Install

```sh
python3 tools/next_autodeploy.py install    # copies the agent, enables and starts the unit
python3 tools/next_autodeploy.py status
```

Run `install` again after changing the agent. The unit starts disarmed.

On 2026-09-27 the installed agent was still the 2026-09-26 22:08 copy. It
lacked `72b1f6136` (retry a run whose workspace is not cloned yet), so from
12:15 it gave up on every "Deploy nodes" run ("not attached … workspace"),
and nothing deployed. The four runs before that had failed on the duplicate
codemagic vars that `72b1f6136` also fixed. Reinstalling from `main` at
13:34 fixed it. Compare `~/projects/forkmesh/bin/node-deploy/` with
`tools/` whenever `status` shows runs that never report a result.

A manual `deploy --commit 15be714b3` then stopped at the build gate:
`marketplace/catalog.json` on `main` had lost a comma in the tour merge
(`57cca3339`). Nothing was activated.

2026-09-28 review of the "Deploy nodes" failures since 09-27 12:19, by cause:

- **Never attached** (runs 1427, 18, 33, 102, 112, 132): the stale installed
  agent above. Fixed by `72b1f6136` and the 13:34 reinstall; none since.
- **Gate 2, `test_next_rekey`** (runs 164, 169, then 736, 751, 755, 765 on
  09-28): new tables whose blind-index columns `next_rekey` did not list.
  Fixed on `main` by `4ee96968d` and `9fc5eacf1` (merged in `53c02bf97`);
  every run since has deployed. Those runs' logs named the two tests and
  nothing else, because gate 2 printed the last 40 lines of a failing file
  only for must-pass files. Since 2026-09-28 (salvaged from
  `571-error-log-deploy-nodes`) it prints them for every file that holds a new
  failure, and still not for failures the deployed commit already had
  (`test_new_failures_against_the_deployed_base_show_their_output`).
- **Wheelhouse race** (run 480, 09-27 23:34): `next_deploy upload` uploads to
  n1-n3 on three threads, and when the requirements change all three need the
  new wheelhouse and call `wheelhouse_tarball` at once. They shared one temp
  name (`.<key>.tar.<pid>`), so the first `os.replace` took the file from
  under the others: "No such file or directory: '.../.12baeef54c13003e.tar.3882803'
  -> '.../12baeef54c13003e.tar'". The tarball is now built under a lock with a
  per-thread temp name
  (`test_parallel_node_uploads_build_the_wheelhouse_tarball_once`).
- **Re-queued run followed the old workspace** (run 603, 01:08): after a
  desktop restart the run restarts in a new sandbox,
  `forkmesh-run-603-<new ms>`, while the old one is left for the sweeper.
  `running_runs` returned whichever directory it met first, and the agent
  attached the old workspace, where its own `node-deploy.log` already existed
  ("not attached ([Errno 17] File exists)"); the new attempt would have waited
  its 3 minutes and failed. `running_runs` now keeps the newest sandbox per
  run id and `Agent.tick` re-attaches a run whose workspace changed
  (`test_a_run_requeued_after_a_desktop_restart_follows_its_new_workspace`).
- **Killed by the restart** (run 620): see the desktop Actions trap "A stop
  signal reaches the Actions runner's children"
  ([../desktop/README.md](../desktop/README.md#traps)).

Like any agent change these take effect after `install`.

## The website Worker (2026-09-27)

forkmesh.com and www.forkmesh.com are served by the Cloudflare Worker
`forkmesh-www`, not by the cores, so a node deploy never reached them and the
website was deployed by hand. After every deploy check that ends well, the
agent now also compares the website:

- **Fingerprint.** `website_fingerprint` is the SHA-256 of `www-tree:<tree id
  of www/ at the target>`. The Worker sends it back as
  `x-forkmesh-deploy-fingerprint`; `live_website_fingerprint` reads it from
  `https://forkmesh.com/`. Equal means nothing to do, including after a
  deploy with no served-stack change, so a website that fell behind catches up
  on the next run.
- **Deploy.** Otherwise it runs `npx wrangler deploy --config
  wrangler.production.toml --var DEPLOY_FINGERPRINT:<fingerprint>` in `www/` of
  the pinned worktree, with Node 22 from `~/.nvm/versions/node/v22*`
  (`node22_bin`) and `CLOUDFLARE_API_TOKEN` / `CLOUDFLARE_ACCOUNT_ID` from
  `config/fleet/cloudflare.env`. The build command in the config stages
  `www/dist`. It then waits up to a minute for the new header.
- **Failure** marks the run failed ("website deploy failed: …") and Pings the
  admins like any failed deploy; the node deploy result is kept in the message.
- **Trigger.** `www/worker.js` and `www/wrangler.production.toml` are release
  files now (`www/public/` already was), so the workflow's path filter queues a
  run for them. `test_workflow_runs_only_here_and_never_deploys_workers` still
  forbids a `wrangler` command in the workflow file itself; the agent runs it.
- **Install.** The agent runs from the pinned copy in
  `~/projects/forkmesh/bin/node-deploy/`; `python3 tools/next_autodeploy.py
  install` refreshes it.
- 2026-09-28: `next_fleet.py roll` and `install` on a core take the deploy lock
  (`<fleet config>/deploy.lock`, waiting up to 30 minutes), like
  `next_deploy`. Before this, a roll ran alongside an auto-deploy: at 23:12,
  while n1 was drained for a roll, the deploy of `9fd809b0f4` restarted the app
  on every core, so n3 answered public 502s and n1 was handed back to the
  health loop instead of undrained
  (`test_roll_waits_for_the_deploy_lock_and_releases_it`). The auto-deploy
  agent already waits up to an hour for a lock someone else holds.

## Installed files (2026-09-30)

`install` copies `render.py` and `caddy_routes.py` from `deploy/core/` next to
the agent. Since node addresses moved out of `deploy/fleet.toml`
([README.md](README.md), "Node addresses"), `fleet_hosts` resolves them through
`render.fill_public_addresses`, and the installed agent cannot import from the
checkout. Without them every deploy would stop at the address lookup. Re-run
`install` after changing either file.
