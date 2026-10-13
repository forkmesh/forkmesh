# Mirror node service

`forkmesh-mirror-node` is the small, headless runtime for public mirror hosts.
It replaces Git synchronization, gateway configuration, gateway/tunnel process
supervision, signed health/capability helpers, catalog publication, telemetry,
and local readiness reporting that previously ran inside the Qt event loop.

The service binds its operator API to loopback only:

- `GET /healthz` reports that the daemon event loop is alive;
- `GET /readyz` requires a running gateway and a recent successful sync; and
- `GET /v1/status` returns bounded, secret-free process, restart, sync, and
  repository-ref state.

The gateway stays on `127.0.0.1:8790` behind an operator-managed HTTPS reverse
proxy. Set `disableCloudflared` when that public origin is independently
reachable; otherwise the daemon continues to supervise Cloudflare Tunnel as a
fallback. An optional `mirrorUrl` advertises a separate direct Git origin while
`publicOrigin` remains the Tunnel control endpoint. The daemon reuses the
existing owner-only Ed25519 identity and, when enabled, connector token; it
never accepts either value in argv, JSON, an HTTP request, or a catalog record.
Its two gateway helper modes accept only the bounded ForkMesh health and masked
proxy protocols on stdin.

The daemon holds one WebSocket to the relay's
per-owner node event channel (`wss://…/api/nodes/events`, the ForkMeshNodes
Durable Object) authenticated with the node's own Ed25519 identity. The relay
pushes a payload-free `{"type":"event","topic","repo"}` frame the instant an
issue, pull, or discussion submission lands. The Go daemon leases only that
changed inbox, writes the repository-native record in a temporary Git worktree,
commits it, and acknowledges the exact durable row with a signed refs
attestation. There is **no** polling, fallback poll, desktop binary, or GUI
process in this path. Missed pushes are covered by the socket itself: every
reconnect fires one catch-up wake that drains whatever queued while the channel
was down.

Repository sync rides the same channel: the relay pushes a `commits` event
when a source node publishes changed public heads or tags, and the daemon
fetches on that push (plus one catch-up per reconnect). The event carries the
source's signed refs digest, so a successful fetch from a stale round-robin
peer is retried on a 2s→15s backoff until the advertised generation lands.
The source desktop also pushes every changed head and tag in the canonical
`forkmesh/forkmesh` working copy directly to each saved, installed managed
host, even when that host has no working-copy Git remote. A service-owned
`post-receive` hook rings the daemon's loopback-only `POST /v1/control/sync`
doorbell as soon as Git makes those refs durable; the desktop sends the same
authenticated wake explicitly for older installed daemons. An all-up-to-date
safety pass sends no wake, so this path remains change-driven.
Because notification fan-out is best-effort, an all-relay node also reconciles
once every 30 minutes; this bounds a completely lost live event without turning
the recovery path back into the fleet's normal workload.
Bootstrap records each exact managed peer URL in `eventDrivenUpstreams`, so a
direct `mirrorN` peer is treated like every other WebSocket-notified source
instead of falling back to the legacy 15-second external-upstream poll.
Third-party upstreams that cannot push and nodes with the catalog disabled
still use `syncInterval` directly. A
four-minute heartbeat cycle republishes the catalog record and renews the HTTPS
endpoint lease from local refs reads. The signed endpoint proof rides the
already-open node event WebSocket and uses HTTPS only when the socket or relay
does not advertise that frame.
If a tunnel health challenge is temporarily pending, its bounded retry repeats
only this local-state/lease publication. It does not fetch upstream Git data,
check web fork jobs, or consume queued collaboration work.

## Owner-authorized web merges

