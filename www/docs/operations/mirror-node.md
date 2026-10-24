# Mirror node service

`forkmesh-mirror-node` is the headless Go runtime for a public mirror host. It
synchronizes repositories, supervises the loopback Git gateway (and optionally a
Cloudflare Tunnel), publishes the node's signed catalog record and endpoint
lease, runs collaboration intake, and can execute owner-authorized merges and
website agent jobs. No desktop app or GUI process is involved.

## Architecture

- **Operator API, loopback only.** `GET /healthz` (event loop alive),
  `GET /readyz` (gateway running and a recent successful sync), `GET /v1/status`
  (bounded, secret-free process, sync and ref state) and `POST /v1/control/sync`
  (sync doorbell).
- **Gateway.** Listens on `127.0.0.1:8790` behind an operator-managed HTTPS
  reverse proxy. Set `disableCloudflared` when that origin is directly
  reachable; otherwise the daemon supervises Cloudflare Tunnel as a fallback.
  `mirrorUrl` advertises a separate direct Git origin while `publicOrigin`
  stays the Tunnel control endpoint.
- **Identity.** The owner-only Ed25519 node key (and the connector token, when
  enabled) is never accepted in argv, JSON, an HTTP request or a catalog record.
- **Event socket.** One WebSocket to the relay's per-owner node event channel
  (`wss://…/api/nodes/events`), authenticated with the node key. The relay
  pushes payload-free `{"type":"event","topic","repo"}` frames.

## Data flow

- **Collaboration intake.** An issue, pull or discussion frame makes the daemon
  lease only the changed inbox, write the repository-native record in a
  temporary worktree, commit it, and acknowledge the exact rows with a signed
  refs attestation. There is no poll; every reconnect fires one catch-up drain.
- **Repository sync.** A `commits` frame triggers a fetch. It carries the
  source's signed refs digest, so a fetch from a stale peer is retried on a
  2s→15s backoff until that generation lands. Direct pushes to a managed host
  ring the doorbell from a service-owned `post-receive` hook. Peers listed in
  `eventDrivenUpstreams` are push-notified; upstreams that cannot push use
  `syncInterval`. One reconciliation every 30 minutes covers a lost event.
- **Catalog and lease.** A four-minute heartbeat republishes the catalog record
  and renews the endpoint lease from local refs reads, over the event socket
  when the relay advertises that frame and over HTTPS otherwise.
- **First clone.** Until a repository has a readable clone the daemon retries it
  on 15s→5m backoff; the gateway refuses to serve refs it cannot pin
  (`integrity.expectedRefsSha256`). `journalctl -u forkmesh-mirror-node` shows
  the chain.

## Owner-authorized web merges

Every served repository advertises `merge-preview`, `merge-pull` and
`update-pull-branch`. The gateway's `mergeExecutorCommand` points at this binary
(`forkmesh-mirror-node --config … --execute-mirror-merge`). The relay
authenticates the account (an owner to merge; for `update-pull-branch`, anyone
who may push the pull's head branch), signs the exact-OID request and hands it
to one healthy mirror. The executor verifies the pinned pull record on the `forkmesh/pulls`
ledger, the base and head tips, the peer-review gate (an owner may waive it with
`overrideReview`) and any `.forkmesh/required-checks.json` policy, then moves the
base branch, the ledger and `forkmesh/pull/<n>` in one atomic `update-ref`.
Patch-backed pulls are replayed onto the exact base in a temporary worktree,
preserving authorship. The result carries a node-signed refs attestation that
the relay admits as the repository's current generation and announces with a
`commits` event. Nothing rewinds a ref: a stale pin fails closed. Job state lives
under `mirror-gateway/pull-jobs/` (owner-only). `"disableMergeExecutor": true`
keeps a node strictly read-only.

## Running website agent jobs

A node that finds `claude` or `codex` on its PATH (or Ollama beside Codex, for
`local-llama`) advertises those providers and runs sessions the website queues
for it. The relay rings the socket with an `org-agents` push; the daemon claims
the job over the signed `org-agent-jobs` lease. For each job it validates the
envelope (provider, model allowlist, prompt of at most 8,000 characters, kind
`start`/`continue`/`steer`, target node and repository), fetches up to five
verified image attachments, clones into `mirror-gateway/agent-jobs/work/`, runs
the provider on a new `agent/web-<job>-<title>` branch (never `main`), pushes
the branch, optionally opens a node-signed pull, and reports status, transcript,
token counts and cost. A run is capped at two hours; a failed run reports one
bounded `lastError`. The browser can sign a provider CLI in through a relayed
pseudo-terminal that runs only the provider's own login command.
`"disableAgentJobs": true` and `"disableAgentLogin": true` turn these off.

## Policy, hardening and logs

- Storage limits, takedowns and protected branches: see the
  [node operator policy guide](node-operator-policy.md).
- Key-only SSH (`server/packaging/ssh/10-forkmesh-hardening.conf`) and a
  fail2ban `sshd` jail (`server/packaging/fail2ban/forkmesh-sshd.local`).
- The gateway streams the host journal at `GET /v1/node/logs` to Ed25519 viewer
  keys in `/etc/forkmesh/log-viewer-keys` (signed, single-use requests).

## Setup

Start from `server/packaging/systemd/mirror-node.json.example`. Upstream lists
may hold several URLs, tried in order. For a tunnel-free bootstrap pass
`--disable-cloudflared` with the public HTTPS origin; the node then advertises
`https://<origin>/<owner>/<repository>.git` behind a signed direct-origin
manifest. Independent dashboard, SSH writes and forks are covered in
[direct-HTTPS mirror gateway](direct-https-mirror-gateway.md); backups,
`--preflight` and key moves in
[node backup, recovery, and key moves](node-backup-and-recovery.md).

## Build and test

```sh
cd server
go test -race -cover ./...
CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' \
  -o forkmesh-mirror-node ./cmd/forkmesh-mirror-node
```

## Rolling migration

1. Install the binary, configuration and unit without stopping the old node.
2. Run `forkmesh-mirror-node --config ... --check` and `--preflight` as
   `forkmesh-node`.
3. Start `forkmesh-mirror-node` and confirm `/readyz`.
4. Remove any retired compatibility unit.
5. Confirm signed external health, one pushed collaboration submission and one
   clone before moving to the next host.

Keep the prior unit and binary as rollback artifacts until the whole fleet has
passed reachability checks.
