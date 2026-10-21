# Headless source publisher

`tools/source_publisher.py` publishes `forkmesh/forkmesh` from the source
checkout on the publishing machine without the desktop app. Commit or merge in
`~/projects/forkmesh/src` and the change reaches the fleet within about a
minute, whether `forkmesh-source.service` (the flagship desktop) is running or
not. Tests: `python3 -m pytest -q tools/test_source_publisher.py`.

## Model

- The app admits a new `forkmesh/forkmesh` state when at least two serving
  nodes agree on it (`_https_mirror_flagship_quorum`,
  `PEER_QUORUM_MIN_NODES = 2` in `app/src/mirrors.py`). States come from the
  nodes' own signed catalog records and from owner-merge attestations; the
  desktop signs no repository-state pins.
- The desktop's part in publishing is a fast-forward-only
  `git fetch <checkout> refs/heads/*:refs/heads/* refs/tags/*:refs/tags/*`
  into `data/mirrors/forkmesh-forkmesh.git` (`MainWindowRepoSync.cpp`). The
  publisher makes the same fetch itself and pushes the result, so no new key
  or write path exists. When the desktop runs too, both write the same refs
  fast-forward only and converge.

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
  2. SSH fan-out to config/fanout/hosts (no targets on the served stack)
  3. each line of config/fanout/gateways: signed HTTPS push through the app
  3b. each line of config/fanout/github: main and tags to the GitHub mirror
  4. state/publisher/status.json; repeat (up to 3 passes) if the checkout moved meanwhile
then, with publish.lock released:
  5. systemctl --user start --no-block forkmesh-mirror-branch-deletes.service
       └─ source_publisher.py delete-branches, holding state/publisher/branch-deletes.lock