Every served public repository also advertises the gateway's `merge-preview`,
`merge-pull`, and `update-pull-branch` operations. The daemon upgrades an
existing gateway configuration in place at startup and points the gateway's
`mergeExecutorCommand` at this same binary
(`forkmesh-mirror-node --config … --execute-mirror-merge`). When an
organization owner merges a pull request on the web, the relay authenticates
the owner, signs the exact-OID request, and hands it to one healthy mirror;
the executor verifies the pinned pull record on the `forkmesh/pulls` ledger,
the current base branch tip, and (for a branch-backed pull) the current head
tip, enforces the same independent peer-review gate as the desktop (a
repository owner may waive it with `overrideReview`) and any
`.forkmesh/required-checks.json` policy on the base commit, and then moves the
base branch, the ledger and the pull's `forkmesh/pull/<n>` metadata branch in
one atomic `update-ref` transaction.
A patch-backed pull (a web or cross-node submission whose reviewed commits
travel as `commits.mbox` / `changes.patch` in the ledger) is replayed onto the
exact base tip in a temporary detached worktree, so the contributor's
authorship is preserved.

The result carries a fresh node-signed attestation of the refs the mirror
serves afterwards. The relay verifies it against the mirror's registered
endpoint key, admits that state into the repository's pin history, treats it
as the repository's *current* generation while the source-of-truth record is
unchanged (the source is typically offline), and pushes a `commits` event
naming that digest so every other mirror keeps fetching through the relay
route until the merge lands on it. The source desktop converges from the mesh
(fast-forward only) when it reconnects and its next publish supersedes the
override automatically. Nothing in this path rewinds a ref: a stale base,
head, or ledger pin fails closed and the owner reloads the pull.

Idempotency results and the temporary merge worktree live beside the gateway
configuration under `mirror-gateway/pull-jobs/` (owner-only). Set
`"disableMergeExecutor": true` in `mirror-node.json` to keep a node strictly
read-only; the gateway then advertises no write capability and the relay never
selects that node for a merge.

## Operator storage and legal-response policy

Operators who want to limit what their node stores and serves, and handle
reports, quarantines, appeals and removals, add an `operatorPolicy` entry to
the gateway configuration. See the
[node operator policy guide](node-operator-policy.md). The policy is local to
this node: anything it refuses stays available from other mirrors.

## Signing the agent CLIs in from the website

A node that finds `claude` or `codex` on its PATH advertises those providers in
its catalog record, which is what puts it in the website's "Run on" list. Those
CLIs still have to be signed in, and until now the only way to do that on a
headless mirror was an SSH shell opened from the ForkMesh desktop app — the
website's "Sign in to Claude Code on <node>" button was a `forkmesh://` link
that the browser handed to the operating system, so on any machine without
that app installed it resolved to nothing at all.

The owner's browser now streams the sign-in from the node itself. Both ends
hold a WebSocket to one per-owner relay object — this daemon with its signed
drain token, the browser with a short-lived account event ticket — and the
relay forwards the terminal frames between them without keeping a copy of
anything. Which object that is depends on the node: a managed mirror is its
own node account and opens the socket under that name, while a desktop node
opens it under the person's account and calls itself by its machine name. So
the browser names the machine it wants when it connects, and the relay — after
checking that the account owns it — joins the tab to whichever of the two
objects that machine is actually attached to. The daemon runs the provider's
own login command (`claude auth login`, `codex login`) on a pseudo-terminal as
its own service account, so the credential lands in `/var/lib/forkmesh` where
the agent CLIs look for it. Only that one command can be started: no argument
comes from the frame, there is no shell, and the browser chooses nothing but
the provider.
One session runs at a time and ends when the browser closes it, when the
command exits, or after thirty minutes.

Set `"disableAgentLogin": true` in `mirror-node.json` to refuse these sessions;
the SSH path from the desktop app then remains the only way in.

The unit ships with `MemoryDenyWriteExecute=false` for this reason alone: the
provider CLIs are V8 programs whose JIT maps executable pages and will not
start under that restriction. Every other confinement in the unit still applies
to the sign-in — no new privileges, an empty capability set, a read-only system
outside `/var/lib/forkmesh`, and a private `/dev` and `/tmp`.

## Running website agent jobs

