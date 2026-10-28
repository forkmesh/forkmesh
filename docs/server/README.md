# Go daemon (`server/`)

`forkmesh-mirror-node` is the Go service on every host that serves or works
on repositories: the core nodes, the runner tier, and any independent mirror.
It keeps the served bare repositories in sync, supervises the Python HTTPS
gateway (`tools/mirror_gateway.py`), publishes the node's catalog record and
endpoint lease to the App, materializes web issue/pull/discussion submissions
into Git, executes owner-authorized web merges, and runs website-queued agent
jobs and, when enabled, repository Actions.

`role` in `mirror-node.json` (`server/config.go`, `applyRole`) picks what a
host does:

- `mirror` (default): an independent mirror; all of the above, optionally with
  Cloudflare Tunnel as a child process.
- `core`: mirror, Git gateway and catalog work plus reconciliation of
  repositories hosted on the core. A core never runs agent jobs or Actions
  (`disableAgentJobs` forced on, `enableActions` off).
- `runner`: no repositories, catalog, tunnel or merge executor. It claims agent
  job leases from the App and runs them in a per-job sandbox
  ([RUNNERS.md](../RUNNERS.md)).

The operator guide for independent mirrors is
[www/docs/operations/mirror-node.md](../../www/docs/operations/mirror-node.md).

## Code map

Package `mirrornode` (module `forkmesh.com/mirror-node`, Go 1.24 with a
pinned toolchain). Paths are relative to `server/`.

| Path | Responsibility |
| --- | --- |
| `cmd/forkmesh-calls/main.go`, `rtc/` | The group-call server (SFU) and our own standard-library WebRTC stack: [rtc.md](rtc.md); the engine replacing WebRTC: [fm-media.md](fm-media.md) |
| `rtc/quic/`, `rtc/h3/`, `rtc/webtransport/` | Our own QUIC v1, minimal HTTP/3 and WebTransport, standard library only: [quic.md](quic.md) |
| `rtc/fmm/` | FM Media engine core: [fmm-protocol.md](fmm-protocol.md) |
| `rtc/fmmserver/`, `rtc/fmmcall/` | FM Media call server (QUIC and WebTransport, standby failover, certificates) and its client side: [fmm-server.md](fmm-server.md) |
| `rtc/e2ee/`, `../app/public/chat-call-e2ee.js` | Group call end-to-end encryption (v2): hybrid X25519 + ML-KEM-768 sender keys per epoch, AES-256-GCM frame keys, a short commit-then-reveal verification code and batch-signed media: [call-e2ee.md](call-e2ee.md) |
| `rtc/netsim/`, `rtc/bench/`, `../tools/call_bench.py` | Network impairment relay and the call-quality bench: [call-bench.md](call-bench.md) |
| `cmd/forkmesh-mirror-node/main.go` | Flags and one-shot modes (`--init`, `--check`, `--preflight`, `--backup`, `--restore`, `--restore-drill`, `--export-identity`, `--import-identity`, `--register-link-code`, helper modes), then `Daemon.Run` |
| `config.go` | `mirror-node.json` schema, defaults, strict validation; `role`, `runner`, `runnerPool` |
| `daemon.go`, `supervisor.go` | Loopback API, sync/publish cycle, gateway generation adoption, backoff; child process supervision (`Reload` vs `Restart`) |
| `nodeevents.go` | Node event WebSocket to the App (`/api/nodes/events`), signed with the node key |
| `gateway.go` | Gateway config, `syncRepository`, clone/seed, `reconcileUpstreamRefs`, `ensureHeadResolves` |
| `catalog.go` | `CatalogPublisher` (per-repository record, agent providers) and `EndpointPublisher` (HTTPS endpoint lease) |
| `identity.go`, `bootstrap.go`, `account.go` | Ed25519 node identity and helper protocols; `--init`; `--register-link-code` |
| `pushhook.go` | `post-receive` hook that rings `POST /v1/control/sync` |
| `intake.go`, `issue_reconcile.go`, `pull_metadata_refs.go` | Collaboration intake, issue convergence, per-pull metadata refs ([intake-and-attestation.md](intake-and-attestation.md)) |
| `pull_head.go` | Deterministic head commits for patch-backed pulls ([pull-heads.md](pull-heads.md)) |
| `merge.go`, `merge_branch.go`, `checks.go` | Merge executor, branch operations, signed required checks ([merge-executor.md](merge-executor.md)) |
| `pullrules.go`, `agentattest.go` | Pull request rules and model attestations ([pull-rules.md](pull-rules.md)) |
| `forks.go`, `hosted.go` | Website fork jobs; core-side hosted repository reconciliation ([HOSTED_REPOS.md](../HOSTED_REPOS.md)) |
| `agents.go`, `agent_*.go`, `agentstream.go`, `terminal.go` | Website agent jobs, errors, attachments, live output, provider sign-in terminal ([agent-jobs.md](agent-jobs.md)) |
| `runner.go`, `runnergrant.go`, `sandbox.go`, `jobhome.go`, `runnerlogin.go` | Runner role: claim, clone with a read grant, run, push `refs/heads/agent/<session>`, report; per-job sandbox and homes |
| `runnerpool.go`, `vultr.go` | Burst runner autoscaler (only fleet-labelled instances) |
| `actions*.go`, `runner_actions.go` | Opt-in Actions on mirrors ([actions-runner.md](actions-runner.md)); Actions jobs on runners ([runner-actions.md](runner-actions.md)) |
| `syncstatus.go`, `dashboard_replica.go`, `nodeops_*.go` | Sync status model, dashboard projection refresh, backup/restore/identity operations |
| `packaging/` | systemd units, sshd and fail2ban configs for independent mirrors |

