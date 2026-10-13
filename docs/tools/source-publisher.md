# Headless source publisher

`tools/source_publisher.py` publishes `forkmesh/forkmesh` from the source
checkout on the deploy box without the desktop app. Commit or merge in
`~/projects/forkmesh/src` and the change reaches the fleet within about a
minute, whether `forkmesh-source.service` (the flagship desktop) is running or
not. Tests: `python3 -m pytest -q tools/test_source_publisher.py`.

On 2026-10-11 the local firewall dropped connections to `127.0.0.1`, while
`::1` and `127.0.0.2` worked. The signing proxy used only `127.0.0.1`, so
publisher passes failed before sending any Git request and the website's
repository stayed behind. The publisher now probes all three loopback
addresses in order and uses the first reachable listener, with IPv6 URL
brackets and proxy bypass for local Git traffic. If every address fails,
the pass fails without sending a push. Reinstall the publisher after updating
its source so the path and timer services use the repaired copy.

Failed passes also log an explicit error and send the existing signed operator
Ping with the cause, a fix, the retry command and the journal/status commands.
The same failure is suppressed for one hour after a successful notification;
recovery clears the suppression so a new outage alerts immediately. A failed
notification stays visible in the journal and is retried on the next pass.

## Why it exists

On 2026-09-24 the flagship stopped twice (03:44 and 10:51 EDT). Each time,
the mirrors stayed at `0276657c8` while local `main` moved on. Only the
desktop moved the bare mirror forward, so nothing new reached the fleet.

What publishing depends on, measured on 2026-09-24:

- The desktop has **not** signed repository-state pins since `f22c0eb16`
  (2026-08-12). No `catalogSig`, `stateSig` or `forkmesh-repostate-v1` code
  remains in `desktop/src`.
- The app admits a new `forkmesh/forkmesh` state when at least two serving
  nodes agree on it: `_https_mirror_flagship_quorum`, with
  `PEER_QUORUM_MIN_NODES = 2` in `app/src/mirrors.py`. States come from the
  nodes' own signed catalog records and proofs, and from owner-merge
  attestations.
- The flagship's only part in publishing was
  `git fetch <checkout> refs/heads/*:refs/heads/* refs/tags/*:refs/tags/*`
  into `data/mirrors/forkmesh-forkmesh.git`, about twice a second
  (`MainWindowRepoSync.cpp`). The fetch uses no `+`, so it is fast-forward
  only. `forkmesh-mirror-fanout.timer` then pushed that bare mirror to the
  fleet.

The publisher makes that same fetch itself, then runs the fan-out. The
signing and trust model are unchanged: a state reaches clones only when the
fleet agrees on it. When the desktop runs too, both processes write the same
refs from the same checkout, fast-forward only, so they converge.

## Options considered

| Option | Verdict |
| --- | --- |
| (a) Headless publisher on the deploy box | **Chosen.** No new key or write path. Idempotent next to the desktop. |
| (b) Go daemon in a source role | Rejected. It needs a new node identity and registration, and it would add a signer where none is needed. |
| (c) Cores as source of truth (`git push https://app.forkmesh.com/forkmesh/forkmesh.git`) | Only as a transport, behind the app's signed push authorization (below). Making the app the write authority for the flagship repository is a trust-model change for the fleet migration to decide. |

## Flow

```
commit / merge / branch update in ~/projects/forkmesh/src
  └─ forkmesh-source-publish.path  (refs/heads, refs/tags, packed-refs, data/actions/spool)
       └─ forkmesh-source-publish.service: source_publisher.py publish --if-changed
forkmesh-mirror-fanout.timer (every 2 min)
  └─ forkmesh-mirror-fanout.service:     source_publisher.py publish
forkmesh-source-cores.timer (every 20 s)
  └─ forkmesh-source-cores.service:      source_publisher.py watch-cores
publish, holding state/publisher/publish.lock:
  1. sync: fast-forward-only fetch checkout → data/mirrors/forkmesh-forkmesh.git
  2. bin/forkmesh-mirror-fanout.sh       (SSH fan-out to config/fanout/hosts; unchanged)
  3. each line of config/fanout/gateways: signed HTTPS push through the app
  4. state/publisher/status.json; repeat (up to 3 passes) if the checkout moved meanwhile
then, with publish.lock released:
  5. systemctl --user start --no-block forkmesh-mirror-branch-deletes.service
       └─ source_publisher.py delete-branches, holding state/publisher/branch-deletes.lock
          (run inline when the unit cannot start, for example before `install`)
```

