# Runners: trusted core and untrusted agent compute

Phase 5 of `docs/PLAN.md` splits the Go daemon (`server/`) into three roles:

| role | serves repositories | gateway, tunnel, catalog, merge executor | agent jobs | Actions |
|---|---|---|---|---|
| `mirror` (default) | yes | yes | yes, unless `disableAgentJobs` | if `enableActions` |
| `core` | yes | yes | **never** | **never** |
| `runner` | **no** | **none** | yes | if `enableActions`: leased from the app while the desktop is offline ([server/runner-actions.md](server/runner-actions.md)); rendered runners turn it on |

A config without `role` is a `mirror`, so every existing config keeps its meaning.

Since 2026-10-02 a mirror's agent jobs use the runner's environment allowlist,
a per-session `HOME`/`TMPDIR`/`XDG_*` and, when the daemon's systemd user manager
is reachable, a `systemd-run --user` scope without resource limits. They still
run as the mirror's own user, next to its node key; only a runner moves jobs to
`forkmesh-job`. Details: [server/agent-jobs.md](server/agent-jobs.md#what-the-agent-process-gets).
`role: "core"` forces `disableAgentJobs` on and `enableActions` off. If the file had
them the other way, the daemon logs one line at startup.

`role: "runner"` does the following:

- Starts no gateway child and no cloudflared.
- Publishes no catalog or endpoint.
- Keeps no repositories. Its `gatewayConfig` is only the node identity document (name and public key), and it must list no repositories.
- Runs no merge executor.

## Architecture

```
 website ── queue ──► app.next (org_agent_jobs)
                         │  POST /api/runners/claim   (runner-signed)
                         ▼
                      runner ──git clone (read grant)──► core gateway /v1/runner/<owner>/<repo>.git
                         │  claude/codex in a per-job systemd scope
                         ├──git push HEAD:refs/heads/agent/<session> (push grant)──► core gateway
                         └─ POST /api/runners/report  (runner-signed) ──► app.next
```

1. **Claim.** The runner calls `POST /api/runners/claim` with `{max, providers, allowedOwners, burst}`. `max` is its free slots, at most 4. `providers` lists the CLIs that are installed and signed in.
   A runner has no node account, so it opens no node event channel and gets no doorbell. Instead the claim is a long poll: the runner sends `waitMs: 20000`, and when nothing is leasable the app holds the request for up to 20 s (`CLAIM_WAIT_MAX_MS` in `app/src/runner_leases.py`). Every 250 ms (`CLAIM_WAIT_TICK_MS`; it was 1 s until 2026-10-06) it reads one mark over the runner's targets: the count, highest id and latest `updated_at` of their `queued`/`leased` jobs, plus this runner's pending sign-ins and account requests. When the mark changes, the app answers at once with no jobs. It does not lease anything after the wait: a runner that restarted while its claim was held would never see that answer, and the job would stay `leased` until its 2 h timeout. The app echoes `waitMs`, and the runner then claims again straight away, at most once a second after the previous claim began (`runnerClaimGap`). That claim leases the job in its first scan. A prompt sent from the web or mobile therefore reaches the runner within about a quarter of a second, not after the next 20 s poll. Its HTTP timeout for the claim is 20 s longer than for other calls. A held claim never waits while the runner has a sign-in or account request open, because those need the 1 s fast polls. The cores do not print a `slow request` line for `/api/runners/claim` (`HELD_REQUEST_PATHS` in `app/host/forkmesh_host/asgi.py`). A held claim also ends, with no jobs, as soon as the core's app starts stopping (`env.HOST_STOPPING`, set by uvicorn's connection shutdown; see [host/README.md](host/README.md), Recent changes, 2026-10-01). Before that, a claim held at a restart kept the old process alive for up to 20 s.
   An app that ignores `waitMs` does not echo it. The runner then polls every 20 s ±25 % jitter, as it did before, and polls again at once when a job finishes. With no free slot it does not claim, and polls `/api/runners/poll` every 20 s. After a failed poll it waits 30 s, doubling up to 5 min, and resets on the next success (`runnerPollDelay` in `server/runner.go`).
2. **Lease.** The app answers with up to `max` leased jobs. Each job carries:
   - `cloneUrl`: `https://<core>/v1/runner/<owner>/<repo>.git`
   - `readGrant` and `pushGrant`
   - `pushRef`: `refs/heads/agent/<session>`, or the session's named branch
     (`agent/<n>-<slug>`, chosen at start, see
     [native/RUNNER_AGENTS.md](native/RUNNER_AGENTS.md))
   - `grantExpiresAt` and `timeoutMs`, both at most the job timeout, and never more than 3 h
   - `attachments`: the prompt's images, metadata only (`id`, `name`, `mediaType`, `size`, `sha256`); see "Prompt images" below
   - `modelGrant`, pool runners only (since 2026-10-07): a router-signed statement of the model the job runs. Before the push, the runner rewrites the run's new commits with a `ForkMesh-Attestation` trailer it signs under that grant, which is what a `model` pull request rule checks. See [server/pull-rules.md](server/pull-rules.md).
3. **Check the lease.** The runner rejects a lease whose grants disagree with each other or with the job (runner, node, repo, ref). It also rejects a lease whose `pushRef` is not under `refs/heads/agent/`, or whose grants expire within 5 min. `runnerBackend.validate` does this. The runner reports a rejected lease as `rejected` without running anything.
4. **Clone.** The runner clones into `runner.workRoot/<sha256(session)[:24]>`. That directory is recreated empty with mode 0700 for every job.
   - **Repository cache (2026-10-06).** Until then every job made a full `clone --bare` of the core before it reported `running`; for a repository the size of this one (about 480 MiB) that took long enough that the runner looked unresponsive after it had already leased the job. The daemon now keeps one bare copy per clone URL in `runner.workRoot/.cache/<sha256(cloneUrl)[:12]>.git` (mode 0700, daemon-owned, never visible to a job). Each job fetches into it with its read grant (`fetch --prune` of `refs/heads/*` and `refs/tags/*`, so only the change since the last job crosses the network), then makes the publish repository as a local hardlinked `clone --bare` of the cache. The core serves every read grant of a repository the same full view (`_dispatch_runner_git` in `tools/mirror_gateway.py`), so a job sees no more from the cache than from a fresh clone; objects the core has since dropped can still be in the cache, as they were readable by the same grants before.
   - Jobs on the same repository take a per-cache lock for the fetch and the local copy. A cache that fails to fetch or copy is left in place and the job clones from the core as before; nothing deletes or re-clones it. A cache unused for 14 days (`jobCacheTTL`) is removed by the next job's sweep, at most once an hour. The cache's `HEAD` is the core's default branch at the first clone; a job without `baseBranch` whose cached default branch is gone from the core falls back to `main`.
   - The grant travels as `X-ForkMesh-Runner-Grant` through `GIT_CONFIG_*` environment variables. It is never on a command line and never written into the clone's config.
   - The directory name is stable per session, so a Claude Code follow-up can `--resume`.
5. **Run.** The runner runs `claude` or `codex` through `agents.go`, the same code a mirror uses: stream-json transcript, idempotent job records, and a `running` report followed by a terminal one. The run happens inside the sandbox described below. Its timeout is the smallest of 2 h, `timeoutMs`, and the grant expiry minus 5 min.
6. **Push.** The job's repository is job-controlled (hooks, `core.fsmonitor`, filters, a rewritten remote), so the daemon never runs git in it:
   - **Inside the job sandbox, as the job user**, `git -c core.hooksPath=/dev/null -c core.fsmonitor=false` runs `status`, `add -A`, `commit --no-verify` and `rev-parse HEAD`, then `bundle create` of the new commits only (everything the core already has is excluded) into the session's `TMPDIR`.
   - **The daemon** reads that bundle with `O_NOFOLLOW` (it must be a regular file owned by the job user, at most 1 GiB) and copies it next to its own bare repository `<workRoot>/.publish/<id>.git`. That repository is a `clone --bare` of the core, made in step 4 before the job ran. The daemon fetches the bundle into it with `transfer.fsckObjects`, then pushes `refs/fm/result:<pushRef>` to the lease's literal clone URL with the push grant. Whatever the workdir's `remote.origin.url` says is ignored.
   - **Every daemon git call** runs with `GIT_CONFIG_NOSYSTEM=1`, `GIT_CONFIG_GLOBAL=/dev/null` and a minimal environment. It also sets `-c core.hooksPath=/dev/null -c core.fsmonitor=false -c protocol.allow=never -c protocol.https.allow=always -c credential.helper= -c http.followRedirects=false`. `file` is allowed only for the local clone and the bundle fetch, and `http` only for a loopback core (tests). There is no `safe.directory`.
   - Tests: `server/runner_publish_test.go` arms a worktree with hooks, an fsmonitor, a clean filter, `insteadOf`/`ext::`/`pushurl` remotes and `core.sshCommand`, and checks that none of them runs as the daemon and that the push lands only on the lease's URL and ref.
7. **Report.** The runner sends `POST /api/runners/report`. The body is the one `agents.go` sends, plus `jobId`.
8. **Clean up.** The runner removes the workdir, the publish repository and the bundle copy. `removeUnderRoot` refuses any path that `filepath.Rel` does not place strictly inside `workRoot`. It then closes the session home (see "Job sandbox").

Code:

- Daemon: `server/runner.go`, `server/sandbox.go`, `server/runnergrant.go`, and the backend split in `server/agents.go`.
- Daemon, web sign-in: `server/runnerlogin.go` (the pty code is `server/terminal.go`'s). Several accounts per provider and their usage limits: `server/runneraccounts.go`.
- Daemon, live agent output: `server/agentstream.go`; app `app/src/agent_streams.py`, migration `0232_agent_session_streams`.
- App: `app/src/runner_leases.py`, migration `0193_runner_nodes`; web sign-in `app/src/runner_logins.py`, migration `0195_runner_login_sessions`; account switching, migration `0220_runner_account_requests`.
- Website: `/dashboard/runners` (`app/public/dashboard/js/09-runners.js`, `partials/views/runners.html`).
- Gateway: the runner route in `tools/mirror_gateway.py`.

## Security model

**What a runner can do:**

- Read the one repository its lease names, for the lease's lifetime (at most 3 h), through its read grant.
- Fast-forward exactly `refs/heads/agent/<session>` of that repository through its push grant.
- Report on leases it holds. The app checks that the reporting runner and the `leaseId` match the `runner_job_leases` row.
- Run arbitrary code inside its own job sandbox. That is the point of a runner.

**What a runner cannot do:**

- **Write any other ref.** The gateway parses the receive-pack command list before git runs. It refuses the whole push, with refs untouched, when:
  - any command names a ref other than the grant's ref
  - there is more than one command
  - a command deletes the ref
  - a push certificate is present
  - a pkt-line or object id is malformed

  `receive.denyNonFastForwards` also refuses force pushes, even on the agent ref. Tests: `app/tests/test_mirror_gateway_runner_grants.py`.
- **Mint or stretch a grant.** Grants are Ed25519-signed by the router key, which only the app holds. The canonical message is `forkmesh-runner-grant-v1` followed by node, owner, repo, access, ref, runner, grant id, issued-at and expires-at. That format is identical in `edge_routing.runner_grant_message`, `mirror_gateway.runner_grant_message` and Go `runnerGrantMessage`. The gateway's Go verifier accepts a grant only for its own node name (`validateCapabilityPayload`). Lifetime is capped at 3 h in all three places.
- **Claim jobs it is not eligible for.** Claims are signed with the runner's node key over the existing `request_message` canonical form: method, path, body sha256, request id and time. Every request id is single-use (`runner_request_nonces`), and a request is accepted only within ±60 s of its timestamp. The app then applies these rules:
  - Only `active` rows in `runner_nodes` may claim.
  - A job pinned to another runner (`runner` in the job record) is never offered.
  - A `personal` runner serves only the owners in its `allowed_owners`.
  - A `pool` runner serves only the core targets in `RUNNER_POOL_TARGET_NODES`, and **only the owners listed in its `allowed_owners`**. The list is the owner's opt-in: registration refuses an empty list for every scope, and a runner row whose list is empty (such as `fmnext-r1`, registered before 2026-09-24) serves nobody.
  - A `steer` job goes to the runner that ran the session, on the same ref.
    Since 2026-09-30, once that runner is revoked or has not claimed for 10
    minutes (`STEER_HANDOVER_MS`), any eligible runner may take it, still
    only on that ref. A runner that does not have the session in its
    `state.json` resumes it on the branch with a fresh conversation and a
    preamble naming the branch and task (`agentResumePreamble`), instead of
    failing with "the scoped agent session is unavailable".
  - A `continue` job is offered only when its branch is under `agent/`.
  - The runner's own `allowedOwners` narrows the set further, on both sides.
- **Reach core secrets.** A runner holds no router key and no core node key, and it never sees the core's repositories on disk. Its node key only authenticates claims and reports; the app does not accept it for any repository write.
- **Reach other jobs, the daemon or the node key.** Jobs run as the dedicated `forkmesh-job` user, never as the daemon's user. That user cannot read the node key, `mirror-node.json` or the connector token. Each job gets its own directory, which is wiped before and after the job, its own session `HOME` and `TMPDIR`, and its own systemd scope, which is stopped when the job ends.

**Residual risks**, which are documented and not mitigated here:

- **All jobs share one uid.** Concurrent jobs (`maxConcurrentRuns` > 1) can see each other's workdir and processes, so keep a pool runner at 1. Between jobs, session homes are root-owned and closed. `/tmp` is shared, because a scope has no `PrivateTmp`; jobs get their own `TMPDIR`, but a job can still write `/tmp`. Whatever the job user leaves there is removed once no job runs (see "Shared /tmp" below).
- **The provider login is readable by agent code.** In mode A, the credential file is copied into the job's HOME, and the job user also owns the persistent home. A job can therefore read or replace the login, or plant files in `/var/lib/forkmesh-job` that the web sign-in (which runs there) would load. Serve only owners you would lend the login to. This applies to the **active** account only: the other saved accounts are in the daemon's 0700 store (see "Several accounts per provider").
- **The read grant exposes the whole repository**, which is inherent to cloning.
- **Loopback is blocked.** The `IPAddressDeny` filter and nftables block loopback, so an agent cannot reach a dev server it starts on `127.0.0.1` (Playwright `webServer` and the like).
- **Non-root daemons.** A daemon that is not root (a desktop or a test) still runs jobs as its own user and logs a warning at startup. A root daemon refuses to start without `runner.jobUid` or `runner.jobUser`.

### Job sandbox

With `systemd-run` and a reachable manager, the daemon runs each CLI as:

```
systemd-run --scope --quiet --collect [--user] -p MemoryMax=<jobMemoryMax> -p MemorySwapMax=<jobMemorySwapMax> \
  -p CPUQuota=<jobCPUQuota> -p TasksMax=512 [--uid=<jobUid> --gid=<jobUid>] -- claude ...
```

- **Root daemon:** the system manager is used with `--uid/--gid` of `runner.jobUid` (or `runner.jobUser`, resolved at startup), which is **required**. The workdir is chowned to that uid. The scope also gets `-p IPAddressDeny=127.0.0.0/8 ::1/128 169.254.0.0/16 fe80::/10 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 100.64.0.0/10 fc00::/7`, plus `IPAddressAllow=` for any loopback nameserver in `/etc/resolv.conf` (for example `127.0.0.53`). This is defence in depth: nftables `meta skuid forkmesh-job` rules on the runner are the primary control. Each scope gets a unit name `fmnext-job-<random>` and is stopped with `systemctl stop` when its main process exits, so nothing the job forked survives it.
- **Non-root daemon:** the user manager is used (`--user`), when `$XDG_RUNTIME_DIR/systemd/private` exists. A user manager cannot attach the IP filter.
- **HOME and TMPDIR:** a job never runs with the job user's persistent HOME. Each agent session gets `<workRoot>/.homes/<id>`, so a follow-up can still `--resume` its own conversation. `TMPDIR` is `<that home>/tmp`, recreated empty for every job (`server/jobhome.go`). A sandboxed command outside a session workdir gets `TMPDIR=<workRoot>/.tmp/<scope unit>` (0700, owned by the job user), removed when the scope ends. `CLAUDE_CODE_TMPDIR` is set to the same directory, because Claude Code otherwise keeps its own files in `/tmp/claude-<uid>` whatever `TMPDIR` says.
  - Before the job, the daemon copies in only `.claude/.credentials.json` and `.codex/auth.json` from the persistent home. Each copy must be a regular file owned by the job user, at most 256 KiB, valid JSON, and no path component may be a symlink.
  - After the job, a credential the CLI refreshed is copied back under the same checks. It is written atomically with `O_EXCL` and a rename, and only over a login that already exists there. Since 2026-09-29 it is copied back only when the job's copy differs from what was handed in (the job itself refreshed it), and never over a persistent credential that changed meanwhile unless the job's copy is newer (a later access-token expiry, or Codex's `last_refresh`). See "Why the r1 login kept expiring" below. Nothing else, such as `settings.json` or hooks, ever flows back.
  - **Known gap (found 2026-09-29).** `systemd-run --scope --uid=<jobUid>` sets `HOME`, `USER`, `LOGNAME` and `SHELL` from the job user's passwd entry and overrides the `HOME` the daemon passes. On r1 (systemd 257) a job therefore runs with `HOME=/var/lib/forkmesh-job`, the persistent home, not its session home; only `TMPDIR`/`CLAUDE_CODE_TMPDIR` are per session. Claude Code writes its transcripts to `/var/lib/forkmesh-job/.claude/projects/-var-lib-forkmesh-runner-work-<id>`, and it reads and refreshes the persistent `.credentials.json` in place. The isolation this section describes for HOME does not hold, so a job can write the persistent home's `settings.json` or shell rc files. Fixing it (for example `/usr/bin/env HOME=<session home>` inside the scope) changes where Claude keeps its config and `--resume` history, so it needs its own change and a check on a runner. The copies are then deleted, `tmp` is removed, and the session home is chowned to root with mode 0700, so no later job of any owner can read it or plant anything in it.
  - Session homes unused for 14 days are removed.
- **Shared /tmp:** the scope inherits the daemon's mount namespace, so a job's `/tmp` and `/var/tmp` are the daemon's. Agents and the tools they run write `/tmp` directly (`/tmp/cc`, `/tmp/fmdeps`, unpacked tarballs, JVM `hsperfdata_*`), ignoring `TMPDIR`. With a dedicated job user the daemon therefore removes every top-level entry of `/tmp` and `/var/tmp` owned by that uid whenever the last running job command (or web sign-in) ends, and once at startup. Nothing of the daemon's or of another uid is touched, and nothing is removed while a job or sign-in still runs. A non-root daemon (jobs as its own uid) never sweeps `/tmp`.
- **Fallback:** with no usable manager, the CLI runs under `sh -c 'ulimit -v …; exec "$@"'`, killed by the job's Go-side timeout. The address-space limit is `4 × jobMemoryMax + 8 GiB`, because V8 reserves a large address space up front. It is a runaway guard, not a quota.
- **Environment:** an allowlist, not a denylist. It contains only:
  - `PATH`, `LANG`, `LANGUAGE`, `LC_*`, `TZ` and `TERM`;
  - the proxy variables;
  - provider keys the operator deliberately set on the daemon (`ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`, `ANTHROPIC_BASE_URL`, `CLAUDE_CODE_OAUTH_TOKEN`, `OPENAI_API_KEY`, `OPENAI_BASE_URL`);
  - the job's own `HOME`, `TMPDIR`, `USER`/`LOGNAME` and `GIT_TERMINAL_PROMPT=0`;
  - with a user manager only, `XDG_RUNTIME_DIR` and `DBUS_SESSION_BUS_ADDRESS`.

  The daemon's `CLAUDE_CONFIG_DIR`/`CODEX_HOME` are never used for a job user.

### Memory on the 1 GB runner

On 2026-09-27 fmnext-run-1 (961 MB RAM, 481 MB zram plus a 2.3 GB swapfile) OOM-killed job scopes over and over ("Memory cgroup out of memory" for a ~250 MB python), the global OOM killer took the `claude` CLI, journald missed its watchdog four times and a session scope failed with `resources`. The cause was `/tmp`. The host's `/tmp` is a tmpfs (50 % of RAM), and `PrivateTmp=true` put the daemon's private `/tmp`, which the job scopes share, on it. tmpfs pages are RAM: they were charged to the job's cgroup, counted against `MemoryMax`, and with `MemorySwapMax=0` could not be swapped out. When a scope ended the pages stayed, reparented, and 419 MB of job leftovers (`cc` 257M, `fmdeps` 62M, an unpacked rqlite 49M, `aqtv`, `rq.tgz`, `qt`, `hsperfdata_forkmesh-job`, `fmvenv`, `claude-997`) permanently took almost half the machine.

The fix (branch `fix/runner-oom-tmp`):

- The runner's daemon unit no longer uses `PrivateTmp`. `BindPaths=/var/lib/forkmesh-runner/tmp:/tmp /var/lib/forkmesh-runner/tmp:/var/tmp` gives it and its jobs a disk-backed, sticky, world-writable `/tmp` of their own, still separate from the host's `/tmp`. A job writing `/tmp` now costs disk and page cache (reclaimable), not pinned RAM. Cores keep `PrivateTmp=true`.
- Every sandboxed command gets a `TMPDIR` on disk, and the job user's leftovers in `/tmp` and `/var/tmp` are swept when no job runs (see "Job sandbox"). The sweep keeps the disk from filling the way the tmpfs did. The daemon's own temporary files go to `TMPDIR=/var/lib/forkmesh/tmp` (0700), and `UMask=0077` keeps anything it writes to `/tmp` unreadable to the job user.
- `jobMemorySwapMax=1G` on this runner. With `MemorySwapMax=0` even global reclaim could not move a job's anonymous pages to zram or the swapfile, so a burst went straight to an OOM kill while swap sat unused. Now cold pages can go to zram (priority 100) and then the swapfile, and `MemoryMax=600M` still caps the RAM a job holds. A swapped job page can land in the root-only swapfile. That is accepted for jobs; the secret-holding units (vault, rqlite, app, certsync, Caddy) keep `MemorySwapMax=0`.

A daemon restarted with the new unit drops its old `PrivateTmp` directory, and systemd deletes the old leftovers with it.

## App endpoints and registration

The implementation is `app/src/runner_leases.py`, and its tables are in migration `0193_runner_nodes`:

- `runner_nodes`
- `runner_job_leases`
- `runner_request_nonces`

| endpoint | auth | purpose |
|---|---|---|
| `POST /api/runners/register` | account session plus a key proof | register a runner name and public key |
| `POST /api/runners/revoke` | registrant or admin | set `status='revoked'` |
| `POST /api/runners/claim` | runner-signed | lease up to N jobs, with grants |
| `POST /api/runners/report` | runner-signed, must hold the lease | report, applied by the same code as `/org-agent-jobs/<id>/result` |
| `POST /api/runners/attachment` | runner-signed, must hold the lease | `{jobId, leaseId, attachmentId}`: the bytes of one image of the leased job |
| `POST /api/runners/pool-status` | core-signed (a node in `RUNNER_POOL_TARGET_NODES`) | queue depth and runner load for the autoscaler |
| `POST /api/runners/poll` | runner-signed | availability and sign-in hand-off when the runner does not claim |
| `POST /api/runners/login/output` | runner-signed | relay terminal output and final state; answers with queued input |
| `POST /api/runners/login/input-poll` | runner-signed | queued input only |
| `POST /api/runners/stream` | runner-signed, must hold the lease | live output of a running job: `{jobId, leaseId, seq, data}`; answers `{watched, seq}` (see "Live agent output") |
| `GET /api/runners` | admin, or registrant (personal runners) | runners, availability, open sign-ins |
| `POST /api/runners/setup-tokens`, `GET …/<id>`, `POST …/<id>/revoke` | account session | one-time install tokens; see "Set up a runner" |
| `POST /api/runners/register-token` | setup token plus a key proof | register a personal runner from the installer |
| `POST /api/runners/ssh/probe`, `POST /api/runners/ssh/install`, `GET /api/runners/ssh/<id>` | account session | server-side SSH setup |
| `GET /api/runners/attention` | account session (cookie or bearer) | the caller's runners whose provider login is expired or expiring; see "Runner attention" |
| `POST /api/runners/<name>/login` | admin, or registrant of a personal runner | start a sign-in: `{provider}` |
| `GET /api/runners/<name>/login/<id>/output?after=<seq>` | same | new output chunks and state |
| `POST /api/runners/<name>/login/<id>/input` | same | `{data}`: the pasted code |
| `POST /api/runners/<name>/login/<id>/cancel` | same | end the sign-in |

**Registration** is the simplest flow that works: the operator signs in to the website and posts the runner's name and public key. The proof is the runner key's signature over:

```
forkmesh-runner-register-v1\n<name>\n<publicKey>\n<account>\n<ts>
```

That proves the caller controls the key.

- A non-admin may register only a `personal` runner that serves their own account.
- Only an app admin may register a `pool` runner.

To get the public key, run `forkmesh-mirror-node --config … --print-public-key` on the runner.

**Core-side prerequisites:**

- The core's gateway serves `/v1/runner/…` for every configured, enabled repository. It never creates a repository on demand.
- The app sets `RUNNER_POOL_TARGET_NODES` to the core node name(s) whose queues the pool drains. In `app/wrangler.toml` it is `fmnext-n1,fmnext-n2,fmnext-n3` (since 9436027): a job queued on any core can be claimed by a pool runner.

### Prompt images (2026-10-04)

A web or phone prompt can carry up to five images ([server/agent-jobs.md](server/agent-jobs.md), step 6). Until 2026-10-04 a runner never received them: the claim answer left out the job's `attachments`, and the runner would have fetched them from `/api/repo/<node>/<repo>/org-agent-jobs/<id>/attachments/<id>`, a route signed with a claimed node's key, which a runner does not have. Runner jobs ran with the text of the prompt only.

Now:

- The claim answer carries `attachments` (metadata only, `_attachment_meta` in `app/src/runner_leases.py`).
- Before the `running` report, the runner fetches each image with `POST /api/runners/attachment {jobId, leaseId, attachmentId}`, signed like a claim (`runnerBackend.fetchAttachment`, `server/runner.go`). The app answers only to the runner that holds the job's current, unexpired lease while the job is `leased` or `running` (`409 lease_not_found` otherwise), and only for an attachment the job names whose stored bytes still match its size and digest (`410 attachment_unavailable`). The lease is checked again on every fetch, so a released or requeued job's images stop being served at once.
- The runner checks size, digest and magic bytes as a mirror does, writes the files to `.forkmesh/attachments/<uuid>.<ext>` in the job's workdir, lists that folder in `.git/info/exclude`, and gives the folders, files and exclude file to the job user (`runner.jobUid`), so the CLI can read them. The prompt gains the same "Attached images" block as on a mirror.
- The workdir is a checkout of a repository anyone with push access controls, and the daemon writes into it as root. A committed `.forkmesh` or `.forkmesh/attachments` symlink would otherwise let a prompt image land anywhere on the runner. `plainAgentFolder` (`server/agent_attachments.go`) creates each folder with `Lstat` and `Mkdir` and refuses anything that is not a real folder; files are created with `O_EXCL` and the exclude file is opened with `O_NOFOLLOW`. The daemon does not run git in the workdir to find the exclude file: on a runner it is always `.git/info/exclude` of the daemon's own clone. No job has run in the workdir yet at this point, so nothing races the checks.
- A job whose images cannot be fetched fails before it runs, with "Could not fetch the attached images, so the agent did not run", as on a mirror.

Tests: `app/tests/test_runner_leases.py` (the claim carries attachments; only the lease holder downloads; wrong lease, unknown id, damaged bytes and a released lease are refused), `server/runner_attachments_test.go` (the backend writes, excludes and re-uses the folder through an in-process transport; damaged or missing images fail; symlinked folders are refused; plus two end-to-end runner runs that need loopback HTTP).

## Runner config

```json
{
  "schemaVersion": 1,
  "role": "runner",
  "gatewayConfig": "/var/lib/forkmesh-mirror/mirror-gateway/config.json",
  "identityKey": "/var/lib/forkmesh-mirror/identity/ed25519.pem",
  "runner": {
    "appUrl": "https://app.next.forkmesh.com",
    "maxConcurrentRuns": 1,
    "workRoot": "/var/lib/forkmesh-mirror/runner-work",
    "jobMemoryMax": "700M",
    "jobCPUQuota": "90%",
    "allowedOwners": ["alice"],
    "jobUser": "forkmesh-job",
    "burst": false
  }
}
```

**Defaults:**

- `maxConcurrentRuns` is 1, and at most 8.
- `workRoot` is `<dir of identityKey>/runner-work`.
- `jobMemoryMax` is `700M` and `jobCPUQuota` is `90%`.
- `jobMemorySwapMax` is `0` (a job's pages never go to swap); `0` or a size such as `1G`.
- `appUrl` must be an HTTPS origin; HTTP is accepted only for loopback.
- `jobUid` (a number) or `jobUser` (a name, resolved with the passwd database at startup) is required when the daemon runs as root. The user must not be root, must have its own home (for example `/var/lib/forkmesh-job`, mode 0700) and must not share the daemon's home.

### What the installer provides on a runner (`tools/next_fleet.py`, `deploy/core/render.py`)

- The system user `forkmesh-job`, with home `/var/lib/forkmesh-job` (0700, owned by it), no login shell and no supplementary groups. It must not be able to read `/etc/forkmesh`, the daemon's state or `/var/lib/forkmesh`.
- The daemon unit running as `User=root`, with `runner.jobUid` (the numeric uid of `forkmesh-job`) merged into `/etc/forkmesh/mirror-node.json`.
- The node identity key, `mirror-node.json` and the connector token owned by root or `forkmesh-node` with mode 0600, in directories of mode 0700.
- `workRoot` in a root-owned directory (`/var/lib/forkmesh-runner/work`, 0711). The daemon creates `.publish` (0700), `.homes` (0711) and `.tmp` (0711) there itself.
- A disk-backed `/tmp` for the daemon and its jobs: `/var/lib/forkmesh-runner/tmp` (root, 1777, made by the `base` and `files` steps) bound onto `/tmp` and `/var/tmp` with `BindPaths=`, instead of `PrivateTmp=true`. See "Memory on the 1 GB runner".
- `jobMemoryMax=600M` and `jobMemorySwapMax=1G` in the overrides.
- nftables output rules for `meta skuid forkmesh-job`, which drop 169.254.0.0/16, RFC 1918, 100.64.0.0/10, loopback (except udp/tcp 53 to a loopback resolver such as 127.0.0.53, if one is used), the WireGuard 10.77.0.0/24 range, and the IPv6 equivalents (`::1`, `fc00::/7`, `fe80::/10`).

### Owner opt-in on a pool runner

A pool runner serves only the owners on its `runner_nodes.allowed_owners` row. To opt an owner in, do one of the following:

- re-register the runner (`POST /api/runners/register` with the runner key's proof and the new `allowedOwners`; the same registrant only);
- as an admin, run this in the Database tab:

  ```sql
  UPDATE runner_nodes SET allowed_owners='["alice","bob"]', updated_at=<ms> WHERE name='fmnext-r1';
  ```

  (`runner_nodes` is not a host table, so the Database-tab write block does not cover it.)

  To stop serving everyone: `allowed_owners='[]'`, or `POST /api/runners/revoke`.

### Runner targets in the owner agent route (2026-09-29)

`GET /api/repo/<owner>/<repo>/agent-sessions` lists runners as targets with
`runtimeMode: "runner-pool"`:

- **The pool target** (`_org_agent_pool_target`), when the repository's node
  is in `RUNNER_POOL_TARGET_NODES`: `node` is the best signed-in pool runner
  (else the core), `queueNode` the core, `providers` the providers signed in
  on at least one opted-in runner, and `needsLogin` the providers only an
  expired runner offers. New: `runner` names the runner even when none is
  signed in, and `runners` lists every opted-in pool runner as `{name, scope,
  online, lastSeenAt, ready, needsLogin, signIn}`, where `signIn` is an
  installed CLI that was never signed in. The target is returned whenever
  any opted-in runner exists, so a client can say "fmnext-r1 needs Claude
  reconnected" instead of "no runner". Before this, a runner whose only CLI
  had expired produced `providers: []` with `node` set to the core and
  `online: false`, and the World dropped the target entirely.
- **Personal targets** (`_org_agent_personal_targets`, new): each active
  personal runner the account registered, when the repository sits on a pool
  core. `node` is the runner, `queueNode` the core, `runnerPin` the runner.
  Queueing to it stores `runner: <name>` in the job record, the pin that
  `runner_leases.py` honours, so only that runner claims it and a pool runner
  never does.

A runner talks to the app only through `/api/runners/*`. It opens no node event channel (`/api/nodes/events`) and makes no catalog, endpoint-lease, fork-job, intake or reclaim calls. Those need a node account, and a pool runner has none. Any `catalogUrl` is ignored, and `upstreams` must be empty.

## Set up a runner (2026-09-29)

Any account can now add a **personal runner**: a Linux machine of theirs that
runs their own agent jobs with its own Claude Code or Codex login. The
dashboard's **Runners** entry is in every account's sidebar (it was
admin-only), and the page has **Set up a runner** with two ways in. The World's
Runners panel links to it (`/dashboard/runners?setup=1` opens it).

### 1. One command

**Make the command** asks for a runner name (default `<account>-runner`) and
shows:

```sh
curl -fsSL https://forkmesh.com/install-runner.sh | sudo bash -s -- \
  --token fmrt_<64 hex> --name <runner> --app https://app.forkmesh.com
```

The page then polls the token until a runner uses it, and highlights the new
runner with its **Connect Claude / Connect Codex** buttons. **Cancel this
command** revokes it.

**Setup tokens** (`app/src/runner_setup.py`, table `runner_setup_tokens`,
migration `0223_runner_setup`):

| endpoint | auth | purpose |
|---|---|---|
| `POST /api/runners/setup-tokens` | account session (same-origin for a cookie) | `{name, via}` -> `{id, token, runner, expiresAt, command, installUrl, appUrl}`, 201 |
| `GET /api/runners/setup-tokens/<id>` | the token's owner | `{setup: {state: pending\|used\|revoked\|expired, runner, usedAt, runnerStatus}}`; never the token |
| `POST /api/runners/setup-tokens/<id>/revoke` | the token's owner | cancel an unused token |
| `POST /api/runners/register-token` | the token itself plus a key proof | register the runner, 201 (200 on a re-key) |

- A token is `fmrt_` plus 256 random bits. Only its SHA-256 is stored; it is
  shown once, in the mint answer, and never logged or audited (the audit
  entries `runner.setup_token.create`/`.revoke` and `runner.register_token`
  name the runner only).
- It lives **30 minutes**, works **once** (a conditional `pending -> used`
  update, so two racing installs cannot both register), and is bound to one
  runner name and one owner. Rows are swept a day after they expire.
- An owner may hold at most 5 unused tokens and make 20 an hour.
- The name is checked at mint time and again at registration: not a core
  (`RUNNER_POOL_TARGET_NODES`), not an account or claimed node name, and not
  another account's runner. Re-running the installer with a new token for the
  owner's own runner re-keys it.
- The runner proves it holds its key by signing
  `forkmesh-runner-register-token-v1\n<name>\n<publicKey>\n<sha256(token)>\n<ts>`
  (±5 minutes). The prefix differs from the session registration's
  `forkmesh-runner-register-v1`, so neither proof can be replayed as the other.
- What gets registered is always `scope: personal`, `allowed_owners: [owner]`,
  `registered_by: owner`. A token can never make a pool runner.

**The installer** (`www/public/install-runner.sh`, served by the www worker at
`https://forkmesh.com/install-runner.sh`) mirrors what `tools/next_fleet.py`
does for `fmnext-r1`, but beside anything already on the machine:

1. Checks the arguments first (token, name, `https://` app origin, providers)
   and changes nothing when one is wrong.
2. Installs `curl openssl python3 git nftables` when missing.
3. Creates the unprivileged `forkmesh-job` user (home
   `/var/lib/forkmesh-job`, 0700, no login shell) and
   `/var/lib/forkmesh-runner/{work,tmp,identity,gateway}`.
4. Gets `forkmesh-mirror-node`: `--binary`, else the one already in
   `/usr/local/bin`, else it runs `install.sh` with `FORKMESH_NO_LAUNCH=1` into
   a scratch folder, which applies the same signed-release verification as a
   node install, and copies only the Go daemon. It removes the files it
   fetched by name and never deletes a folder recursively.
5. Makes an Ed25519 key with OpenSSL (kept on a re-run), signs the proof and
   posts `register-token`. The owner comes back in the answer.
6. Writes `/etc/forkmesh-runner/runner.json` (`role: runner`, `jobUser:
   forkmesh-job`, `allowedOwners: [owner]`, loopback ports 8795/8796 so it can
   sit next to a mirror node on 8791) and the gateway identity document.
7. Installs `forkmesh-runner.service` (root, the same sandbox as r1's unit)
   and `forkmesh-runner-firewall.service`, which loads its own
   `table inet forkmesh_runner`: the job user may not reach loopback, RFC
   1918, CGNAT, link-local or IPv6 private ranges. It never flushes or edits
   the machine's own firewall.
8. Installs Node.js and the Claude Code CLI (`--providers claude,codex|none`).
9. Runs `forkmesh-mirror-node --check`, then starts the service.

It keeps every existing ForkMesh folder and deletes nothing; there is no
uninstall mode. The runner then checks in, shows up in the list and asks for
**Connect Claude**.

### 2. SSH details

**From ForkMesh Desktop (preferred).** **Open in ForkMesh Desktop** mints a
token (`via: desktop`) and opens
`forkmesh://control/cloudflare?mode=ssh&role=runner&vpsHost=…&vpsUser=…&node=<runner>&runnerToken=…&app=…`.
The desktop's Hosts panel fills in the server and runs the same install line
over its own SSH session. The password or key is typed into the desktop and
never reaches the browser or ForkMesh's servers. The one-time token travels
in the local `forkmesh://` URL. It is single-use, expires in 30 minutes and
can only register this one runner name for this account. The page polls it
the same way as the command path.

On the desktop (`MainWindowControlNode.cpp` parses the handoff with
`parseRunnerSetupHandoff`, `RunnerSetupHandoff.cpp`; `MainWindowHostInstall.cpp`
runs it):

- The handoff is taken only with `role=runner`, a well-formed token, a runner
  name and an `https://` app origin. The Hosts form gets the server, the user
  and the runner name, and the password field gets focus.
- **Install ForkMesh** then checks the desktop's own `known_hosts`. For a
  server it has never seen, it runs `ssh-keyscan`, shows the SHA-256
  fingerprints with the same "compare with `ssh-keygen -lf …`" advice, and
  goes on only after **Yes**; it then records those keys, so the SSH session
  (`StrictHostKeyChecking=accept-new`) refuses any other key. **Cancel**
  sends nothing.
- It runs the runner install line (`runnerInstallRemoteCommand`: root runs
  it directly; another user gets `sudo -S -v` with the password on stdin, or
  needs passwordless sudo with a key) instead of the node installer, uploads
  no binary, and never saves the machine as a saved host. A runner host in
  Hosts would be offered "Update all hosts", which installs a mirror node.
- The token is shown as `fmrt_[hidden]` in the install log and output.
  After a successful run the desktop forgets it.
- Tests: `desktop/tests/test_runner_setup_handoff.cpp` (target
  `forkmesh-runner-setup-handoff-tests`).

**From ForkMesh's servers (no desktop).** `app/src/runner_ssh_setup.py`
(table `runner_ssh_setups`):

| endpoint | purpose |
|---|---|
| `POST /api/runners/ssh/probe {host, port}` | resolve, refuse any non-public answer, read the host keys; `{id, address, hostKeys: [{type, fingerprint}], installs}` |
| `POST /api/runners/ssh/install {id, fingerprint, username, password \| privateKey [, passphrase], name}` | 202; mints a token and starts the install |
| `GET /api/runners/ssh/<id>` | `{state: probed\|running\|succeeded\|failed, message, log, registered}` |

Security design:

- **Where SSH runs.** Not in the app process. That process holds `DATA_KEY`
  and every unsealed secret, and an SSH client talking to a host the user
  names would be a new way in. The app hands the request to
  `fmnext-ssh-helper` (`app/host/forkmesh_host/ssh_helper.py`, stdlib only)
  over a Unix socket. The socket unit makes `/run/fmnext-ssh-helper.sock`
  mode 0600, owned by `forkmesh-app`, so only the app can connect. The
  helper runs socket-activated with `DynamicUser=yes`: no credentials, no
  environment file, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
  `ProtectProc=invisible`, no capabilities, `MemorySwapMax=0`, `LimitCORE=0`,
  and `IPAddressDeny` for loopback, link-local, multicast, RFC 1918, CGNAT
  and ULA. A compromised `ssh` process lands in an empty sandbox that cannot
  reach the core's private services.
- **Addresses.** The app resolves the name and refuses unless every answer
  is a public address outside the WireGuard range. The helper checks the
  literal IP again, and SSH connects to that IP (no second DNS lookup).
  Probes are limited to 20 an hour per account.
- **Host key, trust on first use.** The probe runs `ssh-keyscan` and shows
  SHA-256 fingerprints. The page tells the user to compare one with
  `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` and makes them tick "This
  is my server's host key" before it sends anything. The install writes only
  the confirmed key to a one-entry `known_hosts` and runs `ssh -F /dev/null`
  with `StrictHostKeyChecking=yes`, `HostKeyAlgorithms=<that type>` and
  `GlobalKnownHostsFile=/dev/null`. A different key aborts before
  authentication. A probe is good for 10 minutes and one install.
- **The password or key.** It is sent once, over HTTPS, in the install POST
  body. The page reads the fields and clears them before the request is
  sent, and clears them again on `pagehide`. The app never writes it to the
  database, the audit log or a response. It passes it once over the socket,
  then clears the request (`request.clear()`, `data.clear()`). In the
  helper:
  - a password reaches `ssh` only through `SSH_ASKPASS`: a script with no
    secret in it that prints `$FORKMESH_SSH_SECRET` from the child's own
    environment. It is never in argv, which any local user could read. For
    a non-root user it is written once to the remote `sudo -S -v`.
  - a private key is loaded into a private `ssh-agent` through `ssh-add -` on
    stdin, never onto disk. The agent is killed when the run ends.

  Output lines have the token and password replaced by `[hidden]` before
  they reach the app, and again before they are stored (the last 80 lines,
  8 KB). Nothing about the secret is logged, and the connection runs with
  agent, X11 and port forwarding off.
- **What runs.** Only `curl -fsSL <installUrl> | [sudo -n] bash -s --
  --token … --name … --app …`. Every part is checked against a pattern in
  the helper and shell-quoted. A key login as a non-root user needs
  passwordless sudo, and the helper says so (exit 12) instead of hanging.
  Since 2026-10-07 the helper has a second operation, `instance`, for the
  "Start your own instance" page: it runs `install-instance.sh` with its
  settings on standard input ([INSTANCES.md](INSTANCES.md), "Where SSH
  runs").
- **Limits.** A run is killed after 20 minutes. A row that stops reporting
  for 25 minutes turns `failed`. Rows are deleted after a day.
- **Rollout.** With no `RUNNER_SSH_HELPER_SOCKET` (for example before the
  core roll), the SSH endpoints answer `503 ssh_setup_unavailable`, and the
  page points to the desktop and the command.

What remains a risk: the password does transit ForkMesh's servers in this
mode (that is why the desktop path is first), Python strings cannot be wiped
from memory, and trust on first use only helps if the user compares the
fingerprint.

### Tests

- `app/tests/test_runner_setup.py`: mint, hash-only storage, single use,
  30-minute expiry, name/owner/proof binding, revoke, limits, status.
- `app/tests/test_runner_ssh_setup.py`: public-address checks, fingerprints,
  confirmation, one secret, no password/key/token in rows, audits, responses
  or the stored log, failure and stale states, 503 without the helper.
- `app/host/tests_unit/test_ssh_helper.py`: validation, quoting, pinned host
  key options, password only through askpass env and stdin (never argv),
  scrubbing, temp-file cleanup, keyscan parsing.
- `app/tests/test_install_runner_script.py`: runs the installer under a
  sandboxed `HOME`/`XDG_*` with `FORKMESH_HOME` dropped and
  `FORKMESH_RUNNER_TEST=1`/`FORKMESH_RUNNER_PREFIX`, against a local fake app
  that verifies the signed proof; checks the config, key mode, re-run key
  reuse and refusal handling.
- `app/tests/test_runner_setup_dashboard.py`, `test_runners_dashboard.py`:
  the page, the flows and the header chip.
- `deploy/tests/test_render.py`: the helper units' sandbox and the app's
  socket setting.

`runner_setup.py` and `runner_ssh_setup.py` are imported only inside
`runner_leases.handle`, so they are listed in `LAZY_MODULES`
(`world/tools/build_worker_footprint.py`) and stay out of the app's startup
source (the 1.73 MB Cloudflare soft limit they would have crossed was retired
on 2026-09-30, [app/worker-footprint.md](app/worker-footprint.md)).

## Provider credentials

### Migrating the provider login to the job user (one time, 2026-09-24)

Before this change, r1's jobs ran as `forkmesh-node`, and its Claude login is at `/var/lib/forkmesh/.claude/.credentials.json` (the home of `forkmesh-node`). After the installer has created `forkmesh-job` and switched the daemon to root with `jobUid`, run this as root on the runner:

```sh
systemctl stop forkmesh-mirror-node
install -d -o forkmesh-job -g forkmesh-job -m 0700 /var/lib/forkmesh-job /var/lib/forkmesh-job/.claude
install -o forkmesh-job -g forkmesh-job -m 0600 \
  /var/lib/forkmesh/.claude/.credentials.json /var/lib/forkmesh-job/.claude/.credentials.json
shred -u /var/lib/forkmesh/.claude/.credentials.json
# Codex, if it was signed in:
if [ -f /var/lib/forkmesh/.codex/auth.json ]; then
  install -d -o forkmesh-job -g forkmesh-job -m 0700 /var/lib/forkmesh-job/.codex
  install -o forkmesh-job -g forkmesh-job -m 0600 /var/lib/forkmesh/.codex/auth.json /var/lib/forkmesh-job/.codex/auth.json
  shred -u /var/lib/forkmesh/.codex/auth.json
fi
systemctl start forkmesh-mirror-node
journalctl -u forkmesh-mirror-node -n 20 | grep 'runner: jobs run'   # uid=<forkmesh-job uid> ip-filter=true
```

The dashboard's Runners page should then show Claude as signed in. It is simpler, and better, to sign in again from the website instead (**Connect Claude**), which writes to `/var/lib/forkmesh-job`. Then `shred -u` the old file.

The login is the operator's own Claude account. Every job of every listed owner uses it and can read it. For a pool that serves other people, use a dedicated provider account.

**Mode A (implemented).** The runner uses its own locally signed-in `claude` or `codex` CLI. Sign it in from the website (below), or on the host as the job user. The runner advertises only providers whose CLI is present and signed in, through `availability`, and it claims nothing when none is. It still polls (`/api/runners/poll`), so the website can sign it in.

A stored refresh token is not proof of a working login. See "Expired logins" below for how the runner and the app notice one and who is told.

### Expired logins: detection, alerts, and why r1's kept expiring

**What happened.** On 2026-09-27 and again from 2026-09-28 02:46 UTC every Claude Code job on `fmnext-r1` failed with "Failed to authenticate: OAuth session expired and could not be refreshed" (jobs 190-198). The runner kept checking in and kept reporting Claude as `available`, so nobody was told, and every queued job was claimed and failed within seconds.

**Why the r1 login kept expiring (read-only check on r1, 2026-09-29).**

- The credential is `/var/lib/forkmesh-job/.claude/.credentials.json`, owned by `forkmesh-job`, mode 0600, with `accessToken`, `refreshToken`, `expiresAt`, `refreshTokenExpiresAt`, `scopes` and `subscriptionType`. Its `expiresAt` was 2026-09-28 02:20 UTC, eight hours after the web sign-in of 2026-09-27 18:20; the refresh token was valid until 2026-10-25. The file's mtime was 2026-09-28 23:06:10, one second after job 198 ended.
- Jobs do not run with their session home (see the known gap in "Job sandbox"): `systemd-run --uid` resets `HOME` to `/var/lib/forkmesh-job`. The CLI therefore reads, refreshes and rewrites the persistent credential file in place. The copy the daemon put into the session home is never used.
- Job 189 ran from 01:58 to 02:29 UTC on 2026-09-28, across the access token's expiry. Claude Code refreshed it: Anthropic rotates the refresh token on every refresh, and the CLI wrote the new pair into the persistent file. When the job ended, `closeSession` compared the session home's untouched copy (the old pair) with the persistent file, saw that they differed, took that as "the CLI refreshed the copy", and wrote the old pair back over the new one.
- The old refresh token had already been used. Job 190 (02:46) found the access token expired, tried to refresh with the used refresh token, and was refused: "could not be refreshed". Every later job did the same, and each one's close rewrote the file again (hence the 23:06:10 mtime).
- The fix is on our side: copy-back now happens only when the job changed its own copy, and never over a newer credential (`copyCredentialBack` in `server/jobhome.go`, `credentialHandedOut`). Concurrent jobs sharing the persistent file rely on the CLI's own locking; r1 runs one job at a time (`maxConcurrentRuns: 1`). The daemon never refreshes or copies tokens itself.
- The deployed binary on r1 (built 2026-09-27 22:28) predates the in-memory "expired" mark (2026-09-29 01:12 UTC). That mark was keyed on the file's mtime and size, which this same copy-back rewrote after every failed job, so it would have cleared itself after each failure anyway.

**Runner side (`server/runnerauth.go`).**

- **Classification.** A run that fails with an authentication error from the CLI (`providerAuthFailed`: "Failed to authenticate", "OAuth session expired", "could not be refreshed", "API Error: 401", "authentication_error", "Invalid API key", "Please run /login", "invalid_grant", Codex's "refresh token was already used", `refresh_token_reused`, "Provided authentication token is expired", "Please log out and sign in again", and similar) is recorded as `auth_expired`. Only the run's own error text is matched, not the agent's final message, so a job that merely talks about logins is not caught. The session's `lastError` starts with "Claude Code login on this node expired and could not be refreshed. Reconnect it from the Runners page." and the report carries `failureKind: "auth_expired"` (`login_missing` when there is no login at all).
- **needs_login.** The failure is stored in `<agent-jobs>/auth.json`, keyed by a hash of the credential's tokens, not its mtime, so it survives a restart and a rewrite of the same tokens. While it stands, `availability` reports `loginState: "expired"` and the runner **stops leasing** that provider's jobs. It keeps polling, so the website can sign it in. A new sign-in, or a refresh that changes the tokens, clears it. At the first start with this code the daemon also reads its job history: when the newest finished job of a provider failed with an auth error and the credential file has not changed since (a two-minute margin for the old copy-back), it starts out in needs_login, so r1 raises the alert as soon as it runs this build.
- **Why stop leasing rather than lease and fail fast.** A leased job that fails at once still becomes a failed session the user has to notice and resubmit, and a whole queue would burn through in seconds. Unleased jobs stay queued and run once the login is back, and the app refuses new ones with the reason at submit time.
- **Proactive check.** Every claim and poll (about every 20 s, and at startup) the daemon reads the credential file; this is non-billable and makes no network call. It reports `auth: {"claude-code": {state, checkedAt, expiresAt, accessExpiresAt, hasRefresh, reason, message, failedAt, failedJob}, "codex": {...}}` at the top of the claim/poll body and inside each provider's `availability`. `state` is `ok`, `expiring`, `expired` or `missing`:
  - with a refresh token, an expired access token is `ok` (the CLI refreshes it); `refreshTokenExpiresAt` in the past is `expired` (`refresh_expired`), within 24 hours `expiring` (`refresh_expiring`);
  - without a refresh token, the access token's expiry (Claude's `expiresAt`, Codex's JWT `exp`) decides: past is `expired`, within 24 hours `expiring` (`no_refresh_token`);
  - a recorded job failure makes it `expired` (`job_failed`);
  - a Claude login whose `accessToken` and `refreshToken` are both empty strings is `expired` (`refresh_revoked`), not `missing`: that is what Claude Code writes when a refresh answers `invalid_grant` (see "Why switching accounts broke r1's login" below);
  - an API key (environment or `OPENAI_API_KEY` in `auth.json`) is `ok`.

  A refresh token the provider has revoked cannot be seen in the file. The first job that tries to use it marks the runner. With the copy-back fixed, that should no longer happen through our own doing.

**App side (`app/src/runner_auth.py`, `runner_logins.py`, `runner_leases.py`).**

- `runner_availability.data` keeps each provider's `auth` (cleaned: known states, a bounded message, no other fields). A runner that predates it is caught from its job report: a failed report whose `failureKind` is `auth_expired`, or whose result, reason or `lastError` reads like an auth failure, marks that provider `expired` with reason `job_failed_report`. That mark survives the old runner's next poll, which still says `available`, until a web sign-in for that runner and provider succeeds or a job of that provider completes. While a provider is marked expired the claim does not lease its jobs, even to an old runner.
- **Ping.** When a provider turns `expired` or `expiring`, every site admin (`users.is_admin=1`, no service accounts) and, for a personal runner, its registrant get an `operational_alert` Ping: "fmnext-r1: Claude login expired — reconnect", linking to `/dashboard/runners?runner=<name>&provider=<provider>`. The notification dedupe key is `runner-auth:<runner>:<provider>:<state>:<episode>`, where the episode is the failure time, else the expiry time, else the UTC day. A repeated report of the same state sends nothing; a new failure after a reconnect sends a new Ping. `operational_alert` Pings already show on the web, in World, on the desktop and on both phones, so no client change was needed.
- **Email.** A pool runner's expired login turns its `/status` row red, and the existing status-transition alert sends the admin email (`status_alert`, subject to the per-monitor email switch). A personal runner is not on `/status`, so its expired login is emailed directly (`email_kind="operational"`). An `expiring` warning is a Ping only.
- **/status.** A fresh pool runner with an expired provider login is red: "fmnext-r1: Claude login expired — reconnect" (`_status_runner_verdicts`, check "Provider logins").
- **Runners page.** A red banner above the list names each runner and provider whose login expired, with a one-click **Connect Claude** / **Connect Codex**. An expiring login shows an amber line with the expiry time. The provider row reads "Login expired — reconnect".
- **Submit time.** When the only pool runner for a provider needs a login, the owner's start action (web, and signed desktop/phone requests) is refused with `409 runner_login_expired`: "Runner fmnext-r1 needs Claude reconnected. An admin can reconnect it from Runners (Connect Claude); the job was not queued." (`requiredAction: "reconnect_runner_login"`, `targetNode` the runner). The web shows the message; iOS and Android show the server's `message` of a refused start.

Session lists mark a run sent to a runner with `onRunner: true`: its record names a runner other than the core it is queued on (`runnerNode`), or it was created on the `runner_pool` route (`route`, stored since 2026-09-30). The desktop leaves those runs alone. Before this, a desktop with a checkout of the repository moved every cloud run into a local agent, so a prompt sent from a phone to `fmnext-r1` ran on the desktop instead (`docs/desktop/agents-ui.md`).

### Web sign-in

An admin opens **Runners** in the dashboard sidebar (`/dashboard/runners`). Since 2026-09-29 the link is in every account's sidebar: the page lists every runner to an admin and a personal runner to its registrant, and anyone can set one up ("Set up a runner"). Each runner shows when it last checked in and, per provider, whether the CLI is installed and signed in, from the availability snapshot the runner sends with every claim or poll. **Connect Claude** / **Connect Codex** starts a sign-in:

1. **Session.** `POST /api/runners/<name>/login {provider}` creates a `runner_login_sessions` row: `pending`, 10-minute TTL, at most one `pending|running` session per runner and provider (a partial unique index; a second start answers `409 login_active` with the open session's id, which the page re-attaches to).
2. **Hand-off.** The runner's next claim or poll answers with `logins: [{id, provider, ttlMs}]` and marks them `running`. While any session of the runner is open, the answer also carries `fastPollMs: 1000`, and the runner polls every second instead of every ~20 s. A new sign-in ends a held claim, so it starts within about a second. The runner lists its running sessions in `activeLogins`; a `running` session it does not list (it restarted, or never got the answer) is handed over again.
3. **Login process.** The runner runs the provider's own login command on a pty, **as the job user, inside the job sandbox** (`jobSandbox.argv`: the same `systemd-run --scope [--user] -p MemoryMax=…` or rlimit fallback, `--uid` when `jobUid` is set), with the job user's `HOME` and with `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN` and `OPENAI_API_KEY` removed:
   - Claude Code: `claude auth login`. It prints an authorize URL, waits for the pasted code, and stores the OAuth credential in `~/.claude/.credentials.json`, which every later `claude -p` job and the runner's `availability` read. (`claude setup-token` only prints a long-lived token and stores nothing, so it is not used.)
   - Codex: `codex login --device-auth`. It prints a URL and a one-time code to enter there, then polls and writes `~/.codex/auth.json`. The default `codex login` waits for a callback on the runner's own `localhost`, which the admin's browser cannot reach.
4. **Relay.** The runner posts the screen to `POST /api/runners/login/output` in numbered chunks of at most 8 KiB, once a second or as soon as 8 KiB is buffered. The answer carries the session state and any queued input, which the runner writes to the pty. The page polls `GET …/output?after=<seq>` every second, strips terminal escapes, and turns https URLs into links. The code typed into the page goes to `POST …/input`: 1–4096 bytes of printable text plus tab/CR/LF, at most 16 unread chunks.
5. **End.** The session ends when the CLI exits (`succeeded` on exit 0, else `failed`), on **Cancel** (`cancelled`), or at the TTL (`expired`; both the app and the runner enforce it). On cancel or expiry the runner sends SIGHUP, then kills the whole process group 3 s later. It then polls at once, so the runners list shows the new availability.

**From a failed agent session.** When a web agent session failed with a provider login error (the session's diagnostic code is `provider_credential_missing`, or its error reads "OAuth session expired", "Failed to authenticate", "Please run /login", "not logged in", "login was not found", "Invalid API key" or "authentication_error"), the session detail on the Agents page shows **Sign Claude Code in again** (or Codex) under the error. A Gemma (`local-llama`) session never shows it: it has no CLI to sign in. It links to `/dashboard/runners?runner=<targetNode>&provider=<claude-code|codex>` (`orgAgentNeedsRunnerSignIn()` and `orgAgentRunnerSignInUrl()` in `06-repo-content.js`). The Runners page picks the runner the way the desktop's `runnerForTarget()` does (the one named, else the only active runner, else the only one whose CLI for that provider needs a sign-in), rings its card and focuses its sign-in button; it does not start the sign-in by itself, so reloading the page opens nothing. A link with `&connect=1` (the attention chip, the World's Connect buttons, `connectUrl` of `GET /api/runners/attention`) does start it, or attaches to the sign-in already open, and then removes `connect=1` from the address so a reload starts nothing. When the named runner is not one this account may manage, the page says so: a core mirror's CLI, or a runner someone else registered, is signed in by an administrator or its registrant.

**From the start form (2026-10-06).** The "Sign in to Claude Code on <node>" button under a target (web start dialog, iOS and Android start card, and the phones' quick-launch fix) used to stream the node terminal (`node_terminal.py`, `server/terminal.go`) for every target. That terminal travels over the node event channel, and a runner has none. So on a runner target (`runtimeMode: "runner-pool"`) the dialog sat on "Connecting to fmnext-r1…" and nothing ever answered. Now these buttons check the target first:

- **Runner targets** use the runner sign-in described above. The button names `target.runner`. On the pool target, `node` can be the core when no runner is ready, so `node` is used only when no runner is named.
  - The web opens `/dashboard/runners?runner=…&provider=…&connect=1` in a new tab. The dialog and the typed prompt stay open, and the dialog hides the desktop hand-off link.
  - iOS opens `RunnerSignInSheet`, through `RunnerAttentionModel.open(runner:provider:)` from the quick-launch fix `QuickLaunchFix.signInRunner`, and from the start card's own sheet.
  - Android opens `RunnerSignInScreen` (`AgentsFeature.openRunnerSignIn` / `openSignIn`, `QuickLaunchFix.SignInRunner`).
  - The start form offers the button for the providers in the target's `needsLogin`, because a runner's `providers` lists only signed-in CLIs. The web dialog also lists such a runner, marked "needs sign-in", where it used to leave it out.
- **Every other target** still gets the node terminal. On the web, the terminal now starts its "did not answer" timer (`OPEN_TIMEOUT_MS`, 9 s) as soon as it starts. Before, the timer started only once the event channel connected, so a channel that never connected showed "Connecting…" forever.

Tests: `testAnExpiredRunnerOffersTheRunnerSignInNotTheNodeTerminal` and `testRunnerTargetsReadTheRunnerAndItsExpiredLogins` in `QuickAgentLaunchTests.swift` and `QuickLaunchTest.kt`, plus the runner target in `app/browser_tests/tests/dashboard-owner-agent-launch.spec.js`.

**The phones re-check before saying "no Claude Code signed in" (2026-10-06).** After a successful sign-in on fmnext-r1 (runner log 07:46 and 07:48 UTC, `runner_availability` already `available`), the iPhone composer still answered "fmnext-r1 has no Claude Code signed in." The quick launcher (`QuickAgentLauncher` on iOS and Android) fetched the repository's launch options once per app session and decided from that copy forever; the server was right, the phone was stale. Now:

- Before a send reports a sign-in fix (the chosen node lacks the provider, or nothing can take the run), the launcher fetches the options again and decides from the fresh answer.
- Opening the composer re-reads options older than 15 s (`optionsFreshFor` / `OPTIONS_FRESH_MS`), also when it opens on a repository page.
- On iOS, closing the runner sign-in sheet refreshes the launcher at once.
- `targetNamed` also matches a runner target by its `runner`, then the pool target (no `runnerPin`) by its `queueNode`. The pool target's `node` is the core (`fmnext-n1`) while no runner is ready and the runner (`fmnext-r1`) once one is, so a remembered destination keeps working across a sign-in; the server already accepts either name (`queueNode` in the launch route).

Tests: `testARunnerSignedInAfterTheOptionsLoadedTakesTheSendWithoutAFix` and `testOpeningTheComposerAgainRereadsOptionsOnceTheyAreOld` in `QuickAgentLaunchTests.swift` and `QuickLaunchTest.kt`.

**Security model:**

- **Credentials stay on the runner.** The login writes into the job user's persistent `HOME` (`/var/lib/forkmesh-job`), and each job gets a copy (see "Job sandbox"; in practice the job's CLI uses the persistent file itself, see the known gap there). The app never receives, stores or relays the credential. It relays only the terminal.
- **Token-shaped text is scrubbed** before it leaves the runner (`sk-ant-…`, `sk-…`/`sk-proj-…`, three-part JWTs), including a token split across pty reads (`tokenScrubber` holds a trailing token-like run until the next read or a quiet second). The app scrubs every chunk again. `claude auth login` and `codex login --device-auth` do not print tokens; this is defence in depth, and a token deliberately wrapped over several lines by a CLI could still leak its tail.
- **Admin-only.** Only app admins may start, read, type into or cancel a sign-in on a pool runner. The registrant of a personal runner may do so for that runner. Account sessions come from the cookie (a POST must be same-origin) or `Authorization: Bearer`. Starts and cancels are audited (`runner.login.start`, `runner.login.cancel`); chunk content is never logged or audited.
- **One command.** The runner accepts only a provider id; the command and its arguments are fixed in `runnerLoginCommand`. No shell, and nothing from the app reaches the command line. Input only goes to that pty.
- **Bounded and short-lived.** Sessions expire after 10 minutes. The app keeps only the newest 64 output chunks, deletes input chunks as soon as the runner confirms it has read them (and all input when a session ends), deletes all chunks of a session 2 minutes after it ends, and deletes the session row a day later.
- **Runner requests** use the claim signature scheme: node key, body hash, single-use request id, ±60 s. A runner can read or write only its own sessions.

### Runner attention: `GET /api/runners/attention` (2026-09-29)

One small read that every client (web dashboard, World, desktop, iOS,
Android) polls to show "fmnext-r1 needs Claude sign-in" and take the user
straight into the sign-in. It lists only runners the caller may sign in,
which is the same rule as `GET /api/runners` and `POST .../login`: every
active runner for an admin, the personal runners they registered for anyone
else. A caller with no such runner gets an empty list, never a 403.

```json
{
  "ok": true,
  "items": [
    {
      "runner": "fmnext-r1",
      "provider": "claude-code",
      "providerLabel": "Claude",
      "state": "expired",
      "scope": "pool",
      "canConnect": true,
      "online": true,
      "lastSeenAt": 1790735909692,
      "expiresAt": 0,
      "title": "fmnext-r1 needs Claude sign-in",
      "connectUrl": "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1",
      "loginId": ""
    }
  ],
  "count": 1,
  "expired": 1,
  "runners": 1,
  "checkedAt": 1790736000000,
  "pollMs": 300000
}
```

- `items` holds one entry per runner and provider whose login state
  (`runner_auth.state_of`, the heartbeat's `auth` field or the app's
  `job_failed_report` mark) is `expired` or `expiring`. A provider whose CLI
  is not installed (`binaryFound: false`) is never listed. Expired entries
  come first.
- `state` is `expired` (jobs for that provider stop; show it red) or
  `expiring` (within a day; amber). `title` is ready-made copy.
- `connectUrl` opens the dashboard Runners page, focuses that runner and,
  because of `connect=1`, starts **Connect Claude/Codex** at once (or
  re-attaches to the open sign-in). `loginId` is the open sign-in's id when
  one is already running, so a native client can attach to
  `/api/runners/<runner>/login/<loginId>/output` without starting another.
  `canConnect` is always true today: the list only has runners the caller may
  sign in; the field is there so a later version can add read-only entries.
- `online` is a check-in within 10 minutes; `lastSeenAt` is in ms.
- `pollMs` is the suggested poll interval (5 minutes). Poll on load, on
  focus, and then every `pollMs`; there is no push. The response is
  `Cache-Control: private, max-age=60` with `Vary: Cookie, Authorization`, so
  a burst of tabs costs one query pair.
- The shape is stable: fields may be added, none will be renamed or removed.

Cost: one `runner_nodes` join `runner_availability` query, plus one
`runner_login_sessions` query only when the caller has runners. No sweep
runs here (the Runners list and sign-in calls still sweep).

### Several accounts per provider, and usage limits

Built 2026-09-27. A runner keeps any number of Claude Code and Codex accounts (up to 12 per provider) and runs every job with the **active** one. An admin (or the registrant of a personal runner) switches between them from the Runners page or the desktop, and each account shows its current usage limits. The case this is for: an account whose OAuth session died ("OAuth session expired and could not be refreshed"), or one that has hit its 5-hour or weekly limit, can be swapped out without signing the runner out of it.

**Where the logins live (`server/runneraccounts.go`).**

- The daemon keeps every account's credential in `<workRoot>/.accounts/<provider>/<id>.json`, with the list in `state.json` (`{active, accounts: [{id, label, email, subject, plan, addedAt}]}`). The directory is daemon-owned with mode 0700, so the job user, and every job, can read only the active account, never the others. Before this change the job user's home held the only login. Now an inactive account is out of reach of agent code.
- The active account's credential is still at the job user's usual path (`.claude/.credentials.json`, `.codex/auth.json` in `/var/lib/forkmesh-job`), so jobs, `availability` and the provider CLIs work as before. **Switching** copies the chosen account's saved credential there.
- **Import.** On every claim or poll the daemon compares that file with the active account's saved copy. A runner with no saved accounts imports its existing login as the first one, so a runner signed in before this change keeps working without any action. A file that changed some other way (for example the operator ran `claude auth login` on the host) is filed under the account it belongs to, or saved as a new one when it belongs to none. A Claude login is matched by the `oauthAccount` in the job user's `~/.claude.json`, but a switch replaces only `.credentials.json`, so after a switch that file still names the account switched away from. The daemon records that name as `staleSubject` in `state.json` when it installs an account, and a change seen while `~/.claude.json` still names it is filed under the active account. A sign-in clears it. A credential with no token (Claude's cleared `{"accessToken":"","refreshToken":""}`, an empty Codex file) is never imported, saved, switched to or copied back.
- **Sign-in adds an account.** Before a web sign-in starts, the current login is saved to its account. While the sign-in runs that provider is not synced, and when it ends with exit 0 the new credential becomes an account and the active one. Signing the same account in again (the same Claude account UUID and organisation from `~/.claude.json`'s `oauthAccount`, or the same Codex `account_id`/email from `auth.json`'s `id_token`) renews that account's saved login instead of adding a duplicate. Otherwise it adds a new one (up to 12; after that it replaces the active one). A sign-in that fails and leaves no login behind gets the previous active login back.
- **Refreshes go back to the account that was used.** A job records which account was active when its session home was opened. When the job ends, a credential the CLI refreshed is written to *that* account's saved copy, and to the job user's active file only if that account is still active. So a job that was running when an admin switched accounts cannot switch the runner back, and it cannot lose the inactive account's rotated refresh token.
- **Remove** deletes the saved copy. If the removed account was active, the next saved account becomes active; if none is left, the active file is deleted too and the runner reports the provider as not signed in.
- Labels are the account email when it is known; otherwise `Account <n>`. `subject` (the identity key) is never sent to the app.

**Switch and remove (app).** `POST /api/runners/<name>/accounts {provider, account, action: "activate"|"remove"}` (admin, or the registrant of a personal runner; audited as `runner.account.activate` / `runner.account.remove`) stores a `runner_account_requests` row, `202`. The account must be one the runner last reported (`404 unknown_account`). At most 8 requests per runner may be pending at once (`429`). The next claim or poll hands the pending requests over as `accountRequests: [{id, provider, action, account}]`, and asks for fast polls (1 s) while any is pending. The runner applies each request once (it remembers request ids for 15 minutes) and reports `accountResults: [{id, ok, message}]` on its following polls for 2 minutes. The app marks the request `done` or `failed`, and only for the runner that holds it. A request the runner does not apply within 10 minutes is marked `failed`; finished rows are deleted after a day. `GET /api/runners` lists each runner's pending and recently finished `accountRequests`.

**Why switching accounts broke r1's login (2026-10-06).** This is reconstructed from r1's journal and the CLI's code, because the daemon does not log account switches. At 01:45 UTC a job failed with "You've hit your weekly limit", and the admin switched r1 to its second Claude account. The 01:52 job ran on it, and the CLI refreshed the token in place (jobs still use the persistent file; see the known gap in "Job sandbox"). The next sync matched the refreshed login by `~/.claude.json`, which still named the first account. So the daemon saved the second account's new token in the first account's slot and made the first account active again. The second account's slot kept the refresh token the CLI had just used. Switching back to it installed that used token, the 02:13 and 02:19 jobs got `invalid_grant`, and Claude Code blanked the tokens in `.credentials.json` (`tengu_oauth_refresh_token_cleared_on_disk` in the 2.1.x CLI). The daemon reported "not signed in", and the next sync saved the blank file over a slot as well. The fix is `staleSubject` and the token checks above. The runner now reports a blanked login as `expired`/`refresh_revoked`, which asks for a reconnect. A blanked account cannot be repaired from r1's saved copies: after this build is deployed, sign it in again once (Connect Claude), which renews its slot.

**Usage limits.** The daemon reads each account's current limits from the provider with the account's own OAuth access token. These are the endpoints the CLIs' own `/usage` and `/status` screens use:

| provider | request | reported windows |
|---|---|---|
| Claude Code | `GET https://api.anthropic.com/api/oauth/usage`, `Authorization: Bearer <accessToken>`, `anthropic-beta: oauth-2025-04-20` | `five_hour` (5-hour session), `seven_day` (weekly, all models), `seven_day_opus`, `seven_day_sonnet`, and any other `five_hour*`/`seven_day*` window: `utilization` % and `resets_at` |
| Codex | `GET https://chatgpt.com/backend-api/wham/usage`, `Authorization: Bearer <access_token>`, `ChatGPT-Account-Id` | `rate_limit.primary_window` / `secondary_window`: `used_percent`, `limit_window_seconds`, `reset_at`, plus `plan_type` |

- A reading is at most 5 minutes old. It is refreshed at most once a minute after a job of that account ends, or after the account is switched to or signed in. The requests run in the background of the poll loop, never inside a claim.
- The daemon **never refreshes a token itself**. A refresh rotates the refresh token, and the CLI owns that. An account whose access token has expired (Claude's `expiresAt`, or the Codex JWT's `exp`) keeps its last reading, marked `token_expired`, until a job of that account refreshes it. That is usually the case for an inactive account after about 8 hours.
- `unauthorized` (HTTP 401/403 with a token that has not expired) means the provider revoked the login, and the page asks for a new sign-in. A login made with an API key (`OPENAI_API_KEY`) has no subscription limits (`no_oauth_login`).
- The token goes only to the provider's own API. The runner sends the app the windows (`id`, `label`, `usedPercent`, `resetsAt`, `windowMinutes`), the plan, the reading time and an error code, inside each account of the availability snapshot (`availability[provider].accounts[].usage`). The app keeps only those fields, bounded.

**Website.** Under each provider, the Runners page lists the accounts: the label, the plan, **Active**, a bar per usage window (red from 90 %) with "resets in …", and how old the reading is. **Use this account** and **Remove** (after a confirm) post a request, and the page re-reads the list every 2 s while one is pending. The provider button reads **Add account** once there is one.

The dashboard **Agents** page (`/dashboard/agents`) has the same switch next to the agent list (2026-10-01), as the desktop has it next to its own accounts. A **Runner accounts** section, shown only when `GET /api/runners` lists an active runner with the provider's CLI installed or with saved accounts, has one row per runner and provider: a select of the accounts (label, plan and each usage window's percentage, the active one selected), **Switching...** while a change is pending, and **Add account** (or **Connect Claude/Codex**), which opens the Runners page's sign-in for that runner (`?runner=…&provider=…&connect=1`). Choosing an account posts `activate` to the same endpoint and re-reads the list every 2 s while a change is pending, at most 30 times; a request posted from the page that the runner reports `failed` shows its message on the row. The code is `renderRunnerAccountsPicker` and `switchRunnerAccountFromPicker` in `09-runners.js` (core bundle, so the lazy Agents chunk imports them), loaded by `loadAgentsPage`; `app/browser_tests/tests/dashboard-agents-page.spec.js` covers it.

**Phones.** The iOS and Android apps have a **Runner accounts** screen (2026-10-01), reached from the desktop usage card (on the iPhone since 2026-10-09 the usage chip's sheet holds the whole screen, and the Agents list has no separate row). It lists the same runners and accounts with the active one checked, switches with the same `activate` request and re-read rule, and opens the existing runner sign-in for **Add account**. Since 2026-10-06 the iPhone screen also shows usage bars, offers **Log in again…** and **Log out…** (the `remove` request) per account, and opens a runner terminal (below); Android still offers neither remove nor the terminal. See [native/RUNNER_AGENTS.md](native/RUNNER_AGENTS.md#runner-accounts-on-ios-2026-10-01).

**Desktop.** The Runners table shows the active account and its usage in the provider cell ("Signed in · one@example.com (1 of 2)", then "5-hour session 42% · Weekly 12%"). The tooltip lists every account with its usage, reset times and pending or failed changes. A **Claude accounts** / **Codex accounts** menu button offers **Use …** and **Remove …** (with a confirm). Both post to the same endpoint (`MainWindow::requestRunnerAccountChange`), and the list is read again every 2 s while a change is pending (at most 30 times).
The provider usage meter's account menu (click the Claude or Codex chart) has the same switch under **Runner accounts**, next to this computer's own accounts; see [desktop/agents-ui.md](desktop/agents-ui.md).

**Security notes.** The daemon (root) reads a credential only from its own 0700 store or, with the same `O_NOFOLLOW`/owner/size/JSON checks as the job hand-over, from the job user's home. It writes the job user's file only with `writeOwnedFile` after `safeDirChain`. The app still never sees a credential. What it stores per account is the label, email, plan and usage figures, visible to the same people who may sign the runner in.

### Runner terminals (2026-10-06)

An admin (or the registrant of a personal runner) can open the provider CLI itself, `claude` or `codex` with no arguments, in a terminal on the runner, from the iPhone's **Runner accounts** screen. It runs as the runner's active account, so it is the same login, limits and files that the runner's jobs use. A runner has no node event channel, so the terminal travels over the web sign-in relay above, as a second kind of session.

- **Session kind.** `POST /api/runners/<name>/login {provider, kind: "terminal", cols, rows}` creates a `runner_login_sessions` row with `kind='terminal'` (migration 0236; `kind` defaults to `login`). The size is clamped to 20–300 columns and 5–120 rows (100 × 40 when not given). At most one open terminal per runner and provider: a second start answers `409 terminal_active` with the open one's `id`, which the phone attaches to. A terminal and a sign-in for the same provider may be open together (the unique index is now over `runner, provider, kind`). Starts and cancels are audited as `runner.terminal.start` / `runner.terminal.cancel`.
- **Only runners that can.** A runner built with this change sends `features: ["terminal"]` with every claim or poll; the app keeps it in `runner_availability.features` and lists it as `features` in `GET /api/runners`. A start for a runner that does not report it answers `409 terminal_unsupported`, and a pending terminal is never handed to a runner that did not report the feature in that request. An older runner ignores `kind` and would otherwise have run a sign-in.
- **Hand-off.** `logins[]` in the claim/poll answer now carries `kind`, and for a terminal `cols`/`rows`. The runner runs `runnerTerminalCommand` (fixed: `claude`, `codex`, no arguments, nothing from the app on the command line) on a pty in the job sandbox, as the job user with the job user's `HOME`, exactly like a sign-in, but without the sign-in's account bookkeeping (`beginLogin`/`endLogin`): a terminal never adds or saves an account. A login changed inside it (for example `/login`) is imported by the usual sync.
- **Size.** `POST …/login/<id>/resize {cols, rows}` stores a new size; the runner's next `login/output` answer carries `cols`/`rows`, and the runner resizes the pty before writing the input of that answer.
- **Input.** A terminal accepts any text but NUL (escape sequences, Ctrl-C, DEL), 1–4096 bytes per chunk, at most 64 unread chunks (a sign-in keeps printable text and 16). The runner leaves 30 ms between two input chunks it writes from one answer, so a line and the Enter sent after it as its own chunk reach the CLI as two reads: sent together, a CLI may take the Enter as part of a paste.
- **Lifetime.** 60 minutes (`TERMINAL_TTL_MS`, `runnerTerminalMaxTTL`), then `expired` with "The terminal reached its 60-minute limit."; the CLI's exit ends it as `succeeded`/`failed`; closing the phone's screen cancels it. A terminal that exits 0 does not count as a sign-in, and terminals are left out of `logins[]` in `GET /api/runners` and of the attention endpoint, so older clients do not show a terminal as an open sign-in; open terminals are listed as `terminals[]`.
- **Latency.** Output and input still ride the 1 s relay, so a key shows up after one to two seconds. The phone's line box sends a whole line, then Enter.

Tests: `TestRunnerTerminalRunsTheCLIAndFollowsTheSize` and `TestRunnerLoginRefusesAnUnknownKind` in `server/runnerlogin_test.go`; `test_terminal_*` in `app/tests/test_runner_logins.py`.

### Desktop app integration

Built 2026-09-27 (`desktop/`), with no new endpoint. The desktop sends its account session as `Authorization: Bearer <token>` (`MainWindow::accountSessionTokenForUrl`, as the cloud-agent list requests in `MainWindowAgentCloud.cpp` do). Bearer requests skip the same-origin check that cookie POSTs get, so the desktop may start, type into and cancel a sign-in; the admin/registrant rule still applies. A desktop signed in only with its account key (no session token) cannot use these endpoints, and the Runners list says to sign in.

| desktop code | endpoint | behaviour |
|---|---|---|
| `refreshRunners()` (`MainWindowHosts.cpp`), rows by `fillRunnersTable()` (`RunnerListView.cpp`): the **Runners** list under the saved hosts on Network → Hosts | `GET /api/runners` | `parseRunners()` (`RunnerLogin.cpp`): `availability[provider].loginState == "available"` is **Signed in**, `loginState` or `auth.state` `expired` is **Login expired — reconnect**, an installed CLI otherwise **Needs sign-in**, `binaryFound: false` **Not installed**; open `logins[]` show as "sign-in open"; `accounts[]` and `accountRequests[]` feed the account menu (see "Several accounts per provider") |
| the **Claude accounts** / **Codex accounts** menu | `POST /api/runners/<runner>/accounts {provider, account, action}` | `requestRunnerAccountChange`: **Use …** is `activate`, **Remove …** (confirmed) is `remove`; the list is re-read every 2 s while a change is pending |
| `openRunnerAgentLoginTerminal(runner, provider)`: the `hostAgentLoginDialog` that `openHostAgentLoginTerminal` also builds (`buildAgentLoginDialog`) | `POST /api/runners/<runner>/login {provider}` | `RunnerLoginSession::applyStartReply`: `201` starts, `409 login_active` attaches to `id` from the body; a session already listed as open is attached without a POST. One dialog per runner and provider |
| a 1 s `QTimer` in that dialog | `GET /api/runners/<runner>/login/<id>/output?after=<seq>` | `applyOutputReply` base64-decodes `chunks[]` in `seq` order, skips chunks already read or not valid base64, and feeds them to the terminal view. `truncated: true` prints a "[older sign-in output was trimmed]" line; `more: true` reads again at once. Polling stops on any state other than `pending`/`running`, and on 401/403/404 |
| the code box under the terminal, and keys typed into the terminal | `POST …/input {data}` | `runnerLoginInputValid`: 1–4096 UTF-8 bytes of printable text plus tab/CR/LF. Enter sends the line followed by `\r`. Keys are edited locally (Backspace, Ctrl-U) and sent as one line on Enter, because the app refuses DEL, arrows and other escapes; paste is not bracketed in this mode |
| **Cancel sign-in** | `POST …/cancel` | also sent when the dialog is destroyed while the session is still `pending`/`running`, and when the start answer arrives after the dialog was closed |

`TerminalWidget` has a remote mode for this: `setRemoteInput(true)` makes keystrokes and pastes emit `inputTyped(bytes)` instead of writing to a pty, and the public `feed(bytes)` draws received output. `signInUrlDetected` then opens the provider's page in the browser, as for SSH sign-ins: the Claude authorize URL (once its `code_challenge` has arrived) and, new for Codex's device flow, `https://auth.openai.com/codex/device`.

When a sign-in ends the dialog stops polling and the Runners list is read again. The Agents page opens the same dialog from a cloud run that failed on a runner's login (**Sign in runner**, and **Retry in cloud**, which queues the run again once the sign-in succeeds); see [agents/README.md](agents/README.md#continuing-retrying-and-unblocking-ended-cloud-runs).

Tests: `desktop/tests/test_runner_login.cpp` (target `forkmesh-runner-login-tests`) covers parsing, request URLs and bodies, input limits, chunk ordering and decoding, start/attach/cancel state, the Runners table rows and buttons, and the terminal's remote mode, all against fake payloads.

### Sign-in needed indicator: desktop and phones (2026-09-29)

A runner whose provider login expired stops leasing that provider's jobs (see "Expired logins"), so every client that the owner or an admin keeps open now shows it until the runner reports signed in again, and one click or tap starts the sign-in. The wording is the server's `title` from `GET /api/runners/attention` (added on `feature/composer-runners-channels`, commit 6235e304e; see "Runner attention" there): **fmnext-r1 needs Claude sign-in** (red, `expired`) and **fmnext-r1: Claude sign-in expires soon** (amber, `expiring`). The longer line is the web banner's: "fmnext-r1: Claude login expired — reconnect. Jobs for Claude Code are not running on this runner."

**Where the list comes from.** Each client reads `GET /api/runners/attention` with its account session (`Authorization: Bearer`). The parsers accept `items[]` (the served shape), and also `attention[]`, grouped `{runner, providers: [...]}` entries and whole runner rows, so a change of shape on that branch does not blank the indicator. When the endpoint answers 404 or 405 (a core that does not serve it yet), the client reads `GET /api/runners` for 30 minutes instead and derives the same list as the dashboard's red banner (`renderRunnersAuthBanner` in `09-runners.js`): active runners only, a CLI that is installed, `loginState == "expired"` or `auth.state == "expired"` is `expired`, a signed-in login with `auth.state == "expiring"` is `expiring`, and the open sign-in comes from `logins[]`. A 401 or 403 clears the indicator; it is not an error. An account with no runners it may sign in gets an empty list.

**When it is read.** No new timer runs just for this. Reads are never closer together than the answer's `pollMs` (5 minutes today; 5 minutes when the answer has none), except for a forced read (at most every 5 s), which happens when a new `operational_alert` Ping links to `/dashboard/runners`, and 6 s and 20 s after a sign-in ends (the runner reports its new availability on its next poll, so the first read can still say expired).

**Desktop (`desktop/src`).**

| piece | code | behaviour |
|---|---|---|
| parsing and wording | `parseRunnerAttention`, `runnerAttentionFromRunners`, `runnerAttentionTitle`/`Detail`/`ActionText`, `runnerAttentionBadge`, `runnerAttentionPollMs`, `runnerSignInLink` (`RunnerLogin.cpp`) | `RunnerProviderState` now keeps `auth.state` and `auth.expiresAt`; the Runners table reads "Login expired — reconnect" for an expired login |
| the badge | `m_runnerAttentionButton` (`runnerSignInBadge`, `MainWindowNavigation.cpp`), styled in `Theme.h` | in the top bar beside the relay button, hidden while nothing needs a sign-in. Red for any expired login, amber when only expiring. Text is the first entry's title, plus "(+N more)"; the tooltip lists every entry with the web banner's wording |
| reads | `refreshRunnerAttention` (`MainWindowHosts.cpp`) | on every signed heartbeat reply (60 s; the `pollMs` floor makes that one read per 5 minutes), on a runner Ping, after the Runners list is read (which also recomputes it from that list when the endpoint is missing), and after a sign-in dialog ends |
| click | `openRunnerAttention` | one entry: `openRunnerAgentLoginTerminal(runner, provider)`, the Hosts page's sign-in dialog, which attaches to an open sign-in (`409 login_active`) instead of starting a second one. Several: a menu, one "… — Connect Claude" per entry, plus "Open the Runners list". An entry with `canConnect: false` opens the dashboard's Connect page (`runnerConnectPageUrl`, `connect=1`) in the browser |
| Ping row | `openNotificationRow` → `openRunnerSignInLink` | the runner-auth Ping (link `/dashboard/runners?runner=…&provider=…` on the app's host) opens the same dialog instead of the browser; with no runner named, it picks one the way `runnerForTarget` does |

The system log gets one line per new entry and "Every runner login you manage is signed in again." when the list empties.

Tests: `testAttention` in `desktop/tests/test_runner_login.cpp` (target `forkmesh-runner-login-tests`): the served answer, flat, grouped and runner-row shapes, derivation from `GET /api/runners` (expired by `loginState`, by `auth.state`, expiring, inactive runner and missing CLI skipped, open sign-in), de-duplication and order, wording, badge state, the ping link and the URLs.

**iOS (`native/ios`).**

- Core (`ForkMeshCore/Sources/FMApi/Runners/`): `RunnerSignIn.swift` holds `RunnerAttentionItem`, the parsers and the web-banner derivation, the wording, the paths, `terminalText` and `loginUrls` (the dashboard's `runnerTerminalText`/`runnerLoginUrls`), `inputValid`/`codeSubmission`, and `RunnerLoginSession`, the same start/attach/output/cancel state machine as the desktop's. `RunnerAttentionModel.swift` holds `RunnerAttentionModel` (the list, the read floors, the browser fallback) and `RunnerLoginModel` (one sign-in). `AgentsFeature.register()` registers the model and feeds it the notification list, so a new unread `operational_alert` linking to `/dashboard/runners` forces a read.
- Views (`ForkMesh/Features/Agents/RunnerSignInViews.swift`): `RunnerAttentionBanner` is a full banner at the top of the Agents list (one row per entry, the web's detail line, **Sign in** or **Open sign-in**; "N runner sign-ins needed" for several) and a one-line chip under the connection banner on every other tab. `runnerSignInPresenter` on `HomeShell` presents the sheet.
- Reads: the Agents screen and the shell appearing, the app returning to the foreground, subject to the `pollMs` floor (bounded to 30 s–1 hour here).
- Tap: when the entry has `canConnect` and the app has an account session, a sheet starts the sign-in (or joins `loginId`, or the `409 login_active` id), reads the output every second, shows the text with escapes stripped and each https URL as an **Open <host>** button (system browser), sends the pasted code followed by `\r` after the same 1–4096-byte printable check as the server, and has **Cancel sign-in**; closing the sheet while the sign-in is open cancels it. Output and input are never logged. Otherwise, or when the start answers 401/403, the item's `connectUrl` (only a `/dashboard/runners` path is accepted) opens in the browser, where `connect=1` starts the web sign-in.
- Pings: a push or the ping detail's Open with a `/dashboard/runners?runner=…&provider=…` link opens the same sheet (a push also switches to Agents).
- Tests: `RunnerSignInParsingTests` and `RunnerAttentionModelTests` in `ForkMeshCore/Tests/FMApiTests/RunnerSignInTests.swift` (`FM_ONLY=FMApi swift test --filter 'RunnerSignInParsingTests|RunnerAttentionModelTests'`, 18 tests), with a fake HTTP client for the 404 fallback, 401/403, the read floors, a forcing ping, `canConnect: false`, a full sign-in with the code, cancel on close and polling stopping on 404. The SwiftUI files are parse-checked only on Linux.

**Android (`native/android`).**

- Core (`core/.../nativeapp/runners/`): `RunnerAttention.kt` (parsing, the web-banner derivation, wording, ping links, `connectUrl` checks), `RunnerLoginSession.kt` (`RunnerLoginSession` and `RunnerLoginText`, ports of the desktop's session and the dashboard's `runnerTerminalText`/`runnerLoginUrls`), `RunnerAttentionApi.kt` (Bearer requests to the app origin, and `RunnerAttentionStore`, which holds the list and the read floors).
- App (`app/.../feature/agents/`): `RunnerAttentionFeature` (the list for the UI; its `Watcher()` sits in `HomeShell`), `RunnerAttentionBanner` under the Agents header (same rows and heading as iOS), `RunnerAttentionChip` under the offline banner on Inbox, Code and Chat (one entry opens its sign-in, several switch to Agents), and `RunnerSignInScreen`.
- Reads: the shell and the Agents banner appearing and the app resuming, subject to the `pollMs` floor; forced by a new unread runner `operational_alert` Ping and after a sign-in ends.
- Tap: the same rules as iOS, on a full screen ("Sign in Claude Code on fmnext-r1"). Each https link without user-info gets **Open <host>** (system browser) and **Copy link**; leaving the screen while the sign-in is open cancels it. Without a session, with `canConnect: false`, or when the start answers 401/403, the dashboard's Connect page opens in the browser.
- Pings: a runner-auth Ping gets **Connect Claude** / **Connect Codex** in the ping detail and the Inbox item, and tapping its push opens the sign-in (`PushFeature.open`).
- Tests: `RunnerAttentionTest` and `RunnerLoginSessionTest` in `core/src/test/.../runners/` (`./gradlew :core:coreTest -Pfilter=runners`, 15 tests), including the served payload, whose `title` must equal the local wording.

**Security.** The phones and the desktop use the dashboard's endpoints and rules and nothing else: the account session as a Bearer token (admin, or the registrant of a personal runner, as for the web), runner names, providers and login ids checked against the same patterns before they go into a path, the pasted code limited to 1–4096 bytes of printable text plus tab/CR/LF, only https URLs from the relayed terminal offered, opened in the system browser, and no output or input logged. The credential still never leaves the runner; the clients see only the scrubbed terminal the app relays.

**Mode B (TODO, burst runners).** A burst runner is a throwaway instance with no login. Proposed design:

1. **Registration key.** At boot, the burst runner generates a one-time X25519 key pair and registers its public half with its runner registration. The first claim can carry it instead.
2. **Opt-in and sealing.** When the app leases a job to a `burst` runner, it checks the owner's cloud-run opt-in, stored as a per-owner setting. It then seals the owner's provider token to that X25519 key using the existing sealed-box scheme (`server/nodeops_seal.go` and the app's `encrypt_row` key hierarchy), adding it as `sealedCredential: {provider, alg, ciphertext}`.
3. **Unsealing.** The runner unseals the token in memory and passes it to the CLI only through the job's environment (`CLAUDE_CODE_OAUTH_TOKEN` or `OPENAI_API_KEY`), never on disk. It drops the token when the job ends.
4. **Key lifetime.** The runner's X25519 private key lives only in memory, so a reboot invalidates any sealed token still in flight.
5. **Ownership rules.** Owners who have not opted in are never offered to burst runners. Pool runners without mode B keep serving only owners whose jobs can run on the always-on runner's own login.

## Live agent output

The run a person has open in the desktop's Agents view is streamed to it while it runs. It works like the web sign-in relay in the other direction, and it is gated on someone watching. The whole path (runner, relay, desktop) is in [agents/README.md](agents/README.md#live-output-of-cloud-runs).

- **Tap.** `runClaudeAgent` hands each stream-json line it parses to the job's `agentStream` as well (`server/agentstream.go`, `withAgentStreamTap`). Each line is validated as JSON. A line over 32 KiB has every string cut to 4 KiB, and is dropped if it is still too long. The line is then scrubbed with `scrubTokens` (the same patterns as the web sign-in) and appended to a backlog of at most 256 KiB, trimmed a whole line at a time.
- **Gate.** While a job runs, the runner posts a heartbeat to `POST /api/runners/stream`: `{jobId, leaseId, seq: 0}` every 5 s. The answer's `watched` is true for 30 s after any viewer read of the session. Only then does the runner post data: numbered chunks of at most 8 KiB, once a second or as soon as 8 KiB is buffered, with up to 8 posts a tick while it sends the backlog. An unwatched run sends heartbeats and nothing else. Its backlog stays on the runner, so a viewer who opens the run mid-way sees its recent past.
- **Numbering.** `seq` counts per job from 1, like the sign-in output. A retry after a network error resends the same chunk and number, and the app ignores a number it already stored. A gap answers `409 out_of_order`, a lease the runner no longer holds `409 lease_not_found`, and a run the app has closed `409 stream_ended`. On any 4xx the runner stops streaming that job. The job itself carries on and reports as before.
- **End.** When the CLI exits, the runner flushes what is left (only if watched, and for at most 5 s), then sends the terminal report. The report marks the stream ended in the app, which hides its chunks at once and deletes them 2 minutes later.
- **Mirrors.** A mirror node posts the same body to `POST /api/repo/<node>/<repo>/org-agent-jobs/<id>/stream`, with its signed collaboration query and the job's lease id (`mirrorAgentBackend.postStream`).
- **Cost.** One heartbeat every 5 s per running job, plus the output while someone watches. Codex runs send nothing, because Codex prints no stream-json.

### Rollout

The runner side ships in the daemon binary, so live output starts only once the runners (and any mirror that runs jobs) have the new daemon:

1. Deploy the app first (migration `0232_agent_session_streams` and the new routes). Until the app has the route, an updated runner's heartbeat gets a 404 and it stops streaming that job, with no other effect. An old runner never posts.
2. Roll the runner daemon: `python3 tools/next_fleet.py install r1 --steps daemon --execute` (it builds `server/cmd/forkmesh-mirror-node` and restarts the unit). Since 2026-10-04 the node deploy agent does this after a `server/` push, once the runner runs no agent job (`next_fleet.py roll-runners`, [deploy/auto-deploy.md](deploy/auto-deploy.md#runners-2026-10-04)).
3. Ship the desktop build. An older desktop never reads the stream route, and runners see `watched: false` and send only heartbeats.

## Burst pool (`server/runnerpool.go`, `server/vultr.go`)

The pool runs in the core daemon and is **off by default**:

```json
"role": "core",
"runnerPool": {
  "enabled": true,
  "provider": "vultr",
  "maxBurstRunners": 2,
  "monthlySpendCeilingCents": 3000,
  "hourlyPriceCents": 3.6,
  "scaleUpAfter": "2m",
  "idleTimeout": "10m",
  "region": "ewr",
  "plan": "vc2-2c-4gb",
  "snapshotId": "<prebuilt runner snapshot>",
  "apiKeyFile": "/etc/forkmesh-next/vultr-runner-pool.key"
}
```

**Each round (every 30 s):** the loop POSTs `/api/runners/pool-status`, lists the provider's instances, calls the pure `decidePool`, applies the resulting actions, and persists the state to `runner-pool.json`. A failed observation does nothing. A failed round doubles the wait before the next one (60 s, 2 min, 4 min, up to 15 min) so an app or Vultr outage is not polled every 30 s; the first good round goes back to 30 s.

**Decision rules**, each tested in `runnerpool_test.go`:

- **Scale up:** create one instance per round when all of the following hold:
  - The queue has been non-empty for at least `scaleUpAfter`.
  - No always-on runner, seen in the last 5 min, has a free slot.
  - Queued work exceeds the idle and booting burst capacity.
  - The pool is below `maxBurstRunners`.
  - Spent plus one more hour for every live runner fits the ceiling.
- **Labels:** new instances take the lowest free `fmnext-burst-N` (never `fmnext-run-N`: that is the always-on runner r1's label), tagged `forkmesh-next`.
- **Idle:** a new instance counts as booting, not idle, for 5 min. An instance idle for `idleTimeout` is destroyed, and any lease resets its idle clock.
- **Spend:** accrues per instance-hour at `hourlyPriceCents` and resets each UTC month. At the ceiling, every managed instance is destroyed, busy or not, and nothing is created.
- **Ownership:** only instances labelled `fmnext-burst-<n>` and tagged `forkmesh-next` are counted, billed or touched. `VultrPool` enforces this again at every step:
  - `List` filters by tag and label.
  - `Create` refuses other labels.
  - `Destroy` re-reads the live instance and refuses on any mismatch in id, label or tag.
- **API key:** use a Vultr sub-user limited to the instances permission, stored in an owner-only file. Tests only ever talk to a local fake; nothing calls the real API.

**Not yet built:**

- The runner snapshot itself, a Debian image with the daemon, the CLIs and a `role: runner` config.
- The cloud-init that registers the new instance, which needs mode-B-style one-time registration.

Until both exist, keep `enabled: false`.

## Desktop as a bring-your-own runner

`desktop/` has not been changed. The work needed there:

1. **Settings toggle.** Add "Use this computer as a runner for app.next". It generates, or reuses, the desktop's node key, then calls `POST /api/runners/register` with `scope: personal` and the account's own name in `allowedOwners`, using the proof above and the signed-in session.
2. **Claim client.** Add a claim client using the same signed `request_message` headers the Go runner sends (`signedNodePost`). The desktop already claims per-node leases from `/api/repo/<node>/<repo>/org-agent-jobs`, so this is a second source feeding the same local agent launcher. Alternatively, the desktop ships the Go daemon and runs it with `role: runner`.
3. **Clone and push.** Clone with `http.extraHeader=X-ForkMesh-Runner-Grant: <readGrant>` into a scratch directory, never into the user's working copy. Push only `HEAD:<pushRef>` with the push grant.
4. **Sandbox.** Linux can use `systemd-run --user --scope` as above. macOS and Windows would use a job object or `sandbox-exec` equivalent, or fall back to a timeout alone.
5. **Pinning.** Let personal jobs be pinned to the desktop runner. The website sets `runner: "<name>"` in the queued job record, and `runner_leases.py` already honours that pin.

## When a run fails

A failed run reports one error, in the job's `lastError`, and clients show it
beside the transcript; it is no longer appended to the transcript or
substituted for the result (afa0c30, a port of production's a8815d8c4).
`server/agent_errors.go` picks it, in order, from the Claude Code result
event's errors or result text, an API-error assistant message, the last
non-empty lines of stderr, and only then "Claude Code exited with status N".
It is capped at 600 runes and never shows the stream-json init event. A
"does not support this model; version X or newer is required" failure becomes
"Claude Code on <node> is <ver>; <model> needs <X> or newer. Update the host
(Hosts -> Update)." Codex gets the same stderr-tail ordering and an
outdated-CLI message. An outdated CLI is not retried without `--resume`.

A run publishes `agent/<session>` only when it committed something
(`runnerBackend.publish` skips the push when the tip equals the branch or the
base), yet the session reports `branchName` from the claim, so the Agents tab's
"View diff" can point at a branch the mirror never received. The compare page
used to answer that 404 with "refresh in a moment", which never came true. It
now re-reads the mirror's branch list, names the branch that is missing, and
for an `agent/` branch says the run has not committed anything yet or ended
without changes; only when both branches are listed does it keep the
wait-for-sync text.

## Node logs

Burst runners must be discoverable and carry the log reader, or their logs
never reach the desktop: see docs/NODE_LOGS.md, "New and autoscaled nodes".