```

- Branch deletes run in their own unit so slow signed deletes never hold up
  a publish pass. A core's delete job briefly locks `main`, so a protected-ref
  refusal (`cannot lock ref`) is retried once after 10 s (`ref_lock_race`).
- `--if-changed` skips a pass when the bare mirror's heads+tags digest equals
  the last good pass; the timer pass always runs. Passes start at least 2 s
  apart (`MIN_PASS_SPACING_SECONDS`).
- The checkout's `post-commit`/`post-merge` hooks write
  `data/actions/spool/*.commit`; the path unit watches that folder, so a
  commit on any branch or worktree triggers a pass.
- A non-fast-forward in the checkout is reported and left alone. The bare
  mirror is never rewound.
- **A failed sync fails the pass** (`sync FAILED: …`, `ok: false` in
  `status.json`), so the next pass runs again.
- **Empty objects heal themselves.** When the fetch fails with `object file …
  is empty`, `sync_healing` moves every zero-byte loose object of the bare to
  `state/publisher/quarantine/<UTC date>/`, rewrites each from the checkout
  (`restore_object`, checked against its id) and fetches again. Nothing is
  deleted.
- Failed passes log an explicit error and send a signed operator Ping with the
  cause, fix and retry command; the same failure is suppressed for an hour
  after a notification, and recovery clears the suppression.

## Gateway transport

The cores run an IDS, so pushes must use the gateway's normal receive path,
not SSH as the service user. `config/fanout/gateways` holds one target per
line: `name https://app.forkmesh.com/forkmesh/forkmesh.git [key.pem]`; the
default key is the desktop identity `data/identity/ed25519.pem`.

For each target the publisher:

1. starts a token-protected loopback signing proxy, trying `127.0.0.1`,
   `::1` and `127.0.0.2` until one accepts (`loopback_accepts`), with proxy
   bypass for local Git traffic;
2. runs `git push --porcelain <proxy> refs/heads/*:refs/heads/* refs/tags/*:refs/tags/*`
   (never forced, never deleting; deletions go through
   `POST /api/git/branch-deletes`, see [CORE_PUSH.md](../CORE_PUSH.md));
3. has the proxy sign each request as
   `forkmesh-git-push-v1\n{owner}\n{repo}\n{METHOD}\n{ts}\n{sha256(body)}`
   with `X-ForkMesh-Pubkey/-Ts/-Sig/-Body-Sha256`, the scheme the mobile
   clients use (`conformance/protocol/v1`, vector `git-push-v1-advertisement`).

The proxy signs only the configured owner/repo, only
`info/refs?service=git-receive-pack` and `git-receive-pack`, only under its
random path token, and lives for one push. The app verifies the signature
(`_git_push_authorize`, organization owners only), signs a router capability
and streams the push to a core gateway, which is fast-forward only with no
deletes. After an accepted push the app records the core's signed refs digest
and wakes the other cores.

| Rejection | Publisher |
| --- | --- |
| a tag `already exists` (the checkout moved it) | asks for an owner tag move, `POST /api/git/tag-moves` signed `forkmesh-tag-move-v1\n{owner}\n{repo}\n{ref}\n{target}\n{ts}`, then pushes `+refs/tags/<tag>` once (audited `git.tag_move_authorize`, `git.tag_move`); at most 8 tags per pass |
| `main` (`fetch first`): the cores hold an owner's push | fetches the cores' `main` as `refs/remotes/forkmesh-cores/main`, then fast-forwards `main` or merges it (`Merge pushes to main from the cores`) when `git merge-tree` is clean; with or without a worktree on `main`. On a conflict nothing changes and the log says `DIVERGED … merge <sha> by hand` |
| another branch, non-fast-forward | left at the cores' value and listed as `stale`; branches on the cores are never forced |

- A push that reports no ref and fails with a transport error (RPC failed,
  hung up, HTTP 502/503/504, refused, timed out) is retried once after 10 s,
  then logged `unreachable after a retry`. A push with rejected refs is not
  retried, except for two races on the cores, each retried once after 10 s:
  `main` refused with `failed to update ref` or `cannot lock ref` (a core job
  holds the ref lock), and a push that updates nothing because of an `unpacker
  error` (`unpack_race`).
- **Unpack race.** Each HTTP request of a push (the `info/refs` advertisement
  and the `git-receive-pack` POST) is routed to a core on its own. Two quick
  passes can leave a fresh object on one core only: on 2026-10-16 a pass
  pushed branch tip `4c238df8`, then pushed `main` (which merges it) to a core
  that did not have it yet. Git left the object out of the pack because the
  advertisement listed it, and the core refused the pack with `remote: fatal:
  did not receive expected object …` / `remote unpack failed: index-pack
  abnormal exit`, which marks every ref `[remote rejected] (unpacker error)`.
  The cores catch up within seconds (the next pass, 30 s later, went through),
  so the publisher retries the push once.
- **Outrun passes.** If the cores refuse a protected ref the checkout already
  contains and the cores hold what this pass pushed (`cores_hold`,
  `cores_ref_contained`), the pass was outrun by a newer one and counts as ok
  ("main was already past this pass"). If the cores are behind and refused
  it, the pass fails (`gateway cores: the cores are behind and refused
  main`).
- **Lagging read-back.** The fetch that checks a refused ref goes through the
  same edge as the push, so it can reach a core that has not replicated the
  newer value yet. On 2026-10-21 the desktop committed two `pull #N: merged`
  ledger records while a pass ran; the push of `forkmesh/pulls` was refused
  by a core that already held the newer tip, the read-back landed on one that
  still held the older one, and the pass failed as `DIVERGED` and sent a Ping.
  The next pass, 50 s later, went through. A refused `main` or ledger ref that
  does not read as outrun is now read once more after 10 s before the pass
  fails.
- **Ping cause.** A refused ref is named in the Ping (`the cores refused
  refs/heads/forkmesh/pulls (non-fast-forward)`, plus the absorb reason for
  `main`). Before, the Ping carried the last 600 characters of git's output.
  Those were mostly the cores' `receive.denyCurrentBranch` warning, cut off
  mid-word. Stale branches left at the cores' value are not counted as
  causes.

## GitHub mirror

`github.com/forkmesh/forkmesh` is a public, read-only copy of the flagship:
every pass pushes `main` and the tags from the bare mirror to each line of
`config/fanout/github` (`<name> https://github.com/<owner>/<repo>.git
[token-file]`). The token file defaults to `config/fanout/github-token` (one
GitHub classic token with `repo` and `workflow` scopes, mode 600; without
`workflow` GitHub refuses any `main` that touches `.github/workflows/`).
The token goes to git through `GIT_CONFIG_VALUE_0`, never the command line,
and is masked in errors.

- Only `main` and tags go to GitHub. Agent and feature branches stay local
  and on the forge.
- The push is fast-forward only. A GitHub `main` with commits ours lacks (a
  pull request merged on GitHub) is reported as `github <name>: FAILED, …
  rejected` and left as is; take the change into the checkout instead.
- A failed GitHub push does not fail the pass or send the publishing Ping, but
  `--if-changed` passes keep retrying until it succeeds. `status` lists each
  mirror's result under `github`.
- Contributions land on the forge, not GitHub. Pull requests opened on GitHub
  are reviewed and reapplied by hand.
- GitHub `main` (`eb1db58be1`, 2026-07-29) predates the 2026-10-08 history
  rewrite and shares no history with ours, so the first sync replaces it with
  a one-time `--force-with-lease=refs/heads/main:eb1db58be1…` push; after that
  the publisher's fast-forward pushes keep it current. The old commits stay
  reachable through GitHub's `refs/pull/1-3` until the repository is deleted
  or GitHub support purges them. A bundle of the old `main` and PR heads is in
  `data/core/state/github-mirror/github-forkmesh-before-2026-10-21.bundle`.

## Merges made on the cores

Runner and web merges land on a core's `main` first, and only commits on the
checkout's `main` are deployed ([../deploy/auto-deploy.md](../deploy/auto-deploy.md)).
`watch-cores` (every 20 s) reads `main` with `git ls-remote` from each core
(`core_read_urls`, `role = "core"` nodes in `deploy/fleet.toml`) and from the
edge. When the checkout's `main` lacks one, it takes it (`absorb_cores`: a
fast-forward or a merge) and runs a publish pass, which pushes the union to
the cores. The `post-merge` hook then lets the desktop queue its Actions. A
conflicting or untakeable `main` is recorded in
`state/publisher/cores-watch.json` and not retried until that core's `main`
moves. An unreadable `main` does nothing.

## Flagship guard

`forkmesh-source-guard.timer` runs `source_publisher.py guard` every minute
and starts `forkmesh-source.service` when it is `inactive` or `failed`,
unless a hold is set:

```
python3 ~/projects/forkmesh/bin/publisher/source_publisher.py hold 2h --reason "rebuilding by hand"
systemctl --user stop forkmesh-source.service
...
python3 ~/projects/forkmesh/bin/publisher/source_publisher.py unhold
```

A window close writes a hold with `"setBy": "desktop"` (`writeDesktopCloseHold`)
so a closed desktop stays closed; the next desktop start under the unit
removes it (`releaseDesktopCloseHold`) but never a hand-set hold. Settings >
General > Startup > **Reopen ForkMesh after I close it**
(`startup/reopenAfterClose`, off by default) skips the hold. Under a systemd
user service, Rebuild & restart asks systemd to restart the unit
(`requestSupervisingUserServiceRestart`) instead of starting a child.

## Operations

- Install or update, from a checkout on `main`:
  `python3 tools/source_publisher.py install`. This copies the tool and
  `forkmesh_paths.py` into `~/projects/forkmesh/bin/publisher/`, writes the
  units into `~/projects/forkmesh/config/systemd/` (linked from
  `~/.config/systemd/user/`) and enables `forkmesh-mirror-fanout.timer`,
  `forkmesh-source-publish.path`, `forkmesh-source-guard.timer` and
  `forkmesh-source-cores.timer`. The installed copy changes only after
  `install`.
- State: `source_publisher.py status` prints checkout vs bare `main`, the
  last pass, gateway results, the flagship state and any hold; the raw file is
  `~/projects/forkmesh/state/publisher/status.json`.
- Journal: `journalctl --user -u forkmesh-source-publish -u forkmesh-mirror-fanout -u forkmesh-source-guard -u forkmesh-source-cores`.
- Paths come from `tools/forkmesh_paths.py` (`bare_mirror`,
  `fanout_gateways`, `publisher_bin`, `publisher_state`, `action_spool`,
  `systemd_units`, `fleet_*`); `tools/next_common.py` derives its paths from
  them and `FMNEXT_*` variables override each one.

## Traps

- **Loopback blocked.** If a local firewall drops connections to loopback,
  every gateway push fails before sending anything. The journal line reads
  `gateway cores: loopback blocked` and the result has `loopbackBlocked:
  true`; git is not retried. Allow loopback again (check firewall rules, then
  `python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); s.listen(); socket.create_connection(s.getsockname(), timeout=2)'`).
- `main` in the served bare mirror is only ever fast-forwarded from the
  checkout. Land work by committing or merging in the checkout.
- A conflicting cores' `main` needs a hand merge:
  `git merge refs/remotes/forkmesh-cores/main` in the checkout.
- `systemctl --user show -p A -p B --value` prints values in systemd's
  property order, not argument order; the guard parses `key=value` lines.