- Branch deletes run in their own unit (2026-10-06). From 02:45 that day
  each pass also sent up to 20 signed branch deletes to the cores, one at a
  time, each polled until it finished (~12 s apiece), after a cleanup had
  tombstoned ~400 branches. A pass took 5 to 13 minutes instead of 1 s. Both
  publish units are `Type=oneshot`, so while one ran, a ref change that
  started the same unit merged into the running start job and was lost, and
  the other unit's pass waited on `publish.lock`; `--if-changed` passes failed
  with "another publish pass holds publish.lock". A merge into `main`
  reached the mirror and the cores minutes late. The publish pass now ends after
  the gateway push and starts `forkmesh-mirror-branch-deletes.service`
  without waiting. Starting it while it runs is a no-op, and its own lock
  skips an overlapping inline run. The unit name keeps the `forkmesh-mirror`
  prefix that `ProcessReaper` leaves alone when the desktop quits. A delete
  job on a core verifies `refs/heads/main` in its ref transaction, which
  holds `main.lock` for a moment. A push of `main` landing then is refused
  with `[remote rejected] (failed to update ref)` / `cannot lock ref`, so
  `push_gateway` retries such a protected-ref refusal once after 10 s
  (`ref_lock_race`), as it does a hung-up connection.

- `--if-changed` skips a pass when the bare mirror's heads+tags digest equals
  the last good pass. The timer pass always runs, because the fan-out also
  prunes and detects divergence.
- Two passes start at least 2 s apart (`MIN_PASS_SPACING_SECONDS`). This
  was 30 s while the SSH fan-out had targets, to keep bursts of agent commits
  from putting the fleet under constant SSH load. The fan-out is retired and
  each pass is now one signed HTTPS push, but the timer pass still counted as
  a previous pass: a push that landed just after it waited up to 30 s, and
  about 55 s when a second pass ran (15:11:29 → 15:12:23 on 2026-09-26). The
  2 s spacing still merges bursts, because passes queue on the publish lock
  and `--if-changed` skips a digest that was already published. The installed
  copy in `~/projects/forkmesh/bin/publisher/` changes only after
  `source_publisher.py install` from `main`.
- The checkout's `post-commit`/`post-merge` hooks already write
  `data/actions/spool/*.commit` for the desktop. The path unit also watches
  that folder, so a commit on any branch, in any worktree, triggers a pass.
- A non-fast-forward in the checkout (a rewound branch) is reported and left
  alone. The bare mirror is never rewound.

## Gateway transport (the Next cores)

The cores run the IDS (`docs/IDS.md` in the Next repository). A root SSH
push with `runuser -u forkmesh-node -- git-receive-pack`, as the mirror
fan-out does, would start `forkmesh-node` processes outside
`forkmesh-mirror-node.service`. That triggers `service_user_outside_unit`
and `service_user_shell` alerts on every push, so the cores must be fed
through the gateway's normal receive path.

`config/fanout/gateways` holds one target per line:
`name https://app.forkmesh.com/forkmesh/forkmesh.git [key.pem]`. The default
key is the desktop identity `data/identity/ed25519.pem`.

For each target, the publisher:

1. starts a token-protected loopback proxy, trying `127.0.0.1`, `::1` and
   `127.0.0.2` until a local connection works;
