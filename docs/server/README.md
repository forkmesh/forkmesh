# Go daemon (`server/`)

`forkmesh-mirror-node` is the Go service on every host that serves or works
on repositories: the three core nodes, the runner tier, and any independent
mirror. It keeps the served bare repositories in sync, supervises the Python
HTTPS gateway (`tools/mirror_gateway.py`), publishes the node's catalog record
and endpoint lease to the App, materializes web issue/pull/discussion
submissions into Git, executes owner-authorized web merges, and runs
website-queued agent jobs and, when enabled, repository Actions.

`role` in `mirror-node.json` (`server/config.go`) picks what a host does:

- `mirror` (the default): an independent mirror. It does all of the above
  and may run Cloudflare Tunnel as a child process. There is no Qt app on a
  mirror: the desktop only installs and updates hosts over SSH.
- `core` on `n1` to `n3`: the mirror, Git gateway and catalog work, plus the
  reconciler for repositories hosted on the core. A core never runs agent
  jobs or Actions: `applyRole` forces `disableAgentJobs` on and
  `enableActions` off.
- `runner` on `r1` and burst runners: no repositories, no catalog, no tunnel,
  no merge executor. It claims agent job leases from the App and runs them in
  a per-job sandbox ([RUNNERS.md](../RUNNERS.md)).

The App the daemon talks to is the Python App code (`app/`) that the cores
serve on CPython under `app/host/forkmesh_host`. The cores still run this
daemon as their Git gateway, but the mirror hosts are gone: the production
mirrors (mirror35–38, retired 2026-09-24) and the older Cloudflare Tunnel
mirrors (mirror2–34) are deleted, and their roles run on the cores and the
runner `fmnext-r1` ([MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)).

The operator-facing guide for independent mirrors is
[www/docs/operations/mirror-node.md](../../www/docs/operations/mirror-node.md).
This page is the developer's map; topic pages sit beside it.

## Code map

All files are package `mirrornode` (module `forkmesh.com/mirror-node`,
`server/go.mod`: language version Go 1.24, built with the pinned
`toolchain go1.26.8`, which the `go` command fetches when the local one is
older) unless noted. Paths are relative to `server/`.