A node that advertises a provider also runs the agent sessions the website
queues for it; the ForkMesh desktop app is not required. To offer Gemma, install
[Ollama](https://ollama.com/download) next to the Codex CLI: the node then
advertises `local-llama` and runs Gemma 4 through `codex exec --oss`. Pull the
models you want to serve ahead of time (`ollama pull gemma4:e4b`) so the first
run does not spend its time downloading. The relay rings the
node's event socket with an `org-agents` push, and the daemon claims the job
over the same signed `org-agent-jobs` lease the desktop uses, signing with the
node key its owner claimed. A five-minute safety read covers a push lost while
the node was offline.

For each job the daemon:

- rejects any envelope outside the desktop's rules: provider `claude-code`,
  `codex` or `local-llama` (Gemma), a model from the website allowlist (`agentWebsiteModels` in
  `server/agents.go`), a prompt of at most 8,000 characters, a kind of
  `start`, `continue` or `steer`, valid image attachments, and a job addressed
  to this node and repository;
- downloads any images attached to the prompt (at most five, 5 MiB each) over
  the job's signed lease, checks size, digest and magic bytes, and writes them
  into the working tree as `.forkmesh/attachments/<uuid>.<ext>`, which is
  excluded from commits (`server/agent_attachments.go`). A job whose images
  cannot be fetched fails instead of running without them;
- clones the repository into `mirror-gateway/agent-jobs/work/` and runs
  `claude -p --output-format stream-json --verbose
  --dangerously-skip-permissions` (or `codex -a never exec --sandbox
  danger-full-access`; `workspace-write` would also block the loopback
  sockets a test server needs) on a new `agent/web-<job>-<title>` branch. The
  agent never works inside the served repository and never touches `main`;
- pushes the finished branch back as one ref update, opens a node-signed pull
  when the session asked for one, and reports running, then completed or
  failed, with the transcript, token counts and cost.

There is no model safety preflight any more. `agentOwner` is still accepted in
`mirror-node.json` so older configurations keep parsing, but nothing reads it.

A run is bounded to two hours. A `continue` job (a session moved from a
desktop with **To cloud**) waits up to ten minutes for the moved branch to
reach this node, asking for a sync every 15 seconds, before it fails.

### What a failed run reports

A failed run reports one `lastError`; clients show it next to the transcript,
and it is no longer appended to the transcript as well. `server/agent_errors.go`
picks it, in order, from the Claude result event's `errors`/`result` text, an
API-error assistant message, the last non-empty lines of stderr, and only then
"Claude Code exited with status N". It is capped at 600 characters, and the
stream-json init event is never quoted. Codex gets the same stderr-tail order.

When the provider refuses the model because the host's CLI is too old ("does
not support this model; version X or newer is required", or Codex's "requires
a newer version of Codex"), the error reads "Claude Code on <node> is <ver>;
<model> needs <X> or newer. Update the host (Hosts → Update)." The runner then
remembers the refused version per provider and model, in memory; the next run
for that model checks `claude --version` (or `codex --version`) and fails at
once with the same message until the CLI has moved past it. An outdated CLI is
not retried with a fresh conversation, because the binary would be the same.

### Keeping the agent CLIs current

The service runs as `forkmesh-node` and cannot update the root-owned copies in
`/usr/local/bin`. The desktop does it: every host update pipeline (the per-host
**Update** in Hosts, **Install (binary)**, the fleet runs, and the verified
native mirror-node update) ends, after a successful update, with a root-only,
never-fatal refresh of each CLI already present in `/usr/local/bin`. It runs
the official installer, installs the new binary as `/usr/local/bin/<name>.new`
and renames it over the old one, so a running job keeps the binary it opened.
The update's own exit status is what the command returns. The script is
`agentCliServiceRefreshScript()` in `desktop/src/ControlNode.cpp`.

Every session the website queued appears in the ForkMesh desktop's Agents list
for the repository's owner, marked with a cloud on the row's avatar — the ones
a node is running, and the ones still waiting for a node, including work queued
for that desktop itself while it was closed. The Cloud
control in the list's toolbar narrows the list to cloud runs or to the
desktop's own sessions, and searching for a node's name finds its runs.
Opening one shows its conversation, branch and pull request; typing under it
queues a follow-up for the node, and the row's trash button deletes the session
from the relay (a run already in flight on the node is not interrupted). A
desktop that signed in with its account key rather than a password reads and
steers these sessions with a key-signed `user`/`ts`/`sig` request; each
operation has its own proof (`forkmesh-owner-agent-list-v1`, `-read-v1`,
`-prompt-v1`, `-delete-v1`) bound to the exact path, and a follow-up's proof
also binds the SHA-256 of its prompt. Queueing new work is not key-signable.

The other direction starts on the desktop: right-click a session in the Agents
list and choose **Continue in the cloud** (or press **To cloud** in the
session's header). The desktop commits the session's worktree, publishes its
branch to the served mirror, queues the continuation for the account's other
nodes and stops running it locally; the session then shows up on the website
and in the list as a cloud run. Only Claude Code and Codex sessions on a
published repository the desktop can sign for can move; for any other row the
menu entry is disabled and its tooltip says why.

A run that is still waiting can go the other way at any time: right-click it
and choose **Continue on this desktop** (or press **Continue here** in its
header). The desktop asks the relay for that session by name — `POST
/api/repo/{owner}/{repo}/agent-sessions/{id}/claim`, signed with the same node
proof the job queue answers to — and the relay hands over the same lease the
periodic drain would have given it, re-targeting the job and the session at
that node. The run then starts, reports and opens its pull request exactly as
it would have on the mirror, and the list shows it once, as a local session.
This is the answer to a job the chosen mirror never picked up: the claim works
whichever of the account's nodes it was queued for, but only while no node has
started it — once one has, the conversation lives there and a follow-up is the
way to reach it. A desktop that cannot run it (no checkout of the repository,
no provider CLI, no signing key for the account) says so and leaves the run
queued rather than claiming a job it would only fail.

Set `"disableAgentJobs": true` in `mirror-node.json` to refuse these jobs. The
node then stops advertising its providers, so the website no longer offers it as
an agent target.

## Host hardening and logs

New Vultr mirrors installed from the desktop get these at install time
(`desktop/src/MainWindowHostInstall.cpp` carries the same lines inline, so
change both copies together):

- **Key-only SSH.** `server/packaging/ssh/10-forkmesh-hardening.conf` goes to
  `/etc/ssh/sshd_config.d/`: no password or keyboard-interactive logins, root
  by key only, `MaxAuthTries 3`, `LoginGraceTime 20`. It is applied only when
  root has an authorized key and `sshd -t` accepts it. For an existing host,
  run `tools/harden_mirror_sshd.sh <host-ip>...`; it also proves a fresh key
  login works and a password login is refused afterwards.
- **fail2ban.** `server/packaging/fail2ban/forkmesh-sshd.local` enables the
  `sshd` jail in aggressive mode, reading the systemd journal (Debian 13 has no
  `auth.log`): 5 failures in 10 minutes bans for 1 hour, growing up to a week
  for repeat offenders. For an existing host, run
  `tools/mirror_fail2ban.sh <host-ip>...` after the sshd hardening.
- **Host logs over HTTP/3.** The gateway serves the host's journal as
  server-sent events at `GET /v1/node/logs` to Ed25519 viewer keys listed in
  `/etc/forkmesh/log-viewer-keys` (one single-use signed request,
  `forkmesh-node-logs-v1`, 60-second window; see `tools/mirror_gateway.py`).
  Caddy serves it on HTTP/3, so the installer opens 443/udp and lists the
  deploying desktop's key; the unit joins `systemd-journal` to read it. The
  desktop reads it with its own QUIC client (`desktop/src/Http3Get.cpp`) and
  falls back to SSH when a host has no endpoint.

## Build and test

```sh
cd server
go test -race -cover ./...
CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' \
  -o forkmesh-mirror-node ./cmd/forkmesh-mirror-node
```

## Desktop companion

Vultr/host deploys upload the deploying desktop's own local
`forkmesh-mirror-node`, looked up in `PATH`, beside the running executable,
beside the installed client (`~/.local/bin/forkmesh`), then in the source
checkout. The desktop installer (`desktop/install.sh`), the public release
installer, and both in-app update flows (the source rebuild and the prebuilt
auto-update) install or refresh the companion beside the client. A desktop
whose Vultr install fails with "The Go mirror-node package is incomplete
(forkmesh-mirror-node is missing)" therefore heals by running Update & restart
(with a Go toolchain available for source installs) or by reinstalling a
release that publishes the `forkmesh-mirror-node-<os>-<arch>` asset.

Use `server/packaging/systemd/mirror-node.json.example` as the configuration template.
Every upstream list may contain multiple URLs; the daemon tries them in order,
and the public ForkMesh URL itself routes across the currently healthy mirror
set rather than pinning mirror2.

For a tunnel-free bootstrap, pass `--disable-cloudflared` with the public HTTPS
origin. The service writes a signed direct-origin manifest and advertises
`https://<origin>/<owner>/<repository>.git`. A stable ForkMesh clone URL remains
the repository identity and redirects only while this direct URL passes the
same signed refs and operation proof. When a Tunnel endpoint is also configured,
its existing masked proxy path remains the fallback.

To retain the Tunnel while advertising a separate public Git origin, set
`mirrorUrl` in the service configuration (or pass `--mirror-url` at bootstrap)
and route that HTTPS hostname to the same loopback gateway.

## Independent dashboard, SSH writes, and forks

A mirror on its own subdomain (for example `mirror33.forkmesh.com`) can run a
fully independent public Git service — website, SSH push, PR submission, and
forking — that keeps working even when the control-plane Worker is unreachable.
See [direct-https-mirror-gateway.md](direct-https-mirror-gateway.md#independent-mirror-operation)
for the gateway `dashboard` block, the SSH `localAuthorization` broker mode, and
the fork registry. Fresh native ForkMesh bootstraps enable this dashboard by
default when their catalog uses a credential-free HTTPS origin. Existing nodes
can set `dashboard.replicaPath` to retain the controller's last
validated public status/network projections locally; the daemon refreshes the
bundle after normal repository synchronization and retains it through an
outage. Repository identity is unchanged: the dashboard, SSH origin, and mirror
URLs are all replaceable, discoverable metadata layered over the same canonical
repositories. D1 account rows, password hashes, and controller bearer tokens
are never replicated. A controller login issues a separate signed,
expiry-bounded portable browser grant containing only an opaque account/session
identity and public username; direct mirrors verify it with the router public
key so the same browser remains signed in during a controller outage. Breakout
repository management uses a minimal signed SQLite authorization projection
containing only enrolled public authentication keys, repository roles, and
revocation generations; successful key proof mints a short node-local session.

## Backup, recovery, and preflight

`--preflight` checks a node without changing it: configuration, identity,
compatibility, disk space, and whether backups and drills are recent.
`--backup`, `--restore-drill`, and `--restore` handle encrypted whole-node
backups. `--export-identity` and `--import-identity` move the node key. See
[node backup, recovery, and key moves](node-backup-and-recovery.md).

## Rolling migration

1. Install the binary, configuration, and unit without stopping the old node.
2. Run `forkmesh-mirror-node --config ... --check` and
   `forkmesh-mirror-node --config ... --preflight` as `forkmesh-node`.
3. Start `forkmesh-mirror-node` and confirm `/readyz`.
4. Remove any retired `forkmesh-node` or `forkmesh-qt-intake` compatibility
   unit; the Go daemon owns serving and native collaboration intake.
5. Confirm signed external health, one pushed collaboration submission, and one clone before
   moving to the next host.

Keep the prior unit and binaries as rollback artifacts until the whole fleet has
passed reachability checks.

## A new mirror that never publishes

`journalctl -u forkmesh-mirror-node` shows the whole failure chain when a fresh
node stays invisible. A first clone that drops mid-pack ("fetch-pack: unexpected
disconnect", "early EOF") leaves `/var/lib/forkmesh/repositories` empty, and the
rest follows from that: the gateway exits every five seconds with "enabled
public repository requires integrity.expectedRefsSha256" because it will not
serve a repository whose refs it cannot read, and each cycle reports "read
stable refs: exit status 128" and "repository head or state hash unavailable".
The daemon now retries that clone on 15s→5m backoff until one lands; restarting
the service forces an immediate attempt.