2. runs `git push --porcelain <proxy> refs/heads/*:refs/heads/* refs/tags/*:refs/tags/*`
   (never forced, never deleting; deletions go through
   `POST /api/git/branch-deletes` after the pass, see
   [CORE_PUSH.md](../CORE_PUSH.md#branches-the-source-deleted-2026-10-06));
3. has the proxy sign each request as
   `forkmesh-git-push-v1\n{owner}\n{repo}\n{METHOD}\n{ts}\n{sha256(body)}`
   and send it with `X-ForkMesh-Pubkey/-Ts/-Sig/-Body-Sha256`. This is the
   same scheme the mobile clients use (`conformance/protocol/v1`, vector
   `git-push-v1-advertisement`).

The app verifies the signature (`_git_push_authorize`), signs a router
capability, and streams the push to a core gateway. A rejected `main` or
`forkmesh/pulls` is reported as `DIVERGED`.

Organization owners push to the cores too (`docs/CORE_PUSH.md` in the Next
repository), so the publisher is one pusher among several. What it does with
each kind of rejection:

| Rejection | Publisher |
| --- | --- |
| a tag `already exists` (the checkout moved it) | asks for an owner tag move, `POST /api/git/tag-moves` signed `forkmesh-tag-move-v1\n{owner}\n{repo}\n{ref}\n{target}\n{ts}` with the same key, then pushes `+refs/tags/<tag>` once. The app audits both steps (`git.tag_move_authorize`, `git.tag_move`). At most 8 tags per pass. |
| `main` (`fetch first`): the cores hold an owner's push | fetches the cores' `main` into the checkout as `refs/remotes/forkmesh-cores/main`, then fast-forwards `main` (checkout behind) or merges it (`Merge pushes to main from the cores`) when both moved and `git merge-tree` is clean. The next pass pushes the result. On a conflict, or with a merge already in progress, nothing is changed and the log says `DIVERGED … merge <sha> by hand`. |
| `main`, and no worktree has `main` checked out | the same, without a worktree: a fast-forward is an `update-ref` from the old value, a merge is `git merge-tree --write-tree` then `git commit-tree` with both parents. The checkout's `post-merge` hook runs afterwards, so the desktop syncs and queues Actions. A conflict changes nothing. Before 2026-10-03 this answered `main is not checked out` and gave up |
| another branch, non-fast-forward | left at the cores' value and listed as `stale`; the pass stays `ok`. Branches on the cores are never forced. A branch the source deleted is removed by a signed delete after the pass ([CORE_PUSH.md](../CORE_PUSH.md#branches-the-source-deleted-2026-10-06)). |

The cores' accepted `main` is the one order: the publisher only ever
fast-forwards it, like any owner.

**A push that never reached a core (2026-09-28).** A fast deploy restarts the
App, and for a few seconds Caddy answers `502 dial tcp 127.0.0.1:8080:
connection refused`. The push then ended with `fatal: the remote end hung up
unexpectedly` or `RPC failed; HTTP 502`, and no ref was reported. The pass was
logged `FAILED`, and the desktop filed it under **Error**. Now, when a push
reports no ref at all and git's stderr names a transport failure (RPC failed,
hung up, HTTP 502/503/504, refused, could not connect or resolve, timed out),
`push_gateway` waits 10 s and pushes once more. If that also fails, the pass
still fails, but the log says `unreachable after a retry`, not `FAILED`, and
the next timer pass tries again because the stored status is not `ok`. A push
with rejected refs is never retried.

The proxy signs only the one configured owner/repo, only `info/refs?service=git-receive-pack`
and `git-receive-pack`, and only under its random path token. It exists only
for the duration of one push.

**Server-side prerequisites (met on 2026-09-24).** The mirror retirement
(`docs/MIRROR_RETIREMENT.md` in the Next repository) made the cores the push
target, and `config/fanout/gateways` holds
`cores https://app.forkmesh.com/forkmesh/forkmesh.git`:

- The org alias `forkmesh/forkmesh` is backed by `fmnext-n1`. The desktop
  identity key belongs to `jett`, an organization owner, so the push is
  authorized through the organization write path, and a signature over the
  alias owner (`forkmesh`) is accepted for that alias.
- Only organization owners may push to a core-held copy
  (`FLAGSHIP_PUSH_PUBKEYS` was removed).
- The cores' `forkmesh/forkmesh` gateway entry advertises `git-receive-pack`
  (`[app] flagship_receive_pack` in the Next `deploy/fleet.toml`). The
  gateway rule stays fast-forward only with no deletes; a tag moves only
  through the owner tag move above.
- After an accepted push the app records the core's signed refs digest in the
  pin history and wakes the group; the other cores fetch it from that core.

The SSH fan-out has no targets left (`config/fanout/hosts` is a comment, and
`bin/forkmesh-mirror-fanout.sh` was retired), so step 2 is skipped. Release
blobs go to the cores listed in `config/fanout/release-hosts`, over root SSH
with the fleet key (`tools/forkmesh-release-host.py`).

## Merges made on the cores (2026-10-03)

Asked from a runner session: "When we merge from a runner please make sure
appropriate actions also run to deploy changes to the cores."

A runner session's **Merge & clean up** (and a web pull-request merge) is
made by a core's merge executor, so the merge commit is on the cores' `main`
first ([../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md)). The cores
never run Actions (`role: core`, [../RUNNERS.md](../RUNNERS.md)). "Deploy
nodes" runs only on the source desktop, and the node deploy agent deploys
only commits on the checkout's `main`
([../deploy/auto-deploy.md](../deploy/auto-deploy.md)). So a merge made on the
cores deploys only after the publisher takes it into the checkout.

Before, the publisher took it only when a push of `main` was rejected, which
happened on the next 2-minute timer pass at best. If `main` was not checked out
in any worktree, it gave up. On 2026-10-03 the runner merge `d903ba3d4`
(21:53 UTC) was still not in production 11 minutes later. Production ran
`5bb252c092d5`, a source commit the cores did not have.

Now:

- `forkmesh-source-cores.timer` runs `source_publisher.py watch-cores` every
  20 s. For each gateway it reads the cores' `main` with `git ls-remote`
  (one public read, no key). When the checkout's `main` does not contain it,
  it runs a full publish pass at once. That pass is rejected on `main`, absorbs
  the cores' `main` (table above) and pushes the result. Moving the checkout's
  `main` fires the `post-merge` hook. The desktop then syncs the served mirror
  and queues "Deploy nodes" for the whole range, and the node deploy agent
  deploys it as usual.
- If a pass cannot take a cores' `main` (a conflict, a merge in progress, or
  local edits that overlap), the watch records that commit in
  `state/publisher/cores-watch.json` and does not start another pass for it.
  The 2-minute timer pass keeps trying and logs `DIVERGED … merge <sha> by
  hand`. A newer cores' `main` is tried again at once.