| Path | Responsibility |
| --- | --- |
| `cmd/forkmesh-mirror-node/main.go` | Flags and one-shot modes (`--init`, `--check`, `--preflight`, `--backup`, `--restore`, `--restore-drill`, `--export-identity`, `--import-identity`, `--register-link-code`, the helper modes below), then `Daemon.Run`. |
| `config.go` | `mirror-node.json` schema, defaults and validation (strict decoder); `role` and `applyRole`, `runner` (`RunnerConfig`, including `jobUid` and `workRoot`) and `runnerPool` (`RunnerPoolConfig`). |
| `daemon.go` | The `Daemon`: loopback API, sync/publish cycle, gateway generation adoption, retry and backoff state. |
| `supervisor.go` | Restarts the gateway and `cloudflared` child processes; `Reload` (SIGHUP) vs `Restart`. |
| `nodeevents.go` | The node event WebSocket to the App (`/api/nodes/events`), signed with the node key; terminal frames ride it too. |
| `gateway.go` | Gateway config (`GatewayConfig`), `syncRepository`, clone/seed, `reconcileUpstreamRefs`, `ensureHeadResolves`, config writes. |
| `catalog.go` | `CatalogPublisher` (per-repository record) and `EndpointPublisher` (HTTPS endpoint lease); advertises agent providers. |
| `identity.go` | Ed25519 node identity (PEM), and the bounded helper protocols: `--sign-mirror-health`, `--verify-mirror-capability`, `--sign-mirror-manifest`. |
| `bootstrap.go` | `--init`: first configuration, direct-origin manifest. |
| `account.go` | `--register-link-code`: registers the node account with an installer link code. |
| `pushhook.go` | Installs the `post-receive` hook that rings `POST /v1/control/sync`. |
| `intake.go` | Web issue/pull/discussion intake: leases App inbox pages, writes repository-native records, commits, acknowledges. |
| `issue_reconcile.go` | Merges any divergence between a node and its upstream whose issue records differ ([intake-and-attestation.md](intake-and-attestation.md#the-convergence-rule-2026-09-28)). |
| `pull_metadata_refs.go` | Per-pull metadata refs `refs/heads/forkmesh/pull/<n>` (and `refs/pr/<n>/metadata`); `forkmesh/pulls` stays as a compatibility index. |
| `merge.go` | `--execute-mirror-merge`: owner-authorized `merge-pull` / `update-pull-branch` executor. See [merge-executor.md](merge-executor.md). |
| `checks.go` | `forkmesh-check-v1` status-check attestations and the `.forkmesh/required-checks.json` gate; `--sign-check-attestation`. |
| `pullrules.go`, `agentattest.go` | `.forkmesh/pull-rules.json` and organization rules evaluation, the `rules_required` gate, and the signed model attestation on runner commits. See [pull-rules.md](pull-rules.md). |
| `forks.go` | Website-created fork jobs on managed mirrors. |
| `hosted.go` | Core-side reconciliation of hosted repositories against the desired set in the App; `hosted` config (`HostedConfig`) ([HOSTED_REPOS.md](../HOSTED_REPOS.md)). |
| `agents.go`, `agent_errors.go`, `agent_attachments.go` | Website agent jobs; `agent_errors.go` picks a failed run's `lastError`. See [agent-jobs.md](agent-jobs.md). |
| `terminal.go` | Streamed provider sign-in terminal (`claude auth login`, `codex login`). |
| `runner.go` | Runner role: claim on `POST /api/runners/claim`, clone with the lease's read grant, run the CLI, push exactly `refs/heads/agent/<session>`, report on `POST /api/runners/report`, wipe the workdir. |
| `runnergrant.go` | Read and push grants signed by the App with the router key; checked by the core gateway on `/v1/runner/<owner>/<repo>.git`. The canonical message is shared with `app/src/edge_routing.py` and `tools/mirror_gateway.py`. |
| `sandbox.go` | Per-job containment: a transient systemd scope with `MemoryMax` and `CPUQuota`, as `runner.jobUid`; a ulimit fallback without systemd. |
| `jobhome.go` | Per-session job homes and provider credential hand-over, so no job can write the job user's persistent home. |
| `runnerlogin.go` | Web sign-in of a runner's provider CLI over the runner's signed polling. |
| `runnerpool.go`, `vultr.go` | Burst runner autoscaler; only touches instances labelled `fmnext-run-*` and tagged `forkmesh-next`. |
| `actions.go`, `actions_workflow.go`, `actions_control.go` | Opt-in Actions runner (forced off on the cores). See [actions-runner.md](actions-runner.md). |
| `runner_actions.go` | On a runner: Actions jobs leased from the app when the desktop is offline. See [runner-actions.md](runner-actions.md). |
| `dashboard_replica.go` | Schedules the gateway's public dashboard projection refresh. |
| `syncstatus.go` | `forkmesh-sync-status-v1` model, pinned to `conformance/sync-status/v1/`. |
| `nodeops_*.go` | Backup, restore, restore drills, identity export/import, preflight, sealing. |
| `packaging/systemd/` | Units: `forkmesh-mirror-node.service`, the Actions drop-in, reconcile/refresh/renew timers, `mirror-node.json.example`. The cores and the runner use units rendered from `deploy/core/templates/`, based on these. |
| `packaging/ssh/`, `packaging/fail2ban/` | sshd hardening drop-in, SSH gateway configs, fail2ban jail, for independent mirrors. |

Related code outside `server/`: `tools/mirror_gateway.py` (the HTTPS gateway
the daemon supervises), `tools/ssh_gateway.py` and
`tools/ssh_authorization_broker.py` (SSH writes; the served stack runs no
public SSH gateway since `ssh.forkmesh.com` was retired),
`tools/headless_mirror_*.py` (older Python refresh/identity helpers), the
desktop's host installer (`desktop/src/MainWindowHostInstall.cpp`,
`desktop/src/ControlNode.cpp`), and `deploy/core/render.py` with
`tools/next_fleet.py`, which configure and install the daemon on the cores
and the runner.

## Architecture

- **Trusted core, untrusted runners.** Cores hold repositories and serve
  Git; runners hold no core secret and reach the core only with per-job
  grants. Runners have no node event channel: they poll claims with jitter and
  backoff (5941578).
- **Hosted repositories** have no leader: the App (rqlite) holds the desired
  set and each core's daemon converges its own copy.
- **The agent CLIs** run on the runner as `forkmesh-job`; the daemon runs as
  root only to start the job scope.
- **One cycle, many doorbells.** `syncLoop` runs `cycle()`; events from the
  node socket (`commits`, collaboration kinds, `org-agents`, `forks`,
  `replica`, and a catch-up on every reconnect) mark work pending and request a
  sync (`Daemon.handleNodeEvent`). There is no fixed polling for managed
  upstreams: an App-fed node reconciles every 30 minutes as a safety net
  (`relaySafetySyncInterval`), third-party upstreams use `syncInterval`
  (default 15 s), and the endpoint lease is renewed every 4 minutes.
- **Refs generations.** A `commits` event carries the source's signed refs
  digest and a generation; the daemon retries a stale peer (2 s to 15 s
  backoff) until that digest lands, and ignores replays of older generations.
- **Gateway generations.** The daemon writes the gateway config, signals the
  gateway (SIGHUP), and waits up to 90 s for the gateway's acknowledgement.
  A slow reload is waited out, not restarted; a real fail-safe restart waits a
  random 0-20 s per node so a group of nodes does not restart at once
  (33582b221).
- **Capabilities are rewritten at startup.** `Daemon.Run` upgrades the gateway
  config in place: health/verifier helper commands, merge capabilities
  (`ensureGatewayCapabilities`), Actions operations (`ensureGatewayActions`),
  and the collaboration control URL.
- **Publish gates registration.** A cycle registers the HTTPS endpoint only
  after the catalog record published; a repository whose HEAD or state hash
  cannot be read publishes nothing (see Traps).

## Protocols and interfaces

- Loopback API (`listen`, default `127.0.0.1:8791`, loopback-only by
  `localRequestOnly`): `GET /healthz`, `GET /readyz`, `GET /v1/status`,
  `POST /v1/control/sync` (push doorbell), `POST /v1/control/collaboration`
  (direct intake from the gateway).
- Gateway on `127.0.0.1:8790` (`gatewayProbe`), fronted by Caddy (on the
  cores) or Cloudflare Tunnel. Its HTTP routes and schemas:
  [direct-https-mirror-gateway.md](../../www/docs/operations/direct-https-mirror-gateway.md)
  and the `www/docs/mirror-*.schema.json` files. The core gateway also
  serves `/v1/runner/<owner>/<repo>.git` to runners holding grants.
- App: `wss://…/api/nodes/events` (node event channel), catalog
  `catalogUrl` (`/api/repositories`; on the cores
  `https://app.forkmesh.com/api/repositories`), accounts `/api/accounts`,
  `org-agent-jobs` leases, collaboration inbox leases.
- App endpoints used by runners: `/api/runners/register`, `/revoke`, `/claim`,
  `/report`, `/login/output`, `/login/input-poll`, `/pool-status`
  (`app/src/runner_leases.py`, `app/src/runner_logins.py`).
- Hosted repositories: `POST /api/hosted-repositories/replicas`, node-signed.
- Helper protocols on stdin/stdout (one bounded JSON object): health signer,
  capability verifier, manifest signer, merge executor, check signer.
- Files on disk: see [fleet-operations.md](fleet-operations.md#files-on-a-mirror).

## Configuration

`/etc/forkmesh/mirror-node.json` (template
`server/packaging/systemd/mirror-node.json.example`), decoded with
`DisallowUnknownFields`. Keys (`server/config.go`): `schemaVersion`, `listen`,
`gatewayConfig`, `gatewayScript`, `python`, `cloudflared`, `connectorToken`
(a path), `identityKey` (a path), `catalogUrl`, `mirrorUrl`, `syncInterval`,
`syncTimeout`, `gatewayProbe`, `upstreams`, `eventDrivenUpstreams`,
`publishOwner`, `canonicalOwner`, `version`, `disableCatalog`,
`disableCloudflared`, `disableMergeExecutor`, `disableAgentLogin`,
`disableAgentJobs`, `enableActions`, `agentOwner` (accepted, unused), `role`,
`runner`, `runnerPool`, `hosted`.

On the cores and the runner, `deploy/core/render.py` renders per-node role
overrides from `deploy/fleet.toml` that are merged over the `--init` config.
See [RUNNERS.md](../RUNNERS.md) "Runner config" and
[HOSTED_REPOS.md](../HOSTED_REPOS.md).

Environment: `/etc/forkmesh/node.env` (optional `EnvironmentFile`),
`FORKMESH_MIRROR_NODE_CONFIG` (set for helper modes).

## Build, run, test

```sh
cd server
go test -race -cover ./...
CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' \
  -o forkmesh-mirror-node ./cmd/forkmesh-mirror-node
forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --check
forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --preflight --json
```

`python3 tools/next_fleet.py install nN --steps daemon` builds the binary
(linux/amd64) and installs it with the gateway tools on a core or runner.

The gateway's tests are Python: `app/tests/test_mirror_gateway.py`,
`app/tests/test_mirror_gateway_operator_policy.py`,
`app/tests/test_headless_mirror_*.py` (run with `python3 -m pytest` from `app/`).

## Operations

Cores and the runner: [OPERATIONS.md](../OPERATIONS.md) (bring-up step 8 for
cores, step 9 for the runner), [RUNNERS.md](../RUNNERS.md),
[HOSTED_REPOS.md](../HOSTED_REPOS.md).

Independent mirrors are installed and updated from the desktop's Hosts page,
which uploads the deploying desktop's own `forkmesh-mirror-node` and
`tools/mirror_gateway.py`. A capability that needs both halves (merge
executor, Actions) appears only after both are redeployed. Procedures,
hardening and recovery: [fleet-operations.md](fleet-operations.md) (written
for the managed `mirrorN` fleet, whose hosts are deleted).

Diagnosis starts with `journalctl -u forkmesh-mirror-node` and
`curl -s 127.0.0.1:8791/v1/status` on the node.

## Security

- The node identity key and the connector token are read from owner-only
  files; neither is ever accepted in argv, JSON, HTTP or a catalog record.
- Control requests (merges, Actions) are verified against the router's Ed25519
  key (`--router-public-key`) before anything is applied.
- The unit runs as `forkmesh-node` with `ProtectSystem=strict`, write access
  only to `/var/lib/forkmesh`, no capabilities, and
  `MemoryDenyWriteExecute=false` only because the agent CLIs are V8 programs.
  On the runner it runs as root, with the capabilities it needs to start job
  scopes as `forkmesh-job`.
- Agent jobs on a mirror run the provider CLI with full permissions on a
  scratch clone; on the runner they run in the job sandbox. Actions steps run
  in bubblewrap with no network and no view of node state.
- SSH is key-only with fail2ban on the cores and the runner
  (`tools/next_fleet.py`) and on independent mirrors (`packaging/ssh/`,
  `packaging/fail2ban/`; see fleet-operations.md).
- The HTTPS gateway's reads (`blob`, `tree`, `raw`, `compare` and the rest,
  `tools/mirror_gateway.py` `resolve_commit`) resolve only branches, tags,
  `refs/pr/*`, `refs/remotes/origin/*`, `HEAD` and commit ids (2026-10-07).
  Before, any `refs/` name was used as given and an unqualified name fell
  through to git's own lookup, so `ref=refs/forkmesh/trash/deleted/<b>` (or
  `forkmesh/trash/...`) served a deleted branch's files to any reader even
  though `uploadpack.hideRefs` keeps `refs/forkmesh/` out of fetches.
  Test: `test_reads_resolve_only_visible_refs`.