Related code: `tools/mirror_gateway.py` (HTTPS gateway), the SSH gateway and
broker ([gateways.md](gateways.md)), the desktop host installer, and
`deploy/core/render.py` with `tools/next_fleet.py` for cores and runners.

## Architecture

- **Trusted core, untrusted runners.** Runners hold no core secret and reach
  the core only with per-job grants; they poll claims with jitter and backoff.
- **Hosted repositories** have no leader: the App holds the desired set and
  each core converges its own copy.
- **One cycle, many doorbells.** `syncLoop` runs `cycle()`; node socket events
  (`commits`, collaboration kinds, `org-agents`, `forks`, `replica`, and a
  catch-up on reconnect) mark work pending. App-fed nodes reconcile every 5
  minutes as a safety net (`relaySafetySyncInterval`), third-party upstreams
  every `syncInterval` (default 15 s); the endpoint lease renews every 4
  minutes.
- **Refs generations.** A `commits` event carries the source's signed refs
  digest and a generation; the daemon retries a stale peer (2 s to 15 s
  backoff) until that digest lands and ignores older generations.
- **Gateway generations.** The daemon writes the gateway config, sends SIGHUP,
  and waits up to 90 s for the acknowledgement; a fail-safe restart waits a
  random 0-20 s per node.
- **Publish gates registration.** The HTTPS endpoint registers only after the
  catalog record published; a repository whose HEAD or state hash cannot be
  read publishes nothing.

## Protocols and interfaces

- Loopback API (`127.0.0.1:8791`, `localRequestOnly`): `GET /healthz`,
  `GET /readyz`, `GET /v1/status`, `POST /v1/control/sync`,
  `POST /v1/control/collaboration`.
- Gateway on `127.0.0.1:8790`, fronted by Caddy or Cloudflare Tunnel
  ([direct-https-mirror-gateway.md](../../www/docs/operations/direct-https-mirror-gateway.md),
  `www/docs/mirror-*.schema.json`); the core gateway also serves
  `/v1/runner/<owner>/<repo>.git` to runners with grants.
- App: `/api/nodes/events`, `/api/repositories`, `/api/accounts`,
  `org-agent-jobs` leases, collaboration inbox leases, runner routes
  (`/api/runners/register|revoke|claim|report|...`), node-signed
  `POST /api/hosted-repositories/replicas`.
- Helper protocols on stdin/stdout (one bounded JSON object): health signer,
  capability verifier, manifest signer, merge executor, check signer.

## Configuration

`/etc/forkmesh/mirror-node.json` (template
`server/packaging/systemd/mirror-node.json.example`), decoded with
`DisallowUnknownFields`. Keys: `schemaVersion`, `listen`, `gatewayConfig`,
`gatewayScript`, `python`, `cloudflared`, `connectorToken` (path),
`identityKey` (path), `catalogUrl`, `mirrorUrl`, `syncInterval`,
`syncTimeout`, `gatewayProbe`, `upstreams`, `eventDrivenUpstreams`,
`mirrorOf`, `publishOwner`, `canonicalOwner`, `version`, `disableCatalog`,
`disableCloudflared`, `disableMergeExecutor`, `disableAgentLogin`,
`disableAgentJobs`, `enableActions`, `agentOwner` (accepted, unused), `role`,
`runner`, `runnerPool`, `hosted`. On cores and runners
`deploy/core/render.py` merges per-node overrides from `deploy/fleet.toml`.
Environment: `/etc/forkmesh/node.env`, `FORKMESH_MIRROR_NODE_CONFIG`.