- An unreadable cores' `main` (network down, private repository) does nothing.
  The timer pass still covers it.

Tests: `tools/test_source_publisher.py` (`test_watch_*`,
`test_absorb_*_when_no_worktree_has_it`,
`test_absorb_without_a_worktree_refuses_a_conflict`, `test_cores_main_*`).

## Desktop and cores in step (2026-10-04)

Asked: "there are 42 changes on desktop not going out ... changes on cores
should sync among themselves and the desktop should sync with them if there
is any change on a core; please make this all automatic."

What happened:

- A crash on 2026-10-02 22:31 left a zero-byte loose object
  (`objects/2f/32199c…`, a blob) in the served bare mirror
  `data/mirrors/forkmesh-forkmesh.git`; the same crash left empty objects in
  the checkout's `.git`. From the evening of 2026-10-03, every checkout → bare fetch
  failed with `object file … is empty` / `pack has 1 unresolved delta`. The
  pass still reported `ok`, because `ok` counted only the fan-out and the
  gateways. The gateway pushed the stale bare `main` (`f6247dbae`); the cores
  were ahead of it, absorb found that the checkout already had their `main`,
  and the log said `main was already past this pass … nothing to do`. The
  checkout was 45 commits ahead of the bare and 40 ahead of the cores, and
  nothing reported a failure.
- Separately, n1 held a runner merge (`a65a60890`, agent/15) that n2 and n3
  lacked. The edge served the n2/n3 state that two cores agreed on. A core
  reads its peers only when the edge fails (`docs/deploy/README.md`,
  2026-09-28), so n2 and n3 never fetched n1's merge.

Repair on 2026-10-04 (no deletes): the empty file was moved to
`~/projects/forkmesh/builds/core-1363/corrupt-objects-2026-10-04`, and the
blob was rewritten from the checkout (`git cat-file blob … | git hash-object
-w --stdin`). `git fsck --connectivity-only` on the bare was then clean. The
next pass pushed the checkout's `main` and absorbed n1's merge. At 09:53 EDT
the desktop and n1-n3 all held `main` `0de7ef1cd`.

What is now automatic:

- **A failed sync fails the pass.** `ok` also requires the checkout → bare
  fetch to succeed; a rejected non-fast-forward is still logged and left as
  it is. The journal says `sync FAILED: …`, and `status.json` keeps
  `ok: false`, so the next `--if-changed` pass runs again instead of
  answering `up to date`.
- **Empty objects heal themselves.** When the fetch fails with `object file …
  is empty`, `sync_healing` moves every zero-byte loose object of the bare to
  `state/publisher/quarantine/<UTC date>/`, rewrites each one from the
  checkout (`restore_object`, checked against its id), and fetches again.
  Nothing is deleted: a zero-byte file holds no data, and it is kept in the
  quarantine folder.