- See [RUNNERS.md](../RUNNERS.md) "Security model" and "Job sandbox", and
  [IDS.md](../IDS.md) for the detectors that watch the daemon and the job
  user.

## Traps

- **Strict config decoding.** `mirror-node.json`, the gateway config, the
  intake items and helper requests all use `DisallowUnknownFields`. Adding a
  key that an older binary or gateway does not know makes it refuse to start or
  refuse every item (issue #700: the gateway's `signedNumber`/`submittedAt`
  had to be declared in `intake.go`). Retired keys stay declared (`agentOwner`).
- A runner must have empty `upstreams`; `applyRole` refuses otherwise.
- Never run git as the daemon user inside a job tree (7f31c58).
- **The provider CLIs are root's.** They are installed system-wide
  (`/usr/local/bin`) so `forkmesh-job` can run them, and the service cannot
  update them. On a mirror only the desktop's host update does (see
  [agent-jobs.md](agent-jobs.md)). `/var/lib/forkmesh` is 0700
  ([OPERATIONS.md](../OPERATIONS.md) section 9).
- The agent model allowlist is kept twice, in `server/agents.go`
  (`agentWebsiteModels`) and in the desktop; keep them in step.
- **Unborn HEAD hides a node.** A mirror seeded with `git init --bare` on a
  host whose default branch is `master` had no resolvable HEAD, so the catalog
  record never published and the node was invisible (2026-08-19).
  `ensureHeadResolves` (`gateway.go`) now runs after every sync and cycle.
