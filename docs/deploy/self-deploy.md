# Self-deploy: the cores deploy phone-approved merges

`deploy/core/self_deploy.py` runs on every core as `fmnext-self-deploy.service`
(rendered by `add_self_deploy_files` in `deploy/core/render.py` when
`[self_deploy] enabled = true` and `approver_keys` is not empty). It deploys a
merge to `main` that an enrolled phone signed, so a merge ships while the
operator box is offline. Approvals are recorded in `deploy_approvals`
(`app/src/deploy_approvals.py`); the phone side is described in
[../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md).

Each core polls rqlite every 15 s and checks the approval's Ed25519 signature
against `approver_keys`, its age (two hours), that the merged commit is the
approved one, and that it is on `main`. Cores work in a per-approval
rendezvous order (sha256 of `<approval id>:<core>`). A core activates only
after every live core before it reports `deployed` or `skipped`; a failure
stops the rest. A core without a fresh `cluster_members` heartbeat neither
blocks nor holds the approval open. Progress is in `deploy_approval_nodes`;
the outcome is announced to operators and the approver.

## What a core does

| Change | Action |
| --- | --- |
| App release paths (same rules as `tools/next_autodeploy.py`) | build under `fmnext-build` in a memory-limited scope, upload locally, the first core to activate migrates, then activate |
| Go daemon (`server/`, daemon tools including `tools/mirror_gateway.py`) | roll the daemon (below) |
| Other roll paths (`deploy/core/`) | nothing; rendered node files still need `next_fleet.py roll` from the operator box |
| Nothing served | `skipped` |

## Daemon roll

1. **Build** `server/cmd/forkmesh-mirror-node` offline (`GOTOOLCHAIN=local`,
   `GOPROXY=off`, standard library only) in a memory-limited scope. Cores need
   `golang-go`; `roll_daemon = false` turns this off.
2. **Pre-check** the new binary with `--check` against the core's own config.
3. **Swap** inside an IDS window under the activation lock, saving the
   previous binary, config and tools.
4. **Health:** the unit must stay active without restarts for 20 s within
   90 s, else the previous files are restored and the approval fails.

Differences from `next_fleet.py roll`: no DNS drain (the DNS token stays on
the operator box), no config re-render, and runners are not rolled (the next
operator auto-deploy rolls them). The rolled revision is recorded in
`/var/lib/fmnext-self-deploy/daemon-rev`.