- **Every core is watched, not only the edge.** `watch-cores` (every 20 s)
  reads `main` from each core's own host,
  `https://nN.<domain>/forkmesh/forkmesh.git`, with the core list taken from
  the checkout's `deploy/fleet.toml` (`core_read_urls`; `role = "core"`
  nodes only). If one core's `main` is not in the checkout, the publisher
  takes it from that core (`absorb_cores` with the core's URL: a
  fast-forward, or a `Merge pushes to main from the cores` merge) and runs a
  publish pass, which pushes the union to the cores. A conflict is logged as
  `core nN DIVERGED … merge it into main by hand`. The commit is recorded in
  `cores-watch.json` and not retried until that core's `main` moves again.
  After the per-core checks, the edge check runs as before.
- **The cores merge each other's `main`** without the desktop
  (`docs/deploy/README.md`, 2026-10-04).

Tests: `tools/test_source_publisher.py` (`test_heal_*`,
`test_a_sync_blocked_by_an_empty_object_heals_and_retries`,
`test_a_failed_sync_fails_the_pass`, `test_core_read_urls_*`,
`test_watch_takes_a_main_only_one_core_holds`,
`test_watch_records_a_core_main_that_conflicts`).

## Flagship guard

`forkmesh-source-guard.timer` runs `source_publisher.py guard` every minute.
If `forkmesh-source.service` is `inactive` or `failed`, the guard starts it,
unless a hold is set.

```
python3 ~/projects/forkmesh/bin/publisher/source_publisher.py hold 2h --reason "rebuilding by hand"
systemctl --user stop forkmesh-source.service
...
python3 ~/projects/forkmesh/bin/publisher/source_publisher.py unhold
```

A window close still stops the unit (`stopSupervisingUserServiceAfterWindowClose`).
Before 2026-09-27 the guard then started it again within a minute, so a closed
desktop kept coming back. Now the close also writes a hold with
`"setBy": "desktop"` and an `until` 100 years out (`writeDesktopCloseHold`), so
a closed desktop stays closed. The next desktop start under
`forkmesh-source.service` (a manual `systemctl --user start`, or login through
`default.target`) removes that hold (`releaseDesktopCloseHold`), and crash
recovery works again. It never removes a hold set by hand, which has no `setBy`.

Settings > General > Startup > **Reopen ForkMesh after I close it**
(`startup/reopenAfterClose`) is off by default. Turn it on to get the old
behaviour: a close writes no hold, and the guard reopens the desktop. Rebuild &
restart never writes a hold, and a desktop running under any other unit never
writes one either.

### What stopped the flagship on 2026-09-24