- **Empty repository after a dropped first clone.** The gateway refuses to
  serve without `integrity.expectedRefsSha256`; the daemon retries the clone
  on 15 s to 5 min backoff.
- **Refs pins.** The App only routes to a node whose refs digest is one the
  source attested. Pushing a node to a ref set nobody attested makes it
  inadmissible; doing it to all of them 503s every clone. See
  [fleet-operations.md](fleet-operations.md#traps-when-repairing-by-hand).
- **Two merge bases.** When `git merge-base --all` returns more than one base,
  `reconcileIssueOnlyDivergence` (`issue_reconcile.go`) lets
  `git merge-tree --write-tree` resolve the source side (the recursive-merge
  virtual base) and merges issue records by identity, so the number of bases
  does not matter. Since 2026-09-28 nothing in an honest history makes it fail
  closed; see [intake-and-attestation.md](intake-and-attestation.md#the-convergence-rule-2026-09-28).
  The desktop's `UpstreamCheckoutSync.cpp` handles the same shape.
- **Gateway restarts.** Never restart the gateway to adopt a new generation:
  a restart kills an in-flight merge job the App is polling. The merge
  executor writes `mirror-gateway/pull-jobs/gateway-generation` so the cycle
  can tell its own no-op write from a real change.
- **Activity series in one pass.** The catalog record's 52-week activity
  series comes from one
  `git rev-list --since=52 weeks ago --timestamp --parents HEAD` pass: merges
  are the lines with two or more parents. `rev-list --timestamp` reads commit
  dates from the commit-graph, while `git log --format=%ct` parses every commit
  object. On forkmesh/forkmesh (54k commits in the window) the two `git log`
  walks took 3.3 s + 1.8 s of the one vCPU on every publish, on each core;
  the single pass takes 1.0 s and returns the same counts
  (`TestCatalogActivityWeeksMatchesGitLogInOnePass`).

## Recent changes

- 2026-10-08: runners run a repository's push workflows when the default
  branch moves on the cores and the owner's desktop is offline
  (`runner_actions.go`, app `runner_actions.py`, migration 0248). See
  [runner-actions.md](runner-actions.md).
- 2026-10-06 (from branch `agent/adhoc-2076-mirror-direct-ref-update`,
  written 2026-09-02): `reconcileUpstreamRefs` survives a direct push that
  moves a branch between its read of the local tip and its guarded
  `update-ref`. A "cannot lock ref ... expected" or "reference already
  exists" failure (`isRefUpdateContention`) re-reads the ref and chooses
  again (`settleRef`, at most three attempts), so a direct receive that
  already contains the fetched tip is kept instead of failing the sync.
  Creating a ref now passes the all-zero old value (`updateRef`), so it is
  compare-and-swap too and cannot overwrite a branch a direct receive
  created meanwhile; `moveRef` (the peer-main path) uses the same helper.
  Tests: `TestReconcileUpstreamRefsRetriesDirectRefUpdateRace`,
  `TestUpdateRefDoesNotOverwriteConcurrentBranchCreation`.
- 2026-10-04: images attached to a web or phone prompt reach agents on the
  runner tier. The claim answer now carries the job's `attachments`, and the
  runner downloads each image with the runner-signed
  `POST /api/runners/attachment` (lease holder only), writes it under
  `.forkmesh/attachments/` for the job user and names it in the prompt. Before
  this, runner jobs ran with the prompt text only (see
  [RUNNERS.md, Prompt images](../RUNNERS.md#prompt-images-2026-10-04)).
- 2026-09-26: the repository header's Pull requests count stops counting
  deleted pulls. The root `openPulls` count (`tools/mirror_gateway.py`,
  `_root_collaboration_counts`) joins the `forkmesh/pulls` index with every
  `forkmesh/pull/<n>` branch, so a pull the index has not caught up with is
  still counted (issue #683). Deleting a pull removes it from the index, but its
  branch stays: core pushes never delete branches (`docs/CORE_PUSH.md`). So 15
  deleted pulls came back as "open" and the header said 15 while the list said 0.
  `_live_independent_pull_refs` now drops a branch-only pull when the index
  deleted that number after the branch's last commit
  (`_deleted_pull_times`, one `git log --diff-filter=D` over the index, cached
  per index commit). The time check is needed because numbers are reused
  (#168 was deleted on 09-15, opened again and deleted again on 09-26). The
  same filter applies to `pullRefs` in the `.forkmesh/pulls` listing, so the
  web list and the badge agree.
- 2026-09-26: `reconcileUpstreamRefs` reads every local head and tag with one
  `for-each-ref` instead of one `git rev-parse` per ref, and
  `clearRefNamespace` deletes the staging namespace with one
  `git update-ref --stdin`, falling back to one `update-ref -d` per ref if
  that transaction fails. With about 400 heads and tags in `forkmesh/forkmesh`,
  each sync on a 1-vCPU core used to start about 800 git processes before the
  new refs were served.
- 2026-09-24 a8815d8c4, afa0c30: failed agent runs report the CLI's real
  error once; outdated-CLI refusals are remembered per provider and model.
- 2026-09-24: hosted repositories (32b946a); per-job user, HOME and egress
  limits (7f31c58); runner web sign-in (c9c2038); Opus 5.5 default (348a751).
- 2026-09-24 de8481362: the desktop's host updates refresh the mirror's agent
  CLIs (install to `<name>.new`, then rename).
- 2026-09-24 d578fbc2a: image attachments on website-queued jobs; model id
  `claude-opus-5-5`.
- 2026-09-23: runner tier (e92f6b3); runners poll instead of a node event
  channel (5941578).
- 2026-09-22 aa6168871, ab181c469: fail2ban jail and key-only sshd on mirrors.
- 2026-09-21 ba6912142: gateway serves the host journal at `/v1/node/logs`
  over HTTP/3; the unit joins `systemd-journal`.
- 2026-09-21 4d7a70c5b: a session moved to the cloud continues on the
  account's mirror, which waits up to 10 minutes for the branch.
- 2026-09-18 4f88b2a6d: Actions run in the daemon (opt-in `enableActions`).
- 2026-09-16 45c901122: intake accepts the gateway's `signedNumber`/`submittedAt`.
- 2026-09-14 47f7c854f: signed required checks gate mirror merges.
- 2026-09-13 53d9f7941: backup, restore drills, identity moves, preflight.
- 2026-09-13 138a8b14f: node operator storage and legal-response policy.
- 2026-09-13 a7dfa7936: the Haiku safety preflight for agent jobs was dropped.
- 2026-09-12 33582b221: no fleet-wide gateway restarts on refs changes.
- 2026-09-12 f71a9729f: website agent jobs run on the Go daemon.
- 2026-08-19: Go merge executor (`merge.go`), unborn-HEAD fix e0a8fad41.

## Related docs

- [RUNNERS.md](../RUNNERS.md), [HOSTED_REPOS.md](../HOSTED_REPOS.md),
  [OPERATIONS.md](../OPERATIONS.md), [IDS.md](../IDS.md),
  [docs/deploy/README.md](../deploy/README.md),
  [MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md),
  [CORE_PUSH.md](../CORE_PUSH.md)
- [Mirror node service](../../www/docs/operations/mirror-node.md)
- [Direct HTTPS mirror gateway](../../www/docs/operations/direct-https-mirror-gateway.md)
- [Git SSH gateway](../../www/docs/operations/git-ssh-gateway.md)
- [Mirror Actions control](../../www/docs/operations/mirror-actions-control.md)
- [Headless mirror refresh](../../www/docs/operations/headless-mirror-refresh.md) and [identity](../../www/docs/operations/headless-mirror-identity.md)
- [Node backup and recovery](../../www/docs/operations/node-backup-and-recovery.md)
- [Node operator policy](../../www/docs/operations/node-operator-policy.md)
- [Signed required checks](../../www/docs/operations/required-checks.md)
- [Pull request rules and model attestations](pull-rules.md)
- [Self-hostable service roles](../../www/docs/operations/service-roles.md)
- [Breakout authorization replica](../../www/docs/operations/breakout-authorization-replica.md)
- [Git conformance](../../www/docs/operations/git-conformance.md) and [compatibility](../../www/docs/git-compatibility.md)
- [Synchronization status](../../www/docs/sync-status.md)
- Topic pages: [agent-jobs.md](agent-jobs.md), [merge-executor.md](merge-executor.md),
  [intake-and-attestation.md](intake-and-attestation.md),
  [actions-runner.md](actions-runner.md), [gateways.md](gateways.md),
  [fleet-operations.md](fleet-operations.md)