### Upstream events and read-only mirrors

`eventDrivenUpstreams` names upstreams whose changes arrive as `commits`
doorbells. An upstream on the node's own relay (same host as `catalogUrl`
once `app.`/`api.`/`www.` is dropped, or a subdomain of it) is announced on
the node's signed account socket and keeps the 5-minute safety fetch. An
upstream on another relay (a self-hosted instance following forkmesh.com) gets
an unsigned watch socket on that relay,
`/api/nodes/events?watch=<owner>/<repo>` (`upstreamwatch.go`); a doorbell or
a reconnect fetches at once and `syncInterval` stays the fallback poll. The
relay side accepts a watch only for a public repository, resolves an
organization alias to its backing node, and puts watchers on the
`watch:<owner>/<repo>` `ForkMeshNodes` object, where they may only ping;
`notify_repo_mirrors` rings that object for every `commits` doorbell.
`/v1/status` lists `upstreamWatches`.

`mirrorOf` maps a configured repository key to its upstream page
(`https://host/owner/repo`). Such a repository, and every alias on the same
Git directory, is a read-only mirror: its catalog record carries `mirrorOf`,
`upstreamCommit` and `lastSyncedAt` (signed optional fields the relay keeps
only when well formed), and its gateway entries lose `git-receive-pack`,
`merge-pull`, `update-pull-branch` and the agent-branch operations. See
[../INSTANCES.md](../INSTANCES.md#the-platform-source-mirror-ownerforkmesh).

## Build, run, test

```sh
cd server
go test -race -cover ./...
CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' \
  -o forkmesh-mirror-node ./cmd/forkmesh-mirror-node
forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --check
forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --preflight --json
```

`python3 tools/run_critical_tests.py go` runs the same tests offline
(`GOTOOLCHAIN=local`, `GOPROXY=off`); CI (`.forkmesh/ci.yml`), the quality
gate and the auto-deploy gate all run it, so a failing Go test blocks a
deploy. Until 2026-10-15 no gate ran them and two had broken unnoticed.

`python3 tools/next_fleet.py install nN --steps daemon` builds and installs
it on a core or runner. Gateway tests are Python
(`app/tests/test_mirror_gateway*.py`, `app/tests/test_headless_mirror_*.py`).
Diagnosis: `journalctl -u forkmesh-mirror-node`,
`curl -s 127.0.0.1:8791/v1/status`. Fleet runbook:
[OPERATIONS.md](../OPERATIONS.md); independent mirrors:
[fleet-operations.md](fleet-operations.md).

## Security

- The node identity key and connector token are read from owner-only files,
  never accepted in argv, JSON, HTTP or a catalog record.
- Control requests (merges, Actions) are verified against the router's Ed25519
  key before anything is applied.
- The unit runs as `forkmesh-node` with `ProtectSystem=strict` and write access
  only to `/var/lib/forkmesh`; on a runner it runs as root only to start job
  scopes as `forkmesh-job`.
- Actions steps run in bubblewrap with no network and no view of node state.
- SSH is key-only with fail2ban.
- The gateway's reads (`resolve_commit`) resolve only branches, tags,
  `refs/pr/*`, `refs/remotes/origin/*`, `HEAD` and commit ids, so hidden
  namespaces such as `refs/forkmesh/trash/` are never served
  (`test_reads_resolve_only_visible_refs`).
- A commit id must have at least 7 hex digits, and is served only when one of
  those visible refs, or a force-push backup under `refs/forkmesh/history/`
  (kept resolvable for reviews, [CORE_PUSH.md](../CORE_PUSH.md)), contains
  it. Until 2026-10-27 any id of 4+ digits resolved any object in the store,
  so the public read API served deleted branches in `refs/forkmesh/trash/`,
  merge staging, and objects a hosted fork borrows from its pool
  (`test_reads_refuse_commit_reachable_only_from_hidden_refs`).
- The check costs one walk per refs state, not per id: on the first commit-id
  read of a cache generation the gateway lists the visible refs and, when
  they reach at most `VISIBLE_COMMIT_SET_LIMIT` (50,000) commits, keeps that
  commit set (about 5 MB at the limit). A full id is then answered from the
  set without starting git. A bigger repository answers visible tips from
  the ref list and walks the rest per id (`for-each-ref --contains`, memoized
  per generation). Every walk forces `core.commitGraph=true` (repository
  config is untrusted) and takes one of `VISIBLE_COMMIT_WALK_SLOTS` (2)
  process-wide slots; a request that waits more than 5 s gets a 503, so a
  flood of distinct old ids queues instead of taking every CPU
  (`test_commit_id_visibility_is_cached_per_refs_state_and_bounded`).
- The runner daemon reads a Codex job's `last-message.txt`, in a folder the
  job user owns, with `readOwnedFile` (no symlink, no FIFO, owned by the job
  user, at most 4 MiB), like every other job-owned file it reads
  (`TestCodexLastMessageReadRefusesJobPlantedLinksAndFifos`).
- A core (`role: core`) syncs its upstreams with `http.followRedirects=false`
  (`syncCoreRepository`, `upstreamGitArgs` in `gateway.go`), for the fetch,
  the first clone and the peer `main` convergence. The App's public Git route
  can answer with a 307 to the serving node, and before this the core followed
  it to whatever host the App named, so a mirror the App picked could feed the
  core's `main` and tags. Now a redirecting upstream fails, and the sync moves
  on to the next configured upstream, the peer cores listed by
  `core_upstreams` (`deploy/core/render.py`). Mirrors and runners keep
  following redirects: the public route is their only upstream
  (`TestCoreSyncNeverFollowsARedirectToAnotherHost`,
  `TestMirrorSyncStillFollowsRedirects`).
- See [RUNNERS.md](../RUNNERS.md) and [IDS.md](../IDS.md).

## Traps

- **Strict decoding everywhere** (config, gateway config, intake items, helper
  requests): a key an older binary does not know makes it refuse to start or
  refuse every item. Declare new fields first; retired keys stay declared.
- A runner must have empty `upstreams`.
- Never run git as the daemon user inside a job tree.
- The provider CLIs are root-owned in `/usr/local/bin`; only a host update
  refreshes them. The model allowlist is duplicated in `server/agents.go`
  (`agentWebsiteModels`) and the desktop.
- **Unborn HEAD hides a node**; `ensureHeadResolves` runs after every sync.
- **Empty repository after a dropped first clone**: the gateway refuses to
  serve without `integrity.expectedRefsSha256`; the clone retries with
  backoff.
- **Refs pins.** The App routes only to a node whose refs digest the source
  attested; pushing every node to an unattested ref set 503s every clone.
- **Two merge bases** are handled by `reconcileIssueOnlyDivergence` via
  `git merge-tree --write-tree`.
- **Never restart the gateway to adopt a generation**: it kills an in-flight
  merge job. The executor writes `mirror-gateway/pull-jobs/gateway-generation`
  so the cycle can recognise its own write.
- **Size a pty before starting its child.** `startPtyCommand` sets the
  window size on the master before `cmd.Start`; sizing it afterwards let a
  fast CLI read `0 0` as its first size (CI run #3523,
  `TestRunnerTerminalRunsTheCLIAndFollowsTheSize`).
- The catalog's 52-week activity comes from one
  `git rev-list --since=52 weeks ago --timestamp --parents HEAD` pass.
- Ref updates are compare-and-swap: `reconcileUpstreamRefs` reads refs with
  one `for-each-ref` and retries a guarded `update-ref` that lost a race with
  a direct push (at most three attempts).
- **Tests run in the Actions sandbox**, not a login shell: `umask 0077`,
  `GIT_CONFIG_GLOBAL=/dev/null` and a passwd entry git cannot build an identity
  from. A test that wants a group/world mode bit sets it with `os.Chmod` (or
  pins `syscall.Umask(0o022)`, the service umask that
  `makePublicRepositoryTraversable` relies on), and every `git commit` /
  `commit-tree` passes `-c user.name=… -c user.email=…` or runs in a clone
  with a local `user.*`. Four tests broke on this from the day the Go suite
  joined CI (2026-10-14) until it was fixed; reproduce with
  `umask 077; GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_COUNT=1
  GIT_CONFIG_KEY_0=user.useConfigOnly GIT_CONFIG_VALUE_0=true go test ./...`.
- **The sandbox has a CPU quota, not fewer cores.** The desktop runs a step in
  a scope with `CPUQuota` of at most 800% and `CPUWeight=20`, while Go 1.24
  sizes `GOMAXPROCS` (and so `go test -p`) from every host core. On a 36-core
  desktop, 36 packages with 36 threads each got throttled in bursts. Run 6484
  (2026-10-22) failed this way: a loopback `httptest` POST in
  `TestRegisterNodeAccountReservesAndFinalizesLinkCode` hit the 20 s client
  timeout, and the plain datagram window in
  `TestDatagramLossExemptKeepsTheWindowOpen` regrew to 3600 B, short of the old
  4x-the-exempt-window ratio. `tools/run_critical_tests.py go` now sets
  `GOMAXPROCS` from `FORKMESH_ACTIONS_CPUS`, which the desktop exports, so
  package parallelism follows it. That datagram test now checks each
  connection against its own starting window: without the exemption the
  window falls to half or less, and with it the window never shrinks.
- **A timeout on a busy desktop gets one retry.** The CI step shares the host
  with agent sessions, builds and auto-deploy tests, at `CPUWeight=20`. Even
  with `GOMAXPROCS` capped, one in-process loopback exchange sometimes waits
  out its whole timeout while the rest of the package runs at normal speed.
  Run 6458 hit the 30 s context of `TestMirrorNodeRejectsUnsafeAgentJobWithoutRunningIt`,
  run 6484 the 20 s client timeout of the account test, and run 6499
  (2026-10-22) the 30 s dial timeout of `http.DefaultTransport` in
  `TestRunnerClaimsOnlyWithAProviderAndFreeSlots`. Each failed package took
  its usual ~63 s plus exactly that timeout. The host counters show no lost
  packets (`/proc/net/stat/nf_conntrack` drop 0, `ListenOverflows` 0,
  `softnet_stat` dropped 0), so the test process was starved, not the
  network. `tools/run_critical_tests.py go` now reruns the failed tests once,
  with `-count=1 -run '^(…)$'` per package, but only when at most three
  tests failed, each printed a timeout (`i/o timeout`,
  `context deadline exceeded`, `Client.Timeout exceeded`, `timed out`), and
  no package panicked or failed to build. The log says
  `retrying … once: every failure was a timeout`. A wrong value, a panic,
  or a timeout that recurs still fails the step.
  `TestThreePeopleHoldACallThroughImpairedLinks` gets the same one retry
  for any failure, because its frame counts drop when the process is
  starved (`GO_LOAD_SENSITIVE_TESTS`, [../engineering/github-actions.md](../engineering/github-actions.md)).
- **Real-time call tests take three attempts.** The call and QUIC tests that
  judge wall-clock results run their peers, cores and `netsim` links in the
  test process, so a starved process is a worse network, not a slower one:
  on 2026-10-24 an agent sandbox measured RTTs of 40–100 ms on 15/25 ms
  links, a 1.3 s failover with no goroutine stalled for more than 57 ms, and
  listeners that ran 110–121 of 150 steps yet played 15–71 frames. A stall
  allowance or a per-step frame count therefore cannot separate load from a
  regression. `TestThreePeopleHoldACallThroughImpairedLinks`,
  `TestCoreDeathMovesTheCallToTheStandbyCore` (audio back within 500 ms),
  `TestMultipathPathFailureAudio` (a gap within one round trip of the
  surviving path, no loss) and the lower bound of `TestAntiAmplificationLimit`
  run through `retryTiming` (`timing_test.go` in `rtc/fmmserver` and
  `rtc/quic`): each attempt builds fresh cores, parties and links, misses are
  logged, and the test fails only when all three miss. Setup failures and
  the anti-amplification upper bound still fail at once. They blocked
  deploys `e162e9991b8f` (82 ms multipath gap) and `bf3a0be84066` (544 ms
  failover) in run 7942, and earlier deploys on 2026-10-23 and 2026-10-24
  ([../deploy/auto-deploy.md](../deploy/auto-deploy.md)).

## Related docs

- [RUNNERS.md](../RUNNERS.md), [HOSTED_REPOS.md](../HOSTED_REPOS.md),
  [OPERATIONS.md](../OPERATIONS.md), [IDS.md](../IDS.md),
  [CORE_PUSH.md](../CORE_PUSH.md)
- Operations pages under `www/docs/operations/`: mirror node, direct HTTPS
  gateway, Git SSH gateway, Actions control, headless refresh and identity,
  backup and recovery, operator policy, required checks, service roles, Git
  conformance; [sync status](../../www/docs/sync-status.md).
- Topic pages: [agent-jobs.md](agent-jobs.md), [merge-executor.md](merge-executor.md),
  [pull-heads.md](pull-heads.md),
  [intake-and-attestation.md](intake-and-attestation.md),
  [pull-rules.md](pull-rules.md), [actions-runner.md](actions-runner.md),
  [runner-actions.md](runner-actions.md), [gateways.md](gateways.md),
  [fleet-operations.md](fleet-operations.md)