- **10:51:49.** The in-app **Rebuild & restart** began at 10:45:42 on
  HEAD `0276657c8`. The rebuild finished and installed the binary. The app
  started the new binary as a child (pid 280312, v1.2.4) and called
  `quit()`. `closeEvent` then ran `systemctl --user stop forkmesh-source.service`.
  Systemd sent SIGTERM from pid 3051 to both processes (both were in the
  unit's cgroup), and `Restart=always` does not undo an explicit stop.
  Evidence: `data/network_log.txt` lines "quick rebuild & restart started"
  and "=== Restarting now (rebuild & restart) ===", the journal, and
  `desktop/build/forkmesh-mirror-node` with mtime 10:51:49.39.
  **Fixed in the desktop:** under a systemd user service, the relaunch now
  asks systemd to `restart` the unit instead of starting a child
  (`requestSupervisingUserServiceRestart`, `systemdUserServiceCloseArguments`).
- **03:44:20.** The same `closeEvent` path ran ("Session ended." with no
  restart marker and no agent `systemctl` call in any transcript). This was
  most likely the window being closed or quit from the tray. It stayed down
  until a manual start at 04:01:43. The guard now covers this case.

## Operations

- Install or update, from a checkout on `main`:
  `python3 tools/source_publisher.py install`. This copies the tool and
  `forkmesh_paths.py` into `~/projects/forkmesh/bin/publisher/` and writes the
  units into `~/projects/forkmesh/config/systemd/`, linked from
  `~/.config/systemd/user/`. It enables `forkmesh-mirror-fanout.timer`,
  `forkmesh-source-publish.path`, `forkmesh-source-guard.timer` and
  `forkmesh-source-cores.timer`.
- State: `source_publisher.py status` prints checkout vs bare `main`, the last
  pass, gateway results, the fan-out's `state/fanout/status.json`, the flagship
  state and any hold.
- Journal: `journalctl --user -u forkmesh-source-publish -u forkmesh-mirror-fanout -u forkmesh-source-guard -u forkmesh-source-cores`.
- Paths come from `tools/forkmesh_paths.py`: `bare_mirror`,
  `fanout_gateways`, `publisher_bin`, `publisher_state`, `action_spool`,
  `systemd_units`, `fleet_config`, `fleet_ssh`, `fleet_ssh_key`, `fleet_data`
  and `fleet_logs`. Each is `~/projects/forkmesh/<root>/fleet` once that exists;
  before the 2026-09-24 move they fell back to `~/projects/forkmesh-next/<root>`
  (that folder is now `~/projects/forkmesh-backup`, so the fallback is inert).
  `tools/next_common.py` derives `config_dir`, `ssh_key_path`, `data_dir` and
  `logs_dir` from them; the `FMNEXT_*` variables still override each one.

## Traps

- **A local firewall on 127.0.0.1 (2026-09-30).** From 02:35 this machine
  dropped every TCP SYN to its own `127.0.0.1`, both inside and outside the
  agent sandboxes (OpenSnitch runs here). The gateway push goes through the
  signing proxy on `127.0.0.1:<random>`, so git hung for 133 s, retried,
  hung again, and held `publish.lock` for 14 minutes a pass. The cores'
  `main` stayed at `7fe3c12aa495` while the checkout moved on, and the Mac
  iOS builds of the newer commits failed to fetch them (`docs/native/MAC_BUILD.md`).
  The log said only `unreachable after a retry ... Failed to connect to
  127.0.0.1 port N`, which reads like the fleet was down. `push_gateway` now
  connects to its own proxy first (`loopback_accepts`, 3 s). If that fails,
  or git reports `Failed to connect to 127.0.0.1 port <proxy port>`, git is
  not retried. The gateway result gets `loopbackBlocked: true` and an error
  that names the local firewall, and the journal line reads `gateway cores:
  loopback blocked`. The fix is on this machine: allow 127.0.0.1 again
  (OpenSnitch rules, `sudo nft list ruleset`). Check with
  `python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); socket.create_connection(s.getsockname(), timeout=2)'`.
  Since 2026-10-11 the publisher falls back to `::1` or `127.0.0.2`; only
  when every loopback address fails does publishing stop and send a failure
  Ping with these repair instructions.
- `main` in the served bare mirror still belongs to the desktop when it runs.
  The publisher only fast-forwards it from the checkout, never from anywhere
  else. Land work by committing or merging in the checkout.
- If the cores hold commits the checkout lacks, the publisher absorbs them
  into the checkout (above). Only a conflicting merge needs a hand merge:
  `git merge refs/remotes/forkmesh-cores/main` in the checkout, as in the
  divergence repair in `docs/server/fleet-operations.md`.
- `systemctl --user show -p A -p B --value` prints values in systemd's
  property order, not argument order. The guard parses `key=value` lines.
- A pass can be outrun. The bare mirror a pass pushes from may still lag a
  newer pass that already reached the cores, so the cores reject `main` as
  non-fast-forward even though the checkout already contains what they hold
  (`absorb_cores` answers "already contained"). Until 2026-09-28 that was
  logged as `gateway cores: FAILED ... rejected` plus `DIVERGED ... already
  contained` and raised an error Ping on every race (18 in one evening while
  agents merged in quick succession). Such a pass now counts as ok and logs
  "main was already past this pass"; only a real divergence (the cores hold
  commits the checkout lacks) still fails and Pings
  (`test_a_pass_outrun_by_a_newer_publish_is_not_a_failure`).
  The same race hits the pull ledger (`refs/heads/forkmesh/pulls`) when agents
  merge in quick succession and the desktop writes "pull #N: open/merged"
  records between passes (5 FAILED Pings at 21:20). For any rejected protected
  ref other than main, `cores_ref_contained` fetches the cores' value and checks
  it against the checkout's ref. If the checkout already contains it, the pass
  was outrun and counts as ok; otherwise it is still reported as a divergence.
  Outrun also needs the cores to hold what this pass pushed (`cores_hold`:
  the bare mirror's ref is an ancestor of the cores' value). On 2026-10-06 the
  cores refused `main` 1b33a824db while they were still at its parent
  987f32bdef. The checkout contained 987f32bdef, so the pass logged "main was
  already past this pass" and counted as ok, and the merge stayed off the cores
  with nothing flagged. A refused ref the cores are behind on now fails the pass
  and logs `gateway cores: the cores are behind and refused main: <reason>`
  (`test_a_refused_push_to_cores_that_are_behind_is_not_outrun`).
